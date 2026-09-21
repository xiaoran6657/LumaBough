// ============================================================================
// DirectionalLight.h — M4 固定方向光（唯一光源）与固定 shadow volume 契约
// 里程碑：M4（01 篇架构边界与渲染顺序）
// 职责：定义 World 层的方向光数据：颜色/亮度因子与显式固定的光空间正交
//       shadow volume。World 只描述数据，不做 fit、不建 GPU 资源；
//       light view-projection 的数学在 RenderQueueBuilder::BuildLightViewProjection。
// 关联：docs/architecture/README.md（数据边界、Pass 契约）
//       engine/world/src/RenderQueueBuilder.cpp（光视锥构建的消费方）
// ============================================================================

#pragma once

#include <array>

namespace MiniEngine::World
{
// M4 的唯一光源：一盏方向光 + 单张固定 shadow map（01 篇"明确不做"：无 point/spot、
// 无 CSM）。光照方向与 shadow volume 都是固定值，保证截图基线可复现。
struct DirectionalLight final
{
    // 着色点指向光源的单位方向（着色公式中的 L）。
    // 默认值 (0.408, 0.817, 0.408) 为归一化的 (1, 2, 1)，来自场景上方偏前。
    std::array<float, 3> directionToLight{0.408248F, 0.816497F, 0.408248F};
    // 光的线性空间 RGB 颜色（不是 sRGB；PBR 计算全程线性）。
    std::array<float, 3> colorLinear{1.0F, 1.0F, 1.0F};
    // 亮度缩放因子：与 colorLinear 相乘后进入 PBR 的 radiance。
    float illuminanceScale = 3.0F;

    // 显式固定的光空间正交盒（shadow volume），单位为世界距离。
    // 刻意不逐帧 fit 到相机视锥：fit 会让 shadow map 内容随相机抖动，
    // 直接破坏截图基线与 GPU timing 的可复现性（ADR-0005 决策 3）。
    // left/right/bottom/top 是光空间横向范围；near/far 是沿光前向的深度段。
    float shadowLeft = -20.0F;
    float shadowRight = 20.0F;
    float shadowBottom = -20.0F;
    float shadowTop = 20.0F;
    float shadowNear = 0.1F;
    float shadowFar = 60.0F;
};
} // namespace MiniEngine::World
