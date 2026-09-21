// ============================================================================
// CanonicalRecipeTests.cpp — Recipe 确定性重编码测试
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 4、8 节
// 风格：与 tests/tools/RecipeJsonTests.cpp 一致（用 ParseRecipeText 作为输入源）
// ============================================================================
#include "CanonicalRecipe.h"
#include "RecipeJson.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
// 基准 recipe（紧凑书写）。
constexpr std::string_view kCompact =
    R"({"schemaVersion":1,"source":"demo/scene.gltf","assetRoot":"asset://demo/scene","scene":"default","profile":"windows-d3d11","import":{"meshes":true,"textures":true,"world":true,"generateMissingNormals":true},"limits":{"maxFileBytes":67108864,"maxVerticesPerPrimitive":1000000,"maxIndicesPerPrimitive":3000000,"maxTextureDimension":8192,"maxNodes":100000}})";

// 与上面语义完全相同，但缩进/换行/键顺序都不同。
constexpr std::string_view kReformatted = R"({
  "limits" : {
    "maxNodes" : 100000,
    "maxTextureDimension" : 8192,
    "maxIndicesPerPrimitive" : 3000000,
    "maxVerticesPerPrimitive" : 1000000,
    "maxFileBytes" : 67108864
  },
  "import" : {
    "generateMissingNormals" : true,
    "world" : true,
    "textures" : true,
    "meshes" : true
  },
  "profile" : "windows-d3d11",
  "scene" : "default",
  "assetRoot" : "asset://demo/scene",
  "source" : "demo/scene.gltf",
  "schemaVersion" : 1
})";

std::string CanonicalOf(const std::string_view text)
{
    std::string parseError;
    const auto recipe = MiniEngine::Tools::ParseRecipeText(text, parseError);
    EXPECT_TRUE(recipe.has_value()) << parseError;
    if (!recipe.has_value())
    {
        return {};
    }
    const auto canonical = MiniEngine::Tools::EncodeCanonicalRecipe(*recipe);
    EXPECT_TRUE(canonical.has_value());
    return canonical.value_or(std::string{});
}
} // namespace

// 核心契约：只改缩进/键顺序 → canonical bytes 完全相同（不当作变更）。
TEST(CanonicalRecipeTests, WhitespaceAndKeyOrderDoNotChangeCanonicalBytes)
{
    EXPECT_EQ(CanonicalOf(kCompact), CanonicalOf(kReformatted));
}

// 语义值改变 → canonical bytes 必须改变（否则漏失效）。
TEST(CanonicalRecipeTests, SemanticValueChangeChangesCanonicalBytes)
{
    const std::string modified =
        R"({"schemaVersion":1,"source":"demo/scene.gltf","assetRoot":"asset://demo/scene","scene":"default","profile":"windows-d3d12","import":{"meshes":true,"textures":true,"world":true,"generateMissingNormals":true},"limits":{"maxFileBytes":67108864,"maxVerticesPerPrimitive":1000000,"maxIndicesPerPrimitive":3000000,"maxTextureDimension":8192,"maxNodes":100000}})";

    EXPECT_NE(CanonicalOf(kCompact), CanonicalOf(modified));
}

// default 值显式填充后编码：缺省 scene 与显式 "default" 得到相同 bytes。
TEST(CanonicalRecipeTests, DefaultValuesAreEncodedExplicitly)
{
    const std::string withoutScene =
        R"({"schemaVersion":1,"source":"demo/scene.gltf","assetRoot":"asset://demo/scene","profile":"windows-d3d11","import":{"meshes":true,"textures":true,"world":true,"generateMissingNormals":true},"limits":{"maxFileBytes":67108864,"maxVerticesPerPrimitive":1000000,"maxIndicesPerPrimitive":3000000,"maxTextureDimension":8192,"maxNodes":100000}})";

    EXPECT_EQ(CanonicalOf(kCompact), CanonicalOf(withoutScene));
}

// path 规范化：反斜杠与冗余 "./" 必须折叠到同一 canonical 形式。
TEST(CanonicalRecipeTests, PathNormalizationIsStable)
{
    const std::string withBackslash =
        R"({"schemaVersion":1,"source":"demo\\scene.gltf","assetRoot":"asset://demo/scene","scene":"default","profile":"windows-d3d11","import":{"meshes":true,"textures":true,"world":true,"generateMissingNormals":true},"limits":{"maxFileBytes":67108864,"maxVerticesPerPrimitive":1000000,"maxIndicesPerPrimitive":3000000,"maxTextureDimension":8192,"maxNodes":100000}})";

    EXPECT_EQ(CanonicalOf(kCompact), CanonicalOf(withBackslash));
}

// 同一次编码反复执行必须字节一致（确定性）。
TEST(CanonicalRecipeTests, EncodingIsDeterministic)
{
    const std::string first = CanonicalOf(kCompact);
    const std::string second = CanonicalOf(kCompact);
    EXPECT_EQ(first, second);
}

// 输出不含 insignificant whitespace。
TEST(CanonicalRecipeTests, OutputHasNoInsignificantWhitespace)
{
    const std::string canonical = CanonicalOf(kCompact);
    ASSERT_FALSE(canonical.empty());
    // 字符串字面量内部的空格不被计入；这里用"是否存在 ': ' 或 ', '"做粗检。
    EXPECT_EQ(canonical.find(": "), std::string::npos);
    EXPECT_EQ(canonical.find(", "), std::string::npos);
    EXPECT_EQ(canonical.find('\n'), std::string::npos);
}

// 补充用例（落地时补齐）：
// - 未知 key 被 ParseRecipeText 拒绝（需要先给 RecipeJson 加严格模式）；
// - 键排序：断言完整 canonical 字符串等于手写的期望字节序列；
// - 控制字符/转义字符的 round-trip；
// - float 字段（若将来新增）的 round-trip 与 NaN/Inf 拒绝。
