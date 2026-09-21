#pragma once

// ============================================================================
// MeshArtifactWriter.h — .memesh v2（Mesh Baked artifact）写入
// 里程碑：M3-04（v1）→ M4-02（v2：PbrVertex 48B + 格式版本 flags）
// 职责：把导入层产出的 EnginePrimitiveMesh 序列化为 Baked 网格 artifact
//       （header + VERT/INDX 描述符 + 16 对齐数据区），输出字节确定性。
//       v2 的 VERT stride 为 48（PbrVertex：position/normal/tangent/uv0），
//       header.flags = kMeshFormatVersion，供读取端输出 recook diagnostic。
//       与引擎 DecodeMeshChunks 的读取防御对称：elementCount/stride 有界检查，
//       失败返回 false 并给出 error（调用方按 Serialize 退出码 7 处理）。
// 关联：engine/assets/include/MiniEngine/Assets/BakedReader.h（读取端契约）
//       engine/assets/include/MiniEngine/Assets/PbrVertex.h（顶点布局）
// ============================================================================

#include "GltfGeometry.h"
#include "Sha256.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// G4：真实 mesh payload artifact（header + VERT/INDX 描述符 + 16 对齐数据区）。
// wire 布局（与 BakedReader 契约一致）：
//   header 64B（chunkCount=2）→ VERT 描述符@64、INDX 描述符@96 →
//   VERT 数据 @128 → INDX 数据 @Align16(vertEnd)。
// 描述符 32B：type[4]|flags(u32)|offset(u64)|size(u64)|elementCount(u32)|stride(u32)。
// chunk.offset 必须 16 对齐；数据间隙补零保持确定性。空几何也产出合法 chunk
//（count=0），由 runtime Reader/解码端处理。
// 失败（顶点/索引数超 u32）返回 false 且 error 非空，out 不被写入。
[[nodiscard]] bool BuildMeshArtifact(const Sha256Digest& buildKey, const EnginePrimitiveMesh& mesh,
                                     std::vector<std::byte>& out, std::string& error);
} // namespace MiniEngine::Tools
