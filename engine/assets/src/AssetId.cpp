// ============================================================================
// AssetId.cpp — AssetId 的十六进制编解码与哈希器实现
// 里程碑：M3
// 职责：实现 AssetId.h 声明的 IsValid / ToHexString / ParseHex 与 AssetIdHasher。
//       身份的派生逻辑不在此处，而在 AssetRegistry.cpp 的 DeriveAssetId（域分隔）。
// 关联：engine/assets/include/MiniEngine/Assets/AssetId.h
//       engine/assets/src/AssetRegistry.cpp（DeriveAssetId）
// ============================================================================

#include <MiniEngine/Assets/AssetId.h>

#include <algorithm>
#include <charconv>

namespace MiniEngine::Assets
{
namespace
{
constexpr char kHexDigits[] = "0123456789abcdef";

// 解析一对十六进制字符为一字节；任一字符非法即失败。
// 交给 std::from_chars（基数 16）判定，避免手写字符区间表造成遗漏。
std::optional<std::byte> ParseByte(const char high, const char low)
{
    const char text[2]{high, low};
    std::uint8_t value{};
    const auto result = std::from_chars(text, text + 2, value, 16);
    if (result.ec != std::errc{} || result.ptr != text + 2)
    {
        return std::nullopt;
    }

    return static_cast<std::byte>(value);
}
} // namespace

bool AssetId::IsValid() const noexcept
{
    return std::ranges::any_of(bytes, [](const std::byte value) { return value != std::byte{}; });
}

std::string AssetId::ToHexString() const
{
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        const auto value = std::to_integer<std::uint8_t>(bytes[index]);
        result[index * 2] = kHexDigits[value >> 4];
        result[index * 2 + 1] = kHexDigits[value & 0x0FU];
    }

    return result;
}

std::optional<AssetId> AssetId::ParseHex(const std::string_view text)
{
    AssetId result{};
    if (text.size() != result.bytes.size() * 2)
    {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < result.bytes.size(); ++index)
    {
        const auto value = ParseByte(text[index * 2], text[index * 2 + 1]);
        if (!value)
        {
            return std::nullopt;
        }
        result.bytes[index] = *value;
    }

    return result.IsValid() ? std::optional<AssetId>{result} : std::nullopt;
}

std::size_t AssetIdHasher::operator()(const AssetId& id) const noexcept
{
    // A runtime hash table accelerator, not a persistent identity or security hash.
    std::size_t result = 14695981039346656037ULL;
    for (const std::byte value : id.bytes)
    {
        result ^= std::to_integer<std::uint8_t>(value);
        result *= 1099511628211ULL;
    }
    return result;
}
} // namespace MiniEngine::Assets