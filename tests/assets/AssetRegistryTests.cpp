// ============================================================================
// AssetRegistryTests.cpp — Manifest 严格解析与确定性排序
// 里程碑：M3（7-A）
// 职责：验证 ParseAndValidate 的 fail-closed 行为（未知名/缺名/坏 hex/坏 kind/
//       路径逃逸/尾部数据）与 DeriveAssetId 的域分隔派生、条目按 AssetId 字节序。
// 关联：engine/assets/src/AssetRegistry.cpp（被测实现）
// ============================================================================

#include <MiniEngine/Assets/AssetRegistry.h>

#include <MiniEngine/Assets/Sha256.h>

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

namespace
{
std::vector<std::byte> ToBytes(const std::string& text)
{
    const auto span = std::as_bytes(std::span{text.data(), text.size()});
    return {span.begin(), span.end()};
}

// 与 Cooker BuildManifestJson 的字段集合一致（顺序可以不同，key 集合固定）。
std::string EntryJson(const std::string& uri, const std::string& kind = "mesh")
{
    const std::string hash64(64, 'a');
    const std::string buildKey64(64, 'b');
    std::string json =
        "{\"artifactHash\":\"" + hash64 + "\",\"artifactPath\":\"cache/aa/" + buildKey64 + "-mesh-0.memesh\"";
    json +=
        ",\"assetUri\":\"" + uri + "\",\"buildKey\":\"" + buildKey64 + "\",\"fileSize\":128,\"kind\":\"" + kind + "\"}";
    return json;
}

std::string ManifestJson(const std::string& entriesCsv)
{
    return "{\"assets\":[" + entriesCsv + "],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
}
} // namespace

// 合法 Manifest：解析成功且条目可查。
TEST(AssetRegistryTests, AcceptsValidManifest)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("meshes/demo/box"))), "asset-output", error);
    ASSERT_TRUE(registry) << error;
    EXPECT_EQ(registry->EntryCount(), 1U);

    const auto* entry = registry->Find(MiniEngine::Assets::DeriveAssetId("meshes/demo/box"));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->canonicalUri, "meshes/demo/box");
    EXPECT_EQ(entry->kind, MiniEngine::Assets::AssetKind::Mesh);
    EXPECT_EQ(MiniEngine::Assets::ToHexDigest(entry->buildKey.bytes), std::string(64, 'b'));
}

TEST(AssetRegistryTests, DeriveAssetIdIsDomainSeparated)
{
    // "a/b"+"c" 与 "a"+"b/c" 不得同 ID（'\0' 分隔符防止前缀/后缀歧义）。
    const auto first = MiniEngine::Assets::DeriveAssetId("meshes/demo");
    const auto second = MiniEngine::Assets::DeriveAssetId("meshes/demo2");
    const auto trickyOne = MiniEngine::Assets::DeriveAssetId("a/b");
    const auto trickyTwo = MiniEngine::Assets::DeriveAssetId("a");
    EXPECT_NE(first, second);
    EXPECT_NE(trickyOne, trickyTwo);
}

// schemaVersion 非 1 即拒绝。
TEST(AssetRegistryTests, RejectsUnsupportedSchemaVersion)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes("{\"assets\":[],\"profile\":\"p\",\"schemaVersion\":2}"), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 缺必填 key 即拒绝。
TEST(AssetRegistryTests, RejectsMissingRequiredKeys)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(ToBytes("{}"), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 根对象未知名拒绝。
TEST(AssetRegistryTests, RejectsUnknownRootKey)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes("{\"assets\":[],\"profile\":\"p\",\"schemaVersion\":1,\"extra\":1}"), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 条目未知名拒绝。
TEST(AssetRegistryTests, RejectsUnknownEntryKey)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("meshes/demo/box") + ",{\"unknown\":0}")), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// kind 只接受 mesh/texture/world。
TEST(AssetRegistryTests, RejectsUnknownKind)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("meshes/demo/box", "audio"))), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 哈希字段必须是 64 字符小写 hex。
TEST(AssetRegistryTests, RejectsBadHexDigest)
{
    std::string error;
    const std::string shortHash(63, 'a');
    const std::string buildKey64(64, 'b');
    const std::string json = "{\"artifactHash\":\"" + shortHash + "\",\"artifactPath\":\"cache/aa/" + buildKey64 +
                             "-mesh-0.memesh\",\"assetUri\":\"meshes/demo/box\",\"buildKey\":\"" + buildKey64 +
                             "\",\"fileSize\":128,\"kind\":\"mesh\"}";
    const auto registry =
        MiniEngine::Assets::AssetRegistry::ParseAndValidate(ToBytes(ManifestJson(json)), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 同一 assetUri 重复出现（同 AssetId）即拒绝。
TEST(AssetRegistryTests, RejectsDuplicateAssetUri)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("meshes/demo/box") + "," + EntryJson("meshes/demo/box"))), "asset-output",
        error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// artifactPath 含 ".."（逃出 outputRoot）即拒绝。
TEST(AssetRegistryTests, RejectsPathTraversal)
{
    std::string error;
    const std::string hash64(64, 'a');
    const std::string json = "{\"artifactHash\":\"" + hash64 +
                             "\",\"artifactPath\":\"../escape.memesh\",\"assetUri\":\"meshes/demo/box\"" +
                             ",\"buildKey\":\"" + hash64 + "\",\"fileSize\":128,\"kind\":\"mesh\"}";
    const auto registry =
        MiniEngine::Assets::AssetRegistry::ParseAndValidate(ToBytes(ManifestJson(json)), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// artifactPath 为绝对路径即拒绝（路径沙箱）。
TEST(AssetRegistryTests, RejectsAbsolutePath)
{
    std::string error;
    const std::string hash64(64, 'a');
    const std::string json = "{\"artifactHash\":\"" + hash64 +
                             "\",\"artifactPath\":\"/cache/x.memesh\",\"assetUri\":\"meshes/demo/box\"" +
                             ",\"buildKey\":\"" + hash64 + "\",\"fileSize\":128,\"kind\":\"mesh\"}";
    const auto registry =
        MiniEngine::Assets::AssetRegistry::ParseAndValidate(ToBytes(ManifestJson(json)), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 根对象之后还有数据即拒绝（fail-closed）。
TEST(AssetRegistryTests, RejectsTrailingDataAfterObject)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("meshes/demo/box")) + "garbage"), "asset-output", error);
    EXPECT_FALSE(registry);
    EXPECT_FALSE(error.empty());
}

// 条目按 AssetId 字节序排序：Manifest diff 与事件顺序的确定性基础。
TEST(AssetRegistryTests, EntriesAreSortedByAssetIdBytes)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("zz/late") + "," + EntryJson("aa/early"))), "asset-output", error);
    ASSERT_TRUE(registry) << error;
    ASSERT_EQ(registry->EntryCount(), 2U);
    EXPECT_LT(registry->EntryAt(0)->id.bytes, registry->EntryAt(1)->id.bytes);
    // Find 与 EntryAt 指向同一确定性排序结果。
    EXPECT_EQ(registry->Find(registry->EntryAt(0)->id), registry->EntryAt(0));
}

// 查询不存在的 AssetId 返回 nullptr（而不是未定义行为）。
TEST(AssetRegistryTests, FindUnknownIdReturnsNull)
{
    std::string error;
    const auto registry = MiniEngine::Assets::AssetRegistry::ParseAndValidate(
        ToBytes(ManifestJson(EntryJson("meshes/demo/box"))), "asset-output", error);
    ASSERT_TRUE(registry) << error;
    EXPECT_EQ(registry->Find(MiniEngine::Assets::AssetId{}), nullptr);
    EXPECT_EQ(registry->EntryAt(99), nullptr);
}
