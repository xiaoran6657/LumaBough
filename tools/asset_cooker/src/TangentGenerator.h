#pragma once

// ============================================================================
// TangentGenerator.h — MikkTSpace 离线切线生成（02 篇 Tangent 规则第 3 优先级）
// 里程碑：M4-02
// 职责：对 BuildRuntimeMesh 判定为 TangentSource::Generated 的 primitive 运行
//       vendored MikkTSpace（tools/asset_cooker/deps/mikktspace，固定 commit），
//       产出 face-corner 切线后按完整 PbrVertex（48 字节）确定性去重并重建
//       indices。关键约束：MikkTSpace 按 face-corner 输出，seam 两侧（UV/镜像）
//       必须是不同顶点——绝不允许把结果写回共享源 index。
// 前置契约（BuildRuntimeMesh 已保证）：
//   - mesh.tangentSource == Generated；
//   - vertices 为 face-corner 展开（vertices.size() == indices.size() 且为 3 的
//     倍数，indices 为顺序三元组 base/1/2）；
//   - 每个顶点有非零 normal 与有效 uv0（缺失 UV 在 G2 已拒绝）。
// 失败语义：任何失败返回 false 且 error 非空，mesh 保持调用前状态（Cooker 按
// Serialize 失败处理，不产出 artifact）。
// 关联：docs/architecture/README.md「Tangent 规则」
//       tools/asset_cooker/deps/mikktspace/README.txt（版本 pin）
// ============================================================================

#include "GltfGeometry.h"

#include <string>

namespace MiniEngine::Tools
{
[[nodiscard]] bool GenerateTangents(EnginePrimitiveMesh& mesh, std::string& error);
} // namespace MiniEngine::Tools
