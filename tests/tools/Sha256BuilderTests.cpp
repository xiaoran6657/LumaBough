// ============================================================================
// Sha256BuilderTests.cpp — Cooker 侧 CNG SHA-256 的官方向量与 preimage 编码原语
// 里程碑：M3-05
// 职责：用 NIST 官方向量锁定 CNG 实现，验证流式拼接一致性、终态保护，以及
//       AppendU32LE / AppendU64LE 的小端编码（BuildKey preimage 依赖它）；
//       结果与引擎侧纯 C++ 实现互证。
// 关联：tools/asset_cooker/src/Sha256.cpp（被测实现）
//       tests/assets/Sha256Tests.cpp（引擎侧互证测试）
// ============================================================================

#include "Sha256.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace
{
constexpr std::array<std::byte, 32> kEmptyDigest{
    std::byte{0xE3}, std::byte{0xB0}, std::byte{0xC4}, std::byte{0x42}, std::byte{0x98}, std::byte{0xFC},
    std::byte{0x1C}, std::byte{0x14}, std::byte{0x9A}, std::byte{0xFB}, std::byte{0xF4}, std::byte{0xC8},
    std::byte{0x99}, std::byte{0x6F}, std::byte{0xB9}, std::byte{0x24}, std::byte{0x27}, std::byte{0xAE},
    std::byte{0x41}, std::byte{0xE4}, std::byte{0x64}, std::byte{0x9B}, std::byte{0x93}, std::byte{0x4C},
    std::byte{0xA4}, std::byte{0x95}, std::byte{0x99}, std::byte{0x1B}, std::byte{0x78}, std::byte{0x52},
    std::byte{0xB8}, std::byte{0x55},
};

constexpr std::array<std::byte, 32> kAbcDigest{
    std::byte{0xBA}, std::byte{0x78}, std::byte{0x16}, std::byte{0xBF}, std::byte{0x8F}, std::byte{0x01},
    std::byte{0xCF}, std::byte{0xEA}, std::byte{0x41}, std::byte{0x41}, std::byte{0x40}, std::byte{0xDE},
    std::byte{0x5D}, std::byte{0xAE}, std::byte{0x22}, std::byte{0x23}, std::byte{0xB0}, std::byte{0x03},
    std::byte{0x61}, std::byte{0xA3}, std::byte{0x96}, std::byte{0x17}, std::byte{0x7A}, std::byte{0x9C},
    std::byte{0xB4}, std::byte{0x10}, std::byte{0xFF}, std::byte{0x61}, std::byte{0xF2}, std::byte{0x00},
    std::byte{0x15}, std::byte{0xAD},
};

// SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
constexpr std::array<std::byte, 32> kHelloDigest{
    std::byte{0x2C}, std::byte{0xF2}, std::byte{0x4D}, std::byte{0xBA}, std::byte{0x5F}, std::byte{0xB0},
    std::byte{0xA3}, std::byte{0x0E}, std::byte{0x26}, std::byte{0xE8}, std::byte{0x3B}, std::byte{0x2A},
    std::byte{0xC5}, std::byte{0xB9}, std::byte{0xE2}, std::byte{0x9E}, std::byte{0x1B}, std::byte{0x16},
    std::byte{0x1E}, std::byte{0x5C}, std::byte{0x1F}, std::byte{0xA7}, std::byte{0x42}, std::byte{0x5E},
    std::byte{0x73}, std::byte{0x04}, std::byte{0x33}, std::byte{0x62}, std::byte{0x93}, std::byte{0x8B},
    std::byte{0x98}, std::byte{0x24},
};

std::array<std::byte, 32> ToBytes(const std::string_view text)
{
    std::array<std::byte, 32> bytes{};
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(text[index]));
    }
    return bytes;
}

std::span<const std::byte> AsSpan(const std::array<std::byte, 32>& bytes, const std::size_t length)
{
    return {bytes.data(), length};
}
} // namespace

TEST(Sha256BuilderTests, EmptyStringMatchesOfficialVector)
{
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());

    MiniEngine::Tools::Sha256Digest digest{};
    ASSERT_TRUE(builder.Finish(digest));
    EXPECT_EQ(digest, kEmptyDigest);
}

TEST(Sha256BuilderTests, AbcMatchesOfficialVector)
{
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());

    const auto bytes = ToBytes("abc");
    ASSERT_TRUE(builder.AppendBytes(AsSpan(bytes, 3)));
    MiniEngine::Tools::Sha256Digest digest{};
    ASSERT_TRUE(builder.Finish(digest));
    EXPECT_EQ(digest, kAbcDigest);
}

TEST(Sha256BuilderTests, ChunkedHelloMatchesOfficialVector)
{
    // 审计：原用例名为"分块"却只 append 一次。真正按 3 块追加，结果仍等于官方向量。
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());

    const auto bytes = ToBytes("hello");
    ASSERT_TRUE(builder.AppendBytes({bytes.data(), 2}));     // "he"
    ASSERT_TRUE(builder.AppendBytes({bytes.data() + 2, 2})); // "ll"
    ASSERT_TRUE(builder.AppendBytes({bytes.data() + 4, 1})); // "o"
    MiniEngine::Tools::Sha256Digest digest{};
    ASSERT_TRUE(builder.Finish(digest));
    EXPECT_EQ(digest, kHelloDigest);
}

TEST(Sha256BuilderTests, FinishThenAppendFails)
{
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());

    const auto bytes = ToBytes("abc");
    ASSERT_TRUE(builder.AppendBytes(AsSpan(bytes, 3)));
    MiniEngine::Tools::Sha256Digest digest{};
    ASSERT_TRUE(builder.Finish(digest));

    EXPECT_FALSE(builder.AppendBytes(AsSpan(bytes, 3)));
}

TEST(Sha256BuilderTests, FinishTwiceFails)
{
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());

    MiniEngine::Tools::Sha256Digest digest{};
    ASSERT_TRUE(builder.Finish(digest));
    EXPECT_FALSE(builder.Finish(digest));
}

TEST(Sha256BuilderTests, ReadyBuilderAcceptsEmptyAppend)
{
    // 审计：原用例名"AppendAfterFailedConstructionIsFalse"与断言方向矛盾（无法在此环境
    // 强制 BCrypt 失败）。如实表述契约：IsReady 后才可 append，空 append 合法。
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());
    EXPECT_TRUE(builder.AppendBytes({}));
}

TEST(Sha256BuilderTests, AppendU32LEAndU64LEProduceExpectedDigest)
{
    MiniEngine::Tools::Sha256Builder builder;
    ASSERT_TRUE(builder.IsReady());

    ASSERT_TRUE(builder.AppendU32LE(0x01020304U));
    ASSERT_TRUE(builder.AppendU64LE(0x1122334455667788ULL));

    // 预期 = SHA-256( 04 03 02 01 88 77 66 55 44 33 22 11 )，与一次送入相同字节等价。
    MiniEngine::Tools::Sha256Builder reference;
    ASSERT_TRUE(reference.IsReady());
    const std::array<std::byte, 12> raw{
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01}, std::byte{0x88}, std::byte{0x77},
        std::byte{0x66}, std::byte{0x55}, std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},
    };
    ASSERT_TRUE(reference.AppendBytes(raw));

    MiniEngine::Tools::Sha256Digest digest{};
    MiniEngine::Tools::Sha256Digest referenceDigest{};
    ASSERT_TRUE(builder.Finish(digest));
    ASSERT_TRUE(reference.Finish(referenceDigest));
    EXPECT_EQ(digest, referenceDigest);
}
