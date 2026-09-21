// ============================================================================
// BuildKey.cpp — BuildKey preimage 编码
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 3 节
// ============================================================================
#include "BuildKey.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace MiniEngine::Tools
{
namespace
{
// domain 前缀 + u8 0：与其他用途的 SHA-256 做域分离（同 AssetId 的做法）。
constexpr std::string_view kPreimageDomain{"MiniEngineBuildKey"};

[[nodiscard]] bool AppendRawBytes(Sha256Builder& builder, std::span<const char> bytes)
{
    return builder.AppendBytes(std::as_bytes(bytes));
}

[[nodiscard]] bool AppendDomainByte(Sha256Builder& builder, const std::uint8_t value)
{
    const std::array<char, 1> one{static_cast<char>(value)};
    return AppendRawBytes(builder, std::span<const char>{one.data(), one.size()});
}

// 定长字节块（如 32 字节 contentHash）。注意：块本身不带长度前缀，
// 但其长度由类型/协议固定，因此不存在拼接歧义。
[[nodiscard]] bool AppendDigest(Sha256Builder& builder, const Sha256Digest& digest)
{
    return builder.AppendBytes(std::as_bytes(std::span{digest.data(), digest.size()}));
}

// 二进制块（canonical recipe bytes）必须带长度前缀：
// 变长内容不加长度会让 "ab"+"c" 与 "a"+"bc" 混淆。
[[nodiscard]] bool AppendLengthPrefixedBytes(Sha256Builder& builder, const std::string_view bytes)
{
    if (bytes.size() > 0xFFFFFFFFU)
    {
        return false;
    }
    if (!builder.AppendU32LE(static_cast<std::uint32_t>(bytes.size())))
    {
        return false;
    }
    return AppendRawBytes(builder, std::span<const char>{bytes.data(), bytes.size()});
}

[[nodiscard]] bool AppendUtf8WithLength(Sha256Builder& builder, const std::string_view text)
{
    return builder.AppendUtf8WithLength(text);
}
} // namespace

bool ComputeBuildKey(const BuildKeyInputs& inputs, Sha256Digest& digest, std::string& error)
{
    error.clear();
    digest = Sha256Digest{};

    // 依赖表必须先排序；未排序会导致同一组依赖因插入顺序不同而产生不同 key。
    std::vector<DependencyRecord> dependencies = inputs.dependencies;
    SortDependencies(dependencies);
    if (!ValidateDependencies(dependencies, error))
    {
        return false;
    }

    Sha256Builder builder;
    if (!builder.IsReady())
    {
        error = "SHA-256 builder is not ready.";
        return false;
    }

    // --- 固定头部 ---
    if (!AppendRawBytes(builder, std::span<const char>{kPreimageDomain.data(), kPreimageDomain.size()}) ||
        !AppendDomainByte(builder, 0) ||
        !builder.AppendU32LE(inputs.buildKeySchemaVersion) ||
        !AppendUtf8WithLength(builder, inputs.cookerVersion) ||
        !builder.AppendU32LE(inputs.bakedFormatVersion) ||
        !AppendUtf8WithLength(builder, inputs.profile) ||
        !AppendUtf8WithLength(builder, inputs.canonicalAssetUri) ||
        !AppendLengthPrefixedBytes(builder, inputs.canonicalRecipeBytes))
    {
        error = "Failed to append BuildKey header fields.";
        return false;
    }

    // --- 依赖表 ---
    if (dependencies.size() > 0xFFFFFFFFU)
    {
        error = "Too many dependencies.";
        return false;
    }
    if (!builder.AppendU32LE(static_cast<std::uint32_t>(dependencies.size())))
    {
        error = "Failed to append dependency count.";
        return false;
    }

    for (const DependencyRecord& record : dependencies)
    {
        if (!AppendDomainByte(builder, static_cast<std::uint8_t>(record.kind)) ||
            !AppendUtf8WithLength(builder, record.normalizedPathOrName) ||
            !AppendDigest(builder, record.contentHash))
        {
            error = "Failed to append dependency: " + record.normalizedPathOrName;
            return false;
        }
    }

    // --- 工具与规则 ---
    if (!AppendUtf8WithLength(builder, inputs.importerName) ||
        !AppendUtf8WithLength(builder, inputs.importerSettings))
    {
        error = "Failed to append importer identity fields.";
        return false;
    }

    if (!builder.Finish(digest))
    {
        error = "Failed to finish BuildKey hash.";
        return false;
    }

    return true;
}

std::string ToHexDigest(const Sha256Digest& digest)
{
    constexpr std::array<char, 16> kDigits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

    std::string result;
    result.reserve(digest.size() * 2);
    for (const std::byte value : digest)
    {
        const auto byte = static_cast<unsigned char>(value);
        result.push_back(kDigits[(byte >> 4U) & 0xFU]);
        result.push_back(kDigits[byte & 0xFU]);
    }
    return result;
}
} // namespace MiniEngine::Tools
