// ============================================================================
// AssetCache.h — 内容寻址 Cache 布局、命中判定与反向依赖图
// 里程碑：M3-05
// 职责：把 BuildKey/ArtifactHash 映射为不可变的 Cache 路径；按 6 条条件判定
//       cache hit；维护"依赖 → asset"的反向图以支持选择性失效。
// 关联：docs/architecture/README.md 第 5、6、7 节
// ============================================================================
#pragma once

#include "BuildKey.h"
#include "DependencyRecord.h"
#include "Sha256.h"

#include <MiniEngine/Assets/BakedFormat.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MiniEngine::Tools
{
// ---------------------------------------------------------------------------
// Cache 布局
//
//   <output>/cache/
//   └── ab/
//       └── <64-hex-build-key>/
//           ├── <64-hex-artifact-hash>-mesh-0-primitive-0.memesh
//           ├── <64-hex-artifact-hash>-image-0.metex
//           └── <64-hex-artifact-hash>-world-default.meworld
//
//   - 两位 shard 来自 BuildKey 前两 hex（避免单目录文件过多）；
//   - 目录名只由 BuildKey 构成，不含 source 绝对路径；
//   - artifact 文件名由完整 ArtifactHash + 可读稳定 suffix 构成；
//   - Artifact 发布后不可变，只有 Manifest snapshot 可替换。
// ---------------------------------------------------------------------------

// BuildKey → <output>/cache/<2-hex>/<64-hex>/
[[nodiscard]] std::filesystem::path BuildCacheDirectory(const std::filesystem::path& outputRoot, const Sha256Digest& buildKey);

// ArtifactHash + 稳定 suffix + 扩展名 → cache 目录下的不可变文件名。
// stableSuffix 必须稳定可读（如 "mesh-0-primitive-0"），不得包含绝对路径、
// 时间戳或 PID，否则两次 clean cook 不会产生相同树。
[[nodiscard]] std::filesystem::path BuildArtifactPath(const std::filesystem::path& cacheDirectory, const Sha256Digest& artifactHash, const std::string& stableSuffix, const std::string& extension);

// ---------------------------------------------------------------------------
// Cache hit 判定（第 5 节）。任一失败都视为 miss/corruption 并重建，
// 不把"文件存在"当命中。
// ---------------------------------------------------------------------------
struct CacheHitInput final
{
    std::filesystem::path artifactPath;
    Sha256Digest expectedBuildKey{};      // 本次算出的 BuildKey
    Sha256Digest expectedArtifactHash{};  // Manifest 记录的 artifact SHA-256
    MiniEngine::Assets::BakedAssetKind kind{};
};

struct CacheHitResult final
{
    bool hit{};
    std::string reason;  // miss/损坏原因；命中时为空。日志必须带上它以便解释失效。
};

// 按顺序检查：
//   1 期望 BuildKey 已算出（调用方保证）；
//   2 Manifest/目标路径命中（路径由 BuildKey+ArtifactHash 决定）；
//   3 文件存在且为普通文件（拒绝目录/reparse point/symlink）；
//   4 runtime Reader 通过（BakedReader::Parse，含 header 结构校验）；
//   5 header BuildKey 等于期望（通过 expectation.buildKey 校验）；
//   6 artifact SHA-256 等于 Manifest artifactHash。
[[nodiscard]] CacheHitResult EvaluateCacheHit(const CacheHitInput& input);

// 读取整个文件（仅用于已通过 budget 检查的 artifact；artifact 有明确大小上限）。
[[nodiscard]] std::optional<std::string> ReadArtifactBytes(const std::filesystem::path& path, std::string& error);

// ---------------------------------------------------------------------------
// 反向依赖图（第 6 节）
//
//   Dependency path/hash → AssetId(s)
//   AssetId → DependencyRecord(s)
//
// 使用 std::map 而非 unordered_map：遍历顺序必须确定，
// 否则两次 clean cook 可能产出不同的遍历/写入顺序（determinism 红线）。
// ---------------------------------------------------------------------------
class ReverseDependencyGraph final
{
  public:
    // 登记一个资产与其依赖表（内部会先排序）；重复登记同一 assetUri 会整体覆盖。
    // 同时维护 path → owners 映射，owners 保持去重与有序。
    void AddAsset(const std::string& assetUri, std::vector<DependencyRecord> dependencies);

    // 返回依赖路径（先按 NormalizeDependencyPath 归一化再查找）指向的全部资产；
    // 输出按字节序排列，保证同一输入得到同一顺序（determinism）。
    [[nodiscard]] std::vector<std::string> FindAffectedAssets(std::string_view changedPath) const;

    // 返回全部已登记的 assetUri，按 std::map 键序（即确定性顺序）输出。
    [[nodiscard]] std::vector<std::string> GetAllAssets() const;

  private:
    std::map<std::string, std::vector<DependencyRecord>> m_assetToDependencies;
    std::map<std::string, std::vector<std::string>> m_pathToAssets;
};
}  // namespace MiniEngine::Tools
