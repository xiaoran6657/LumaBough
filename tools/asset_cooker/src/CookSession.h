// ============================================================================
// CookSession.h — Cooker 命令会话：阶段状态机与日志
// 里程碑：M3-04
// 职责：承载 validate / cook / inspect / watch 四个命令的执行逻辑，为每个命令
//       维护"阶段"状态机并按契约输出日志（每条含 stage、recipe、Asset URI）。
//       本文件只依赖 RecipeJson、Sha256 与 MiniEngine::Assets 的 BakedFormat/Reader
//       （后者仅用于 PublishArtifact 的 kind 参数类型），不涉及 glTF 解析。
// 关联：docs/architecture/README.md
//       docs/architecture/README.md
// ============================================================================
#pragma once

#include "RecipeJson.h"
#include "Sha256.h"

#include <MiniEngine/Assets/BakedFormat.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// Cooker 单条命令的解析结果；由 main 填充后传给 CookSession::Run。
struct CookCommandLine final
{
    std::string command;                    // validate / cook / inspect / watch
    std::filesystem::path sourceRoot;       // --source-root
    std::filesystem::path recipe;           // validate 的 --recipe
    std::filesystem::path recipeRoot;       // cook / watch 的 --recipe-root
    std::filesystem::path report;           // validate 的 --report
    std::filesystem::path outputRoot;       // cook / watch 的 --output
    std::filesystem::path artifact;         // inspect 的 --artifact
    std::filesystem::path validator;        // 可配置的 gltf_validator 可执行文件
    std::string profile;                    // --profile
    bool deterministic{};                   // cook 的 --deterministic
};

// 单个 recipe cook 成功后的一条 artifact 记录，用于最后发布 Manifest。
// 全部字段来自确定性输入（BuildKey/ArtifactHash/相对路径），
// 不含时间戳、PID 或绝对路径——这是双目录确定性比较的前提。
struct CookedArtifactRecord final
{
    std::string assetUri;        // recipe.assetRoot
    std::string kind;            // "mesh" / "texture" / "world"
    Sha256Digest buildKey{};
    Sha256Digest artifactHash{};
    std::uint64_t fileSize{};
    std::string artifactPath;    // 相对 outputRoot 的 '/' 分隔路径
};

// Cooker 阶段状态机；每个阶段会写一条日志。
enum class CookStage
{
    Discovered,        // 命令已识别、参数已解析
    ParseRecipe,       // 正在解析 Recipe
    ResolveSource,     // source 沙箱路径解析
    ValidateSource,    // source 文件存在性检查
    KhronosValidate,   // 调用 gltf-validator
    Import,            // glTF 导入（M3-05 接线）
    Serialize,         // artifact 序列化到 staging（M3-05 接线）
    ReopenVerify,      // 用 runtime Reader 重开验证（M3-05 接线）
    PublishArtifacts,  // 原子发布 artifact（M3-05 接线）
    PublishManifest,   // 最后发布 Manifest（M3-05 接线）
};

// 执行 Cooker 单条命令。命令体按阶段状态机推进，退出码遵循 M3-03 CLI 契约：
// 0 成功 / 2 CLI-Recipe / 3 validator-source / 4 importer / 5 serialization /
// 6 publish-IO / 7 internal invariant。
class CookSession final
{
  public:
    explicit CookSession(CookCommandLine commandLine);

    CookSession(const CookSession&) = delete;
    CookSession& operator=(const CookSession&) = delete;

    // 分发到对应命令，返回进程退出码。
    [[nodiscard]] int Run();

  private:
    // 记录当前阶段并输出一条结构化日志：stage、recipe、Asset URI、消息。
    void LogStage(CookStage stage, std::string_view recipe, std::string_view assetUri, std::string_view message) const;

    [[nodiscard]] int RunValidate();
    [[nodiscard]] int RunCook();
    [[nodiscard]] int RunInspect();
    [[nodiscard]] int RunWatch();

    // 校验并返回 --validator 指向的可执行文件路径；缺失或不可用返回 nullopt。
    [[nodiscard]] std::optional<std::filesystem::path> ResolveValidator() const;

    // 对单个 source 调用 gltf-validator，stdout 捕获写入 report 文件。
    // 返回 0 表示 validator 报告无 error；非 0 表示存在 error 或调用失败。
    [[nodiscard]] int RunKhronosValidator(const std::filesystem::path& source,
                                          const std::filesystem::path& report) const;

    // 对单个 recipe 执行：Import（依赖收集/glTF 导入）→ Serialize/Bake 三阶段。
    // 成功的 artifact 记录追加进 collectedArtifacts，供 PublishManifest 汇总。
    // 返回 0 成功；非 0 为 CLI 契约退出码。
    [[nodiscard]] int CookRecipe(const std::filesystem::path& recipePath, const Recipe& recipe,
                                 const std::filesystem::path& resolvedSource,
                                 std::vector<CookedArtifactRecord>& collectedArtifacts);

    // 所有 artifact 发布成功后，最后发布 manifest.json snapshot（replace-existing）。
    // Manifest 是唯一允许替换的文件；无记录时保持旧文件不动（不发布空 snapshot）。
    [[nodiscard]] int PublishManifest(const std::vector<CookedArtifactRecord>& records) const;

    // ---- 结构性重构（审计 3.1）：CookRecipe 487 行按 Import/Serialize/Bake 拆分 ----
    // RecipeCookScope 是本类的私有嵌套类型，完整定义仅在 CookSession.cpp（本文件只
    // 前向声明）；持有 Import 阶段产物与日志上下文，供 CookMesh/CookTexture/BakeWorld
    // 三个子阶段消费。拆分只改组织，不改变日志顺序、BuildKey 计算顺序与发布顺序
    //（Manifest 条目确定性不变）。
    struct RecipeCookScope;

    // 每个 primitive 一个 .memesh（Serialize）。
    [[nodiscard]] int CookMesh(const RecipeCookScope& scope);
    // 每个 (image, usage) 一个 .metex v2（Serialize + WIC decode + 角色化 mip）。
    [[nodiscard]] int CookTexture(const RecipeCookScope& scope);
    // 每个 glTF material（+ 无材质 primitive 的 default）一个 .memat v1（Serialize）。
    [[nodiscard]] int CookMaterials(const RecipeCookScope& scope);
    // recipe.environment 段（M4-04）：.hdr → .metex HdrEnvironment（RGBA16F/Linear/
    // 单 mip；stb 解码 + 数值校验；无 environment 段时 no-op）。
    [[nodiscard]] int CookEnvironment(const RecipeCookScope& scope);
    // 场景图 → .meworld v2（MSHR 引用 mesh + material AssetId）。
    [[nodiscard]] int BakeWorld(const RecipeCookScope& scope);
    // 通用 artifact 发布：cache 命中判定 → miss 走 staging → ReopenVerify → 发布，
    // 并把成功记录追加进 scope.collectedArtifacts。mesh/texture/world 共用。
    [[nodiscard]] int PublishArtifact(const RecipeCookScope& scope, const std::string& publishUri,
                                      const std::string& kindText, MiniEngine::Assets::BakedAssetKind bakedKind,
                                      const std::string& extension, const Sha256Digest& buildKey,
                                      const std::vector<std::byte>& bytes, const std::string& fileNamePrefix);

    CookCommandLine m_commandLine;
};
} // namespace MiniEngine::Tools
