// ============================================================================
// DependencyRecord.h — 依赖记录、排序与校验
// 里程碑：M3-05
// 职责：定义进入 BuildKey preimage 的依赖条目，以及"严格排序 + 去重冲突"的
//       规范化规则。BuildKey 与反向依赖图都以此为准。
// 关联：docs/architecture/README.md 第 4 节
// ============================================================================
#pragma once

#include "Sha256.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace MiniEngine::Tools
{
enum class DependencyKind : std::uint8_t
{
    Source = 1,        // source 文件（glTF JSON / BIN / PNG / ...）
    Recipe = 2,        // recipe JSON 文件本身
    Tool = 3,          // 工具身份：cooker 版本 / fastgltf commit / WIC policy
    GeneratedRule = 4  // 生成规则：坐标转换策略 / 格式版本
};

struct DependencyRecord final
{
    DependencyKind kind{};
    // Source/Recipe：规范化后的 source-root 相对路径（'/' 分隔符，不含绝对路径）。
    // Tool/GeneratedRule：稳定的 name，例如 "tool/asset-cooker"、"rule/rh-to-lh"。
    std::string normalizedPathOrName;
    // Source/Recipe：文件字节的 SHA-256。
    // Tool/GeneratedRule：name/version 字符串的 SHA-256（内容同样按字节哈希）。
    Sha256Digest contentHash{};
};

// 按 (kind, normalizedPathOrName bytes) 严格排序。
// 排序是 BuildKey 确定性的前提：同一组依赖以不同顺序到达时，
// canonical 排序后必须得到完全相同的 preimage。
// 实现用 std::stable_sort 或直接 sort + 完整比较器，不做任何与
// 容器插入顺序相关的假设。
void SortDependencies(std::vector<DependencyRecord>& records);

// 校验排序后的依赖表：
//   - 重复 (kind, path) 失败；
//   - 大小写别名（同一 canonical path 的不同大小写写法）失败；
//   - 同一 canonical path 出现不同 contentHash 失败。
// 返回 false 时 error 写明冲突条目。
[[nodiscard]] bool ValidateDependencies(const std::vector<DependencyRecord>& records, std::string& error);

// Tool/GeneratedRule 依赖的便捷构造：把 name/version 字符串按字节哈希。
// 例如 MakeToolDependency("tool/fastgltf", "0.9.0")。
[[nodiscard]] DependencyRecord MakeToolDependency(std::string_view name, std::string_view version);

// 依赖 path 的规范化：统一分隔符并词法归一化；用于 Source/Recipe 条目。
[[nodiscard]] std::string NormalizeDependencyPath(std::string_view path);
}  // namespace MiniEngine::Tools
