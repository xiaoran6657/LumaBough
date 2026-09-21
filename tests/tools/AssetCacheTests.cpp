// ============================================================================
// AssetCacheTests.cpp — Cache 布局、命中判定与反向依赖图测试
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 5、6、8 节
// 风格：与 tests/tools/Sha256BuilderTests.cpp 一致
// 注意：本文件会用到 MiniEngineAssets（BakedReader），落地时测试 target 需
//       链接 MiniEngine::Assets，并把 ContentHash/BuildKey/CanonicalRecipe/
//       DependencyRecord/AssetCache 源文件加入编译。
// ============================================================================
#include "AssetCache.h"
#include "BuildKey.h"
#include "DependencyRecord.h"
#include "Sha256.h"

#include <MiniEngine/Assets/BakedFormat.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
MiniEngine::Tools::Sha256Digest DigestFilled(const unsigned char value)
{
    MiniEngine::Tools::Sha256Digest digest{};
    digest.fill(std::byte{value});
    return digest;
}

// 临时目录：使用测试框架提供的 TMPDIR。
// 已知环境限制（见 M2 验收记录）：%TEMP% 含非 ASCII 时部分用例会失败，
// 运行前设置 $env:TEMP='C:\tools\tmp'。
std::filesystem::path TempDir()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "MiniEngineAssetCacheTests";
    std::filesystem::create_directories(dir);
    return dir;
}

void AppendU16(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value)
{
    if (bytes.size() < offset + 2)
    {
        bytes.resize(offset + 2, std::byte{0});
    }
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void AppendU32(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value)
{
    if (bytes.size() < offset + 4)
    {
        bytes.resize(offset + 4, std::byte{0});
    }
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void AppendU64(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value)
{
    if (bytes.size() < offset + 8)
    {
        bytes.resize(offset + 8, std::byte{0});
    }
    for (std::size_t index = 0; index < 8; ++index)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

// 审计：cache hit 是 M3 核心行为，之前只有 miss 用例。这里用真实 header-only
// artifact 走完整 EvaluateCacheHit 判定链（存在性→Reader→BuildKey→SHA-256）。
// 布局与 Cooker writer/WorldLoaderTests 的 MakeMeshArtifact 相同（header only，64B）。
std::filesystem::path WriteHeaderOnlyMeshArtifact(const std::filesystem::path& dir, const std::string& name,
                                                  const MiniEngine::Tools::Sha256Digest& buildKey,
                                                  MiniEngine::Tools::Sha256Digest& outArtifactHash)
{
    std::vector<std::byte> bytes(MiniEngine::Assets::kBakedHeaderSize, std::byte{0});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendU16(bytes, 4, MiniEngine::Assets::kBakedFormatVersion);
    AppendU16(bytes, 6, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::Mesh));
    AppendU32(bytes, 8, static_cast<std::uint32_t>(MiniEngine::Assets::kBakedHeaderSize));
    AppendU32(bytes, 12, 0); // chunkCount = 0（header only）
    AppendU64(bytes, 16, static_cast<std::uint64_t>(bytes.size()));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);

    MiniEngine::Tools::Sha256Builder hasher;
    MiniEngine::Tools::Sha256Digest digest{};
    if (hasher.IsReady() && hasher.AppendBytes(bytes))
    {
        static_cast<void>(hasher.Finish(digest));
    }
    outArtifactHash = digest;

    const std::filesystem::path file = dir / name;
    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file;
}
} // namespace

// shard = BuildKey 前两 hex；目录名 = 完整 64 hex。
TEST(AssetCacheTests, CacheDirectoryUsesTwoHexShardAndFullKey)
{
    const MiniEngine::Tools::Sha256Digest key = DigestFilled(0xAB);
    const std::filesystem::path dir = MiniEngine::Tools::BuildCacheDirectory(std::filesystem::path{"C:/out"}, key);

    const std::string hex = MiniEngine::Tools::ToHexDigest(key);
    const std::string expected = "C:/out/cache/" + hex.substr(0, 2) + "/" + hex;
    EXPECT_EQ(dir.generic_string(), expected);
}

// artifact 文件名 = 完整 ArtifactHash + 稳定 suffix + 扩展名，不含绝对路径。
TEST(AssetCacheTests, ArtifactPathContainsHashAndStableSuffixOnly)
{
    const MiniEngine::Tools::Sha256Digest hash = DigestFilled(0x01);
    const std::filesystem::path path = MiniEngine::Tools::BuildArtifactPath(
        std::filesystem::path{"C:/out/cache/ab/key"}, hash, "mesh-0-primitive-0", "memesh");

    const std::string name = path.filename().string();
    EXPECT_EQ(name, MiniEngine::Tools::ToHexDigest(hash) + "-mesh-0-primitive-0.memesh");
    EXPECT_EQ(name.find("C:"), std::string::npos);
}

// 文件不存在 → miss，且给出原因。
TEST(AssetCacheTests, MissingFileIsMiss)
{
    const auto result = MiniEngine::Tools::EvaluateCacheHit(
        MiniEngine::Tools::CacheHitInput{.artifactPath = TempDir() / "does-not-exist.memesh",
                                         .expectedBuildKey = DigestFilled(0x02),
                                         .expectedArtifactHash = DigestFilled(0x03),
                                         .kind = MiniEngine::Assets::BakedAssetKind::Mesh});

    EXPECT_FALSE(result.hit);
    EXPECT_FALSE(result.reason.empty());
}

// "文件存在"不等于命中：内容不是合法 artifact 必须 miss。
TEST(AssetCacheTests, ExistingButInvalidFileIsMissNotHit)
{
    const std::filesystem::path file = TempDir() / "garbage.memesh";
    {
        std::ofstream out{file, std::ios::binary};
        out << "not a baked artifact";
    }

    const auto result = MiniEngine::Tools::EvaluateCacheHit(
        MiniEngine::Tools::CacheHitInput{.artifactPath = file,
                                         .expectedBuildKey = DigestFilled(0x02),
                                         .expectedArtifactHash = DigestFilled(0x03),
                                         .kind = MiniEngine::Assets::BakedAssetKind::Mesh});

    EXPECT_FALSE(result.hit);
    EXPECT_FALSE(result.reason.empty());
}

// artifactHash 与记录不一致 → corruption，必须 miss（不静默复用）。
TEST(AssetCacheTests, ArtifactHashMismatchIsMiss)
{
    const std::filesystem::path file = TempDir() / "hashmismatch.memesh";
    {
        std::ofstream out{file, std::ios::binary};
        out << "payload";
    }

    // expectedArtifactHash 故意设成与实际内容不符的值。
    const auto result = MiniEngine::Tools::EvaluateCacheHit(
        MiniEngine::Tools::CacheHitInput{.artifactPath = file,
                                         .expectedBuildKey = DigestFilled(0x02),
                                         .expectedArtifactHash = DigestFilled(0x7F),
                                         .kind = MiniEngine::Assets::BakedAssetKind::Mesh});

    EXPECT_FALSE(result.hit);
    EXPECT_NE(result.reason.find("artifactHash"), std::string::npos);
}

// 审计补强：valid hit 是缓存核心行为——真实 artifact + 正确 hash 必须命中且不重写。
TEST(AssetCacheTests, ValidArtifactIsHitAndDoesNotRewrite)
{
    const MiniEngine::Tools::Sha256Digest buildKey = DigestFilled(0x2A);
    MiniEngine::Tools::Sha256Digest artifactHash{};
    const std::filesystem::path file =
        WriteHeaderOnlyMeshArtifact(TempDir(), "valid-hit.memesh", buildKey, artifactHash);
    const auto before = std::filesystem::last_write_time(file);

    const MiniEngine::Tools::CacheHitInput input{.artifactPath = file,
                                                 .expectedBuildKey = buildKey,
                                                 .expectedArtifactHash = artifactHash,
                                                 .kind = MiniEngine::Assets::BakedAssetKind::Mesh};
    const auto first = MiniEngine::Tools::EvaluateCacheHit(input);
    EXPECT_TRUE(first.hit) << first.reason;
    EXPECT_TRUE(first.reason.empty());

    // 再次判定仍是 hit，且文件未被改写（时间戳不变）。
    const auto second = MiniEngine::Tools::EvaluateCacheHit(input);
    EXPECT_TRUE(second.hit) << second.reason;
    EXPECT_EQ(std::filesystem::last_write_time(file), before);
}

// header BuildKey 与期望不符 → miss（不能拿 buildKey 不同的旧产物顶替）。
TEST(AssetCacheTests, HeaderBuildKeyMismatchIsMiss)
{
    MiniEngine::Tools::Sha256Digest artifactHash{};
    const std::filesystem::path file =
        WriteHeaderOnlyMeshArtifact(TempDir(), "wrong-key.memesh", DigestFilled(0x5A), artifactHash);

    const auto result = MiniEngine::Tools::EvaluateCacheHit(
        MiniEngine::Tools::CacheHitInput{.artifactPath = file,
                                         .expectedBuildKey = DigestFilled(0x2A), // 与 header 里的 0x5A 不同
                                         .expectedArtifactHash = artifactHash,
                                         .kind = MiniEngine::Assets::BakedAssetKind::Mesh});

    EXPECT_FALSE(result.hit);
    std::string lower = result.reason;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    EXPECT_NE(lower.find("buildkey"), std::string::npos) << result.reason;
}

// 反向依赖图：一个依赖变化应能解释受影响的 asset。
TEST(AssetCacheTests, ReverseGraphExplainsAffectedAssets)
{
    MiniEngine::Tools::ReverseDependencyGraph graph;

    MiniEngine::Tools::DependencyRecord shared;
    shared.kind = MiniEngine::Tools::DependencyKind::Source;
    shared.normalizedPathOrName = "demo/Triangle.bin";
    shared.contentHash = DigestFilled(0x10);

    MiniEngine::Tools::DependencyRecord other;
    other.kind = MiniEngine::Tools::DependencyKind::Source;
    other.normalizedPathOrName = "demo/other.bin";
    other.contentHash = DigestFilled(0x11);

    graph.AddAsset("asset://demo/a", {shared});
    graph.AddAsset("asset://demo/b", {shared, other});

    const auto affected = graph.FindAffectedAssets("demo/Triangle.bin");
    ASSERT_EQ(affected.size(), 2);
    EXPECT_EQ(affected[0], "asset://demo/a");
    EXPECT_EQ(affected[1], "asset://demo/b");

    // 只影响依赖它的 asset。
    const auto narrow = graph.FindAffectedAssets("demo/other.bin");
    ASSERT_EQ(narrow.size(), 1);
    EXPECT_EQ(narrow[0], "asset://demo/b");
}

// 反向图输出顺序必须确定（两次 clean cook 的 determinism 前提）。
TEST(AssetCacheTests, ReverseGraphOutputOrderIsDeterministic)
{
    MiniEngine::Tools::ReverseDependencyGraph first;
    MiniEngine::Tools::ReverseDependencyGraph second;

    MiniEngine::Tools::DependencyRecord dep;
    dep.kind = MiniEngine::Tools::DependencyKind::Source;
    dep.normalizedPathOrName = "demo/x.bin";

    // 两次以不同顺序登记 asset。
    first.AddAsset("asset://demo/a", {dep});
    first.AddAsset("asset://demo/b", {dep});

    second.AddAsset("asset://demo/b", {dep});
    second.AddAsset("asset://demo/a", {dep});

    EXPECT_EQ(first.FindAffectedAssets("demo/x.bin"), second.FindAffectedAssets("demo/x.bin"));
    EXPECT_EQ(first.GetAllAssets(), second.GetAllAssets());
}

// 补充用例（落地时补齐；多数需要真实的 artifact writer）：
// - valid hit 不重写 artifact（用真实 writer 产出一个 artifact，第二次 EvaluateCacheHit 应为 hit）；
// - header BuildKey mismatch → miss（构造一个合法但 buildKey 不同的 artifact）；
// - truncated / chunk overlap → miss（Reader 防御已覆盖，此处做 cache 层联动）；
// - 未改变 mtime touch → no-op（需 ContentHash + 依赖图联动，属 watch 层）；
// - 目录是 reparse point / symlink → miss；
// - 两次独立 clean cook 的 hash tree 完全相同（端到端，非单测，见验收脚本）。
