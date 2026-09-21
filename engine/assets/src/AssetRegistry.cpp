// ============================================================================
// AssetRegistry.cpp — Manifest 严格解析与 Registry 构建的实现
// 里程碑：M3（7-A）
// 职责：实现 DeriveAssetId 的域分隔派生与 AssetRegistry::ParseAndValidate。
//       解析由文件内的最小 JSON 扫描器完成（面向固定 schema，非通用解析器）：
//       未知名、缺名、重复名、错误字面量与不安全路径都在这里拒绝。
// 关联：docs/architecture/DECISIONS.md §3
//       engine/assets/include/MiniEngine/Assets/AssetRegistry.h（解析契约）
// ============================================================================

#include <MiniEngine/Assets/AssetRegistry.h>

#include <MiniEngine/Assets/Sha256.h>

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniEngine::Assets
{
namespace
{
// AssetId 域分隔前缀；'\0' 分隔 domain 与 URI，避免前缀/后缀歧义。
constexpr std::string_view kAssetIdDerivationDomain = "MiniEngine/AssetId/v1";

bool IsLowerHexDigit(const char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// artifactPath 沙箱：'/' 分隔的相对路径，拒绝绝对路径、'\'、'.'、'..' 与空段。
bool IsValidArtifactRelativePath(const std::string& text)
{
    if (text.empty() || text.front() == '/' || text.find('\\') != std::string::npos ||
        text.find(':') != std::string::npos)
    {
        return false;
    }

    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = text.find('/', start);
        const std::size_t segmentLength = slash == std::string::npos ? text.size() - start : slash - start;
        const std::string_view segment{text.data() + start, segmentLength};
        if (segment.empty() || segment == "." || segment == "..")
        {
            return false;
        }
        if (slash == std::string::npos)
        {
            return true;
        }
        start = slash + 1;
        if (start == text.size())
        {
            return false; // 末尾 '/' 视为空段
        }
    }
}

// 面向固定 manifest schema 的最小严格 JSON 扫描器：
// 只接受 schemaVersion=1 的 {"assets":[…],"profile":…,"schemaVersion":1}，
// 未知 key、缺 key、重复 key、错误字面量均失败。
struct JsonCursor final
{
    std::string_view text{};
    std::size_t position{};
    std::string error{};

    [[nodiscard]] bool Fail(const std::string_view message)
    {
        if (error.empty())
        {
            error = std::string(message);
        }
        return false;
    }

    void SkipWhitespace() noexcept
    {
        while (position < text.size() &&
               (text[position] == ' ' || text[position] == '\t' || text[position] == '\n' || text[position] == '\r'))
        {
            ++position;
        }
    }

    [[nodiscard]] bool Consume(const char expected)
    {
        SkipWhitespace();
        if (position >= text.size() || text[position] != expected)
        {
            return Fail("unexpected character in manifest");
        }
        ++position;
        return true;
    }

    [[nodiscard]] bool AtEnd()
    {
        SkipWhitespace();
        return position >= text.size();
    }

    [[nodiscard]] bool ReadString(std::string& out)
    {
        SkipWhitespace();
        if (position >= text.size() || text[position] != '"')
        {
            return Fail("expected string literal");
        }
        ++position;
        out.clear();
        while (true)
        {
            if (position >= text.size())
            {
                return Fail("unterminated string literal");
            }
            const char c = text[position++];
            if (c == '"')
            {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                return Fail("raw control character in string");
            }
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }

            if (position >= text.size())
            {
                return Fail("unterminated escape sequence");
            }
            const char escape = text[position++];
            switch (escape)
            {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
            {
                // M3 的 URI/路径为 ASCII；非 ASCII \u 明确拒绝，不做隐式宽度转换。
                if (position + 4 > text.size())
                {
                    return Fail("truncated \\u escape");
                }
                std::uint32_t codePoint{};
                for (std::size_t i = 0; i < 4; ++i)
                {
                    const char digit = text[position + i];
                    std::uint32_t value{};
                    if (digit >= '0' && digit <= '9')
                    {
                        value = static_cast<std::uint32_t>(digit - '0');
                    }
                    else if (digit >= 'a' && digit <= 'f')
                    {
                        value = static_cast<std::uint32_t>(digit - 'a' + 10);
                    }
                    else if (digit >= 'A' && digit <= 'F')
                    {
                        value = static_cast<std::uint32_t>(digit - 'A' + 10);
                    }
                    else
                    {
                        return Fail("invalid \\u escape");
                    }
                    codePoint = codePoint * 16U + value;
                }
                position += 4;
                if (codePoint >= 0x80U)
                {
                    return Fail("non-ASCII \\u escape");
                }
                out.push_back(static_cast<char>(codePoint));
                break;
            }
            default:
                return Fail("invalid escape sequence");
            }
        }
    }

    [[nodiscard]] bool ReadUint64(std::uint64_t& out)
    {
        SkipWhitespace();
        std::size_t length = 0;
        while (position + length < text.size() && text[position + length] >= '0' && text[position + length] <= '9')
        {
            ++length;
        }
        if (length == 0 || length > 20)
        {
            return Fail("expected unsigned integer");
        }
        const char* begin = text.data() + position;
        std::uint64_t value{};
        const auto result = std::from_chars(begin, begin + length, value, 10);
        if (result.ec != std::errc{})
        {
            return Fail("invalid unsigned integer");
        }
        position += length;
        out = value;
        return true;
    }

    [[nodiscard]] bool ReadLowerHexDigest32(std::array<std::byte, 32>& out)
    {
        std::string hex;
        if (!ReadString(hex))
        {
            return false;
        }
        if (hex.size() != 64)
        {
            return Fail("digest must be 64 hex characters");
        }
        for (std::size_t i = 0; i < 32; ++i)
        {
            if (!IsLowerHexDigit(hex[i * 2]) || !IsLowerHexDigit(hex[i * 2 + 1]))
            {
                return Fail("digest must be lowercase hex");
            }
            const auto high = static_cast<std::uint8_t>(hex[i * 2] <= '9' ? hex[i * 2] - '0' : hex[i * 2] - 'a' + 10);
            const auto low =
                static_cast<std::uint8_t>(hex[i * 2 + 1] <= '9' ? hex[i * 2 + 1] - '0' : hex[i * 2 + 1] - 'a' + 10);
            out[i] = static_cast<std::byte>(high * 16U + low);
        }
        return true;
    }
};

// 解析 assets[] 中的一个条目对象。
// schemaVersion=1 的条目固定六个 key：artifactHash / artifactPath / assetUri /
// buildKey / fileSize / kind；重复 key、未知 key、缺 key 均失败。
// fileSize 自 M7-06 起保存在 RegistryEntry 中（异步加载的字节预算与尺寸上限），
// 但仍以 BakedReader 的 header.fileSize 作为产物内部尺寸的权威。
[[nodiscard]] bool ParseAssetEntry(JsonCursor& cursor, std::vector<RegistryEntry>& entries)
{
    if (!cursor.Consume('{'))
    {
        return false;
    }

    bool hasArtifactHash = false;
    bool hasArtifactPath = false;
    bool hasAssetUri = false;
    bool hasBuildKey = false;
    bool hasFileSize = false;
    bool hasKind = false;

    RegistryEntry entry{};
    std::string kindText;

    cursor.SkipWhitespace();
    if (cursor.position < cursor.text.size() && cursor.text[cursor.position] == '}')
    {
        ++cursor.position;
    }
    else
    {
        while (true)
        {
            std::string key;
            if (!cursor.ReadString(key) || !cursor.Consume(':'))
            {
                return false;
            }

            if (key == "artifactHash")
            {
                if (hasArtifactHash)
                {
                    return cursor.Fail("duplicate key in manifest entry");
                }
                hasArtifactHash = true;
                if (!cursor.ReadLowerHexDigest32(entry.artifactHash.bytes))
                {
                    return false;
                }
            }
            else if (key == "artifactPath")
            {
                if (hasArtifactPath)
                {
                    return cursor.Fail("duplicate key in manifest entry");
                }
                hasArtifactPath = true;
                std::string path;
                if (!cursor.ReadString(path))
                {
                    return false;
                }
                entry.artifactRelativePath = path;
            }
            else if (key == "assetUri")
            {
                if (hasAssetUri)
                {
                    return cursor.Fail("duplicate key in manifest entry");
                }
                hasAssetUri = true;
                if (!cursor.ReadString(entry.canonicalUri))
                {
                    return false;
                }
            }
            else if (key == "buildKey")
            {
                if (hasBuildKey)
                {
                    return cursor.Fail("duplicate key in manifest entry");
                }
                hasBuildKey = true;
                if (!cursor.ReadLowerHexDigest32(entry.buildKey.bytes))
                {
                    return false;
                }
            }
            else if (key == "fileSize")
            {
                if (hasFileSize)
                {
                    return cursor.Fail("duplicate key in manifest entry");
                }
                hasFileSize = true;
                std::uint64_t fileSize{};
                if (!cursor.ReadUint64(fileSize))
                {
                    return false;
                }
                entry.fileSize = fileSize;
            }
            else if (key == "kind")
            {
                if (hasKind)
                {
                    return cursor.Fail("duplicate key in manifest entry");
                }
                hasKind = true;
                if (!cursor.ReadString(kindText))
                {
                    return false;
                }
            }
            else
            {
                return cursor.Fail("unknown manifest entry key");
            }

            if (cursor.Consume(','))
            {
                continue;
            }
            if (!cursor.Consume('}'))
            {
                return false;
            }
            break;
        }
    }

    if (!hasArtifactHash || !hasArtifactPath || !hasAssetUri || !hasBuildKey || !hasFileSize || !hasKind)
    {
        return cursor.Fail("manifest entry is missing a required key");
    }
    if (kindText == "mesh")
    {
        entry.kind = AssetKind::Mesh;
    }
    else if (kindText == "texture")
    {
        entry.kind = AssetKind::Texture;
    }
    else if (kindText == "world")
    {
        entry.kind = AssetKind::World;
    }
    else if (kindText == "material")
    {
        entry.kind = AssetKind::Material;
    }
    else
    {
        return cursor.Fail("unknown asset kind");
    }
    if (entry.canonicalUri.empty())
    {
        return cursor.Fail("assetUri must not be empty");
    }
    if (!IsValidArtifactRelativePath(entry.artifactRelativePath.generic_string()))
    {
        return cursor.Fail("artifactPath is not a safe relative path");
    }

    entry.id = DeriveAssetId(entry.canonicalUri);
    entries.push_back(std::move(entry));
    return true;
}

// 解析 Manifest 根对象：{ "assets":[…], "profile":…, "schemaVersion":1 }。
// 只接受 schemaVersion=1；根对象之后不允许任何多余数据（fail-closed）。
[[nodiscard]] bool ParseManifestRoot(JsonCursor& cursor, std::vector<RegistryEntry>& entries)
{
    if (!cursor.Consume('{'))
    {
        return false;
    }

    bool hasAssets = false;
    bool hasProfile = false;
    bool hasSchemaVersion = false;
    std::uint64_t schemaVersion = 0;
    std::string profile;

    cursor.SkipWhitespace();
    if (cursor.position < cursor.text.size() && cursor.text[cursor.position] == '}')
    {
        ++cursor.position;
    }
    else
    {
        while (true)
        {
            std::string key;
            if (!cursor.ReadString(key) || !cursor.Consume(':'))
            {
                return false;
            }

            if (key == "assets")
            {
                if (hasAssets)
                {
                    return cursor.Fail("duplicate key in manifest");
                }
                hasAssets = true;
                if (!cursor.Consume('['))
                {
                    return false;
                }
                cursor.SkipWhitespace();
                if (cursor.position < cursor.text.size() && cursor.text[cursor.position] == ']')
                {
                    ++cursor.position;
                }
                else
                {
                    while (true)
                    {
                        if (!ParseAssetEntry(cursor, entries))
                        {
                            return false;
                        }
                        if (cursor.Consume(','))
                        {
                            continue;
                        }
                        if (!cursor.Consume(']'))
                        {
                            return false;
                        }
                        break;
                    }
                }
            }
            else if (key == "profile")
            {
                if (hasProfile)
                {
                    return cursor.Fail("duplicate key in manifest");
                }
                hasProfile = true;
                if (!cursor.ReadString(profile))
                {
                    return false;
                }
            }
            else if (key == "schemaVersion")
            {
                if (hasSchemaVersion)
                {
                    return cursor.Fail("duplicate key in manifest");
                }
                hasSchemaVersion = true;
                if (!cursor.ReadUint64(schemaVersion))
                {
                    return false;
                }
            }
            else
            {
                return cursor.Fail("unknown manifest key");
            }

            if (cursor.Consume(','))
            {
                continue;
            }
            if (!cursor.Consume('}'))
            {
                return false;
            }
            break;
        }
    }

    if (!hasAssets || !hasProfile || !hasSchemaVersion)
    {
        return cursor.Fail("manifest is missing a required key");
    }
    if (schemaVersion != 1)
    {
        return cursor.Fail("unsupported manifest schemaVersion");
    }
    if (!cursor.AtEnd())
    {
        return cursor.Fail("trailing data after manifest object");
    }
    return true;
}
} // namespace

AssetId DeriveAssetId(const std::string& canonicalUri)
{
    Sha256Builder builder;
    Sha256Digest digest{};
    const auto domainBytes = std::as_bytes(std::span{kAssetIdDerivationDomain.data(), kAssetIdDerivationDomain.size()});
    constexpr std::byte kSeparator{0U};
    // 此处 builder 处于初始状态，Append 不可能失败；显式丢弃返回值以满足 [[nodiscard]]。
    static_cast<void>(builder.Append(domainBytes));
    static_cast<void>(builder.Append(std::span{&kSeparator, 1}));
    static_cast<void>(builder.Append(std::as_bytes(std::span{canonicalUri.data(), canonicalUri.size()})));
    static_cast<void>(builder.Finish(digest));

    AssetId id{};
    std::copy_n(digest.begin(), id.bytes.size(), id.bytes.begin());
    return id;
}

std::optional<AssetRegistry> AssetRegistry::ParseAndValidate(const std::span<const std::byte> manifestBytes,
                                                             const std::filesystem::path& outputRoot,
                                                             std::string& error)
{
    error.clear();
    if (manifestBytes.empty())
    {
        error = "empty manifest";
        return std::nullopt;
    }

    JsonCursor cursor{std::string_view{reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()}};
    std::vector<RegistryEntry> entries;
    if (!ParseManifestRoot(cursor, entries))
    {
        error = cursor.error.empty() ? "manifest parse failed" : cursor.error;
        return std::nullopt;
    }

    // output root 边界复核：RegistryEntry 只存相对路径，绝对化后不得逃逸 outputRoot。
    std::string rootText = outputRoot.lexically_normal().generic_string();
    if (rootText.empty() || rootText.back() != '/')
    {
        rootText.push_back('/');
    }
    for (const RegistryEntry& entry : entries)
    {
        const std::filesystem::path absolute = (outputRoot / entry.artifactRelativePath).lexically_normal();
        if (!absolute.generic_string().starts_with(rootText))
        {
            error = "artifact path escapes output root";
            return std::nullopt;
        }
    }

    // 按 AssetId 字节序排序：Manifest diff 与 AssetChanged 事件的确定性基础。
    std::sort(entries.begin(), entries.end(),
              [](const RegistryEntry& left, const RegistryEntry& right) { return left.id.bytes < right.id.bytes; });
    for (std::size_t index = 1; index < entries.size(); ++index)
    {
        if (entries[index].id == entries[index - 1].id)
        {
            error = "duplicate asset identity: " + entries[index].canonicalUri;
            return std::nullopt;
        }
    }

    return AssetRegistry{std::move(entries)};
}

// 由已排序、已去重的条目构建 AssetId → 下标索引；排序责任在 ParseAndValidate。
AssetRegistry::AssetRegistry(std::vector<RegistryEntry> entries) : m_entries{std::move(entries)}
{
    for (std::size_t index = 0; index < m_entries.size(); ++index)
    {
        m_byId.emplace(m_entries[index].id, index);
    }
}

const RegistryEntry* AssetRegistry::Find(const AssetId id) const noexcept
{
    const auto found = m_byId.find(id);
    return found == m_byId.end() ? nullptr : &m_entries[found->second];
}

std::size_t AssetRegistry::EntryCount() const noexcept
{
    return m_entries.size();
}

const RegistryEntry* AssetRegistry::EntryAt(const std::size_t index) const noexcept
{
    return index < m_entries.size() ? &m_entries[index] : nullptr;
}
} // namespace MiniEngine::Assets
