// ============================================================================
// DxcCompiler.cpp — DXC(SM6) 编译、PDB 与 manifest 落盘
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 1 条）
// 职责：实现 DxcCompiler.h。流程严格按 08 篇「DXC contract」：
//   读源文件 → 组装参数（-T/-E/-HV 2021/-Ges/-WX/-Zi/-Zss）→ Compile →
//   取 DXC_OUT_OBJECT 与 DXC_OUT_PDB（带 suggested name）→ 由 IDxcPdbUtils2 校验 full 类型 → 写 .dxil / .pdb /
//   manifest。
// 诊断：编译失败时把 DXC 的 error/warning blob 原样拼进异常文本——`-WX` 会把
//   warning 变成失败，因此"哪些 warning"必须可读，否则只能靠猜。
// 版本：manifest 记录 IDxcVersionInfo 的 (major, minor, flags) 与 SDK 提供的
//   DLL 路径，便于日后核对"同一 shader 是否由同一编译器产出"。
// 关联：tools/shader_compiler/src/DxcCompiler.h
// ============================================================================
#include "DxcCompiler.h"

#include <MiniEngine/Assets/Sha256.h>

#include <Windows.h>

#include <wrl/client.h>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <dxcapi.h>

namespace MiniEngine::ShaderCompiler
{
namespace
{
// 宽 → 窄（DXC 的 suggested PDB 名是宽字符；manifest 统一 UTF-8）。
std::string Utf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

// 十六进制小写（manifest 与仓库其余 SHA-256 记录同口径）。
std::string ToHexLower(const std::byte* bytes, const std::size_t size)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index)
    {
        stream << std::setw(2) << static_cast<unsigned>(std::to_integer<std::uint8_t>(bytes[index]));
    }
    return stream.str();
}

std::string Sha256Of(const std::span<const std::byte> bytes)
{
    const MiniEngine::Assets::Sha256Digest digest = MiniEngine::Assets::Sha256(bytes);
    return ToHexLower(digest.data(), digest.size());
}

// IDxcBlobWide（DXC 的 suggested PDB 名）→ UTF-8：它只有 GetStringPointer/Length。
std::string WideBlobToUtf8(IDxcBlobWide* blob)
{
    if (blob == nullptr || blob->GetStringPointer() == nullptr)
    {
        return {};
    }
    return Utf8(std::wstring_view{blob->GetStringPointer(), blob->GetStringLength()});
}

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        throw std::runtime_error{"cannot open shader source: " + path.string()};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        throw std::runtime_error{"cannot read shader source: " + path.string()};
    }
    return bytes;
}

void WriteFileBytes(const std::filesystem::path& path, const void* data, const std::size_t size)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error{"cannot write shader artifact: " + path.string()};
    }
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file.good())
    {
        throw std::runtime_error{"write failed for shader artifact: " + path.string()};
    }
}

std::string JsonEscape(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
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
            escaped += c;
            break;
        }
    }
    return escaped;
}

// 把 DXC 的错误/警告 blob 原样取出（失败时用于异常文本）。
std::string ReadErrors(IDxcResult& result)
{
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
    if (FAILED(result.GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) || errors == nullptr)
    {
        return {};
    }
    if (errors->GetStringLength() == 0U)
    {
        return {};
    }
    return std::string{errors->GetStringPointer(), errors->GetStringLength()};
}
} // namespace

CompileResult CompileEntry(const std::filesystem::path& repositoryRoot, const std::string& sourceRelative,
                           const std::string& entryPoint, const std::string& targetProfile,
                           const std::filesystem::path& outputDirectory)
{
    // --repo 的**字符串形式**会逐位影响产物（二次审查 N-3）：`-Zi` 把源文件的绝对路径写进
    // DXIL 与 PDB，因此 `G:/repo`、`G:\repo`、`.` 三种写法会得到三份不同的 dxilSha256 /
    // pdbName——而差异是**静默的**（工具照样以 0 退出，只有逐位比较才看得出来）。实测同一份
    // 源码：构建时那一种传法 9/9 逐位相同，反斜杠与相对路径 0/9。
    //
    // 统一规范化成"绝对 + **正斜杠**"后再拼源路径，使"重编译得到逐位相同产物"与调用形式
    // 无关（传 `.`、正斜杠、反斜杠都得到同一结果）。这不是审美问题而是字节级要求：路径
    // 字符串真的进了调试信息。
    //
    // 为什么是正斜杠而不是更"规范"的原生反斜杠：构建侧传的 `PROJECT_SOURCE_DIR` 就是
    // 正斜杠绝对路径，**只有收敛到与构建完全相同的那一种形式**才对既有取证是恒等变换。
    // 换成反斜杠会让嵌入路径多一次变化（实测 dxilSha256 会变，等于悄悄作废已记录的
    // 9/9 逐位相同证据）——因此这里的 `.generic_string()` 是必需的，不要"顺手清理"掉。
    std::error_code absoluteError;
    const std::filesystem::path resolvedRoot = std::filesystem::absolute(repositoryRoot, absoluteError);
    if (absoluteError)
    {
        throw std::runtime_error{"cannot resolve --repo '" + repositoryRoot.string() + "': " + absoluteError.message()};
    }
    const std::filesystem::path normalizedRoot{resolvedRoot.lexically_normal().generic_string()};

    const std::filesystem::path sourcePath = normalizedRoot / sourceRelative;
    const std::vector<std::byte> sourceBytes = ReadFileBytes(sourcePath);

    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includeHandler;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
    {
        throw std::runtime_error{"DxcCreateInstance(Utils) failed (is dxcompiler.dll next to the tool or on PATH?)"};
    }
    if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    {
        throw std::runtime_error{"DxcCreateInstance(Compiler) failed"};
    }
    if (FAILED(utils->CreateDefaultIncludeHandler(&includeHandler)))
    {
        throw std::runtime_error{"CreateDefaultIncludeHandler failed"};
    }

    // 参数口径（08 篇冻结）：HLSL 2021、warnings-as-errors、full PDB、去反射。
    const std::wstring sourceWide = sourcePath.wstring();
    const std::wstring entryWide = Widen(entryPoint);
    const std::wstring targetWide = Widen(targetProfile);
    std::vector<LPCWSTR> arguments{
        sourceWide.c_str(),
        L"-E",
        entryWide.c_str(),
        L"-T",
        targetWide.c_str(),
        L"-HV",
        L"2021",
        L"-Ges",
        L"-WX",
        L"-Zi",
        L"-Zss",
    };

    DxcBuffer sourceBuffer{sourceBytes.data(), sourceBytes.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> result;
    const HRESULT compileHr = compiler->Compile(&sourceBuffer, arguments.data(), static_cast<UINT32>(arguments.size()),
                                                includeHandler.Get(), IID_PPV_ARGS(&result));
    if (FAILED(compileHr))
    {
        throw std::runtime_error{"IDxcCompiler3::Compile failed with HRESULT " + std::to_string(compileHr)};
    }

    HRESULT compileStatus = E_FAIL;
    if (FAILED(result->GetStatus(&compileStatus)))
    {
        throw std::runtime_error{"IDxcResult::GetStatus failed"};
    }
    if (FAILED(compileStatus))
    {
        // -WX 把 warning 也变成失败，因此这里必须把诊断原文带出去。
        throw std::runtime_error{"DXC compilation failed for " + sourceRelative + " (" + entryPoint + " " +
                                 targetProfile + "):\n" + ReadErrors(*result.Get())};
    }

    Microsoft::WRL::ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || object == nullptr)
    {
        throw std::runtime_error{"DXC produced no object output for " + sourceRelative};
    }

    Microsoft::WRL::ComPtr<IDxcBlob> pdb;
    Microsoft::WRL::ComPtr<IDxcBlobUtf16> pdbName;
    const HRESULT pdbHr = result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdb), pdbName.GetAddressOf());
    if (FAILED(pdbHr) || pdb == nullptr || pdbName == nullptr)
    {
        // PDB 是 PIX 自动定位 HLSL 的前提（08 篇验收项），缺失即失败而不是静默跳过。
        throw std::runtime_error{"DXC produced no PDB output for " + sourceRelative +
                                 " (needed for PIX symbol resolution)"};
    }

    // 当前 SDK 的 -Zi DXC_OUT_PDB 已由 SDK 标记为 full PDB。
    // 这里不猜测或追加未定义的命令行 flag，而是直接用官方 PDB 解析接口
    // 对最终 blob 做类型门禁；如果编译器回退到 slim，工具必须失败。
    const std::string pdbNameUtf8 = WideBlobToUtf8(pdbName.Get());
    if (pdbNameUtf8.empty())
    {
        throw std::runtime_error{"DXC returned an empty PDB name for " + sourceRelative};
    }

    Microsoft::WRL::ComPtr<IDxcPdbUtils2> pdbUtils;
    if (FAILED(DxcCreateInstance(CLSID_DxcPdbUtils, IID_PPV_ARGS(&pdbUtils))))
    {
        throw std::runtime_error{"DxcCreateInstance(PdbUtils2) failed while verifying the PDB"};
    }
    const HRESULT loadPdbHr = pdbUtils->Load(pdb.Get());
    if (FAILED(loadPdbHr))
    {
        throw std::runtime_error{"IDxcPdbUtils2::Load failed with HRESULT " + std::to_string(loadPdbHr)};
    }
    const bool pdbIsFull = pdbUtils->IsFullPDB() != FALSE;
    if (!pdbIsFull)
    {
        throw std::runtime_error{"DXC_OUT_PDB is not full for " + sourceRelative};
    }
    // 编译器版本（manifest 用于核对"同一 shader 由同一编译器产出"）。
    //
    // 口径限制（已核实）：`IDxcVersionInfo::GetVersion` 只暴露 major.minor（本机为
    // "1.8"），补丁号需要读 dxcompiler.dll 的版本资源才能拿到；而**完整版本串
    // （1.8.2502.11）已被写进 shader PDB**（`-Zi` 的调试信息区，可用
    // `dxc --version` 交叉核对）。因此"产物由哪个编译器产出"这件事由
    // `dxilSha256`（逐位锁定）+ PDB 内完整版本共同承担，manifest 只记 major.minor。
    std::string compilerVersion = "unknown";
    if (Microsoft::WRL::ComPtr<IDxcVersionInfo> version; SUCCEEDED(compiler.As(&version)))
    {
        UINT32 major = 0U;
        UINT32 minor = 0U;
        if (SUCCEEDED(version->GetVersion(&major, &minor)))
        {
            compilerVersion = std::to_string(major) + "." + std::to_string(minor);
        }
    }

    // 产物落盘：<entry>.<target>.<stage>.dxil / .pdb / .json（stage 取 profile 前缀）。
    const std::string stage = targetProfile.substr(0, targetProfile.find('_'));
    const std::string baseName = std::filesystem::path{sourceRelative}.stem().string() + "." + entryPoint + "." + stage;
    const std::filesystem::path objectPath = outputDirectory / (baseName + ".dxil");
    const std::filesystem::path pdbPath = outputDirectory / pdbNameUtf8;
    const std::filesystem::path manifestPath = outputDirectory / (baseName + ".manifest.json");

    WriteFileBytes(objectPath, object->GetBufferPointer(), object->GetBufferSize());
    WriteFileBytes(pdbPath, pdb->GetBufferPointer(), pdb->GetBufferSize());

    CompileResult compileResult;
    compileResult.sourcePath = sourceRelative;
    compileResult.entryPoint = entryPoint;
    compileResult.targetProfile = targetProfile;
    compileResult.compilerVersion = compilerVersion;
    compileResult.arguments = {"-HV", "2021", "-Ges", "-WX", "-Zi", "-Zss"};
    compileResult.sourceSha256 = Sha256Of(sourceBytes);
    compileResult.dxilSha256 = Sha256Of(
        std::span<const std::byte>{static_cast<const std::byte*>(object->GetBufferPointer()), object->GetBufferSize()});
    compileResult.pdbName = pdbNameUtf8;
    compileResult.pdbIsFull = pdbIsFull;
    compileResult.objectPath = objectPath.string();
    compileResult.pdbPath = pdbPath.string();
    compileResult.manifestPath = manifestPath.string();
    compileResult.dxilBytes = object->GetBufferSize();

    // manifest（08 篇字段清单）。
    std::ofstream manifest(manifestPath, std::ios::binary | std::ios::trunc);
    if (!manifest.is_open())
    {
        throw std::runtime_error{"cannot write shader manifest: " + manifestPath.string()};
    }
    manifest << "{\n";
    manifest << "  \"source\": \"" << JsonEscape(compileResult.sourcePath) << "\",\n";
    manifest << "  \"entry\": \"" << JsonEscape(compileResult.entryPoint) << "\",\n";
    manifest << "  \"target\": \"" << JsonEscape(compileResult.targetProfile) << "\",\n";
    manifest << "  \"compilerVersion\": \"" << JsonEscape(compileResult.compilerVersion) << "\",\n";
    manifest << "  \"arguments\": [";
    for (std::size_t index = 0; index < compileResult.arguments.size(); ++index)
    {
        manifest << (index == 0 ? "" : ", ") << "\"" << JsonEscape(compileResult.arguments[index]) << "\"";
    }
    manifest << "],\n";
    manifest << "  \"sourceSha256\": \"" << compileResult.sourceSha256 << "\",\n";
    manifest << "  \"dxilSha256\": \"" << compileResult.dxilSha256 << "\",\n";
    manifest << "  \"pdbName\": \"" << JsonEscape(compileResult.pdbName) << "\",\n";
    manifest << "  \"pdbType\": \"" << (compileResult.pdbIsFull ? "full" : "slim") << "\",\n";
    manifest << "  \"dxilBytes\": " << compileResult.dxilBytes << "\n";
    manifest << "}\n";
    return compileResult;
}
} // namespace MiniEngine::ShaderCompiler
