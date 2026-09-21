// ============================================================================
// AssetManagerTests.cpp — Manifest 加载事务的端到端契约
// 里程碑：M3（7-A / 7-C / glTF / WIC / whole-world / 审计 3.1）
// 职责：覆盖两阶段事务的失败语义与热重载语义：初始加载、no-op、失败保留
//       last-known-good、reload 保持 Handle 且 revision+1、header 谎报拒绝、
//       World artifact 验证不落池、chunk 解码、BeginCommit 先建槽等。
// 关联：engine/assets/src/AssetManager.cpp（被测实现）
//       docs/architecture/DECISIONS.md §4、§7
// ============================================================================

#include <MiniEngine/Assets/AssetManager.h>

#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Assets/TextureFormatV2.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace
{
using namespace MiniEngine::Assets;

std::uint32_t g_testDirCounter = 0;

std::filesystem::path MakeTempAssetDir()
{
    // gtest_discover_tests 让每个测试用例跑在独立进程里，进程内静态计数器
    // 都会从 1 重新开始：若只有计数器，后跑的测试会撞见先跑的测试留在
    // temp 的同名目录（MissingArtifact 误读到旧 artifact 的根因）。
    // 进程级基址（首次调用时刻）+ 进程内计数器 = 实际唯一目录。
    static const std::uint64_t processBase =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    ++g_testDirCounter;
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("MiniEngineAssetManagerTests-" + std::to_string(processBase) + "-" + std::to_string(g_testDirCounter));
    std::filesystem::create_directories(dir);
    return dir;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

Sha256Digest HashText(const std::string& text)
{
    Sha256Builder builder;
    Sha256Digest digest{};
    static_cast<void>(builder.Append(std::as_bytes(std::span{text.data(), text.size()})));
    static_cast<void>(builder.Finish(digest));
    return digest;
}

// 按 wire 偏移构造最小合法 .memesh（64 字节 header + 1 字节 payload）。
// 不能 memcpy BakedHeader：MSVC 对齐会在 fileSize 前插 padding（wire 布局无 padding）。
std::vector<std::byte> MakeMeshArtifact(const Sha256Digest& buildKey, const std::byte payloadTag)
{
    std::vector<std::byte> bytes(kBakedHeaderSize + 1, std::byte{0U});
    const auto write16 = [&bytes](const std::size_t offset, const std::uint16_t value)
    {
        bytes[offset] = static_cast<std::byte>(value & 0xFFU);
        bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    };
    const auto write32 = [&bytes](const std::size_t offset, const std::uint32_t value)
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
        }
    };
    const auto write64 = [&bytes](const std::size_t offset, const std::uint64_t value)
    {
        for (std::size_t i = 0; i < 8; ++i)
        {
            bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
        }
    };

    bytes[0] = static_cast<std::byte>('M');
    bytes[1] = static_cast<std::byte>('E');
    bytes[2] = static_cast<std::byte>('A');
    bytes[3] = static_cast<std::byte>('3');
    write16(4, kBakedFormatVersion);
    write16(6, static_cast<std::uint16_t>(BakedAssetKind::Mesh));
    write32(8, static_cast<std::uint32_t>(kBakedHeaderSize));
    write32(12, 0); // chunkCount：7-A 阶段 Cooker 只产 header-only artifact
    write64(16, kBakedHeaderSize + 1);
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    write32(56, kMeshFormatVersion); // flags：M4 起为 per-kind 格式版本载波（.memesh v2）
    write32(60, 0);                  // reserved
    bytes[64] = payloadTag;
    return bytes;
}

std::string EntryForArtifact(const std::string& uri, const Sha256Digest& artifactHash, const Sha256Digest& buildKey,
                             const std::string& kind = "mesh")
{
    const std::string artifactHashHex = ToHexDigest(artifactHash);
    const std::string buildKeyHex = ToHexDigest(buildKey);
    return "{\"artifactHash\":\"" + artifactHashHex + "\",\"artifactPath\":\"cache/aa/" + buildKeyHex +
           "-mesh-0.memesh\",\"assetUri\":\"" + uri + "\",\"buildKey\":\"" + buildKeyHex +
           "\",\"fileSize\":65,\"kind\":\"" + kind + "\"}";
}

std::string ManifestJson(const std::string& entriesCsv)
{
    return "{\"assets\":[" + entriesCsv + "],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
}

std::vector<std::byte> ToBytes(const std::string& text)
{
    const auto span = std::as_bytes(std::span{text.data(), text.size()});
    return {span.begin(), span.end()};
}

struct LoadedAsset final
{
    std::filesystem::path dir;
    Sha256Digest buildKey;
    AssetManager manager;
};

// 建立一个已 commit 的初始状态：1 个 mesh artifact（payloadTag=7）。
LoadedAsset LoadInitial(const std::byte payloadTag = std::byte{7U})
{
    LoadedAsset loaded;
    loaded.dir = MakeTempAssetDir();
    loaded.buildKey = HashText("mesh-build-key");

    const auto artifact = MakeMeshArtifact(loaded.buildKey, payloadTag);
    const auto artifactPath = loaded.dir / "cache" / "aa" / (ToHexDigest(loaded.buildKey) + "-mesh-0.memesh");
    EXPECT_TRUE(WriteFileBytes(artifactPath, artifact));

    const std::string manifest = ManifestJson(EntryForArtifact("meshes/demo/box", Sha256(artifact), loaded.buildKey));
    EXPECT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    EXPECT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();
    EXPECT_FALSE(loaded.manager.HasPendingCommit());
    return loaded;
}
} // namespace

TEST(AssetManagerTests, InitialLoadCommitsMeshSlotWithRevisionOne)
{
    LoadedAsset loaded = LoadInitial();

    ASSERT_NE(loaded.manager.Registry(), nullptr);
    EXPECT_EQ(loaded.manager.Registry()->EntryCount(), 1U);

    const auto handle = loaded.manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));
    const auto view = loaded.manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 1U);
    EXPECT_EQ(loaded.manager.GetStats().assetsCommitted, 1U);
}

TEST(AssetManagerTests, IdenticalManifestIsNoOp)
{
    LoadedAsset loaded = LoadInitial();

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    EXPECT_FALSE(loaded.manager.HasPendingCommit()); // digest 未变 → 无 pending
    EXPECT_EQ(loaded.manager.GetStats().manifestNoOps, 1U);

    const auto handle = loaded.manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));
    const auto view = loaded.manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 1U); // no-op 不递增 revision
}

TEST(AssetManagerTests, FailedPrepareKeepsActiveRegistryAndSlots)
{
    LoadedAsset loaded = LoadInitial();

    // Manifest 声称的 artifactHash 与磁盘内容不一致。
    const Sha256Digest wrongHash = HashText("not-the-artifact");
    const std::string badManifest = ManifestJson(EntryForArtifact("meshes/demo/box", wrongHash, loaded.buildKey));
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(badManifest)));

    std::string error;
    EXPECT_FALSE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error));
    EXPECT_NE(error.find("hash mismatch"), std::string::npos) << error;
    EXPECT_NE(error.find("meshes/demo/box"), std::string::npos) << error;
    EXPECT_FALSE(loaded.manager.HasPendingCommit());

    // active Registry 与 slot 状态完整保留（last-known-good）。
    ASSERT_NE(loaded.manager.Registry(), nullptr);
    EXPECT_EQ(loaded.manager.Registry()->EntryCount(), 1U);
    const auto handle = loaded.manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));
    const auto view = loaded.manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 1U);
}

TEST(AssetManagerTests, ReloadKeepsHandleAndBumpsRevision)
{
    LoadedAsset loaded = LoadInitial();
    const auto originalHandle = loaded.manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));

    // 内容变化（payloadTag 7→8）、AssetId/URI/buildKey 不变 → 同 Handle、revision+1。
    const auto newArtifact = MakeMeshArtifact(loaded.buildKey, std::byte{8U});
    const auto artifactPath = loaded.dir / "cache" / "aa" / (ToHexDigest(loaded.buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, newArtifact));
    const std::string newManifest =
        ManifestJson(EntryForArtifact("meshes/demo/box", Sha256(newArtifact), loaded.buildKey));
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(newManifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    ASSERT_TRUE(loaded.manager.HasPendingCommit());
    loaded.manager.CommitPending();

    const auto reloadedHandle = loaded.manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));
    EXPECT_EQ(reloadedHandle, originalHandle); // 热重载不变式：Handle 稳定
    const auto view = loaded.manager.Meshes().TryGet(reloadedHandle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 2U);
    EXPECT_EQ(loaded.manager.GetStats().assetsCommitted, 2U);
}

TEST(AssetManagerTests, HeaderBuildKeyMismatchIsRejected)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest realBuildKey = HashText("real-build-key");
    const Sha256Digest claimedBuildKey = HashText("other-build-key");

    // artifact 实际 header 带 realBuildKey，Manifest 谎报 claimedBuildKey：
    // artifactHash 自洽，但 BakedReader header/buildKey 校验必须拦截。
    const auto artifact = MakeMeshArtifact(realBuildKey, std::byte{1U});
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(claimedBuildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
    const std::string manifest = ManifestJson(EntryForArtifact("meshes/demo/box", Sha256(artifact), claimedBuildKey));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    EXPECT_FALSE(manager.PrepareManifestLoad(dir / "manifest.json", error));
    EXPECT_NE(error.find("header rejected"), std::string::npos) << error;
    EXPECT_EQ(manager.Registry(), nullptr);
}

std::vector<std::byte> MakeWorldArtifact(const Sha256Digest& buildKey)
{
    // header-only World artifact（.meworld payload 由 WorldLoader 消费，AssetManager 只验证）。
    std::vector<std::byte> bytes(kBakedHeaderSize, std::byte{0U});
    const auto write16 = [&bytes](const std::size_t offset, const std::uint16_t value)
    {
        bytes[offset] = static_cast<std::byte>(value & 0xFFU);
        bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    };
    const auto write32 = [&bytes](const std::size_t offset, const std::uint32_t value)
    {
        for (std::size_t index = 0; index < 4; ++index)
        {
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
        }
    };
    const auto write64 = [&bytes](const std::size_t offset, const std::uint64_t value)
    {
        for (std::size_t index = 0; index < 8; ++index)
        {
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
        }
    };
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    write16(4, kBakedFormatVersion);
    write16(6, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::World));
    write32(8, static_cast<std::uint32_t>(kBakedHeaderSize));
    write32(12, 0);
    write64(16, static_cast<std::uint64_t>(bytes.size()));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    write32(56, kWorldFormatVersion); // flags：M4 起为 per-kind 格式版本载波（.meworld v2）
    write32(60, 0);                   // reserved
    return bytes;
}

// .meworld 篇：World 条目 artifact 照常验证（kind=World header），但不进任何池。
TEST(AssetManagerTests, WorldArtifactIsVerifiedAndNotPooled)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("world-build-key");
    const auto artifact = MakeWorldArtifact(buildKey);
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-world-0.meworld");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));

    // 文件名不受 Registry 约束，Manifest artifactPath 指向实际写入位置。
    const std::string artifactHashHex = ToHexDigest(Sha256(artifact));
    const std::string buildKeyHex = ToHexDigest(buildKey);
    const std::string entry = "{\"artifactHash\":\"" + artifactHashHex + "\",\"artifactPath\":\"cache/aa/" +
                              buildKeyHex + "-world-0.meworld\",\"assetUri\":\"worlds/demo/scene\",\"buildKey\":\"" +
                              buildKeyHex + "\",\"fileSize\":" + std::to_string(artifact.size()) +
                              ",\"kind\":\"world\"}";
    const std::string manifest = ManifestJson(entry);
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    ASSERT_NE(manager.Registry(), nullptr);
    EXPECT_EQ(manager.Registry()->EntryCount(), 1U);
    // World 条目不产生 mesh/texture slot。
    EXPECT_FALSE(manager.Meshes().TryFind(DeriveAssetId("asset://world/no-pool")).has_value());
    EXPECT_FALSE(manager.Textures().TryFind(DeriveAssetId("asset://world/no-pool")).has_value());
}

// Manifest 称 World 但 artifact 头是 Mesh → Reader kind 校验必须拒绝。
TEST(AssetManagerTests, WorldKindClaimWithMeshHeaderIsRejected)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("mesh-build-key");
    const auto artifact = MakeMeshArtifact(buildKey, std::byte{1U});
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));

    const std::string manifest =
        ManifestJson(EntryForArtifact("worlds/demo/scene", Sha256(artifact), buildKey, "world"));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    EXPECT_FALSE(manager.PrepareManifestLoad(dir / "manifest.json", error));
    EXPECT_NE(error.find("mismatch"), std::string::npos) << error;
}

TEST(AssetManagerTests, MissingArtifactFileIsRejected)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("mesh-build-key");
    const Sha256Digest fakeHash = HashText("missing-file");
    const std::string manifest = ManifestJson(EntryForArtifact("meshes/demo/box", fakeHash, buildKey));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    EXPECT_FALSE(manager.PrepareManifestLoad(dir / "manifest.json", error));
    EXPECT_NE(error.find("failed to read artifact"), std::string::npos) << error;
}

TEST(AssetManagerTests, CommitWithoutPendingIsNoOp)
{
    AssetManager manager;
    manager.CommitPending();
    EXPECT_EQ(manager.Registry(), nullptr);
    EXPECT_EQ(manager.GetStats().assetsCommitted, 0U);
}

// ---------- 7-C：Manifest diff 与增量事务 ----------

// 批量建立 mesh 资产：uri 决定 buildKey（HashText(uri)），payloadTag 区分内容。
struct LoadedMeshes final
{
    std::filesystem::path dir;
    AssetManager manager;
    std::vector<std::pair<std::string, Sha256Digest>> assets; // uri → buildKey
};

void WriteMeshAsset(const std::filesystem::path& dir, const std::string& uri, const std::byte payloadTag)
{
    const Sha256Digest buildKey = HashText(uri);
    const auto artifact = MakeMeshArtifact(buildKey, payloadTag);
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
}

// ---------- G4：带 VERT/INDX 真实 payload 的 artifact 构造与解码 ----------

void AppendBytes16(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void AppendBytes32(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void AppendBytes64(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

// 与 Cooker 的 BuildMeshArtifact 同 schema（v2）：header(chunkCount=2, flags=2) +
// VERT@64/INDX@96 描述符 + 数据 @Align16。2 顶点（stride 48 = PbrVertex，
// vertex0.position.x = tag 的 float）+ 3 个 uint32 索引。顶点/索引值确定性，
// tag 改变 vertex0 的 position.x。
std::vector<std::byte> MakeMeshArtifactWithPayload(const Sha256Digest& buildKey, const std::byte payloadTag)
{
    constexpr std::size_t kDescriptorStart = kBakedHeaderSize;                       // 64
    constexpr std::size_t kVertDescriptor = kDescriptorStart;                        // 64
    constexpr std::size_t kIndxDescriptor = kDescriptorStart + kChunkDescriptorSize; // 96
    constexpr std::size_t kDataStart = kIndxDescriptor + kChunkDescriptorSize;       // 128

    constexpr std::size_t kVertexByteCount = 2 * 48U; // PbrVertex stride 48（.memesh v2）
    constexpr std::size_t kIndexByteCount = 3 * 4U;
    const std::size_t indexDataOffset = (kDataStart + kVertexByteCount + kChunkAlignment - 1) & ~(kChunkAlignment - 1);
    const std::size_t fileSize = indexDataOffset + kIndexByteCount;

    std::vector<std::byte> bytes(fileSize, std::byte{0U});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendBytes16(bytes, 4, kBakedFormatVersion);
    AppendBytes16(bytes, 6, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::Mesh));
    AppendBytes32(bytes, 8, static_cast<std::uint32_t>(kBakedHeaderSize));
    AppendBytes32(bytes, 12, 2);
    AppendBytes64(bytes, 16, static_cast<std::uint64_t>(fileSize));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24); // Sha256Digest = std::array
    AppendBytes32(bytes, 56, kMeshFormatVersion); // flags：.memesh v2（DecodeMeshChunks 语义层校验入口）

    const auto writeType = [&bytes](const std::size_t offset, const char* type)
    {
        bytes[offset] = static_cast<std::byte>(type[0]);
        bytes[offset + 1] = static_cast<std::byte>(type[1]);
        bytes[offset + 2] = static_cast<std::byte>(type[2]);
        bytes[offset + 3] = static_cast<std::byte>(type[3]);
    };
    writeType(kVertDescriptor, "VERT");
    AppendBytes32(bytes, kVertDescriptor + 4, 0);
    AppendBytes64(bytes, kVertDescriptor + 8, static_cast<std::uint64_t>(kDataStart));
    AppendBytes64(bytes, kVertDescriptor + 16, static_cast<std::uint64_t>(kVertexByteCount));
    AppendBytes32(bytes, kVertDescriptor + 24, 2);
    AppendBytes32(bytes, kVertDescriptor + 28, 48);

    writeType(kIndxDescriptor, "INDX");
    AppendBytes32(bytes, kIndxDescriptor + 4, 0);
    AppendBytes64(bytes, kIndxDescriptor + 8, static_cast<std::uint64_t>(indexDataOffset));
    AppendBytes64(bytes, kIndxDescriptor + 16, static_cast<std::uint64_t>(kIndexByteCount));
    AppendBytes32(bytes, kIndxDescriptor + 24, 3);
    AppendBytes32(bytes, kIndxDescriptor + 28, 4);

    // vertex0.position.x = float(tag)（其余为零）；索引 0,1,2。
    const float positionX = static_cast<float>(std::to_integer<int>(payloadTag));
    const std::uint32_t bits = [positionX]
    {
        std::uint32_t raw{};
        std::memcpy(&raw, &positionX, sizeof(raw));
        return raw;
    }();
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes[kDataStart + index] = static_cast<std::byte>((bits >> (index * 8U)) & 0xFFU);
    }
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        const std::size_t base = indexDataOffset + static_cast<std::size_t>(index) * 4U;
        bytes[base] = static_cast<std::byte>(index & 0xFFU);
    }
    return bytes;
}

float ReadPositionX(const std::vector<std::byte>& vertexData)
{
    std::uint32_t bits = 0;
    for (std::size_t index = 0; index < 4; ++index)
    {
        bits |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(vertexData[index])) << (index * 8U);
    }
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

TEST(AssetManagerTests, PayloadChunksAreDecodedIntoMeshAsset)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("mesh-build-key");
    const auto artifact = MakeMeshArtifactWithPayload(buildKey, std::byte{7U});
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
    const std::string manifest = ManifestJson(EntryForArtifact("meshes/demo/box", Sha256(artifact), buildKey));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    const auto handle = manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));
    const auto view = manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->asset->vertexCount, 2U);
    EXPECT_EQ(view->asset->vertexStride, 48U); // PbrVertex（.memesh v2）
    EXPECT_EQ(view->asset->vertexData.size(), 96U);
    EXPECT_EQ(view->asset->indexCount, 3U);
    EXPECT_EQ(view->asset->indexStride, 4U);
    EXPECT_EQ(view->asset->indexData.size(), 12U);
    EXPECT_FLOAT_EQ(ReadPositionX(view->asset->vertexData), 7.0F);
}

TEST(AssetManagerTests, PayloadReloadReplacesMeshDataAndBumpsRevision)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("mesh-build-key");
    const auto firstArtifact = MakeMeshArtifactWithPayload(buildKey, std::byte{7U});
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, firstArtifact));
    const std::string firstManifest =
        ManifestJson(EntryForArtifact("meshes/demo/box", Sha256(firstArtifact), buildKey));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(firstManifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();
    const auto handle = manager.Meshes().ResolveOrCreate(DeriveAssetId("meshes/demo/box"));

    // 内容变化（tag 7→9）→ revision+1，MeshAsset payload 整体替换。
    const auto secondArtifact = MakeMeshArtifactWithPayload(buildKey, std::byte{9U});
    ASSERT_TRUE(WriteFileBytes(artifactPath, secondArtifact));
    const std::string secondManifest =
        ManifestJson(EntryForArtifact("meshes/demo/box", Sha256(secondArtifact), buildKey));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(secondManifest)));

    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    const auto view = manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 2U);
    EXPECT_FLOAT_EQ(ReadPositionX(view->asset->vertexData), 9.0F);
}

// ---------- .metex v2（INFO/MIPS/DATA）真实 payload 构造与解码 ----------

// 与 Cooker BuildTextureArtifact 同 schema（v2）：header(chunkCount=3, flags=2) +
// INFO@64/MIPS@96/DATA@128 描述符 + INFO 数据@160(28B) + MIPS 数据@192(12B) +
// DATA@208。2x2 RGBA8 sRGB BaseColor、mipCount=1；tag 写入 pixel0 的 R。
std::vector<std::byte> MakeTextureArtifactWithPayload(const Sha256Digest& buildKey, const std::byte payloadTag)
{
    constexpr std::size_t kInfoDescriptor = kBakedHeaderSize;                                             // 64
    constexpr std::size_t kMipsDescriptor = kInfoDescriptor + kChunkDescriptorSize;                       // 96
    constexpr std::size_t kDataDescriptor = kMipsDescriptor + kChunkDescriptorSize;                       // 128
    constexpr std::size_t kInfoData = kDataDescriptor + kChunkDescriptorSize;                             // 160
    constexpr std::size_t kInfoSize = 28;                                                                 // 7 × u32
    const std::size_t mipsData = (kInfoData + kInfoSize + kChunkAlignment - 1) & ~(kChunkAlignment - 1);  // 192
    constexpr std::size_t kMipsSize = 12;                                                                 // 1 级 × 12B
    const std::size_t dataOffset = (mipsData + kMipsSize + kChunkAlignment - 1) & ~(kChunkAlignment - 1); // 208
    constexpr std::uint32_t kWidth = 2;
    constexpr std::uint32_t kHeight = 2;
    constexpr std::size_t dataSize = static_cast<std::size_t>(kWidth) * 4U * static_cast<std::size_t>(kHeight);

    std::vector<std::byte> bytes(dataOffset + dataSize, std::byte{0U});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendBytes16(bytes, 4, kBakedFormatVersion);
    AppendBytes16(bytes, 6, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::Texture));
    AppendBytes32(bytes, 8, static_cast<std::uint32_t>(kBakedHeaderSize));
    AppendBytes32(bytes, 12, 3);
    AppendBytes64(bytes, 16, static_cast<std::uint64_t>(bytes.size()));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24); // Sha256Digest = std::array
    AppendBytes32(bytes, 56, kTextureFormatVersion);                 // flags：.metex v2（DecodeTextureChunks 校验入口）

    const auto writeType = [&bytes](const std::size_t offset, const char* type)
    {
        bytes[offset] = static_cast<std::byte>(type[0]);
        bytes[offset + 1] = static_cast<std::byte>(type[1]);
        bytes[offset + 2] = static_cast<std::byte>(type[2]);
        bytes[offset + 3] = static_cast<std::byte>(type[3]);
    };
    writeType(kInfoDescriptor, "INFO");
    AppendBytes32(bytes, kInfoDescriptor + 4, 0);
    AppendBytes64(bytes, kInfoDescriptor + 8, static_cast<std::uint64_t>(kInfoData));
    AppendBytes64(bytes, kInfoDescriptor + 16, kInfoSize);
    AppendBytes32(bytes, kInfoDescriptor + 24, 7);
    AppendBytes32(bytes, kInfoDescriptor + 28, 4);

    writeType(kMipsDescriptor, "MIPS");
    AppendBytes32(bytes, kMipsDescriptor + 4, 0);
    AppendBytes64(bytes, kMipsDescriptor + 8, static_cast<std::uint64_t>(mipsData));
    AppendBytes64(bytes, kMipsDescriptor + 16, kMipsSize);
    AppendBytes32(bytes, kMipsDescriptor + 24, 1); // elementCount = mipCount
    AppendBytes32(bytes, kMipsDescriptor + 28, 12);

    writeType(kDataDescriptor, "DATA");
    AppendBytes32(bytes, kDataDescriptor + 4, 0);
    AppendBytes64(bytes, kDataDescriptor + 8, static_cast<std::uint64_t>(dataOffset));
    AppendBytes64(bytes, kDataDescriptor + 16, dataSize);
    AppendBytes32(bytes, kDataDescriptor + 24, kHeight);
    AppendBytes32(bytes, kDataDescriptor + 28, kWidth * 4U);

    // INFO v2：width | height | pixelFormat | colorSpace | usage | mipCount | mip0RowPitch。
    AppendBytes32(bytes, kInfoData, kWidth);
    AppendBytes32(bytes, kInfoData + 4, kHeight);
    AppendBytes32(bytes, kInfoData + 8, 1);            // pixelFormat = Rgba8Unorm
    AppendBytes32(bytes, kInfoData + 12, 1);           // colorSpace = Srgb
    AppendBytes32(bytes, kInfoData + 16, 1);           // usage = BaseColor
    AppendBytes32(bytes, kInfoData + 20, 1);           // mipCount = 1
    AppendBytes32(bytes, kInfoData + 24, kWidth * 4U); // mip0 rowPitch

    // MIPS 唯一一级：offset=0、rowPitch=8、byteSize=16（恰好覆盖 DATA）。
    AppendBytes32(bytes, mipsData, 0);
    AppendBytes32(bytes, mipsData + 4, kWidth * 4U);
    AppendBytes32(bytes, mipsData + 8, static_cast<std::uint32_t>(dataSize));

    bytes[dataOffset] = payloadTag; // pixel0.R 唯一标记
    return bytes;
}

std::string TextureEntryJson(const std::string& uri, const Sha256Digest& buildKey,
                             const std::vector<std::byte>& artifact)
{
    // EntryForArtifact 硬编码 mesh 后缀与 fileSize，纹理条目手工构造
    // （路径/文件名是练习 fixture 的约定，Reader 只按 Manifest artifactPath 读文件）。
    return "{\"artifactHash\":\"" + ToHexDigest(Sha256(artifact)) + "\",\"artifactPath\":\"cache/aa/" +
           ToHexDigest(buildKey) + "-mesh-0.memesh\",\"assetUri\":\"" + uri + "\",\"buildKey\":\"" +
           ToHexDigest(buildKey) + "\",\"fileSize\":" + std::to_string(artifact.size()) + ",\"kind\":\"texture\"}";
}

TEST(AssetManagerTests, TexturePayloadChunksAreDecodedIntoTextureAsset)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("texture-build-key");
    const auto artifact = MakeTextureArtifactWithPayload(buildKey, std::byte{7U});
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
    const std::string manifest = ManifestJson(TextureEntryJson("textures/demo/logo", buildKey, artifact));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    const auto handle = manager.Textures().ResolveOrCreate(DeriveAssetId("textures/demo/logo"));
    const auto view = manager.Textures().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->asset->width, 2U);
    EXPECT_EQ(view->asset->height, 2U);
    EXPECT_EQ(view->asset->usage, TextureUsage::BaseColor);
    EXPECT_EQ(view->asset->colorSpace, TextureColorSpace::Srgb);
    EXPECT_EQ(view->asset->pixelFormat, TexturePixelFormat::Rgba8Unorm);
    EXPECT_EQ(view->asset->mipCount, 1U);
    ASSERT_EQ(view->asset->mips.size(), 1U);
    EXPECT_EQ(view->asset->mips[0].rowPitch, 8U);
    EXPECT_EQ(view->asset->mips[0].byteSize, 16U);
    EXPECT_EQ(view->asset->pixels.size(), 16U);
    EXPECT_EQ(view->asset->pixels[0], std::byte{7U});
}

TEST(AssetManagerTests, TexturePayloadReloadReplacesDataAndBumpsRevision)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("texture-build-key");
    const auto firstArtifact = MakeTextureArtifactWithPayload(buildKey, std::byte{7U});
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-mesh-0.memesh");
    ASSERT_TRUE(WriteFileBytes(artifactPath, firstArtifact));
    const std::string firstManifest = ManifestJson(TextureEntryJson("textures/demo/logo", buildKey, firstArtifact));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(firstManifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();
    const auto handle = manager.Textures().ResolveOrCreate(DeriveAssetId("textures/demo/logo"));

    // 内容变化（tag 7→9）→ revision+1，TextureAsset payload 整体替换。
    const auto secondArtifact = MakeTextureArtifactWithPayload(buildKey, std::byte{9U});
    ASSERT_TRUE(WriteFileBytes(artifactPath, secondArtifact));
    const std::string secondManifest = ManifestJson(TextureEntryJson("textures/demo/logo", buildKey, secondArtifact));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(secondManifest)));

    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    const auto view = manager.Textures().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 2U);
    EXPECT_EQ(view->asset->pixels[0], std::byte{9U});
}

// ---------- .memat（INFO/TEXR）真实 payload 构造与解码 ----------

// 与 Cooker MaterialArtifactWriter 同 schema（v1）：header(chunkCount=2, flags=1) +
// INFO@64/TEXR@96 描述符 + INFO 数据@128(48B) + TEXR 数据@176(5×20B)。
// INFO：baseColor(4f) emissive(3f) metallic/roughness/normalScale/occlusion(4f)
// alphaMode(u32)；TEXR：5 条 (usage u32 + AssetId 16B)，usage 固定次序 1..5，
// AssetId 全零 = 未使用贴图位（渲染端绑定 fallback SRV）。
std::vector<std::byte> MakeMaterialArtifact(const Sha256Digest& buildKey, const float metallic, const float roughness)
{
    constexpr std::size_t kInfoDescriptor = kBakedHeaderSize;                       // 64
    constexpr std::size_t kTexrDescriptor = kInfoDescriptor + kChunkDescriptorSize; // 96
    constexpr std::size_t kInfoData = kTexrDescriptor + kChunkDescriptorSize;       // 128
    constexpr std::size_t kInfoSize = 48;
    const std::size_t texrData = (kInfoData + kInfoSize + kChunkAlignment - 1) & ~(kChunkAlignment - 1); // 176
    constexpr std::size_t kTexrSize = 5 * 20;          // 5 条 (usage+AssetId)
    const std::size_t fileSize = texrData + kTexrSize; // 276

    std::vector<std::byte> bytes(fileSize, std::byte{0U});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendBytes16(bytes, 4, kBakedFormatVersion);
    AppendBytes16(bytes, 6, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::Material));
    AppendBytes32(bytes, 8, static_cast<std::uint32_t>(kBakedHeaderSize));
    AppendBytes32(bytes, 12, 2);
    AppendBytes64(bytes, 16, static_cast<std::uint64_t>(fileSize));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    AppendBytes32(bytes, 56, kMaterialFormatVersion); // flags：.memat v1（DecodeMaterialChunks 校验入口）

    const auto writeType = [&bytes](const std::size_t offset, const char* type)
    {
        bytes[offset] = static_cast<std::byte>(type[0]);
        bytes[offset + 1] = static_cast<std::byte>(type[1]);
        bytes[offset + 2] = static_cast<std::byte>(type[2]);
        bytes[offset + 3] = static_cast<std::byte>(type[3]);
    };
    const auto putFloat = [&bytes](const std::size_t offset, const float value)
    {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        AppendBytes32(bytes, offset, bits);
    };

    writeType(kInfoDescriptor, "INFO");
    AppendBytes32(bytes, kInfoDescriptor + 4, 0);
    AppendBytes64(bytes, kInfoDescriptor + 8, static_cast<std::uint64_t>(kInfoData));
    AppendBytes64(bytes, kInfoDescriptor + 16, kInfoSize);
    AppendBytes32(bytes, kInfoDescriptor + 24, 12);
    AppendBytes32(bytes, kInfoDescriptor + 28, 4);

    writeType(kTexrDescriptor, "TEXR");
    AppendBytes32(bytes, kTexrDescriptor + 4, 0);
    AppendBytes64(bytes, kTexrDescriptor + 8, static_cast<std::uint64_t>(texrData));
    AppendBytes64(bytes, kTexrDescriptor + 16, kTexrSize);
    AppendBytes32(bytes, kTexrDescriptor + 24, 5);
    AppendBytes32(bytes, kTexrDescriptor + 28, 20);

    // INFO：baseColor=(1,1,1,1)、emissive=0、因子入参、normalScale/occlusion=1、alphaMode=0。
    putFloat(kInfoData, 1.0F);
    putFloat(kInfoData + 4, 1.0F);
    putFloat(kInfoData + 8, 1.0F);
    putFloat(kInfoData + 12, 1.0F);
    putFloat(kInfoData + 16, 0.0F);
    putFloat(kInfoData + 20, 0.0F);
    putFloat(kInfoData + 24, 0.0F);
    putFloat(kInfoData + 28, metallic);
    putFloat(kInfoData + 32, roughness);
    putFloat(kInfoData + 36, 1.0F);
    putFloat(kInfoData + 40, 1.0F);
    AppendBytes32(bytes, kInfoData + 44, 0);

    // TEXR：usage 1..5 固定次序 + 全零 AssetId。
    for (std::size_t record = 0; record < 5; ++record)
    {
        AppendBytes32(bytes, texrData + record * 20U, static_cast<std::uint32_t>(record + 1U));
    }
    return bytes;
}

std::string MaterialEntryJson(const std::string& uri, const Sha256Digest& buildKey,
                              const std::vector<std::byte>& artifact)
{
    return "{\"artifactHash\":\"" + ToHexDigest(Sha256(artifact)) + "\",\"artifactPath\":\"cache/aa/" +
           ToHexDigest(buildKey) + "-material-0.memat\",\"assetUri\":\"" + uri + "\",\"buildKey\":\"" +
           ToHexDigest(buildKey) + "\",\"fileSize\":" + std::to_string(artifact.size()) + ",\"kind\":\"material\"}";
}

// M4-04：Rgba16Float（HdrEnvironment）的运行时解码——审查发现的测试缺口：
// rowPitch 按 pixelFormat 分派的修复（width*8）此前只有真实 HDRI 端到端覆盖，
// 本用例把解码端锁进单测（构造 2×1 RGBA16F .metex → decode → 数值断言）。
std::vector<std::byte> MakeHdrTextureArtifact(const Sha256Digest& buildKey)
{
    // 2×1 RGBA16F：像素 0 = (1.0, 2.0, 0.5, 1.0)、像素 1 = (0.25, 1.0, 4.0, 1.0)
    // （IEEE binary16 位型：1.0=0x3C00、2.0=0x4000、0.5=0x3800、0.25=0x3400、4.0=0x4400）。
    const std::array<std::uint16_t, 8> halfPixels{0x3C00U, 0x4000U, 0x3800U, 0x3C00U,
                                                  0x3400U, 0x3C00U, 0x4400U, 0x3C00U};

    constexpr std::size_t kInfoDescriptor = kBakedHeaderSize;                                            // 64
    constexpr std::size_t kMipsDescriptor = kInfoDescriptor + kChunkDescriptorSize;                      // 96
    constexpr std::size_t kDataDescriptor = kMipsDescriptor + kChunkDescriptorSize;                      // 128
    constexpr std::size_t kInfoData = kDataDescriptor + kChunkDescriptorSize;                            // 160
    constexpr std::size_t kInfoSize = 28;                                                                // 7 × u32
    const std::size_t mipsData = (kInfoData + kInfoSize + kChunkAlignment - 1) & ~(kChunkAlignment - 1); // 192
    constexpr std::size_t kMipsSize = 12;
    const std::size_t dataOffset = (mipsData + kMipsSize + kChunkAlignment - 1) & ~(kChunkAlignment - 1); // 208
    constexpr std::size_t kDataSize = 32;                                                                 // 2×1×8B

    std::vector<std::byte> bytes(dataOffset + kDataSize, std::byte{0U});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendBytes16(bytes, 4, kBakedFormatVersion);
    AppendBytes16(bytes, 6, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::Texture));
    AppendBytes32(bytes, 8, static_cast<std::uint32_t>(kBakedHeaderSize));
    AppendBytes32(bytes, 12, 3);
    AppendBytes64(bytes, 16, static_cast<std::uint64_t>(bytes.size()));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    AppendBytes32(bytes, 56, kTextureFormatVersion);

    const auto writeType = [&bytes](const std::size_t offset, const char* type)
    {
        bytes[offset] = static_cast<std::byte>(type[0]);
        bytes[offset + 1] = static_cast<std::byte>(type[1]);
        bytes[offset + 2] = static_cast<std::byte>(type[2]);
        bytes[offset + 3] = static_cast<std::byte>(type[3]);
    };
    writeType(kInfoDescriptor, "INFO");
    AppendBytes32(bytes, kInfoDescriptor + 4, 0);
    AppendBytes64(bytes, kInfoDescriptor + 8, static_cast<std::uint64_t>(kInfoData));
    AppendBytes64(bytes, kInfoDescriptor + 16, kInfoSize);
    AppendBytes32(bytes, kInfoDescriptor + 24, 7);
    AppendBytes32(bytes, kInfoDescriptor + 28, 4);

    writeType(kMipsDescriptor, "MIPS");
    AppendBytes32(bytes, kMipsDescriptor + 4, 0);
    AppendBytes64(bytes, kMipsDescriptor + 8, static_cast<std::uint64_t>(mipsData));
    AppendBytes64(bytes, kMipsDescriptor + 16, kMipsSize);
    AppendBytes32(bytes, kMipsDescriptor + 24, 1);
    AppendBytes32(bytes, kMipsDescriptor + 28, 12);

    writeType(kDataDescriptor, "DATA");
    AppendBytes32(bytes, kDataDescriptor + 4, 0);
    AppendBytes64(bytes, kDataDescriptor + 8, static_cast<std::uint64_t>(dataOffset));
    AppendBytes64(bytes, kDataDescriptor + 16, kDataSize);
    AppendBytes32(bytes, kDataDescriptor + 24, 1);  // height（单行）
    AppendBytes32(bytes, kDataDescriptor + 28, 16); // rowPitch = width*8

    // INFO v2：width | height | pixelFormat(Rgba16Float=2) | colorSpace(Linear=0) |
    // usage(HdrEnvironment=6) | mipCount | mip0RowPitch(16)。
    AppendBytes32(bytes, kInfoData, 2);
    AppendBytes32(bytes, kInfoData + 4, 1);
    AppendBytes32(bytes, kInfoData + 8, 2);
    AppendBytes32(bytes, kInfoData + 12, 0);
    AppendBytes32(bytes, kInfoData + 16, 6);
    AppendBytes32(bytes, kInfoData + 20, 1);
    AppendBytes32(bytes, kInfoData + 24, 16);

    // MIPS 单级：offset 0 / rowPitch 16 / byteSize 32。
    AppendBytes32(bytes, mipsData, 0);
    AppendBytes32(bytes, mipsData + 4, 16);
    AppendBytes32(bytes, mipsData + 8, 32);

    // DATA：半精度像素流（字节序小端直接拷贝）。
    std::memcpy(bytes.data() + dataOffset, halfPixels.data(), halfPixels.size() * sizeof(std::uint16_t));
    return bytes;
}

std::string HdrTextureEntryJson(const Sha256Digest& buildKey, const std::vector<std::byte>& artifact)
{
    return "{\"artifactHash\":\"" + ToHexDigest(Sha256(artifact)) + "\",\"artifactPath\":\"cache/aa/" +
           ToHexDigest(buildKey) + "-environment.metex\",\"assetUri\":\"asset://environments/m4-baseline\"," +
           "\"buildKey\":\"" + ToHexDigest(buildKey) + "\",\"fileSize\":" + std::to_string(artifact.size()) +
           ",\"kind\":\"texture\"}";
}

// M4-04：Rgba16Float 运行时解码（rowPitch 按 pixelFormat 分派的回归测试）。
TEST(AssetManagerTests, Rgba16FloatTextureChunksAreDecoded)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("environment-build-key");
    const auto artifact = MakeHdrTextureArtifact(buildKey);
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-environment.metex");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
    const std::string manifest = ManifestJson(HdrTextureEntryJson(buildKey, artifact));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    const auto handle = manager.Textures().ResolveOrCreate(DeriveAssetId("asset://environments/m4-baseline"));
    const auto view = manager.Textures().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->asset->width, 2U);
    EXPECT_EQ(view->asset->height, 1U);
    EXPECT_EQ(view->asset->pixelFormat, MiniEngine::Assets::TexturePixelFormat::Rgba16Float);
    EXPECT_EQ(view->asset->colorSpace, MiniEngine::Assets::TextureColorSpace::Linear);
    EXPECT_EQ(view->asset->usage, MiniEngine::Assets::TextureUsage::HdrEnvironment);
    EXPECT_EQ(view->asset->mipCount, 1U);
    ASSERT_EQ(view->asset->mips.size(), 1U);
    EXPECT_EQ(view->asset->mips[0].rowPitch, 16U); // width*8（分派修复的回归锚点）
    EXPECT_EQ(view->asset->mips[0].byteSize, 32U);
    // 半精度载荷按字节透传（runtime 不做 float 转换）。
    ASSERT_EQ(view->asset->pixels.size(), 32U);
    EXPECT_EQ(view->asset->pixels[0], std::byte{0x00U}); // 0x3C00 小端低字节
    EXPECT_EQ(view->asset->pixels[1], std::byte{0x3CU}); // 高字节
}

// M4-02：材质 chunk 解码（因子域 + alphaMode + TEXR usage 次序）。
TEST(AssetManagerTests, MaterialChunksAreDecodedIntoMaterialAsset)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("material-build-key");
    const auto artifact = MakeMaterialArtifact(buildKey, 0.25F, 0.75F);
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-material-0.memat");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
    const std::string manifest = ManifestJson(MaterialEntryJson("materials/demo/sphere", buildKey, artifact));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    ASSERT_TRUE(manager.PrepareManifestLoad(dir / "manifest.json", error)) << error;
    manager.CommitPending();

    const auto handle = manager.Materials().ResolveOrCreate(DeriveAssetId("materials/demo/sphere"));
    const auto view = manager.Materials().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_FLOAT_EQ(view->asset->metallicFactor, 0.25F);
    EXPECT_FLOAT_EQ(view->asset->roughnessFactor, 0.75F);
    EXPECT_FLOAT_EQ(view->asset->baseColorFactor[3], 1.0F);
    EXPECT_EQ(view->asset->alphaMode, AlphaMode::Opaque);
    EXPECT_FALSE(view->asset->baseColorTexture.IsValid()); // 全零 AssetId = 未使用贴图位
    EXPECT_FALSE(view->asset->normalTexture.IsValid());
}

// 负向：因子越界（glTF core 各分量必须在 [0,1]）→ Prepare 拒绝，不落池。
TEST(AssetManagerTests, MaterialFactorOutOfRangeIsRejected)
{
    const auto dir = MakeTempAssetDir();
    const Sha256Digest buildKey = HashText("material-build-key");
    const auto artifact = MakeMaterialArtifact(buildKey, 1.5F, 0.5F); // metallic 越界
    const auto artifactPath = dir / "cache" / "aa" / (ToHexDigest(buildKey) + "-material-0.memat");
    ASSERT_TRUE(WriteFileBytes(artifactPath, artifact));
    const std::string manifest = ManifestJson(MaterialEntryJson("materials/demo/sphere", buildKey, artifact));
    ASSERT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

    AssetManager manager;
    std::string error;
    EXPECT_FALSE(manager.PrepareManifestLoad(dir / "manifest.json", error));
    EXPECT_NE(error.find("outside [0,1]"), std::string::npos) << error;
}

std::string MeshEntryJson(const std::string& uri, const std::byte payloadTag)
{
    const Sha256Digest buildKey = HashText(uri);
    const auto artifact = MakeMeshArtifact(buildKey, payloadTag);
    return EntryForArtifact(uri, Sha256(artifact), buildKey);
}

LoadedMeshes LoadMeshes(const std::vector<std::pair<std::string, std::byte>>& specs)
{
    LoadedMeshes loaded;
    loaded.dir = MakeTempAssetDir();
    std::vector<std::string> entries;
    for (const auto& [uri, tag] : specs)
    {
        WriteMeshAsset(loaded.dir, uri, tag);
        const Sha256Digest buildKey = HashText(uri);
        const auto artifact = MakeMeshArtifact(buildKey, tag);
        entries.push_back(EntryForArtifact(uri, Sha256(artifact), buildKey));
        loaded.assets.emplace_back(uri, buildKey);
    }
    std::string manifest = "{\"assets\":[";
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        if (index != 0)
        {
            manifest += ",";
        }
        manifest += entries[index];
    }
    manifest += "],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
    // 非 void 函数里不能用 ASSERT_*（其 return; 只在 void 函数合法），用 EXPECT_*。
    EXPECT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    EXPECT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();
    return loaded;
}

AssetHandle<MeshAsset> MeshHandle(AssetManager& manager, const std::string& uri)
{
    return manager.Meshes().ResolveOrCreate(DeriveAssetId(uri));
}

TEST(AssetManagerTests, SemanticallyEqualManifestDoesNotTouchSlots)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto handle = MeshHandle(loaded.manager, "meshes/demo/box");

    // 同语义、不同字节（profile 改写）：diff 判 Unchanged → slot/revision 不动。
    const std::string variantManifest = "{\"assets\":[" + MeshEntryJson("meshes/demo/box", std::byte{7U}) +
                                        "],\"profile\":\"Windows-D3D11\",\"schemaVersion\":1}";
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(variantManifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    ASSERT_TRUE(loaded.manager.HasPendingCommit());
    loaded.manager.CommitPending();

    EXPECT_TRUE(loaded.manager.LastCommit().added.empty());
    EXPECT_TRUE(loaded.manager.LastCommit().changed.empty());
    EXPECT_TRUE(loaded.manager.LastCommit().removed.empty());
    EXPECT_EQ(loaded.manager.GetStats().assetsCommitted, 1U); // 初始那次，未新增
    const auto view = loaded.manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 1U);
}

TEST(AssetManagerTests, ChangedArtifactReloadsOnlyChangedEntry)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto handle = MeshHandle(loaded.manager, "meshes/demo/box");

    // 内容变化（tag 7→8），URI/buildKey 不变 → Changed。
    WriteMeshAsset(loaded.dir, "meshes/demo/box", std::byte{8U});
    const std::string manifest = ManifestJson(MeshEntryJson("meshes/demo/box", std::byte{8U}));
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();

    const auto& report = loaded.manager.LastCommit();
    EXPECT_TRUE(report.added.empty());
    ASSERT_EQ(report.changed.size(), 1U);
    EXPECT_EQ(report.changed[0], DeriveAssetId("meshes/demo/box"));
    EXPECT_TRUE(report.removed.empty());

    const auto view = loaded.manager.Meshes().TryGet(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->revision, 2U);
}

TEST(AssetManagerTests, RemovedEntryUnloadsSlot)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto handle = MeshHandle(loaded.manager, "meshes/demo/box");
    ASSERT_TRUE(loaded.manager.Meshes().TryGet(handle).has_value());

    // Manifest 移除该 entry → CommitPending 只登记 Removed，ApplyPendingRemovals 才 Unload。
    const std::string emptyManifest = "{\"assets\":[],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(emptyManifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();
    loaded.manager.ApplyPendingRemovals();

    ASSERT_EQ(loaded.manager.LastCommit().removed.size(), 1U);
    EXPECT_EQ(loaded.manager.LastCommit().removed[0], DeriveAssetId("meshes/demo/box"));
    EXPECT_FALSE(loaded.manager.Meshes().TryGet(handle).has_value());
    EXPECT_EQ(loaded.manager.GetStats().assetsRemoved, 1U);
}

// P2-1：Prepare 每次都是原子动作——失败必须清空任何陈旧 pending，
// 否则残留旧 pending（含旧 removal）会在下一次 no-op/错误路径被误 Commit。
TEST(AssetManagerTests, PrepareFailureClearsStalePending)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto handle = MeshHandle(loaded.manager, "meshes/demo/box");

    // 建立"未 Commit 的 removal pending"：空 manifest 移除 mesh。
    const std::string emptyManifest = "{\"assets\":[],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(emptyManifest)));
    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    ASSERT_TRUE(loaded.manager.HasPendingCommit());

    // 下一次 Prepare 失败（非法 JSON）→ 陈旧 pending 必须被清空，池保持不变。
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes("{ this is not json")));
    EXPECT_FALSE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error));
    EXPECT_FALSE(loaded.manager.HasPendingCommit());
    EXPECT_TRUE(loaded.manager.Meshes().TryGet(handle).has_value());
    EXPECT_EQ(loaded.manager.GetStats().assetsRemoved, 0U);
}

// P1-1：CommitPending 不立即卸载 Removed——延迟到 ApplyPendingRemovals，
// 保证 whole-world 重建过渡期旧 Handle 仍有效（last-known-good）。
TEST(AssetManagerTests, RemovedEntryIsDeferredUntilApply)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto handle = MeshHandle(loaded.manager, "meshes/demo/box");

    const std::string emptyManifest = "{\"assets\":[],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(emptyManifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();

    // Commit 后、Apply 前：旧 Handle 必须仍然可读（World 重建失败时旧画面依赖它）。
    ASSERT_EQ(loaded.manager.LastCommit().removed.size(), 1U);
    EXPECT_TRUE(loaded.manager.Meshes().TryGet(handle).has_value());
    EXPECT_EQ(loaded.manager.GetStats().assetsRemoved, 0U);

    loaded.manager.ApplyPendingRemovals();
    EXPECT_FALSE(loaded.manager.Meshes().TryGet(handle).has_value());
    EXPECT_EQ(loaded.manager.GetStats().assetsRemoved, 1U);
}

TEST(AssetManagerTests, KindChangeForSameUriIsRejected)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/thing", std::byte{7U}}});

    // 同 URI 改 kind → diff 阶段拒绝（不读 artifact、不动 active 状态）。
    const Sha256Digest buildKey = HashText("meshes/demo/thing");
    const std::string manifest = "{\"assets\":[{\"artifactHash\":\"" + std::string(64, 'a') +
                                 "\",\"artifactPath\":\"cache/aa/x.metex\",\"assetUri\":\"meshes/demo/thing\"" +
                                 ",\"buildKey\":\"" + ToHexDigest(buildKey) +
                                 "\",\"fileSize\":1,\"kind\":\"texture\"}" +
                                 "],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    EXPECT_FALSE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error));
    EXPECT_NE(error.find("kind changed"), std::string::npos) << error;
    EXPECT_FALSE(loaded.manager.HasPendingCommit());
}

TEST(AssetManagerTests, AddedEntryStartsAtRevisionOneWhileUnchangedStays)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto boxHandle = MeshHandle(loaded.manager, "meshes/demo/box");

    WriteMeshAsset(loaded.dir, "meshes/demo/sphere", std::byte{9U});
    const std::string manifest = ManifestJson(MeshEntryJson("meshes/demo/box", std::byte{7U}) + "," +
                                              MeshEntryJson("meshes/demo/sphere", std::byte{9U}));
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();

    ASSERT_EQ(loaded.manager.LastCommit().added.size(), 1U);
    EXPECT_EQ(loaded.manager.LastCommit().added[0], DeriveAssetId("meshes/demo/sphere"));
    EXPECT_TRUE(loaded.manager.LastCommit().changed.empty());

    // Unchanged 的 box revision 不动；新 sphere 从 1 开始。
    EXPECT_EQ(loaded.manager.Meshes().TryGet(boxHandle)->revision, 1U);
    const auto sphereHandle = MeshHandle(loaded.manager, "meshes/demo/sphere");
    EXPECT_EQ(loaded.manager.Meshes().TryGet(sphereHandle)->revision, 1U);
}

TEST(AssetManagerTests, CommitReportOrderIsDeterministic)
{
    LoadedMeshes loaded = LoadMeshes(
        {{"meshes/demo/b", std::byte{7U}}, {"meshes/demo/a", std::byte{7U}}, {"meshes/demo/c", std::byte{7U}}});

    // 三个 entry 全部 Changed（tag 7→8）。
    WriteMeshAsset(loaded.dir, "meshes/demo/a", std::byte{8U});
    WriteMeshAsset(loaded.dir, "meshes/demo/b", std::byte{8U});
    WriteMeshAsset(loaded.dir, "meshes/demo/c", std::byte{8U});
    const std::string manifest = ManifestJson(MeshEntryJson("meshes/demo/b", std::byte{8U}) + "," +
                                              MeshEntryJson("meshes/demo/a", std::byte{8U}) + "," +
                                              MeshEntryJson("meshes/demo/c", std::byte{8U}));
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();

    const auto& report = loaded.manager.LastCommit();
    ASSERT_EQ(report.changed.size(), 3U);
    for (std::size_t index = 1; index < report.changed.size(); ++index)
    {
        EXPECT_LT(report.changed[index - 1].bytes, report.changed[index].bytes); // AssetId 字节序
    }
}

// 审计 3.1 根治：BeginCommit 先建槽，使"全新 AssetId 且 world 引用它"的 whole-world reload
// 在 FinishCommit 前即可解析（WorldLoader TryFind 不再失败）；payload 写入留在 FinishCommit。
TEST(AssetManagerTests, BeginCommitMakesAddedIdsResolvableBeforeFinish)
{
    LoadedMeshes loaded = LoadMeshes({{"meshes/demo/box", std::byte{7U}}});
    const auto boxHandle = MeshHandle(loaded.manager, "meshes/demo/box");

    WriteMeshAsset(loaded.dir, "meshes/demo/sphere", std::byte{9U});
    const std::string manifest = ManifestJson(MeshEntryJson("meshes/demo/box", std::byte{7U}) + "," +
                                              MeshEntryJson("meshes/demo/sphere", std::byte{9U}));
    ASSERT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    ASSERT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    const auto sphereId = DeriveAssetId("meshes/demo/sphere");
    EXPECT_FALSE(loaded.manager.Meshes().TryFind(sphereId).has_value()); // Begin 前池中无新 id

    loaded.manager.BeginCommit();
    const auto sphereHandle = loaded.manager.Meshes().TryFind(sphereId);
    ASSERT_TRUE(sphereHandle.has_value()); // 新 Handle 立即可解析（world 重建依赖此点）
    EXPECT_FALSE(loaded.manager.Meshes().TryGet(*sphereHandle).has_value()); // payload 尚未写入
    EXPECT_TRUE(loaded.manager.Meshes().TryGet(boxHandle).has_value());      // 既有不受影响

    loaded.manager.FinishCommit();
    loaded.manager.ApplyPendingRemovals();
    ASSERT_TRUE(loaded.manager.Meshes().TryGet(*sphereHandle).has_value());
    EXPECT_EQ(loaded.manager.Meshes().TryGet(*sphereHandle)->revision, 1U);
    EXPECT_EQ(loaded.manager.Meshes().TryGet(boxHandle)->revision, 1U);
}
