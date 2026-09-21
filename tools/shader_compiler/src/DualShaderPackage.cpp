// ============================================================================
// DualShaderPackage.cpp — M6-04 双后端 shader package 离线证据
// 里程碑：M6-04（Pipeline、Binding 与 Shader 契约）
// 职责：对固定 graphics pass 同时运行 D3DCompileFromFile/FXC 与既有 DXC，
//       读取两份实际 shader reflection，比较规范化 source closure 与接口，
//       全部成功后再发布可消费的 package manifest.json。
// 失败语义：所有中间产物只写入新的 revision candidate；编译、reflection、
//       语义比较或发布失败时，既有 package manifest 保持最后一次正确版本。
// 关联：tools/shader_compiler/src/DxcCompiler.h（M5 DXC 入口）
//       samples/common/ParityIdentity.h（语义 token/closure 口径的参考实现）
//       docs/architecture/README.md
// ============================================================================
#include "DualShaderPackage.h"

#include "DxcCompiler.h"

#include <MiniEngine/Assets/Sha256.h>

#include <Windows.h>

#include <d3d11shader.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace MiniEngine::ShaderCompiler
{
namespace
{
using Microsoft::WRL::ComPtr;

struct AssetSpec final
{
    std::string sourceStem;
    std::string entry;
    std::string stage;
    std::string d3d11Source;
    std::string d3d12Source;
};

struct SourceFileEvidence final
{
    std::string repositoryPath;
    std::string relativeName;
    std::string sourceHash;
    std::string semanticTokens;
};

struct SourceClosure final
{
    std::vector<SourceFileEvidence> files;
};

struct BindingEvidence final
{
    std::string name;
    std::string kind;
    std::uint32_t registerIndex = 0U;
    std::uint32_t space = 0U;
    std::uint32_t count = 0U;
    std::uint32_t uniformBytes = 0U;
    std::string dimension;
    bool comparisonSampler = false;
};

struct VertexInputEvidence final
{
    std::string semantic;
    std::uint32_t index = 0U;
    std::uint32_t components = 0U;
};

struct ReflectionEvidence final
{
    std::vector<BindingEvidence> bindings;
    std::vector<VertexInputEvidence> vertexInputs;
    std::uint32_t colorOutputMask = 0U;
    bool writesDepth = false;
};

struct PdbEvidence final
{
    std::string name;
    std::string type;
    std::string sha256;
    std::size_t bytes = 0U;
    std::filesystem::path path;
};

struct VariantEvidence final
{
    std::string backend;
    std::string sourcePath;
    std::string sourceHash;
    std::filesystem::path bytecodePath;
    std::string bytecodeFormat;
    std::string bytecodeHash;
    std::size_t bytecodeBytes = 0U;
    std::string compiler;
    std::string compilerVersion;
    std::string target;
    std::vector<std::string> options;
    SourceClosure closure;
    ReflectionEvidence reflection;
    std::string semanticSourceKey;
    std::string semanticSourceHash;
    std::string reflectionKey;
    std::string semanticHash;
    std::optional<PdbEvidence> pdb;
};

struct PackageAsset final
{
    AssetSpec spec;
    VariantEvidence d3d11;
    VariantEvidence d3d12;
};

std::string HrString(const HRESULT hr)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(hr);
    return stream.str();
}

std::string Win32ErrorString(const DWORD error)
{
    return "Win32=" + std::to_string(static_cast<unsigned long>(error));
}

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        throw std::runtime_error{"cannot open shader file: " + path.string()};
    }

    const std::streampos end = file.tellg();
    if (end < 0)
    {
        throw std::runtime_error{"cannot determine shader file size: " + path.string()};
    }
    const std::size_t size = static_cast<std::size_t>(end);
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size);
    if (size > 0U && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
    {
        throw std::runtime_error{"cannot read shader file: " + path.string()};
    }
    return bytes;
}

void WriteFileBytes(const std::filesystem::path& path, const void* data, const std::size_t size)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        throw std::runtime_error{"cannot create shader artifact directory: " + path.parent_path().string() + ": " +
                                 error.message()};
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error{"cannot write shader artifact: " + path.string()};
    }
    if (size > 0U)
    {
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!file.good())
    {
        throw std::runtime_error{"write failed for shader artifact: " + path.string()};
    }
}

std::string Sha256Hex(const std::span<const std::byte> bytes)
{
    return MiniEngine::Assets::ToHexDigest(MiniEngine::Assets::Sha256(bytes));
}

std::string Sha256Text(const std::string& text)
{
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return Sha256Hex(std::span<const std::byte>{data, text.size()});
}

std::string JsonEscape(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string JsonString(const std::string& text)
{
    return "\"" + JsonEscape(text) + "\"";
}

std::filesystem::path NormalizeRoot(const std::filesystem::path& repositoryRoot)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(repositoryRoot, error);
    if (error)
    {
        throw std::runtime_error{"cannot resolve --repo '" + repositoryRoot.string() + "': " + error.message()};
    }
    return std::filesystem::path{absolute.lexically_normal().generic_string()};
}

bool IsPathInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    const std::string rootText = root.lexically_normal().generic_string();
    const std::string candidateText = candidate.lexically_normal().generic_string();
    if (candidateText == rootText)
    {
        return true;
    }
    return candidateText.size() > rootText.size() && candidateText.rfind(rootText + "/", 0U) == 0U;
}

std::string RelativePath(const std::filesystem::path& root, const std::filesystem::path& path)
{
    const std::filesystem::path normalizedRoot{root.lexically_normal().generic_string()};
    const std::filesystem::path normalizedPath{path.lexically_normal().generic_string()};
    if (!IsPathInside(normalizedRoot, normalizedPath))
    {
        throw std::runtime_error{"shader path escapes repository root: " + path.string()};
    }
    const std::filesystem::path relative = normalizedPath.lexically_relative(normalizedRoot);
    if (relative.empty())
    {
        throw std::runtime_error{"shader path has empty repository-relative name: " + path.string()};
    }
    return relative.generic_string();
}

std::string StripComments(const std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    bool lineComment = false;
    bool blockComment = false;
    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        const char character = source[index];
        const char next = index + 1U < source.size() ? source[index + 1U] : '\0';
        if (!lineComment && !blockComment && character == '/' && next == '/')
        {
            lineComment = true;
            result += ' ';
            ++index;
            continue;
        }
        if (!lineComment && !blockComment && character == '/' && next == '*')
        {
            blockComment = true;
            result += ' ';
            ++index;
            continue;
        }
        if (lineComment)
        {
            if (character == '\n')
            {
                lineComment = false;
                result += '\n';
            }
            continue;
        }
        if (blockComment)
        {
            if (character == '*' && next == '/')
            {
                blockComment = false;
                result += ' ';
                ++index;
            }
            else if (character == '\n')
            {
                result += '\n';
            }
            continue;
        }
        result += character;
    }
    return result;
}

std::vector<std::string> IncludedFiles(const std::string_view source)
{
    std::vector<std::string> includes;
    std::istringstream lines(StripComments(source));
    std::string line;
    while (std::getline(lines, line))
    {
        const std::size_t includePosition = line.find("#include");
        if (includePosition == std::string::npos)
        {
            continue;
        }
        const std::size_t opening = line.find_first_of("\"<", includePosition + 8U);
        if (opening == std::string::npos)
        {
            continue;
        }
        const char closingCharacter = line[opening] == '"' ? '"' : '>';
        const std::size_t closing = line.find(closingCharacter, opening + 1U);
        if (closing == std::string::npos)
        {
            throw std::runtime_error{"malformed shader include: " + line};
        }
        includes.push_back(line.substr(opening + 1U, closing - opening - 1U));
    }
    return includes;
}

std::map<std::string, std::string> ReadBindingMacros(const std::filesystem::path& bindingContract)
{
    std::map<std::string, std::string> macros;
    if (!std::filesystem::exists(bindingContract))
    {
        return macros;
    }

    const std::vector<std::byte> bytes = ReadFileBytes(bindingContract);
    const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    std::istringstream lines(StripComments(text));
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream words(line);
        std::string directive;
        std::string name;
        std::string value;
        if (words >> directive >> name >> value && directive == "#define" && name.rfind("ME_", 0U) == 0U)
        {
            macros[name] = value;
        }
    }
    return macros;
}

std::string NormalizeTokens(const std::string_view source, const std::map<std::string, std::string>& macros)
{
    const std::string stripped = StripComments(source);
    std::istringstream lines(stripped);
    std::vector<std::string> sourceLines;
    std::string line;
    while (std::getline(lines, line))
    {
        sourceLines.push_back(line);
    }

    std::size_t first = 0U;
    while (first < sourceLines.size() && sourceLines[first].find_first_not_of(" \t\r") == std::string::npos)
    {
        ++first;
    }
    std::size_t last = sourceLines.size();
    while (last > first && sourceLines[last - 1U].find_first_not_of(" \t\r") == std::string::npos)
    {
        --last;
    }

    std::string guard;
    if (first < last)
    {
        std::istringstream words(sourceLines[first]);
        std::string directive;
        std::string name;
        if (words >> directive >> name && directive == "#ifndef" && name.rfind("MINIENGINE_", 0U) == 0U)
        {
            guard = name;
        }
    }

    std::string filtered;
    for (std::size_t index = 0U; index < sourceLines.size(); ++index)
    {
        line = sourceLines[index];
        std::istringstream words(line);
        std::string directive;
        std::string name;
        words >> directive >> name;
        if (directive == "#include")
        {
            continue;
        }
        if (index == first && !guard.empty())
        {
            continue;
        }
        if (index == first + 1U && !guard.empty() && directive == "#define" && name == guard)
        {
            continue;
        }
        if (index + 1U == last && !guard.empty() && directive == "#endif")
        {
            continue;
        }
        filtered += line;
        filtered += '\n';
    }

    std::string result;
    std::string token;
    const auto flush = [&result, &token, &macros]()
    {
        if (token.empty())
        {
            return;
        }
        if (!result.empty())
        {
            result += ' ';
        }
        const auto macro = macros.find(token);
        result += macro == macros.end() ? token : macro->second;
        token.clear();
    };

    for (const unsigned char character : filtered)
    {
        if (std::isalnum(character) != 0 || character == '_')
        {
            token += static_cast<char>(character);
            continue;
        }
        flush();
        if (std::isspace(character) == 0)
        {
            if (!result.empty())
            {
                result += ' ';
            }
            result += static_cast<char>(character);
        }
    }
    flush();
    return result;
}

SourceClosure BuildSourceClosure(const std::filesystem::path& repositoryRoot, const std::string& sourceRelative)
{
    const std::filesystem::path sourcePath = repositoryRoot / sourceRelative;
    const std::filesystem::path backendRoot = sourcePath.parent_path();
    const std::map<std::string, std::string> macros = ReadBindingMacros(backendRoot / "BindingContract.hlsli");
    std::map<std::string, SourceFileEvidence> collected;

    const auto collect = [&](const auto& self, const std::filesystem::path& path) -> void
    {
        const std::filesystem::path normalizedPath{path.lexically_normal().generic_string()};
        if (!IsPathInside(repositoryRoot, normalizedPath))
        {
            throw std::runtime_error{"shader include escapes repository root: " + path.string()};
        }
        const std::string repositoryPath = RelativePath(repositoryRoot, normalizedPath);
        if (collected.contains(repositoryPath))
        {
            return;
        }

        const std::vector<std::byte> bytes = ReadFileBytes(normalizedPath);
        const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        SourceFileEvidence evidence;
        evidence.repositoryPath = repositoryPath;
        evidence.relativeName = normalizedPath.lexically_relative(backendRoot).generic_string();
        evidence.sourceHash = Sha256Hex(std::span<const std::byte>{bytes.data(), bytes.size()});
        evidence.semanticTokens =
            evidence.relativeName == "BindingContract.hlsli" ? std::string{} : NormalizeTokens(text, macros);
        collected.emplace(repositoryPath, evidence);

        for (const std::string& include : IncludedFiles(text))
        {
            self(self, normalizedPath.parent_path() / include);
        }
    };

    collect(collect, sourcePath);

    SourceClosure closure;
    for (auto& [repositoryPath, evidence] : collected)
    {
        (void)repositoryPath;
        closure.files.push_back(std::move(evidence));
    }
    std::sort(closure.files.begin(), closure.files.end(),
              [](const SourceFileEvidence& left, const SourceFileEvidence& right)
              { return left.relativeName < right.relativeName; });
    return closure;
}

std::string UpperAscii(const std::string& text)
{
    std::string result = text;
    for (char& character : result)
    {
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - ('a' - 'A'));
        }
    }
    return result;
}

bool IsSystemSemantic(const char* semanticName, const D3D_NAME systemValue)
{
    if (systemValue != D3D_NAME_UNDEFINED)
    {
        return true;
    }
    return semanticName != nullptr && UpperAscii(semanticName).rfind("SV_", 0U) == 0U;
}

void AddVertexInput(ReflectionEvidence& reflection, const char* semanticName, const UINT semanticIndex, const UINT mask,
                    const D3D_NAME systemValue)
{
    if (semanticName == nullptr || IsSystemSemantic(semanticName, systemValue))
    {
        return;
    }
    const std::uint32_t components = static_cast<std::uint32_t>(std::popcount(static_cast<unsigned int>(mask & 0xFU)));
    if (components == 0U)
    {
        throw std::runtime_error{"shader reflection returned a vertex input with no components: " +
                                 std::string{semanticName}};
    }
    reflection.vertexInputs.push_back(VertexInputEvidence{semanticName, semanticIndex, components});
}

void AddPixelOutput(ReflectionEvidence& reflection, const char* semanticName, const UINT semanticIndex,
                    const D3D_NAME systemValue)
{
    const std::string semantic = semanticName == nullptr ? std::string{} : UpperAscii(semanticName);
    if (systemValue == D3D_NAME_TARGET || semantic.rfind("SV_TARGET", 0U) == 0U)
    {
        if (semanticIndex >= 32U)
        {
            throw std::runtime_error{"shader reflection returned an output target index >= 32"};
        }
        reflection.colorOutputMask |= 1U << semanticIndex;
    }
    if (semantic == "SV_DEPTH" || semantic == "SV_DEPTHGREATEREQUAL" || semantic == "SV_DEPTHLESSEQUAL")
    {
        reflection.writesDepth = true;
    }
}

std::string BindingKind(const D3D_SHADER_INPUT_TYPE type)
{
    switch (type)
    {
    case D3D_SIT_CBUFFER:
        return "cb";
    case D3D_SIT_TEXTURE:
        return "texture";
    case D3D_SIT_SAMPLER:
        return "sampler";
    default:
        throw std::runtime_error{"unsupported reflected shader resource type: " +
                                 std::to_string(static_cast<int>(type))};
    }
}

std::string DimensionName(const D3D_SRV_DIMENSION dimension)
{
    switch (dimension)
    {
    case D3D_SRV_DIMENSION_UNKNOWN:
        return "unknown";
    case D3D_SRV_DIMENSION_BUFFER:
        return "buffer";
    case D3D_SRV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case D3D_SRV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case D3D_SRV_DIMENSION_TEXTURE2DMS:
        return "texture2dms";
    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
        return "texture2dmsarray";
    case D3D_SRV_DIMENSION_TEXTURE3D:
        return "texture3d";
    case D3D_SRV_DIMENSION_TEXTURECUBE:
        return "texturecube";
    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
        return "texturecubearray";
    case D3D_SRV_DIMENSION_BUFFEREX:
        return "bufferex";
    default:
        return "unknown";
    }
}

BindingEvidence MakeBinding(const char* name, const D3D_SHADER_INPUT_TYPE type, const D3D_SRV_DIMENSION dimension,
                            const UINT bindPoint, const UINT bindCount, const UINT flags, const UINT space,
                            const UINT uniformBytes)
{
    if (name == nullptr)
    {
        throw std::runtime_error{"shader reflection returned a resource without a name"};
    }
    BindingEvidence binding;
    binding.name = name;
    binding.kind = BindingKind(type);
    binding.registerIndex = bindPoint;
    binding.space = space;
    binding.count = bindCount;
    binding.uniformBytes = uniformBytes;
    binding.dimension = binding.kind == "texture" ? DimensionName(dimension) : "none";
    binding.comparisonSampler = binding.kind == "sampler" && (flags & D3D_SIF_COMPARISON_SAMPLER) != 0U;
    return binding;
}

ReflectionEvidence ReflectD3D11(ID3DBlob* bytecode, const std::string& stage)
{
    if (bytecode == nullptr || bytecode->GetBufferPointer() == nullptr || bytecode->GetBufferSize() == 0U)
    {
        throw std::runtime_error{"D3D11 reflection received an empty bytecode blob"};
    }

    ComPtr<ID3D11ShaderReflection> shader;
    const HRESULT reflectHr =
        D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), IID_PPV_ARGS(&shader));
    if (FAILED(reflectHr) || shader == nullptr)
    {
        throw std::runtime_error{"D3DReflect failed: " + HrString(reflectHr)};
    }

    D3D11_SHADER_DESC description{};
    const HRESULT descHr = shader->GetDesc(&description);
    if (FAILED(descHr))
    {
        throw std::runtime_error{"ID3D11ShaderReflection::GetDesc failed: " + HrString(descHr)};
    }

    ReflectionEvidence reflection;
    for (UINT index = 0U; index < description.BoundResources; ++index)
    {
        D3D11_SHADER_INPUT_BIND_DESC bindingDescription{};
        const HRESULT bindingHr = shader->GetResourceBindingDesc(index, &bindingDescription);
        if (FAILED(bindingHr))
        {
            throw std::runtime_error{"ID3D11ShaderReflection::GetResourceBindingDesc failed: " + HrString(bindingHr)};
        }
        UINT uniformBytes = 0U;
        if (bindingDescription.Type == D3D_SIT_CBUFFER)
        {
            if (bindingDescription.Name == nullptr)
            {
                throw std::runtime_error{"D3D11 cbuffer reflection returned no name"};
            }
            ID3D11ShaderReflectionConstantBuffer* constantBuffer =
                shader->GetConstantBufferByName(bindingDescription.Name);
            if (constantBuffer == nullptr)
            {
                throw std::runtime_error{"D3D11 reflection could not resolve cbuffer: " +
                                         std::string{bindingDescription.Name}};
            }
            D3D11_SHADER_BUFFER_DESC bufferDescription{};
            const HRESULT bufferHr = constantBuffer->GetDesc(&bufferDescription);
            if (FAILED(bufferHr))
            {
                throw std::runtime_error{"D3D11 cbuffer GetDesc failed: " + HrString(bufferHr)};
            }
            uniformBytes = bufferDescription.Size;
        }
        reflection.bindings.push_back(MakeBinding(
            bindingDescription.Name, bindingDescription.Type, bindingDescription.Dimension,
            bindingDescription.BindPoint, bindingDescription.BindCount, bindingDescription.uFlags, 0U, uniformBytes));
    }

    if (stage == "vs")
    {
        for (UINT index = 0U; index < description.InputParameters; ++index)
        {
            D3D11_SIGNATURE_PARAMETER_DESC parameter{};
            const HRESULT parameterHr = shader->GetInputParameterDesc(index, &parameter);
            if (FAILED(parameterHr))
            {
                throw std::runtime_error{"D3D11 input signature reflection failed: " + HrString(parameterHr)};
            }
            AddVertexInput(reflection, parameter.SemanticName, parameter.SemanticIndex, parameter.Mask,
                           parameter.SystemValueType);
        }
    }
    else if (stage == "ps")
    {
        for (UINT index = 0U; index < description.OutputParameters; ++index)
        {
            D3D11_SIGNATURE_PARAMETER_DESC parameter{};
            const HRESULT parameterHr = shader->GetOutputParameterDesc(index, &parameter);
            if (FAILED(parameterHr))
            {
                throw std::runtime_error{"D3D11 output signature reflection failed: " + HrString(parameterHr)};
            }
            AddPixelOutput(reflection, parameter.SemanticName, parameter.SemanticIndex, parameter.SystemValueType);
        }
    }
    else
    {
        throw std::runtime_error{"unsupported shader stage for D3D11 reflection: " + stage};
    }
    return reflection;
}

ReflectionEvidence ReflectD3D12(const std::filesystem::path& bytecodePath, const std::string& stage)
{
    const std::vector<std::byte> bytecode = ReadFileBytes(bytecodePath);
    if (bytecode.empty())
    {
        throw std::runtime_error{"DXIL reflection received an empty bytecode file: " + bytecodePath.string()};
    }

    ComPtr<IDxcUtils> utils;
    const HRESULT utilsHr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(utilsHr) || utils == nullptr)
    {
        throw std::runtime_error{"DxcCreateInstance(Utils) for reflection failed: " + HrString(utilsHr)};
    }

    const DxcBuffer buffer{bytecode.data(), bytecode.size(), 0U};
    ComPtr<ID3D12ShaderReflection> shader;
    const HRESULT reflectHr = utils->CreateReflection(&buffer, IID_PPV_ARGS(&shader));
    if (FAILED(reflectHr) || shader == nullptr)
    {
        throw std::runtime_error{"IDxcUtils::CreateReflection failed for " + bytecodePath.string() + ": " +
                                 HrString(reflectHr)};
    }

    D3D12_SHADER_DESC description{};
    const HRESULT descHr = shader->GetDesc(&description);
    if (FAILED(descHr))
    {
        throw std::runtime_error{"ID3D12ShaderReflection::GetDesc failed: " + HrString(descHr)};
    }

    ReflectionEvidence reflection;
    for (UINT index = 0U; index < description.BoundResources; ++index)
    {
        D3D12_SHADER_INPUT_BIND_DESC bindingDescription{};
        const HRESULT bindingHr = shader->GetResourceBindingDesc(index, &bindingDescription);
        if (FAILED(bindingHr))
        {
            throw std::runtime_error{"ID3D12ShaderReflection::GetResourceBindingDesc failed: " + HrString(bindingHr)};
        }
        UINT uniformBytes = 0U;
        if (bindingDescription.Type == D3D_SIT_CBUFFER)
        {
            if (bindingDescription.Name == nullptr)
            {
                throw std::runtime_error{"DXIL cbuffer reflection returned no name"};
            }
            ID3D12ShaderReflectionConstantBuffer* constantBuffer =
                shader->GetConstantBufferByName(bindingDescription.Name);
            if (constantBuffer == nullptr)
            {
                throw std::runtime_error{"DXIL reflection could not resolve cbuffer: " +
                                         std::string{bindingDescription.Name}};
            }
            D3D12_SHADER_BUFFER_DESC bufferDescription{};
            const HRESULT bufferHr = constantBuffer->GetDesc(&bufferDescription);
            if (FAILED(bufferHr))
            {
                throw std::runtime_error{"DXIL cbuffer GetDesc failed: " + HrString(bufferHr)};
            }
            uniformBytes = bufferDescription.Size;
        }
        reflection.bindings.push_back(MakeBinding(bindingDescription.Name, bindingDescription.Type,
                                                  bindingDescription.Dimension, bindingDescription.BindPoint,
                                                  bindingDescription.BindCount, bindingDescription.uFlags,
                                                  bindingDescription.Space, uniformBytes));
    }

    if (stage == "vs")
    {
        for (UINT index = 0U; index < description.InputParameters; ++index)
        {
            D3D12_SIGNATURE_PARAMETER_DESC parameter{};
            const HRESULT parameterHr = shader->GetInputParameterDesc(index, &parameter);
            if (FAILED(parameterHr))
            {
                throw std::runtime_error{"DXIL input signature reflection failed: " + HrString(parameterHr)};
            }
            AddVertexInput(reflection, parameter.SemanticName, parameter.SemanticIndex, parameter.Mask,
                           parameter.SystemValueType);
        }
    }
    else if (stage == "ps")
    {
        for (UINT index = 0U; index < description.OutputParameters; ++index)
        {
            D3D12_SIGNATURE_PARAMETER_DESC parameter{};
            const HRESULT parameterHr = shader->GetOutputParameterDesc(index, &parameter);
            if (FAILED(parameterHr))
            {
                throw std::runtime_error{"DXIL output signature reflection failed: " + HrString(parameterHr)};
            }
            AddPixelOutput(reflection, parameter.SemanticName, parameter.SemanticIndex, parameter.SystemValueType);
        }
    }
    else
    {
        throw std::runtime_error{"unsupported shader stage for DXIL reflection: " + stage};
    }
    return reflection;
}

std::string ReflectionKey(ReflectionEvidence reflection)
{
    std::sort(reflection.bindings.begin(), reflection.bindings.end(),
              [](const BindingEvidence& left, const BindingEvidence& right)
              {
                  return std::tie(left.name, left.kind, left.registerIndex, left.space, left.count, left.uniformBytes,
                                  left.dimension, left.comparisonSampler) <
                         std::tie(right.name, right.kind, right.registerIndex, right.space, right.count,
                                  right.uniformBytes, right.dimension, right.comparisonSampler);
              });
    std::sort(reflection.vertexInputs.begin(), reflection.vertexInputs.end(),
              [](const VertexInputEvidence& left, const VertexInputEvidence& right)
              {
                  return std::tie(left.semantic, left.index, left.components) <
                         std::tie(right.semantic, right.index, right.components);
              });

    std::ostringstream stream;
    for (const BindingEvidence& binding : reflection.bindings)
    {
        stream << "binding|" << binding.name << '|' << binding.kind << '|' << binding.registerIndex << '|'
               << binding.space << '|' << binding.count << '|' << binding.uniformBytes << '|' << binding.dimension
               << '|' << (binding.comparisonSampler ? 1 : 0) << '\n';
    }
    for (const VertexInputEvidence& input : reflection.vertexInputs)
    {
        stream << "input|" << input.semantic << '|' << input.index << '|' << input.components << '\n';
    }
    stream << "colorOutputMask|" << reflection.colorOutputMask << '\n';
    stream << "writesDepth|" << (reflection.writesDepth ? 1 : 0) << '\n';
    return stream.str();
}

std::string SemanticSourceKey(const AssetSpec& spec, const SourceClosure& closure)
{
    std::ostringstream stream;
    stream << "MiniEngine/M6-04/ShaderSemantic/v1\n";
    stream << "sourceStem=" << spec.sourceStem << '\n';
    stream << "entry=" << spec.entry << '\n';
    stream << "stage=" << spec.stage << '\n';
    for (const SourceFileEvidence& file : closure.files)
    {
        // BindingContract 的宏已在 NormalizeTokens 中折叠为实际寄存器；D3D11
        // 没有对应 include，因此不把只存在于 D3D12 的包装文件作为额外语义文件。
        if (file.relativeName == "BindingContract.hlsli")
        {
            continue;
        }
        stream << "file=" << file.relativeName << '\n';
        stream << file.semanticTokens << '\n';
    }
    return stream.str();
}

void CompleteSemanticEvidence(const AssetSpec& spec, VariantEvidence& variant)
{
    variant.semanticSourceKey = SemanticSourceKey(spec, variant.closure);
    variant.semanticSourceHash = Sha256Text(variant.semanticSourceKey);
    variant.reflectionKey = ReflectionKey(variant.reflection);
    variant.semanticHash = Sha256Text(variant.semanticSourceKey + "reflection\n" + variant.reflectionKey);
}

std::string ErrorBlobText(ID3DBlob* errors)
{
    if (errors == nullptr || errors->GetBufferPointer() == nullptr || errors->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()};
}

VariantEvidence CompileD3D11Variant(const std::filesystem::path& repositoryRoot, const AssetSpec& spec,
                                    const std::filesystem::path& candidateDirectory)
{
    const std::filesystem::path sourcePath = repositoryRoot / spec.d3d11Source;
    const std::vector<std::byte> sourceBytes = ReadFileBytes(sourcePath);
    const std::string target = spec.stage == "vs" ? "vs_5_0" : "ps_5_0";
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const std::wstring sourceWide = sourcePath.wstring();
    const HRESULT compileHr = D3DCompileFromFile(sourceWide.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                 spec.entry.c_str(), target.c_str(), flags, 0U, &bytecode, &errors);
    if (FAILED(compileHr) || bytecode == nullptr)
    {
        const std::string diagnostics = ErrorBlobText(errors.Get());
        throw std::runtime_error{"D3DCompileFromFile failed for " + spec.d3d11Source + " (" + spec.entry + " " +
                                 target + ") " + HrString(compileHr) +
                                 (diagnostics.empty() ? std::string{} : ":\n" + diagnostics)};
    }

    const std::filesystem::path bytecodePath =
        candidateDirectory / "d3d11" / (spec.sourceStem + "." + spec.entry + "." + spec.stage + ".dxbc");
    WriteFileBytes(bytecodePath, bytecode->GetBufferPointer(), bytecode->GetBufferSize());

    VariantEvidence variant;
    variant.backend = "d3d11";
    variant.sourcePath = spec.d3d11Source;
    variant.sourceHash = Sha256Hex(std::span<const std::byte>{sourceBytes.data(), sourceBytes.size()});
    variant.bytecodePath = bytecodePath;
    variant.bytecodeFormat = "DXBC";
    variant.bytecodeHash = Sha256Hex(std::span<const std::byte>{
        static_cast<const std::byte*>(bytecode->GetBufferPointer()), bytecode->GetBufferSize()});
    variant.bytecodeBytes = bytecode->GetBufferSize();
    variant.compiler = "D3DCompileFromFile";
    variant.compilerVersion = "Windows SDK";
    variant.target = target;
    variant.options = {"D3DCOMPILE_ENABLE_STRICTNESS", "D3DCOMPILE_WARNINGS_ARE_ERRORS"};
    variant.closure = BuildSourceClosure(repositoryRoot, spec.d3d11Source);
    variant.reflection = ReflectD3D11(bytecode.Get(), spec.stage);
    CompleteSemanticEvidence(spec, variant);
    return variant;
}

VariantEvidence CompileD3D12Variant(const std::filesystem::path& repositoryRoot, const AssetSpec& spec,
                                    const std::filesystem::path& candidateDirectory)
{
    const MiniEngine::ShaderCompiler::CompileResult result =
        CompileEntry(repositoryRoot, spec.d3d12Source, spec.entry, spec.stage == "vs" ? "vs_6_0" : "ps_6_0",
                     candidateDirectory / "d3d12");
    const std::vector<std::byte> sourceBytes = ReadFileBytes(repositoryRoot / spec.d3d12Source);
    const std::vector<std::byte> bytecodeBytes = ReadFileBytes(result.objectPath);
    if (result.sourceSha256 != Sha256Hex(std::span<const std::byte>{sourceBytes.data(), sourceBytes.size()}))
    {
        throw std::runtime_error{"DXC source hash disagrees with the package source: " + spec.d3d12Source};
    }

    VariantEvidence variant;
    variant.backend = "d3d12";
    variant.sourcePath = spec.d3d12Source;
    variant.sourceHash = result.sourceSha256;
    variant.bytecodePath = result.objectPath;
    variant.bytecodeFormat = "DXIL";
    variant.bytecodeHash = Sha256Hex(std::span<const std::byte>{bytecodeBytes.data(), bytecodeBytes.size()});
    if (variant.bytecodeHash != result.dxilSha256)
    {
        throw std::runtime_error{"DXC object hash disagrees with the package bytecode: " + spec.d3d12Source};
    }
    variant.bytecodeBytes = bytecodeBytes.size();
    variant.compiler = "DXC";
    variant.compilerVersion = result.compilerVersion;
    variant.target = result.targetProfile;
    variant.options = result.arguments;
    variant.closure = BuildSourceClosure(repositoryRoot, spec.d3d12Source);
    variant.reflection = ReflectD3D12(result.objectPath, spec.stage);

    const std::vector<std::byte> pdbBytes = ReadFileBytes(result.pdbPath);
    PdbEvidence pdb;
    pdb.name = result.pdbName;
    pdb.type = result.pdbIsFull ? "full" : "slim";
    pdb.sha256 = Sha256Hex(std::span<const std::byte>{pdbBytes.data(), pdbBytes.size()});
    pdb.bytes = pdbBytes.size();
    pdb.path = result.pdbPath;
    variant.pdb = std::move(pdb);
    CompleteSemanticEvidence(spec, variant);
    return variant;
}

bool HasEntryDefinition(const std::filesystem::path& path, const std::string& entry)
{
    const std::vector<std::byte> bytes = ReadFileBytes(path);
    const std::string source = StripComments(std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    std::size_t cursor = 0U;
    while ((cursor = source.find(entry, cursor)) != std::string::npos)
    {
        const bool leftIsIdentifier =
            cursor > 0U &&
            (std::isalnum(static_cast<unsigned char>(source[cursor - 1U])) != 0 || source[cursor - 1U] == '_');
        std::size_t after = cursor + entry.size();
        while (after < source.size() && std::isspace(static_cast<unsigned char>(source[after])) != 0)
        {
            ++after;
        }
        if (!leftIsIdentifier && after < source.size() && source[after] == '(')
        {
            return true;
        }
        cursor += entry.size();
    }
    return false;
}

std::vector<AssetSpec> PackageSpecs(const std::filesystem::path& repositoryRoot)
{
    (void)repositoryRoot;
    std::vector<AssetSpec> specs{
        {"GraphTriangle", "VSMain", "vs", "shaders/d3d11/GraphTriangle.hlsl", "shaders/d3d12/GraphTriangle.hlsl"},
        {"GraphTriangle", "PSMain", "ps", "shaders/d3d11/GraphTriangle.hlsl", "shaders/d3d12/GraphTriangle.hlsl"},
        {"PbrForward", "VSMain", "vs", "shaders/d3d11/PbrForward.hlsl", "shaders/d3d12/PbrForward.hlsl"},
        {"PbrForward", "PSMain", "ps", "shaders/d3d11/PbrForward.hlsl", "shaders/d3d12/PbrForward.hlsl"},
        {"ShadowDepth", "VSMain", "vs", "shaders/d3d11/ShadowDepth.hlsl", "shaders/d3d12/ShadowDepth.hlsl"},
        {"Skybox", "VSMain", "vs", "shaders/d3d11/Skybox.hlsl", "shaders/d3d12/Skybox.hlsl"},
        {"Skybox", "PSMain", "ps", "shaders/d3d11/Skybox.hlsl", "shaders/d3d12/Skybox.hlsl"},
        {"ToneMap", "VSMain", "vs", "shaders/d3d11/ToneMap.hlsl", "shaders/d3d12/ToneMap.hlsl"},
        {"ToneMap", "PSMain", "ps", "shaders/d3d11/ToneMap.hlsl", "shaders/d3d12/ToneMap.hlsl"},
    };

    const std::filesystem::path shadow11 = repositoryRoot / "shaders/d3d11/ShadowDepth.hlsl";
    const std::filesystem::path shadow12 = repositoryRoot / "shaders/d3d12/ShadowDepth.hlsl";
    const bool hasShadowPs11 = HasEntryDefinition(shadow11, "PSMain");
    const bool hasShadowPs12 = HasEntryDefinition(shadow12, "PSMain");
    if (hasShadowPs11 != hasShadowPs12)
    {
        throw std::runtime_error{"ShadowDepth PSMain presence differs between D3D11 and D3D12 sources"};
    }
    if (hasShadowPs11)
    {
        specs.insert(specs.begin() + 5, {"ShadowDepth", "PSMain", "ps", "shaders/d3d11/ShadowDepth.hlsl",
                                         "shaders/d3d12/ShadowDepth.hlsl"});
    }
    return specs;
}

std::string RevisionToken()
{
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    return std::to_string(static_cast<long long>(now.count())) + "-" +
           std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
}

struct RevisionPaths final
{
    std::filesystem::path candidate;
    std::filesystem::path final;
    std::string name;
};

RevisionPaths CreateRevision(const std::filesystem::path& outputDirectory)
{
    std::error_code error;
    std::filesystem::create_directories(outputDirectory / "revisions", error);
    if (error)
    {
        throw std::runtime_error{"cannot create shader package revisions directory: " + error.message()};
    }

    const std::string token = RevisionToken();
    for (unsigned int attempt = 0U; attempt < 1000U; ++attempt)
    {
        const std::string suffix = attempt == 0U ? std::string{} : "-" + std::to_string(attempt);
        const std::string name = "m6-04-" + token + suffix;
        const std::filesystem::path candidate = outputDirectory / "revisions" / ("." + name + ".candidate");
        const std::filesystem::path final = outputDirectory / "revisions" / name;
        if (std::filesystem::exists(candidate) || std::filesystem::exists(final))
        {
            continue;
        }
        std::filesystem::create_directories(candidate, error);
        if (!error)
        {
            return RevisionPaths{candidate, final, name};
        }
    }
    throw std::runtime_error{"cannot allocate a unique shader package revision directory"};
}

std::string ManifestRelativePath(const std::filesystem::path& outputDirectory,
                                 const std::filesystem::path& candidateDirectory,
                                 const std::filesystem::path& finalDirectory,
                                 const std::filesystem::path& candidatePath)
{
    const std::filesystem::path relativeToCandidate = candidatePath.lexically_relative(candidateDirectory);
    if (relativeToCandidate.empty() || relativeToCandidate.generic_string().rfind("..", 0U) == 0U)
    {
        throw std::runtime_error{"shader artifact is outside its revision candidate: " + candidatePath.string()};
    }
    return RelativePath(outputDirectory, finalDirectory / relativeToCandidate);
}

void WriteStringArray(std::ostringstream& stream, const std::vector<std::string>& values)
{
    stream << '[';
    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        if (index != 0U)
        {
            stream << ", ";
        }
        stream << JsonString(values[index]);
    }
    stream << ']';
}

void WriteReflection(std::ostringstream& stream, const ReflectionEvidence& reflection, const std::string& indent)
{
    ReflectionEvidence ordered = reflection;
    std::sort(ordered.bindings.begin(), ordered.bindings.end(),
              [](const BindingEvidence& left, const BindingEvidence& right)
              {
                  return std::tie(left.name, left.kind, left.registerIndex, left.space, left.count, left.uniformBytes,
                                  left.dimension, left.comparisonSampler) <
                         std::tie(right.name, right.kind, right.registerIndex, right.space, right.count,
                                  right.uniformBytes, right.dimension, right.comparisonSampler);
              });
    std::sort(ordered.vertexInputs.begin(), ordered.vertexInputs.end(),
              [](const VertexInputEvidence& left, const VertexInputEvidence& right)
              {
                  return std::tie(left.semantic, left.index, left.components) <
                         std::tie(right.semantic, right.index, right.components);
              });

    stream << indent << "{\n";
    stream << indent << "  \"bindings\": [\n";
    for (std::size_t index = 0U; index < ordered.bindings.size(); ++index)
    {
        const BindingEvidence& binding = ordered.bindings[index];
        stream << indent << "    {\"name\": " << JsonString(binding.name) << ", \"kind\": " << JsonString(binding.kind)
               << ", \"register\": " << binding.registerIndex << ", \"space\": " << binding.space
               << ", \"count\": " << binding.count << ", \"uniformBytes\": " << binding.uniformBytes
               << ", \"dimension\": " << JsonString(binding.dimension)
               << ", \"comparisonSampler\": " << (binding.comparisonSampler ? "true" : "false") << "}"
               << (index + 1U == ordered.bindings.size() ? "\n" : ",\n");
    }
    stream << indent << "  ],\n";
    stream << indent << "  \"vertexInputs\": [\n";
    for (std::size_t index = 0U; index < ordered.vertexInputs.size(); ++index)
    {
        const VertexInputEvidence& input = ordered.vertexInputs[index];
        stream << indent << "    {\"semantic\": " << JsonString(input.semantic) << ", \"index\": " << input.index
               << ", \"components\": " << input.components << "}"
               << (index + 1U == ordered.vertexInputs.size() ? "\n" : ",\n");
    }
    stream << indent << "  ],\n";
    stream << indent << "  \"colorOutputMask\": " << reflection.colorOutputMask << ",\n";
    stream << indent << "  \"writesDepth\": " << (reflection.writesDepth ? "true" : "false") << '\n';
    stream << indent << "}";
}

void WriteClosure(std::ostringstream& stream, const SourceClosure& closure, const std::string& indent)
{
    stream << indent << "[\n";
    for (std::size_t index = 0U; index < closure.files.size(); ++index)
    {
        const SourceFileEvidence& file = closure.files[index];
        stream << indent << "  {\"path\": " << JsonString(file.repositoryPath)
               << ", \"sha256\": " << JsonString(file.sourceHash) << "}"
               << (index + 1U == closure.files.size() ? "\n" : ",\n");
    }
    stream << indent << "]";
}

void WriteVariant(std::ostringstream& stream, const VariantEvidence& variant,
                  const std::filesystem::path& outputDirectory, const std::filesystem::path& candidateDirectory,
                  const std::filesystem::path& finalDirectory, const std::string& indent)
{
    const std::string bytecodePath =
        ManifestRelativePath(outputDirectory, candidateDirectory, finalDirectory, variant.bytecodePath);
    stream << indent << "{\n";
    stream << indent << "  \"source\": " << JsonString(variant.sourcePath) << ",\n";
    stream << indent << "  \"sourceHash\": " << JsonString(variant.sourceHash) << ",\n";
    stream << indent << "  \"bytecode\": {\"format\": " << JsonString(variant.bytecodeFormat)
           << ", \"sha256\": " << JsonString(variant.bytecodeHash) << ", \"bytes\": " << variant.bytecodeBytes
           << ", \"path\": " << JsonString(bytecodePath) << "},\n";
    stream << indent << "  \"compiler\": " << JsonString(variant.compiler) << ",\n";
    stream << indent << "  \"compilerVersion\": " << JsonString(variant.compilerVersion) << ",\n";
    stream << indent << "  \"target\": " << JsonString(variant.target) << ",\n";
    stream << indent << "  \"options\": ";
    WriteStringArray(stream, variant.options);
    stream << ",\n";
    stream << indent << "  \"sourceClosure\": ";
    WriteClosure(stream, variant.closure, indent + "  ");
    stream << ",\n";
    stream << indent << "  \"reflection\": ";
    WriteReflection(stream, variant.reflection, indent + "  ");
    if (variant.pdb.has_value())
    {
        const PdbEvidence& pdb = *variant.pdb;
        const std::string pdbPath = ManifestRelativePath(outputDirectory, candidateDirectory, finalDirectory, pdb.path);
        stream << ",\n"
               << indent << "  \"pdb\": {\"name\": " << JsonString(pdb.name) << ", \"type\": " << JsonString(pdb.type)
               << ", \"sha256\": " << JsonString(pdb.sha256) << ", \"bytes\": " << pdb.bytes
               << ", \"path\": " << JsonString(pdbPath) << "}";
    }
    else
    {
        stream << ",\n" << indent << "  \"pdb\": null";
    }
    stream << '\n' << indent << "}";
}

std::string BuildManifest(const std::vector<PackageAsset>& assets, const std::string& revisionName,
                          const std::filesystem::path& outputDirectory, const std::filesystem::path& candidateDirectory,
                          const std::filesystem::path& finalDirectory)
{
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"formatVersion\": 1,\n";
    stream << "  \"status\": \"ready\",\n";
    stream << "  \"revision\": " << JsonString(revisionName) << ",\n";
    stream << "  \"assets\": [\n";
    for (std::size_t index = 0U; index < assets.size(); ++index)
    {
        const PackageAsset& asset = assets[index];
        stream << "    {\n";
        stream << "      \"assetId\": " << JsonString(asset.spec.sourceStem + asset.spec.entry) << ",\n";
        stream << "      \"sourceStem\": " << JsonString(asset.spec.sourceStem) << ",\n";
        stream << "      \"stage\": " << JsonString(asset.spec.stage) << ",\n";
        stream << "      \"entry\": " << JsonString(asset.spec.entry) << ",\n";
        stream << "      \"semanticHash\": " << JsonString(asset.d3d11.semanticHash) << ",\n";
        stream << "      \"variants\": {\n";
        stream << "        \"d3d11\": ";
        WriteVariant(stream, asset.d3d11, outputDirectory, candidateDirectory, finalDirectory, "          ");
        stream << ",\n";
        stream << "        \"d3d12\": ";
        WriteVariant(stream, asset.d3d12, outputDirectory, candidateDirectory, finalDirectory, "          ");
        stream << "\n";
        stream << "      }\n";
        stream << "    }" << (index + 1U == assets.size() ? "\n" : ",\n");
    }
    stream << "  ]\n";
    stream << "}\n";
    return stream.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error{"cannot write shader package manifest: " + path.string()};
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good())
    {
        throw std::runtime_error{"write failed for shader package manifest: " + path.string()};
    }
}

void ReplaceManifestAtomically(const std::filesystem::path& outputDirectory, const std::string& text,
                               const std::string& token)
{
    const std::filesystem::path finalPath = outputDirectory / "manifest.json";
    const std::filesystem::path temporaryPath = outputDirectory / (".manifest.json." + token + ".tmp");
    WriteTextFile(temporaryPath, text);

    const std::wstring finalWide = finalPath.wstring();
    const std::wstring temporaryWide = temporaryPath.wstring();
    const DWORD attributes = GetFileAttributesW(finalWide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        if (!ReplaceFileW(finalWide.c_str(), temporaryWide.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr,
                          nullptr))
        {
            throw std::runtime_error{"ReplaceFileW(manifest.json) failed: " + Win32ErrorString(GetLastError())};
        }
        return;
    }

    const DWORD attributeError = GetLastError();
    if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
    {
        throw std::runtime_error{"GetFileAttributesW(manifest.json) failed: " + Win32ErrorString(attributeError)};
    }
    if (!MoveFileExW(temporaryWide.c_str(), finalWide.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        throw std::runtime_error{"MoveFileExW(manifest.json) failed: " + Win32ErrorString(GetLastError())};
    }
}

void CompareVariants(const PackageAsset& asset)
{
    if (asset.d3d11.semanticSourceKey != asset.d3d12.semanticSourceKey)
    {
        throw std::runtime_error{"M6-04 semantic source mismatch for " + asset.spec.sourceStem + "." +
                                 asset.spec.entry};
    }
    if (asset.d3d11.reflectionKey != asset.d3d12.reflectionKey)
    {
        throw std::runtime_error{"M6-04 reflection mismatch for " + asset.spec.sourceStem + "." + asset.spec.entry};
    }
    if (asset.d3d11.semanticHash != asset.d3d12.semanticHash)
    {
        throw std::runtime_error{"M6-04 semanticHash mismatch for " + asset.spec.sourceStem + "." + asset.spec.entry};
    }
}
} // namespace

std::size_t BuildDualShaderPackage(const std::filesystem::path& repositoryRoot,
                                   const std::filesystem::path& outputDirectory)
{
    const std::filesystem::path normalizedRoot = NormalizeRoot(repositoryRoot);
    std::error_code absoluteOutputError;
    const std::filesystem::path absoluteOutput = std::filesystem::absolute(outputDirectory, absoluteOutputError);
    if (absoluteOutputError)
    {
        throw std::runtime_error{"cannot resolve shader package output directory: " + absoluteOutputError.message()};
    }
    const std::filesystem::path normalizedOutput{absoluteOutput.lexically_normal().generic_string()};

    std::error_code outputError;
    std::filesystem::create_directories(normalizedOutput, outputError);
    if (outputError)
    {
        throw std::runtime_error{"cannot create shader package output directory: " + outputError.message()};
    }

    const RevisionPaths revision = CreateRevision(normalizedOutput);
    std::vector<PackageAsset> assets;
    for (const AssetSpec& spec : PackageSpecs(normalizedRoot))
    {
        PackageAsset asset;
        asset.spec = spec;
        asset.d3d11 = CompileD3D11Variant(normalizedRoot, spec, revision.candidate);
        asset.d3d12 = CompileD3D12Variant(normalizedRoot, spec, revision.candidate);
        CompareVariants(asset);
        assets.push_back(std::move(asset));
    }

    std::error_code renameError;
    std::filesystem::rename(revision.candidate, revision.final, renameError);
    if (renameError)
    {
        throw std::runtime_error{"cannot publish shader package revision directory: " + renameError.message()};
    }

    const std::string manifest =
        BuildManifest(assets, revision.name, normalizedOutput, revision.candidate, revision.final);
    ReplaceManifestAtomically(normalizedOutput, manifest, RevisionToken());
    return assets.size();
}
} // namespace MiniEngine::ShaderCompiler
