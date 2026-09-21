// ============================================================================
// CanonicalRecipe.h — Recipe 的确定性重编码
// 里程碑：M3-05
// 职责：把已解析的 Recipe 结构体重编码为确定性 bytes，供 BuildKey preimage 使用。
//       编码输入是 Recipe 结构体字段，不是 source JSON 原字节。
// 关联：docs/architecture/README.md 第 4 节
// ============================================================================
#pragma once

#include "RecipeJson.h"

#include <optional>
#include <string>

namespace MiniEngine::Tools
{
// 把 Recipe 重编码为确定性 JSON bytes（UTF-8 无 BOM，无 insignificant whitespace）。
//
// 规则（M3-05 第 4 节）：
//   - keys 按 ASCII byte order（实现层对每层对象先排序再输出，天然稳定）；
//   - 无 insignificant whitespace（紧凑输出）；
//   - integer 十进制最短格式；
//   - float 使用明确 round-trip 格式，禁止 NaN/Inf
//     （当前 Recipe 字段无 float；新增 float 字段时必须先定义编码规则，
//      禁止直接依赖 iostream 的默认格式化）；
//   - default 值显式填充后编码（scene 的 "default" 也要写进去）；
//   - path 先规范化（统一 '/' 分隔符 + lexically_normal）；
//   - unknown key 失败（由 ParseRecipeText 严格模式保证，见下方注意事项）；
//   - array 保持语义顺序；allowlist issue codes 排序并去重（当前 Recipe 无此字段）。
//
// 为什么不用 source JSON 原字节：只改缩进/键顺序会造成无意义重建；
// 而忽略 default/unknown keys 会造成该重建时不重建。两端都要避免。
//
// 注意事项：本函数依赖"未知字段已被解析器拒绝"。当前 RecipeJson.cpp 的
// GetOptionalBool/GetOptionalNumber 对未知字段静默忽略，落地时必须补严格模式
// （未知 key → ParseRecipeText 返回 nullopt），否则 unknown key 无法进入 BuildKey。
[[nodiscard]] std::optional<std::string> EncodeCanonicalRecipe(const Recipe& recipe);
}  // namespace MiniEngine::Tools
