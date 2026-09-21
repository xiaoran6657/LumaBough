// ============================================================================
// Sha256Tests.cpp — 引擎侧 SHA-256 的官方向量与流式契约
// 里程碑：M3（7-A）
// 职责：用 NIST 官方 SHA-256 测试向量锁定引擎侧纯 C++ 实现，并验证流式 Builder
//       的分块一致性、终态（Finish 后不可续用）等契约；结果与 Cooker 的 CNG
//       实现互证（ADR-0004「影响」：双重实现是有意设计）。
// 关联：engine/assets/src/Sha256.cpp（被测实现）
//       tests/tools/Sha256BuilderTests.cpp（Cooker CNG 侧的互证测试）
// ============================================================================

#include <MiniEngine/Assets/Sha256.h>

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>

namespace
{
std::string ToHex(const MiniEngine::Assets::Sha256Digest& digest)
{
    return MiniEngine::Assets::ToHexDigest(digest);
}

MiniEngine::Assets::Sha256Digest HashText(const std::string& text)
{
    MiniEngine::Assets::Sha256Builder builder;
    MiniEngine::Assets::Sha256Digest digest{};
    static_cast<void>(builder.Append(std::as_bytes(std::span{text.data(), text.size()})));
    static_cast<void>(builder.Finish(digest));
    return digest;
}
} // namespace

// 官方向量：空串摘要（NIST FIPS 180-4 附录示例）。
TEST(Sha256Tests, EmptyStringMatchesOfficialVector)
{
    EXPECT_EQ(ToHex(HashText("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// 官方向量："abc"。
TEST(Sha256Tests, AbcMatchesOfficialVector)
{
    EXPECT_EQ(ToHex(HashText("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// 官方向量："abcd"（覆盖跨 448 位边界前的填充路径）。
TEST(Sha256Tests, AbcdMatchesOfficialVector)
{
    EXPECT_EQ(ToHex(HashText("abcd")), "88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589");
}

// 分块 Append 的结果必须与一次性 Append 完全一致（流式拼接无歧义）。
TEST(Sha256Tests, ChunkedAppendMatchesOneShot)
{
    MiniEngine::Assets::Sha256Builder chunked;
    MiniEngine::Assets::Sha256Digest chunkedDigest{};
    static_cast<void>(chunked.Append(std::as_bytes(std::span{"ab", 2})));
    static_cast<void>(chunked.Append(std::as_bytes(std::span{"cd", 2})));
    static_cast<void>(chunked.Finish(chunkedDigest));
    EXPECT_EQ(ToHex(chunkedDigest), ToHex(HashText("abcd")));
}

// Finish 之后 Append 必须失败：摘要不可被续写。
TEST(Sha256Tests, FinishThenAppendFails)
{
    MiniEngine::Assets::Sha256Builder builder;
    MiniEngine::Assets::Sha256Digest digest{};
    static_cast<void>(builder.Finish(digest));
    EXPECT_FALSE(builder.Append(std::as_bytes(std::span{"x", 1})));
}

// Finish 只能成功一次：重复 Finish 返回 false（终态保护）。
TEST(Sha256Tests, FinishTwiceFails)
{
    MiniEngine::Assets::Sha256Builder builder;
    MiniEngine::Assets::Sha256Digest digest{};
    EXPECT_TRUE(builder.Finish(digest));
    EXPECT_FALSE(builder.Finish(digest));
}

// ---------------------------------------------------------------------------
// P1（2026-09-10 审查修复）：填充跨分组边界。
// 旧实现在缓冲内已有 56..63 字节消息时把长度写进 m_buffer[56..63] 并漏掉第二次
// 压缩，mod 64 ∈ [56,63] 的输入摘要完全错误；以下向量（'a' × N）逐个钉住
// "恰好放得下 / 0x80 落组尾 / 需要第二个填充分组"三类路径。
// 期望值由 .NET System.Security.Cryptography.SHA256 独立生成，
// 其中 1,000,000 × 'a' 与 FIPS 180-4 官方向量一致（cdc76e5c…）。
// ---------------------------------------------------------------------------

// 'a' × N 的构造器：覆盖填充边界的消息长度档位。
namespace
{
MiniEngine::Assets::Sha256Digest HashRepeatedBytes(const std::size_t count)
{
    const std::vector<std::byte> payload(count, std::byte{0x61U}); // 'a'
    return MiniEngine::Assets::Sha256(payload);
}
} // namespace

// 55/56/57：0x80 分别落在"同组仍有空间 / 恰好占满本组尾部之前的最后位置 / 需要跨组"的档位。
TEST(Sha256Tests, PaddingBoundaryAround56Bytes)
{
    EXPECT_EQ(ToHex(HashRepeatedBytes(55)), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(ToHex(HashRepeatedBytes(56)), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_EQ(ToHex(HashRepeatedBytes(57)), "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6");
}

// 62/63/64/65：长度字段必须进入第二个填充分组的档位（旧缺陷区间）与整组边界。
TEST(Sha256Tests, PaddingRequiresSecondBlockRange)
{
    EXPECT_EQ(ToHex(HashRepeatedBytes(62)), "f506898cc7c2e092f9eb9fadae7ba50383f5b46a2a4fe5597dbb553a78981268");
    EXPECT_EQ(ToHex(HashRepeatedBytes(63)), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    EXPECT_EQ(ToHex(HashRepeatedBytes(64)), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    EXPECT_EQ(ToHex(HashRepeatedBytes(65)), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
}

// 1000 字节：多分组 + 常规填充路径（FIPS "million a" 的近邻档位）。
TEST(Sha256Tests, ThousandBytesMatchesReference)
{
    EXPECT_EQ(ToHex(HashRepeatedBytes(1000)), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

// FIPS 180-4 官方向量：1,000,000 × 'a'。
TEST(Sha256Tests, MillionAMatchesOfficialVector)
{
    EXPECT_EQ(ToHex(HashRepeatedBytes(1000000)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// 复现 M5-01 P1 的实际形态：28350 字节（mod 64 = 62，恰为缺陷区间），
// 且分块 Append 与一次性结果一致——manifest hash 的使用方式就是分块/一次性混合。
TEST(Sha256Tests, ManifestLike28350BytesMatchesReference)
{
    // 内容不影响填充路径，用递增字节模拟任意二进制（含 0x00 与高位字节）。
    std::vector<std::byte> payload(28350);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<std::byte>(i & 0xFFU);
    }
    const auto oneShot = MiniEngine::Assets::Sha256(payload);

    MiniEngine::Assets::Sha256Builder chunked;
    MiniEngine::Assets::Sha256Digest chunkedDigest{};
    // 按 4096 分块（近似流式读取的真实块大小）。
    constexpr std::size_t kChunk = 4096;
    for (std::size_t offset = 0; offset < payload.size(); offset += kChunk)
    {
        const std::size_t take = std::min(kChunk, payload.size() - offset);
        static_cast<void>(chunked.Append(std::span{payload.data() + offset, take}));
    }
    static_cast<void>(chunked.Finish(chunkedDigest));

    EXPECT_EQ(ToHex(chunkedDigest), ToHex(oneShot)) << "分块与一次性必须一致";
    // 该输入 mod 64 = 62，必须走"第二个填充分组"路径且与 .NET 参考一致。
    EXPECT_EQ(ToHex(oneShot), "4f2078e554ec1f703af3e86bd0a0143d7d34924cfc77050fa91d6b95adfcf4c8")
        << "28350 字节递增字节的参考摘要（.NET 独立生成）；如实现回退，此断言先失败";
}
