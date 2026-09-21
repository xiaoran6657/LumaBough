// ============================================================================
// CanonicalRecipe.cpp — Recipe 的确定性重编码实现
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 4 节
// ============================================================================
#include "CanonicalRecipe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniEngine::Tools
{
namespace
{
constexpr std::array<char, 16> kHexDigits{
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void AppendHex2(std::string& out, const unsigned char value)
{
    out.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[value & 0x0FU]);
}

// 最小转义：只转义 JSON 要求的最小集合；控制字符走 \u00XX 保持确定性。
// 不转义 '/' 与非 ASCII 字符（UTF-8 原样输出，无 BOM）。
void AppendJsonString(std::string& out, const std::string_view text)
{
    out.push_back('"');
    for (const char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
            {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20U)
                {
                    out += "\\u00";
                    AppendHex2(out, byte);
                }
                else
                {
                    out.push_back(c);
                }
            }
        }
    }
    out.push_back('"');
}

// path 规范化：统一 '/' 分隔符并做词法归一化，避免 Windows 反斜杠与
// "a//b"、"a/./b" 之类的等价写法产生不同的 canonical bytes。
[[nodiscard]] std::string NormalizePathForCanonical(std::string_view path)
{
    const std::filesystem::path normalized = std::filesystem::path{path}.lexically_normal();
    return normalized.generic_string(); // generic_string 使用 '/' 分隔符
}

// 对象编码：先按 key 的 ASCII byte 顺序排序，再紧凑输出。
// std::string::operator< 对纯 ASCII key 即为字节序比较；若将来出现非 ASCII key，
// 需改为显式 unsigned char 比较。
[[nodiscard]] std::string EncodeObject(std::vector<std::pair<std::string, std::string>> fields)
{
    std::sort(fields.begin(), fields.end(),
              [](const std::pair<std::string, std::string>& left, const std::pair<std::string, std::string>& right)
              { return left.first < right.first; });

    std::string out;
    out.push_back('{');
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        if (index != 0)
        {
            out.push_back(',');
        }
        AppendJsonString(out, fields[index].first);
        out.push_back(':');
        out += fields[index].second;
    }
    out.push_back('}');
    return out;
}

[[nodiscard]] std::string EncodeBool(const bool value)
{
    return value ? "true" : "false";
}

// 十进制最短格式；整数不带小数点与指数。
[[nodiscard]] std::string EncodeUint(const std::uint64_t value)
{
    return std::to_string(value);
}

[[nodiscard]] std::string EncodeStringValue(const std::string_view value)
{
    std::string out;
    AppendJsonString(out, value);
    return out;
}
}  // namespace

std::optional<std::string> EncodeCanonicalRecipe(const Recipe& recipe)
{
    // 每层对象独立排序；嵌套对象整体作为一个 value 参与外层排序。
    // default 值显式填充：即使等于缺省值也编码，避免"删掉字段"绕过失效。
    const std::string importObject = EncodeObject({
        {"meshes", EncodeBool(recipe.importMeshes)},
        {"textures", EncodeBool(recipe.importTextures)},
        {"world", EncodeBool(recipe.importWorld)},
        {"generateMissingNormals", EncodeBool(recipe.generateMissingNormals)},
    });

    const std::string limitsObject = EncodeObject({
        {"maxFileBytes", EncodeUint(recipe.maxFileBytes)},
        {"maxVerticesPerPrimitive", EncodeUint(recipe.maxVerticesPerPrimitive)},
        {"maxIndicesPerPrimitive", EncodeUint(recipe.maxIndicesPerPrimitive)},
        {"maxTextureDimension", EncodeUint(recipe.maxTextureDimension)},
        {"maxNodes", EncodeUint(recipe.maxNodes)},
    });

    // 数值字段必须在解析阶段完成 range 检查；此处只做无损编码。
    if (recipe.schemaVersion < 0)
    {
        return std::nullopt;
    }

    // M4-04：可选 environment 段（存在/缺失是 recipe 语义的一部分——两种形态
    // 的 canonical 编码不同，保证"加/删 environment"必然触发失效）。
    std::vector<std::pair<std::string, std::string>> rootFields{
        {"schemaVersion", EncodeUint(static_cast<std::uint64_t>(recipe.schemaVersion))},
        {"source", EncodeStringValue(NormalizePathForCanonical(recipe.source))},
        {"assetRoot", EncodeStringValue(recipe.assetRoot)},
        {"scene", EncodeStringValue(recipe.scene)},
        {"profile", EncodeStringValue(recipe.profile)},
        {"import", importObject},
        {"limits", limitsObject},
    };
    if (recipe.environment.has_value())
    {
        rootFields.emplace_back("environment",
                                EncodeObject({
                                    {"source", EncodeStringValue(NormalizePathForCanonical(recipe.environment->source))},
                                    {"assetUri", EncodeStringValue(recipe.environment->assetUri)},
                                    {"usage", EncodeStringValue(recipe.environment->usage)},
                                    {"license", EncodeStringValue(recipe.environment->license)},
                                    {"sourceUrl", EncodeStringValue(recipe.environment->sourceUrl)},
                                    {"sourceSha256", EncodeStringValue(recipe.environment->sourceSha256)},
                                }));
    }

    return EncodeObject(std::move(rootFields));
}
}  // namespace MiniEngine::Tools