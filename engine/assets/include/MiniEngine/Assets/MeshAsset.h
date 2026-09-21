// ============================================================================
// MeshAsset.h — 网格资产的 CPU 载荷
// 里程碑：M3
// 职责：定义运行时持有的网格数据（顶点 / 索引字节流 + 数量与步长），它是
//       AssetPool<MeshAsset> 的 payload，也是 D3D11AssetCache 创建 GPU 顶点缓冲与
//       索引缓冲的唯一输入。刻意不含任何 D3D / 平台类型——Assets 不反向依赖 RHI。
// 关联：docs/architecture/DECISIONS.md §1、§5
//       engine/rhi/d3d11/src/D3D11AssetCache.cpp（按 stride 建输入布局并上传）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniEngine::Assets
{
// 7-A 阶段的 CPU mesh payload。Cooker 目前只产 header-only 最小 .memesh
// （M3-05），VERT/INDX chunk 解码在 glTF 导入篇落地后由 AssetManager 填充。
// 不含 D3D/平台类型；GPU 资源由 rhi/d3d11 目标创建。
struct MeshAsset final
{
    // 顶点字节流：Cooker 的 EngineVertex（position[3] + normal[3] + uv[2]，32 字节）
    // 数组的原样拷贝，可直接上传为 GPU 顶点缓冲。
    std::vector<std::byte> vertexData;
    // 索引字节流：Cooker 侧统一为 32 位索引。
    std::vector<std::byte> indexData;
    // 顶点个数（不是字节数）；与 vertexData.size() / vertexStride 一致。
    std::uint32_t vertexCount{};
    // 索引个数（不是字节数）；三角形列表语义——Cooker 的几何校验阶段拒绝非 3 倍数。
    std::uint32_t indexCount{};
    // 单个顶点的字节步长；当前 Cooker 固定 32（sizeof(EngineVertex)），输入布局据此建立。
    std::uint32_t vertexStride{};
    // 当前 Cooker 恒写 4；渲染端固定按 DXGI_FORMAT_R32_UINT 绑定，不读本字段做分支。
    std::uint32_t indexStride{}; // 2（uint16 索引）或 4（uint32 索引）
};
} // namespace MiniEngine::Assets
