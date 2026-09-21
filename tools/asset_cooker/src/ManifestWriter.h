#pragma once

// ============================================================================
// ManifestWriter.h — manifest.json snapshot 的确定性 JSON 输出
// 里程碑：M3-05（审计 3.1 结构性重构抽取）
// 职责：把 CookedArtifactRecord 列表序列化为 Manifest JSON；字段顺序固定、
//       无时间戳/PID/绝对路径，是双目录确定性比较的前提。
// ============================================================================

#include <string>
#include <vector>

namespace MiniEngine::Tools
{
struct CookedArtifactRecord;

// Manifest snapshot 的确定性 JSON。字段顺序固定（与 ASCII 排序一致），
// 条目按 cook 处理顺序（recipe 文件名已排序）；无时间戳、PID 或绝对路径。
[[nodiscard]] std::string BuildManifestJson(const std::vector<CookedArtifactRecord>& records,
                                            const std::string& profile);

// 返回出现多于一次的 assetUri（按 URI 字节序排序，确定性）。运行期
// `AssetRegistry::ParseAndValidate` 对重复 AssetId 是 fail-closed（duplicate asset
// identity → 整份 manifest 被拒），所以重复 URI 是发布期就必须暴露的冲突：
// M4-09 审查 D2 的裁决是"冲突该报错，不该静默去重"。
[[nodiscard]] std::vector<std::string> FindDuplicateAssetUris(const std::vector<CookedArtifactRecord>& records);
} // namespace MiniEngine::Tools
