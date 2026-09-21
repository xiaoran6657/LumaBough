// ============================================================================
// RecipeJsonTests.cpp — Recipe 解析与 source 沙箱的契约
// 里程碑：M3-04 / M3-05
// 职责：验证合法 Recipe 解析、必填项与未知 key 的 fail-closed、解析深度上限，
//       以及 ResolveSourceWithinRoot 的沙箱规则（嵌套相对路径、.." 逃逸、
//       绝对路径、兄弟前缀目录）。
// 关联：tools/asset_cooker/src/RecipeJson.cpp（被测实现）
// ============================================================================

#include "RecipeJson.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
constexpr std::string_view kValidRecipe =
    R"({
  "schemaVersion": 1,
  "source": "demo/scene.gltf",
  "assetRoot": "asset://demo/scene",
  "scene": "default",
  "profile": "windows-d3d11",
  "import": {
    "meshes": true,
    "textures": true,
    "world": true,
    "generateMissingNormals": true
  },
  "limits": {
    "maxFileBytes": 67108864,
    "maxVerticesPerPrimitive": 1000000,
    "maxIndicesPerPrimitive": 3000000,
    "maxTextureDimension": 8192,
    "maxNodes": 100000
  }
})";
} // namespace

// 合法 recipe 全字段解析成功，且缺省字段填充为文档默认值。
TEST(RecipeJsonTests, ParsesValidRecipe)
{
    std::string error;
    const auto recipe = MiniEngine::Tools::ParseRecipeText(kValidRecipe, error);
    ASSERT_TRUE(recipe.has_value());
    EXPECT_EQ(recipe->schemaVersion, 1);
    EXPECT_EQ(recipe->source, "demo/scene.gltf");
    EXPECT_EQ(recipe->assetRoot, "asset://demo/scene");
    EXPECT_EQ(recipe->scene, "default");
    EXPECT_EQ(recipe->profile, "windows-d3d11");
    EXPECT_TRUE(recipe->importMeshes);
    EXPECT_TRUE(recipe->importTextures);
    EXPECT_TRUE(recipe->importWorld);
    EXPECT_TRUE(recipe->generateMissingNormals);
    EXPECT_EQ(recipe->maxFileBytes, 67108864U);
    EXPECT_EQ(recipe->maxVerticesPerPrimitive, 1000000U);
    EXPECT_EQ(recipe->maxIndicesPerPrimitive, 3000000U);
    EXPECT_EQ(recipe->maxTextureDimension, 8192U);
    EXPECT_EQ(recipe->maxNodes, 100000U);
}

// 必填 key 缺失即失败（fail-closed，不猜默认值）。
TEST(RecipeJsonTests, MissingSchemaVersionFails)
{
    const std::string_view text = R"({ "source": "a.gltf", "assetRoot": "asset://a", "profile": "p" })";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
    EXPECT_FALSE(error.empty());
}

// schemaVersion 不是 1 即拒绝，不做跨版本猜测。
TEST(RecipeJsonTests, UnsupportedSchemaVersionFails)
{
    const std::string_view text =
        R"({ "schemaVersion": 2, "source": "a.gltf", "assetRoot": "asset://a", "profile": "p" })";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
}

// 必填 key 缺失即失败（source）。
TEST(RecipeJsonTests, MissingSourceFails)
{
    const std::string_view text = R"({ "schemaVersion": 1, "assetRoot": "asset://a", "profile": "p" })";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
}

// 必填 key 缺失即失败（assetRoot）。
TEST(RecipeJsonTests, MissingAssetRootFails)
{
    const std::string_view text = R"({ "schemaVersion": 1, "source": "a.gltf", "profile": "p" })";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
}

// 语法错误（截断/坏字面量）即失败，错误信息可读。
TEST(RecipeJsonTests, MalformedJsonFails)
{
    const std::string_view text = R"({ "schemaVersion": 1, "source": "a.gltf", )";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
    EXPECT_FALSE(error.empty());
}

// 根必须是对象；数组或标量即失败。
TEST(RecipeJsonTests, RootMustBeObject)
{
    const std::string_view text = R"([1, 2, 3])";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
}

// 沙箱：嵌套相对路径解析到 source-root 内的规范绝对路径。
TEST(RecipeJsonTests, ResolveSourceWithinRootAcceptsNestedRelativePath)
{
    const auto resolved = MiniEngine::Tools::ResolveSourceWithinRoot(std::filesystem::path{"C:/assets/source"},
                                                                     std::filesystem::path{"demo/scene.gltf"});
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->generic_string(), "C:/assets/source/demo/scene.gltf");
}

// 沙箱：".." 逃出 source-root 即拒绝。
TEST(RecipeJsonTests, ResolveSourceWithinRootRejectsTraversal)
{
    EXPECT_FALSE(MiniEngine::Tools::ResolveSourceWithinRoot(std::filesystem::path{"C:/assets/source"},
                                                            std::filesystem::path{"../secret.gltf"})
                     .has_value());
    EXPECT_FALSE(MiniEngine::Tools::ResolveSourceWithinRoot(std::filesystem::path{"C:/assets/source"},
                                                            std::filesystem::path{"demo/../../secret.gltf"})
                     .has_value());
}

// 沙箱：绝对路径一律拒绝（不与 source-root 拼接判断）。
TEST(RecipeJsonTests, ResolveSourceWithinRootRejectsAbsolutePath)
{
    EXPECT_FALSE(MiniEngine::Tools::ResolveSourceWithinRoot(std::filesystem::path{"C:/assets/source"},
                                                            std::filesystem::path{"C:/outside.gltf"})
                     .has_value());
}

// M3-02 §5 第 4 条：不能用字符串前缀把 source2 误判为 source 的子目录。
// 实现用 lexically_relative 组件比较（非字符串前缀），此用例锁定该契约。
// 沙箱：按路径组件比较包含关系，"root2" 不会被误判为 "root" 的子路径。
TEST(RecipeJsonTests, ResolveSourceWithinRootDoesNotConfuseSiblingPrefix)
{
    const auto resolved = MiniEngine::Tools::ResolveSourceWithinRoot(std::filesystem::path{"C:/assets/source"},
                                                                     std::filesystem::path{"source2/demo.gltf"});
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->generic_string(), "C:/assets/source/source2/demo.gltf");
}

// 未知 key 严格拒绝：这是 CanonicalRecipe"未知字段必失效"承诺的前提。
TEST(RecipeJsonTests, UnknownKeyFails)
{
    const std::string_view text = R"({ "schemaVersion": 1, "source": "a.gltf",
        "assetRoot": "asset://a", "profile": "p", "unknownField": 123 })";
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(text, error).has_value());
    EXPECT_NE(error.find("unknownField"), std::string::npos);
}

std::string DeeplyNestedRecipe(const int depth)
{
    // 根对象满足必需字段；limits.maxNodes 的值是 depth 层嵌套数组包着 0。
    std::string text = R"({ "schemaVersion": 1, "source": "a.gltf", "assetRoot": "asset://a", "profile": "p",
        "limits": { "maxNodes": )";
    text.append(static_cast<std::size_t>(depth), '[');
    text.push_back('0');
    text.append(static_cast<std::size_t>(depth), ']');
    text.append(" } }");
    return text;
}

// 二轮审查：递归下降必须有嵌套深度上限（容器总深度=根对象+limits+嵌套数组）。
// 根对象(1)+limits(1)+62 层数组 = 64 层 → 合法应通过（off-by-one 检查）。
// 嵌套深度上限 64：防御恶意深嵌套输入导致的栈溢出。
TEST(RecipeJsonTests, MaxDepthAllowedIsSixtyFour)
{
    std::string error;
    EXPECT_TRUE(MiniEngine::Tools::ParseRecipeText(DeeplyNestedRecipe(62), error).has_value()) << error;
}

// 63 层数组 → 总深度 65 > 64 → 应返回可读错误而非栈溢出。
TEST(RecipeJsonTests, NestingBeyondSixtyFourFailsInsteadOfStackOverflow)
{
    std::string error;
    EXPECT_FALSE(MiniEngine::Tools::ParseRecipeText(DeeplyNestedRecipe(63), error).has_value());
    EXPECT_NE(error.find("nesting exceeds"), std::string::npos);
}
