// ============================================================================
// BuildKeyTests.cpp — BuildKey preimage 语义测试
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 3、8 节
// 风格：与 tests/tools/Sha256BuilderTests.cpp 一致（裸 TEST + EXPECT/ASSERT）
// ============================================================================
#include "BuildKey.h"
#include "DependencyRecord.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace
{
MiniEngine::Tools::DependencyRecord MakeSourceDep(std::string path, const unsigned char seed)
{
    MiniEngine::Tools::DependencyRecord record;
    record.kind = MiniEngine::Tools::DependencyKind::Source;
    record.normalizedPathOrName = std::move(path);
    record.contentHash.fill(std::byte{seed});
    return record;
}

MiniEngine::Tools::Sha256Digest KeyOf(const MiniEngine::Tools::BuildKeyInputs& inputs)
{
    MiniEngine::Tools::Sha256Digest digest{};
    std::string error;
    EXPECT_TRUE(MiniEngine::Tools::ComputeBuildKey(inputs, digest, error)) << error;
    return digest;
}
} // namespace

// 依赖到达顺序不同，canonical 排序后必须得到同一个 key。
TEST(BuildKeyTests, DependencyOrderIsCanonicalized)
{
    MiniEngine::Tools::BuildKeyInputs a;
    a.profile = "windows-d3d11";
    a.canonicalAssetUri = "asset://demo/scene";
    a.canonicalRecipeBytes = "{}";
    a.dependencies = {MakeSourceDep("demo/Triangle.bin", 1), MakeSourceDep("demo/scene.gltf", 2)};

    MiniEngine::Tools::BuildKeyInputs b = a;
    // 故意以相反顺序给出依赖
    b.dependencies = {MakeSourceDep("demo/scene.gltf", 2), MakeSourceDep("demo/Triangle.bin", 1)};

    EXPECT_EQ(KeyOf(a), KeyOf(b));
}

// 依赖 hash 改变 → key 必须改变（否则漏失效）。
TEST(BuildKeyTests, DependencyHashChangeChangesKey)
{
    MiniEngine::Tools::BuildKeyInputs a;
    a.canonicalAssetUri = "asset://demo/scene";
    a.dependencies = {MakeSourceDep("demo/scene.gltf", 1)};

    MiniEngine::Tools::BuildKeyInputs b = a;
    b.dependencies = {MakeSourceDep("demo/scene.gltf", 2)};

    EXPECT_NE(KeyOf(a), KeyOf(b));
}

// preimage schema 版本改变 → key 改变（域分离）。
TEST(BuildKeyTests, SchemaVersionChangeChangesKey)
{
    MiniEngine::Tools::BuildKeyInputs a;
    a.canonicalAssetUri = "asset://demo/scene";

    MiniEngine::Tools::BuildKeyInputs b = a;
    b.buildKeySchemaVersion = a.buildKeySchemaVersion + 1;

    EXPECT_NE(KeyOf(a), KeyOf(b));
}

// 工具链身份进入 key：Cooker 版本改变必须全量失效。
TEST(BuildKeyTests, CookerVersionChangeChangesKey)
{
    MiniEngine::Tools::BuildKeyInputs a;
    a.canonicalAssetUri = "asset://demo/scene";

    MiniEngine::Tools::BuildKeyInputs b = a;
    b.cookerVersion = "0.2.0-m3";

    EXPECT_NE(KeyOf(a), KeyOf(b));
}

// 格式版本进入 key。
TEST(BuildKeyTests, BakedFormatVersionChangeChangesKey)
{
    MiniEngine::Tools::BuildKeyInputs a;
    a.canonicalAssetUri = "asset://demo/scene";

    MiniEngine::Tools::BuildKeyInputs b = a;
    b.bakedFormatVersion = a.bakedFormatVersion + 1;

    EXPECT_NE(KeyOf(a), KeyOf(b));
}

// length-prefix 反例：profile="ab"+uri="c" 不能与 profile="a"+uri="bc" 混淆。
TEST(BuildKeyTests, AdjacentFieldsDoNotCollapse)
{
    MiniEngine::Tools::BuildKeyInputs a;
    a.profile = "ab";
    a.canonicalAssetUri = "c";

    MiniEngine::Tools::BuildKeyInputs b;
    b.profile = "a";
    b.canonicalAssetUri = "bc";

    EXPECT_NE(KeyOf(a), KeyOf(b));
}

// 依赖表冲突（同一 path 出现两次）必须失败，而不是静默产生一个 key。
TEST(BuildKeyTests, DuplicateDependencyPathFails)
{
    MiniEngine::Tools::BuildKeyInputs inputs;
    inputs.canonicalAssetUri = "asset://demo/scene";
    inputs.dependencies = {MakeSourceDep("demo/scene.gltf", 1), MakeSourceDep("demo/scene.gltf", 1)};

    MiniEngine::Tools::Sha256Digest digest{};
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ComputeBuildKey(inputs, digest, error));
    EXPECT_FALSE(error.empty());
}

// hex 输出：64 位小写，且与实际 digest 字节一致。
TEST(BuildKeyTests, ToHexDigestProduces64LowerCaseChars)
{
    MiniEngine::Tools::Sha256Digest digest{};
    digest.fill(std::byte{0xAB});

    const std::string hex = MiniEngine::Tools::ToHexDigest(digest);
    ASSERT_EQ(hex.size(), 64);

    // 32 个 0xAB 字节 → "ab" 重复 32 次。
    std::string expected;
    for (int i = 0; i < 32; ++i)
    {
        expected += "ab";
    }
    EXPECT_EQ(hex, expected);

    for (const char c : hex)
    {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

// 补充用例（落地时补齐）：
// - Recipe 只改 whitespace → key 相同（跨文件，见 CanonicalRecipeTests）；
// - Recipe 语义值改变 → key 不同；
// - importer name/settings 改变 → key 不同；
// - 依赖数量相同但 kind 不同 → key 不同；
// - 超长字符串（> u32 上限）构造失败路径（若可构造）。
