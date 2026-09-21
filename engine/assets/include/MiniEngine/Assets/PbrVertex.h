// ============================================================================
// PbrVertex.h — M4 `.memesh` v2 的 48 字节 PBR 顶点（磁盘契约与 input layout 唯一真源）
// 里程碑：M4（02 篇 Material 资产与 metallic-roughness PBR）
// 职责：定义 PBR 顶点的内存布局与 mesh 格式版本常量。position/normal/tangent/uv0
//       逐字段 little-endian 序列化（写入端按 wire 偏移显式写出，禁止整结构体
//       memcpy 依赖编译器对齐）；tangent.w 是 bitangent 手性符号（-1/+1）。
// 关联：docs/architecture/README.md「PBR Vertex」
//       tools/asset_cooker/src/MeshArtifactWriter.cpp（写入端）
//       engine/assets/src/AssetManager.cpp（DecodeMeshChunks 读取端，校验 stride=48）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace MiniEngine::Assets
{
// M4 PBR 顶点。字段顺序与偏移由 static_assert 锁定：
//   position @0（12B）、normal @12（12B）、tangent @24（16B）、uv0 @40（8B）。
// tangent.xyz 为单位切线，tangent.w ∈ {-1, +1}：B = cross(N, T) * tangent.w，
// 镜像/负缩放由 WorldHandedness 在 shader 侧二次修正（与 raster state 是两回事）。
struct PbrVertex final
{
    float position[3];
    float normal[3];
    float tangent[4]; // xyz = 单位切线，w = bitangent 手性符号。
    float uv0[2];
};

// 守护契约：48 字节是 `.memesh` v2 的磁盘 stride 与 D3D11 input layout 的共同真源，
// 任何字段增删都必须同时更新 Reader/Writer/input layout 并重新烘焙缓存。
static_assert(sizeof(PbrVertex) == 48);
static_assert(offsetof(PbrVertex, position) == 0);
static_assert(offsetof(PbrVertex, normal) == 12);
static_assert(offsetof(PbrVertex, tangent) == 24);
static_assert(offsetof(PbrVertex, uv0) == 40);

// `.memesh` 的格式版本：M3 为 1（32 字节 EngineVertex），M4 为 2（本结构）。
// Reader 遇到低版本输出 "unsupported mesh formatVersion=1; delete derived cache
// and recook with M4"，不做静默猜测或原地迁移（02 篇「格式迁移策略」）。
inline constexpr std::uint32_t kMeshFormatVersion = 2;
} // namespace MiniEngine::Assets
