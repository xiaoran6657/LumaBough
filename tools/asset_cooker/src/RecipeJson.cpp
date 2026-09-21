// ============================================================================
// RecipeJson.cpp — Recipe 最小 JSON 解析与 source 沙箱的实现
// 里程碑：M3-04 / M3-05
// 职责：递归下降解析 recipe JSON（只支持本项目的字段子集），未知 key、缺必填项、
//       类型不符与越界数值一律拒绝（fail-closed）；ResolveSourceWithinRoot 做
//       source 路径沙箱（拒绝绝对路径与逃出 source-root 的 ".."）。
//       不引入任何第三方 JSON 依赖。
// 关联：tools/asset_cooker/src/RecipeJson.h（对外契约）
//       assets/recipes/*.asset.json（schema 实例）
// ============================================================================

#include "RecipeJson.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <array>
#include <span>

namespace MiniEngine::Tools
{
namespace
{
// 最小 JSON 节点树：只覆盖 Recipe 用到的类型。
// 数字统一存 double；整数字段由调用方做 range 检查后窄化。
struct JsonNode final
{
    enum class Kind
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    Kind kind{Kind::Null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<JsonNode> array;
    std::vector<std::pair<std::string, JsonNode>> object;
};

// 递归下降 JSON 解析器。只接受合法 JSON；未知 token 或尾随内容立即失败。
class JsonParser final
{
  public:
    // 审计 P2-1：递归下降必须限制嵌套深度，否则 `[[[[...` 恶意/异常输入会栈溢出，
    // 而不是按 CLI 契约返回可读错误。64 层远超 Recipe 真实结构。
    static constexpr int kMaxNestingDepth = 64;

    explicit JsonParser(const std::string_view text) : m_text(text)
    {
    }

    [[nodiscard]] bool Parse(JsonNode& root, std::string& error)
    {
        if (!SkipWhitespace())
        {
            error = "Unexpected end of JSON input.";
            return false;
        }
        if (!ParseValue(root, error))
        {
            return false;
        }
        if (SkipWhitespace())
        {
            error = "Unexpected trailing content after root value.";
            return false;
        }
        return true;
    }

  private:
    // RAII：离开数组/对象解析分支时递减深度计数（覆盖所有 return 路径）。
    struct DepthGuard final
    {
        explicit DepthGuard(JsonParser& owner) : parser(owner) {}
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        ~DepthGuard() { --parser.m_depth; }
        JsonParser& parser;
    };

    bool SkipWhitespace()
    {
        while (m_index < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_index])) != 0)
        {
            ++m_index;
        }
        return m_index < m_text.size();
    }

    bool Consume(const char expected)
    {
        if (m_index < m_text.size() && m_text[m_index] == expected)
        {
            ++m_index;
            return true;
        }
        return false;
    }

    bool ParseValue(JsonNode& node, std::string& error)
    {
        if (m_index >= m_text.size())
        {
            error = "Expected a JSON value.";
            return false;
        }

        switch (m_text[m_index])
        {
            case 'n':
                return ParseLiteral("null", JsonNode::Kind::Null, node, error);
            case 't':
                return ParseLiteral("true", JsonNode::Kind::Boolean, node, error);
            case 'f':
                return ParseLiteral("false", JsonNode::Kind::Boolean, node, error);
            case '"':
                node.kind = JsonNode::Kind::String;
                return ParseString(node.string, error);
            case '[':
                return ParseArray(node, error);
            case '{':
                return ParseObject(node, error);
            default:
                return ParseNumber(node, error);
        }
    }

    bool ParseLiteral(const std::string_view literal, const JsonNode::Kind kind, JsonNode& node, std::string& error)
    {
        if (m_text.compare(m_index, literal.size(), literal) != 0)
        {
            error = "Invalid JSON literal.";
            return false;
        }
        m_index += literal.size();
        node.kind = kind;
        if (kind == JsonNode::Kind::Boolean)
        {
            node.boolean = literal == "true";
        }
        return true;
    }

    bool ParseString(std::string& output, std::string& error)
    {
        if (!Consume('"'))
        {
            error = "Expected opening quote for string.";
            return false;
        }

        output.clear();
        while (m_index < m_text.size())
        {
            const char current = m_text[m_index];
            if (current == '"')
            {
                ++m_index;
                return true;
            }
            if (current == '\\')
            {
                ++m_index;
                if (m_index >= m_text.size())
                {
                    error = "Unterminated escape sequence.";
                    return false;
                }
                switch (m_text[m_index])
                {
                    case '"':
                        output.push_back('"');
                        break;
                    case '\\':
                        output.push_back('\\');
                        break;
                    case '/':
                        output.push_back('/');
                        break;
                    case 'b':
                        output.push_back('\b');
                        break;
                    case 'f':
                        output.push_back('\f');
                        break;
                    case 'n':
                        output.push_back('\n');
                        break;
                    case 'r':
                        output.push_back('\r');
                        break;
                    case 't':
                        output.push_back('\t');
                        break;
                    default:
                        error = "Unsupported escape sequence.";
                        return false;
                }
                ++m_index;
                continue;
            }
            if (static_cast<unsigned char>(current) < 0x20)
            {
                error = "Control character inside string.";
                return false;
            }
            output.push_back(current);
            ++m_index;
        }

        error = "Unterminated string.";
        return false;
    }

    bool ParseNumber(JsonNode& node, std::string& error)
    {
        const std::size_t start = m_index;
        if (m_index < m_text.size() && m_text[m_index] == '-')
        {
            ++m_index;
        }
        while (m_index < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_index])) != 0)
        {
            ++m_index;
        }
        if (m_index < m_text.size() && m_text[m_index] == '.')
        {
            ++m_index;
            while (m_index < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_index])) != 0)
            {
                ++m_index;
            }
        }
        if (m_index < m_text.size() && (m_text[m_index] == 'e' || m_text[m_index] == 'E'))
        {
            ++m_index;
            if (m_index < m_text.size() && (m_text[m_index] == '+' || m_text[m_index] == '-'))
            {
                ++m_index;
            }
            while (m_index < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_index])) != 0)
            {
                ++m_index;
            }
        }

        if (m_index == start || m_index > m_text.size())
        {
            error = "Invalid number.";
            return false;
        }

        std::istringstream stream{std::string{m_text.substr(start, m_index - start)}};
        double value{};
        if (!(stream >> value))
        {
            error = "Invalid number value.";
            return false;
        }
        node.kind = JsonNode::Kind::Number;
        node.number = value;
        return true;
    }

    bool ParseArray(JsonNode& node, std::string& error)
    {
        if (!Consume('['))
        {
            error = "Expected '[' for array.";
            return false;
        }
        if (m_depth >= kMaxNestingDepth)
        {
            error = "JSON nesting exceeds limit of 64 levels.";
            return false;
        }
        ++m_depth;
        const DepthGuard depthGuard{*this};
        node.kind = JsonNode::Kind::Array;

        if (!SkipWhitespace())
        {
            error = "Unterminated array.";
            return false;
        }
        if (Consume(']'))
        {
            return true;
        }

        for (;;)
        {
            if (!SkipWhitespace())
            {
                error = "Unterminated array.";
                return false;
            }

            JsonNode element;
            if (!ParseValue(element, error))
            {
                return false;
            }
            node.array.push_back(std::move(element));

            if (!SkipWhitespace())
            {
                error = "Unterminated array.";
                return false;
            }
            if (Consume(']'))
            {
                return true;
            }
            if (!Consume(','))
            {
                error = "Expected ',' or ']' in array.";
                return false;
            }
        }
    }

    bool ParseObject(JsonNode& node, std::string& error)
    {
        if (!Consume('{'))
        {
            error = "Expected '{' for object.";
            return false;
        }
        if (m_depth >= kMaxNestingDepth)
        {
            error = "JSON nesting exceeds limit of 64 levels.";
            return false;
        }
        ++m_depth;
        const DepthGuard depthGuard{*this};
        node.kind = JsonNode::Kind::Object;

        if (!SkipWhitespace())
        {
            error = "Unterminated object.";
            return false;
        }
        if (Consume('}'))
        {
            return true;
        }

        for (;;)
        {
            if (!SkipWhitespace())
            {
                error = "Unterminated object.";
                return false;
            }

            JsonNode key;
            if (!ParseValue(key, error) || key.kind != JsonNode::Kind::String)
            {
                error = "Object key must be a string.";
                return false;
            }

            if (!SkipWhitespace() || !Consume(':'))
            {
                error = "Expected ':' after object key.";
                return false;
            }
            if (!SkipWhitespace())
            {
                error = "Expected value after ':'.";
                return false;
            }

            JsonNode value;
            if (!ParseValue(value, error))
            {
                return false;
            }
            node.object.emplace_back(key.string, std::move(value));

            if (!SkipWhitespace())
            {
                error = "Unterminated object.";
                return false;
            }
            if (Consume('}'))
            {
                return true;
            }
            if (!Consume(','))
            {
                error = "Expected ',' or '}' in object.";
                return false;
            }
        }
    }

    std::string_view m_text;
    std::size_t m_index{};
    int m_depth{};
};

// 检查对象的所有 key 都在白名单内；发现未知 key 返回 false 并写 error。
// M3-05：未知 key 必须失败，否则新增字段不会进入 BuildKey（漏失效）。
bool ValidateKnownKeys(const JsonNode& object, const std::string_view context, const std::span<const std::string_view> knownKeys, std::string& error)
{
    for (const auto& entry : object.object)
    {
        bool known = false;
        for (const std::string_view key : knownKeys)
        {
            if (entry.first == key)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            error = "Unknown recipe key '" + entry.first + "' in " + std::string{context};
            return false;
        }
    }
    return true;
}

// 在对象节点中按名查找字段；key 为单层字段名（如 "source" 或 "import"）。
const JsonNode* FindField(const JsonNode& object, const std::string_view key)
{
    if (object.kind != JsonNode::Kind::Object)
    {
        return nullptr;
    }
    const auto it = std::find_if(object.object.begin(), object.object.end(),
                                 [key](const std::pair<std::string, JsonNode>& entry)
                                 { return entry.first == key; });
    return it == object.object.end() ? nullptr : &it->second;
}

bool GetRequiredString(const JsonNode& object, const std::string_view key, std::string& output, std::string& error)
{
    const JsonNode* field = FindField(object, key);
    if (field == nullptr || field->kind != JsonNode::Kind::String || field->string.empty())
    {
        error = "Recipe field '" + std::string{key} + "' is required and must be a non-empty string.";
        return false;
    }
    output = field->string;
    return true;
}

bool GetOptionalBool(const JsonNode& object, const std::string_view key, bool& output)
{
    const JsonNode* field = FindField(object, key);
    if (field == nullptr || field->kind != JsonNode::Kind::Boolean)
    {
        return true; // 缺省或类型不符时保留当前值
    }
    output = field->boolean;
    return true;
}

bool GetOptionalNumber(const JsonNode& object, const std::string_view key, double& output)
{
    const JsonNode* field = FindField(object, key);
    if (field == nullptr || field->kind != JsonNode::Kind::Number)
    {
        return true;
    }
    output = field->number;
    return true;
}

bool GetImportSubfield(const JsonNode& recipeObject, const std::string_view key, bool& output)
{
    const JsonNode* import = FindField(recipeObject, "import");
    if (import == nullptr)
    {
        return true;
    }
    return GetOptionalBool(*import, key, output);
}

bool GetLimitsSubfield(const JsonNode& recipeObject, const std::string_view key, double& output)
{
    const JsonNode* limits = FindField(recipeObject, "limits");
    if (limits == nullptr)
    {
        return true;
    }
    return GetOptionalNumber(*limits, key, output);
}
} // namespace

std::optional<Recipe> ParseRecipeText(const std::string_view jsonText, std::string& error)
{
    JsonParser parser{jsonText};
    JsonNode root;
    if (!parser.Parse(root, error))
    {
        return std::nullopt;
    }
    if (root.kind != JsonNode::Kind::Object)
    {
        error = "Recipe root must be a JSON object.";
        return std::nullopt;
    }

    // --- 未知 key 拒绝（M3-05）---
    constexpr std::array<std::string_view, 8> kRootKeys{
        "schemaVersion", "source", "assetRoot", "scene", "profile", "import", "limits", "environment"};
    if (!ValidateKnownKeys(root, "root", kRootKeys, error))
    {
        return std::nullopt;
    }

    const JsonNode* import = FindField(root, "import");
    if (import != nullptr && import->kind == JsonNode::Kind::Object)
    {
        constexpr std::array<std::string_view, 4> kImportKeys{
            "meshes", "textures", "world", "generateMissingNormals"};
        if (!ValidateKnownKeys(*import, "import", kImportKeys, error))
        {
            return std::nullopt;
        }
    }

    const JsonNode* limits = FindField(root, "limits");
    if (limits != nullptr && limits->kind == JsonNode::Kind::Object)
    {
        constexpr std::array<std::string_view, 5> kLimitsKeys{
            "maxFileBytes", "maxVerticesPerPrimitive", "maxIndicesPerPrimitive",
            "maxTextureDimension", "maxNodes"};
        if (!ValidateKnownKeys(*limits, "limits", kLimitsKeys, error))
        {
            return std::nullopt;
        }
    }

    Recipe recipe;

    // M4-04：可选 environment 段（HDR panorama）。子对象存在时字段全必填
    //（来源记录 license/sourceUrl/sourceSha256 是篇目契约，缺一拒绝——
    // "下载了 HDRI 但没登记来源"不允许通过）。
    const JsonNode* environment = FindField(root, "environment");
    if (environment != nullptr)
    {
        if (environment->kind != JsonNode::Kind::Object)
        {
            error = "Recipe field 'environment' must be an object.";
            return std::nullopt;
        }
        constexpr std::array<std::string_view, 6> kEnvironmentKeys{
            "source", "assetUri", "usage", "license", "sourceUrl", "sourceSha256"};
        if (!ValidateKnownKeys(*environment, "environment", kEnvironmentKeys, error))
        {
            return std::nullopt;
        }
        RecipeEnvironment env;
        if (!GetRequiredString(*environment, "source", env.source, error) ||
            !GetRequiredString(*environment, "assetUri", env.assetUri, error) ||
            !GetRequiredString(*environment, "license", env.license, error) ||
            !GetRequiredString(*environment, "sourceUrl", env.sourceUrl, error) ||
            !GetRequiredString(*environment, "sourceSha256", env.sourceSha256, error))
        {
            return std::nullopt;
        }
        // usage 有默认值；显式给出时必须与 M4 唯一支持值一致（fail-closed）。
        const JsonNode* usage = FindField(*environment, "usage");
        if (usage != nullptr && usage->kind == JsonNode::Kind::String)
        {
            if (usage->string != env.usage)
            {
                error = "Recipe environment usage must be 'HdrEnvironment' (only supported value in M4).";
                return std::nullopt;
            }
        }
        recipe.environment = std::move(env);
    }

    const JsonNode* schemaVersion = FindField(root, "schemaVersion");
    if (schemaVersion == nullptr || schemaVersion->kind != JsonNode::Kind::Number)
    {
        error = "Recipe field 'schemaVersion' is required and must be a number.";
        return std::nullopt;
    }
    if (schemaVersion->number != 1.0)
    {
        error = "Unsupported recipe schemaVersion.";
        return std::nullopt;
    }
    recipe.schemaVersion = 1;

    if (!GetRequiredString(root, "source", recipe.source, error) ||
        !GetRequiredString(root, "assetRoot", recipe.assetRoot, error) ||
        !GetRequiredString(root, "profile", recipe.profile, error))
    {
        return std::nullopt;
    }

    const JsonNode* scene = FindField(root, "scene");
    if (scene != nullptr && scene->kind == JsonNode::Kind::String && !scene->string.empty())
    {
        recipe.scene = scene->string;
    }

    GetImportSubfield(root, "meshes", recipe.importMeshes);
    GetImportSubfield(root, "textures", recipe.importTextures);
    GetImportSubfield(root, "world", recipe.importWorld);
    GetImportSubfield(root, "generateMissingNormals", recipe.generateMissingNormals);

    double value = 0.0;
    if (GetLimitsSubfield(root, "maxFileBytes", value))
    {
        recipe.maxFileBytes = static_cast<std::uint64_t>(value);
    }
    if (GetLimitsSubfield(root, "maxVerticesPerPrimitive", value))
    {
        recipe.maxVerticesPerPrimitive = static_cast<std::uint32_t>(value);
    }
    if (GetLimitsSubfield(root, "maxIndicesPerPrimitive", value))
    {
        recipe.maxIndicesPerPrimitive = static_cast<std::uint32_t>(value);
    }
    if (GetLimitsSubfield(root, "maxTextureDimension", value))
    {
        recipe.maxTextureDimension = static_cast<std::uint32_t>(value);
    }
    if (GetLimitsSubfield(root, "maxNodes", value))
    {
        recipe.maxNodes = static_cast<std::uint32_t>(value);
    }

    return recipe;
}

std::optional<Recipe> ParseRecipeFile(const std::filesystem::path& recipePath, std::string& error)
{
    std::ifstream stream{recipePath, std::ios::binary};
    if (!stream)
    {
        error = "Failed to open recipe file: " + recipePath.string();
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof())
    {
        error = "Failed to read recipe file: " + recipePath.string();
        return std::nullopt;
    }

    return ParseRecipeText(buffer.str(), error);
}

std::optional<std::filesystem::path> ResolveSourceWithinRoot(const std::filesystem::path& sourceRoot,
                                                             const std::filesystem::path& source)
{
    if (source.empty() || source.is_absolute())
    {
        return std::nullopt;
    }

    const std::filesystem::path joined = sourceRoot / source;
    const std::filesystem::path rootNormal = sourceRoot.lexically_normal();
    const std::filesystem::path joinedNormal = joined.lexically_normal();

    // lexically_relative 在 joined 不在 root 内时会产生 "../" 前缀，据此拒绝逃逸。
    const std::filesystem::path relative = joinedNormal.lexically_relative(rootNormal);
    if (relative.empty() || relative.is_absolute())
    {
        return std::nullopt;
    }
    for (const std::filesystem::path& component : relative)
    {
        if (component == "..")
        {
            return std::nullopt;
        }
    }

    return joinedNormal;
}
} // namespace MiniEngine::Tools
