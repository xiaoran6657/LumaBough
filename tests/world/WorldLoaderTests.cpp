// ============================================================================
// WorldLoaderTests.cpp — .meworld 两遍实例化加载的契约
// 里程碑：M3（whole-world 篇）
// 职责：验证层级/变换/RenderItem 与 Mesh/Texture 句柄链接的正确性，以及
//       all-or-nothing 失败语义（引用未加载资产、parent 乱序、chunk 数量不一致）。
// 关联：engine/world/src/WorldLoader.cpp（被测实现）
//       docs/architecture/DECISIONS.md §7、§8
// ============================================================================

#include <MiniEngine/World/WorldLoader.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Assets/TextureFormatV2.h>
#include <MiniEngine/World/World.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine
{
namespace
{
using Sha256Digest = Assets::Sha256Digest;

std::filesystem::path MakeTempBaseDir()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "MiniEngineWorldLoaderTests";
    std::filesystem::create_directories(root);
    return root;
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
    return Assets::Sha256(std::as_bytes(std::span{text.data(), text.size()}));
}

Sha256Digest HashBytes(const std::vector<std::byte>& bytes)
{
    return Assets::Sha256(bytes);
}

std::vector<std::byte> ToBytes(const std::string& text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char ch : text)
    {
        bytes.push_back(static_cast<std::byte>(ch));
    }
    return bytes;
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

void AppendFloat(std::vector<std::byte>& bytes, const std::size_t offset, const float value)
{
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32(bytes, offset, bits);
}

// 最小合法 mesh artifact（header only）——AssetManager 池有 slot 即可解析 Handle。
// M4-02：header.flags 携带 per-kind 格式版本（DecodeMeshChunks 语义层校验入口）。
std::vector<std::byte> MakeMeshArtifact(const Sha256Digest& buildKey)
{
    std::vector<std::byte> bytes(Assets::kBakedHeaderSize, std::byte{0});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendU16(bytes, 4, Assets::kBakedFormatVersion);
    AppendU16(bytes, 6, static_cast<std::uint16_t>(Assets::BakedAssetKind::Mesh));
    AppendU32(bytes, 8, static_cast<std::uint32_t>(Assets::kBakedHeaderSize));
    AppendU32(bytes, 12, 0);
    AppendU64(bytes, 16, static_cast<std::uint64_t>(bytes.size()));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    AppendU32(bytes, 56, Assets::kMeshFormatVersion);
    return bytes;
}

// 最小合法 material artifact（header only）——pass2 只需池内可解析 Material Handle。
std::vector<std::byte> MakeMaterialArtifact(const Sha256Digest& buildKey)
{
    std::vector<std::byte> bytes(Assets::kBakedHeaderSize, std::byte{0});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendU16(bytes, 4, Assets::kBakedFormatVersion);
    AppendU16(bytes, 6, static_cast<std::uint16_t>(Assets::BakedAssetKind::Material));
    AppendU32(bytes, 8, static_cast<std::uint32_t>(Assets::kBakedHeaderSize));
    AppendU32(bytes, 12, 0);
    AppendU64(bytes, 16, static_cast<std::uint64_t>(bytes.size()));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    AppendU32(bytes, 56, Assets::kMaterialFormatVersion);
    return bytes;
}

// 建立 AssetManager：header-only mesh + material slot（Pool 可解析 Handle，无需真实
// payload）。M4-02：MSHR v2 引用 mesh + material 两种 AssetId，texture 由 `.memat` 承载。
struct LoadedAssets final
{
    std::filesystem::path dir;
    Assets::AssetManager manager;
    Assets::AssetId meshId;
    Assets::AssetId materialId;
    std::string meshUri;
    std::string materialUri;
};

LoadedAssets LoadMeshSlot()
{
    static std::uint64_t sequence = 0;
    LoadedAssets loaded;
    loaded.dir = MakeTempBaseDir() / ("case-" + std::to_string(sequence++));
    std::filesystem::remove_all(loaded.dir);
    loaded.meshUri = "asset://demo/box#mesh/0/primitive/0";
    loaded.materialUri = "asset://demo/box#material/0";
    loaded.meshId = Assets::DeriveAssetId(loaded.meshUri);
    loaded.materialId = Assets::DeriveAssetId(loaded.materialUri);

    std::string manifestEntries;
    const std::array<std::string, 2> uris{loaded.meshUri, loaded.materialUri};
    const std::array<std::string, 2> kinds{"mesh", "material"};
    for (std::size_t index = 0; index < 2; ++index)
    {
        const Sha256Digest buildKey = HashText(uris[index]);
        const auto artifact = index == 0 ? MakeMeshArtifact(buildKey) : MakeMaterialArtifact(buildKey);
        const auto artifactPath = loaded.dir / "cache" / "aa" / (Assets::ToHexDigest(buildKey) + "-mesh-0.memesh");
        EXPECT_TRUE(WriteFileBytes(artifactPath, artifact));
        const std::string artifactHashHex = Assets::ToHexDigest(HashBytes(artifact));
        const std::string buildKeyHex = Assets::ToHexDigest(buildKey);
        manifestEntries += "{\"artifactHash\":\"" + artifactHashHex + "\",\"artifactPath\":\"cache/aa/" + buildKeyHex +
                           "-mesh-0.memesh\",\"assetUri\":\"" + uris[index] + "\",\"buildKey\":\"" + buildKeyHex +
                           "\",\"fileSize\":64,\"kind\":\"" + kinds[index] + "\"}";
        if (index == 0)
        {
            manifestEntries += ",";
        }
    }
    const std::string manifest =
        "{\"assets\":[" + manifestEntries + "],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
    EXPECT_TRUE(WriteFileBytes(loaded.dir / "manifest.json", ToBytes(manifest)));

    std::string error;
    EXPECT_TRUE(loaded.manager.PrepareManifestLoad(loaded.dir / "manifest.json", error)) << error;
    loaded.manager.CommitPending();
    return loaded;
}

// 与 Cooker BuildMeworldArtifact 相同的 wire 布局（schema 锁定契约，v2）。
struct TestMeworldSpec final
{
    std::array<std::string, 2> names{"Root", "Child"};
    std::array<std::int32_t, 2> parents{-1, 0};
    std::array<bool, 2> hasMesh{false, true};
    std::array<bool, 2> hasMaterial{false, true};
    Assets::AssetId meshId{};
    Assets::AssetId materialId{};
    bool corruptParentOrder{};
    bool corruptCounts{}; // 让 MSHR 的实体数与其他 chunk 不一致（writer 一致性回归）
};

std::vector<std::byte> BuildMeworld(const TestMeworldSpec& spec)
{
    constexpr std::size_t kChunkCount = 4;
    constexpr std::uint32_t kNone = 0xFFFFFFFFU;
    constexpr std::size_t kEntityCount = 2;

    std::array<std::array<float, 16>, kEntityCount> locals{};
    for (auto& matrix : locals)
    {
        matrix.fill(0.0F);
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0F;
    }
    locals[1][12] = 5.0F; // child 本地平移 X=5

    // STRS：u32 count + 每条 (u32 len + bytes)
    std::vector<std::byte> strs;
    AppendU32(strs, 0, static_cast<std::uint32_t>(kEntityCount));
    std::array<std::uint32_t, kEntityCount> nameIndices{};
    for (std::size_t index = 0; index < kEntityCount; ++index)
    {
        nameIndices[index] = static_cast<std::uint32_t>(strs.size());
        AppendU32(strs, strs.size(), static_cast<std::uint32_t>(spec.names[index].size()));
        for (const char ch : spec.names[index])
        {
            strs.push_back(static_cast<std::byte>(ch));
        }
    }

    // ENTY
    std::vector<std::byte> enty;
    AppendU32(enty, 0, static_cast<std::uint32_t>(kEntityCount));
    for (std::size_t index = 0; index < kEntityCount; ++index)
    {
        const std::size_t base = 4 + index * 8U;
        std::uint32_t parent = spec.parents[index] < 0 ? kNone : static_cast<std::uint32_t>(spec.parents[index]);
        if (spec.corruptParentOrder && index == 1)
        {
            parent = 5; // parent >= child index
        }
        AppendU32(enty, base, parent);
        AppendU32(enty, base + 4, nameIndices[index]);
    }

    // TRFM（64B/实体，16 float row-major）
    std::vector<std::byte> trfm;
    AppendU32(trfm, 0, static_cast<std::uint32_t>(kEntityCount));
    for (std::size_t index = 0; index < kEntityCount; ++index)
    {
        for (std::size_t component = 0; component < 16; ++component)
        {
            AppendFloat(trfm, 4 + index * 64U + component * 4U, locals[index][component]);
        }
    }

    // MSHR v2（48B/实体）：flags(4) + mesh AssetId(16) + material AssetId(16) + 保留(12)。
    // 材质语义（factors/贴图）全部由 `.memat` 承载，world 只引用身份。
    std::vector<std::byte> mshr;
    AppendU32(mshr, 0, static_cast<std::uint32_t>(spec.corruptCounts ? kEntityCount + 1 : kEntityCount));
    for (std::size_t index = 0; index < kEntityCount; ++index)
    {
        const std::size_t base = 4 + index * 48U;
        if (mshr.size() < base + 48)
        {
            mshr.resize(base + 48, std::byte{0});
        }
        const std::uint32_t flags = (spec.hasMesh[index] ? 1U : 0U) | (spec.hasMaterial[index] ? 2U : 0U);
        AppendU32(mshr, base, flags);
        if (spec.hasMesh[index])
        {
            std::copy(spec.meshId.bytes.begin(), spec.meshId.bytes.end(), mshr.begin() + base + 4);
        }
        if (spec.hasMaterial[index])
        {
            std::copy(spec.materialId.bytes.begin(), spec.materialId.bytes.end(), mshr.begin() + base + 20);
        }
    }

    // header + descriptor table + payloads
    const std::size_t dataStart = Assets::kBakedHeaderSize + kChunkCount * Assets::kChunkDescriptorSize;
    const std::size_t dataStartAligned =
        (dataStart + Assets::kChunkAlignment - 1) & ~static_cast<std::size_t>(Assets::kChunkAlignment - 1);
    const std::array<std::vector<std::byte>*, kChunkCount> payloads{&strs, &enty, &trfm, &mshr};
    std::size_t offsets[kChunkCount]{};
    std::size_t total = dataStartAligned;
    for (std::size_t index = 0; index < kChunkCount; ++index)
    {
        total = (total + Assets::kChunkAlignment - 1) & ~static_cast<std::size_t>(Assets::kChunkAlignment - 1);
        offsets[index] = total;
        total += payloads[index]->size();
    }

    std::vector<std::byte> bytes(total, std::byte{0});
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    AppendU16(bytes, 4, Assets::kBakedFormatVersion);
    AppendU16(bytes, 6, static_cast<std::uint16_t>(Assets::BakedAssetKind::World));
    AppendU32(bytes, 8, static_cast<std::uint32_t>(Assets::kBakedHeaderSize));
    AppendU32(bytes, 12, static_cast<std::uint32_t>(kChunkCount));
    AppendU64(bytes, 16, static_cast<std::uint64_t>(total));
    const Sha256Digest buildKey = HashText("meworld-build-key");
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    AppendU32(bytes, 56, Assets::kWorldFormatVersion); // flags：.meworld v2（loader 语义层校验入口）

    const char* types[kChunkCount] = {"STRS", "ENTY", "TRFM", "MSHR"};
    for (std::size_t index = 0; index < kChunkCount; ++index)
    {
        const std::size_t descriptorBase = Assets::kBakedHeaderSize + index * Assets::kChunkDescriptorSize;
        std::memcpy(bytes.data() + descriptorBase, types[index], 4);
        AppendU32(bytes, descriptorBase + 4, 0);
        AppendU64(bytes, descriptorBase + 8, static_cast<std::uint64_t>(offsets[index]));
        AppendU64(bytes, descriptorBase + 16, static_cast<std::uint64_t>(payloads[index]->size()));
        AppendU32(bytes, descriptorBase + 24, 0);
        AppendU32(bytes, descriptorBase + 28, 0);
        if (payloads[index]->size() > 0)
        {
            std::memcpy(bytes.data() + offsets[index], payloads[index]->data(), payloads[index]->size());
        }
    }
    return bytes;
}

TEST(WorldLoaderTests, BuildsHierarchyTransformsAndRendererTwoPass)
{
    LoadedAssets loaded = LoadMeshSlot();
    TestMeworldSpec spec;
    spec.meshId = loaded.meshId;
    spec.materialId = loaded.materialId;

    const std::vector<std::byte> meworld = BuildMeworld(spec);
    World::WorldLoadResult result;
    std::string error;
    ASSERT_TRUE(World::TryBuildWorldFromArtifact(meworld, loaded.manager, result, error)) << error;
    ASSERT_NE(result.world, nullptr);
    ASSERT_EQ(result.entities.size(), 2U);

    // pass1：名字 + 层级 + 本地矩阵
    const World::NameComponent* rootName = result.world->TryGetName(result.entities[0]);
    ASSERT_NE(rootName, nullptr);
    EXPECT_EQ(rootName->value, "Root");
    const World::NameComponent* childName = result.world->TryGetName(result.entities[1]);
    ASSERT_NE(childName, nullptr);
    EXPECT_EQ(childName->value, "Child");

    // parent-before-child 顺序 = 序列化顺序：entity0 是 root，entity1 是其 child。
    const World::TransformComponent* childTransform = result.world->TryGetTransform(result.entities[1]);
    ASSERT_NE(childTransform, nullptr);
    EXPECT_EQ(childTransform->parent, result.entities[0]);

    // 更新后 child world = root(恒等) * child local(translation X=5)。
    EXPECT_FLOAT_EQ(childTransform->world.values[12], 5.0F);
    EXPECT_FLOAT_EQ(childTransform->world.values[13], 0.0F);
    EXPECT_FLOAT_EQ(childTransform->world.values[14], 0.0F);

    // pass2：MeshRenderer Handle 与 AssetManager 池内一致（v2：mesh + material）。
    const World::MeshRendererComponent* renderer = result.world->TryGetMeshRenderer(result.entities[1]);
    ASSERT_NE(renderer, nullptr);
    const auto expectedHandle = loaded.manager.Meshes().ResolveOrCreate(loaded.meshId);
    const auto expectedMaterialHandle = loaded.manager.Materials().ResolveOrCreate(loaded.materialId);
    EXPECT_EQ(renderer->mesh, expectedHandle);
    EXPECT_EQ(renderer->material, expectedMaterialHandle);
    EXPECT_TRUE(renderer->visible);

    // entity0 无 MeshRenderer。
    EXPECT_EQ(result.world->TryGetMeshRenderer(result.entities[0]), nullptr);

    // BuildRenderItems：只有 child 有 mesh → 1 个 RenderItem，world 矩阵取 child。
    const std::vector<World::RenderItem> items = result.world->BuildRenderItems();
    ASSERT_EQ(items.size(), 1U);
    EXPECT_FLOAT_EQ(items[0].world.values[12], 5.0F);
}

TEST(WorldLoaderTests, MissingAssetReferenceFailsAllOrNothing)
{
    LoadedAssets loaded = LoadMeshSlot();
    TestMeworldSpec spec;
    spec.meshId = Assets::DeriveAssetId("asset://nope/box"); // 池中不存在
    spec.materialId = loaded.materialId;

    const std::vector<std::byte> meworld = BuildMeworld(spec);
    World::WorldLoadResult result;
    std::string error;
    EXPECT_FALSE(World::TryBuildWorldFromArtifact(meworld, loaded.manager, result, error));
    EXPECT_EQ(result.world, nullptr);
    EXPECT_NE(error.find("unloaded mesh asset"), std::string::npos) << error;
}

TEST(WorldLoaderTests, CorruptParentOrderIsRejected)
{
    LoadedAssets loaded = LoadMeshSlot();
    TestMeworldSpec spec;
    spec.meshId = loaded.meshId;
    spec.materialId = loaded.materialId;
    spec.corruptParentOrder = true;

    const std::vector<std::byte> meworld = BuildMeworld(spec);
    World::WorldLoadResult result;
    std::string error;
    EXPECT_FALSE(World::TryBuildWorldFromArtifact(meworld, loaded.manager, result, error));
    EXPECT_EQ(result.world, nullptr);
    EXPECT_NE(error.find("parent must precede child"), std::string::npos) << error;
}

// 审计补强（writer 一致性）：四 chunk 实体数不一致 = writer/序列化 bug，loader 必须拒绝。
TEST(WorldLoaderTests, DisagreeingChunkCountsAreRejected)
{
    LoadedAssets loaded = LoadMeshSlot();
    TestMeworldSpec spec;
    spec.meshId = loaded.meshId;
    spec.materialId = loaded.materialId;
    spec.corruptCounts = true;

    const std::vector<std::byte> meworld = BuildMeworld(spec);
    World::WorldLoadResult result;
    std::string error;
    EXPECT_FALSE(World::TryBuildWorldFromArtifact(meworld, loaded.manager, result, error));
    EXPECT_EQ(result.world, nullptr);
    EXPECT_NE(error.find("chunk counts disagree"), std::string::npos) << error;
}
} // namespace
} // namespace MiniEngine
