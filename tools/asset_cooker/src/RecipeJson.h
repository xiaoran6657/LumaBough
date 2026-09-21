// ============================================================================
// RecipeJson.h — Recipe 定义与最小 JSON 解析
// 里程碑：M3-04
// 职责：定义 Cooker 的 Recipe 结构（对应 assets/recipes/*.asset.json），提供只支持
//       该子集的递归下降 JSON 解析器，以及 source 路径沙箱（source-root 内规范化）。
//       本文件不引入任何第三方 JSON 依赖，也不承担 glTF 解析。
// 关联：docs/architecture/README.md
//       docs/architecture/README.md
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace MiniEngine::Tools
{
// Recipe 的可选 environment 段（M4-04 篇「HDR source contract」）：独立于 glTF
// source 的 .hdr panorama。assetUri 是完整 asset:// URI（与 assetRoot 无派生关系，
// m4-baseline 模板即如此）。license/sourceUrl/sourceSha256 是资产来源记录的
// 冗余声明（权威记录在 assets/LICENSES.md；此处进入 canonical JSON 参与 BuildKey，
// 使"换了 HDRI 但忘改 recipe"必然失效）。
struct RecipeEnvironment final
{
    std::string source;       // 相对 --source-root 的 .hdr 路径
    std::string assetUri;     // 完整 asset:// URI（如 asset://environments/m4-baseline）
    std::string usage = "HdrEnvironment"; // M4 固定唯一取值
    std::string license;      // 如 "CC0"
    std::string sourceUrl;    // 原始下载 URL（来源记录）
    std::string sourceSha256; // 原始文件哈希（来源记录）
};

// Recipe 对应 assets/recipes/*.asset.json 的展开字段。
// 必填：schemaVersion、source、assetRoot、profile；其余有默认值。
struct Recipe final
{
    std::int32_t schemaVersion{};
    std::string source;                 // 相对 --source-root 的 glTF 路径
    std::string assetRoot;              // asset:// URI，作为 AssetId 派生输入
    std::string scene = "default";      // glTF scene 名
    std::string profile;                // 与 Cooker --profile 匹配
    bool importMeshes{};
    bool importTextures{};
    bool importWorld{};
    bool generateMissingNormals{};
    std::uint64_t maxFileBytes{};
    std::uint32_t maxVerticesPerPrimitive{};
    std::uint32_t maxIndicesPerPrimitive{};
    std::uint32_t maxTextureDimension{};
    std::uint32_t maxNodes{};
    std::optional<RecipeEnvironment> environment; // 存在时烘焙 .metex HdrEnvironment
};

// 解析 recipe JSON 文本。
//
// 参数：
//   jsonText —— 完整的 recipe JSON 文本。
//   error    —— 失败时写入可读原因。
// 返回：成功返回 Recipe；失败返回 nullopt 且 error 非空。
[[nodiscard]] std::optional<Recipe> ParseRecipeText(std::string_view jsonText, std::string& error);

// 读取并解析 recipe 文件（内部调用 ParseRecipeText）。
//
// 参数：
//   recipePath —— recipe JSON 文件路径。
//   error      —— 失败时写入可读原因。
// 返回：成功返回 Recipe；失败返回 nullopt 且 error 非空。
[[nodiscard]] std::optional<Recipe> ParseRecipeFile(const std::filesystem::path& recipePath, std::string& error);

// 把 recipe 的 source 相对路径解析为 source-root 内的规范绝对路径（沙箱）。
// 拒绝绝对路径与任何逃出 source-root 的 ".." 组件；纯词法运算，不访问文件系统。
//
// 参数：
//   sourceRoot —— Cooker --source-root。
//   source     —— Recipe.source 的相对路径。
// 返回：规范路径（source-root 内）；越界或非法时返回 nullopt。
[[nodiscard]] std::optional<std::filesystem::path> ResolveSourceWithinRoot(
    const std::filesystem::path& sourceRoot, const std::filesystem::path& source);
} // namespace MiniEngine::Tools
