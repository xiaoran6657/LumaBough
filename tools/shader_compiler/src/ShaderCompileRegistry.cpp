// ============================================================================
// ShaderCompileRegistry.cpp — 入口清单实现
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移）
// 职责：集中登记 D3D12 侧全部编译入口。顺序即处理顺序；新增 shader 必须先在此
//       登记（包括"两个入口共用一份源码"的情况）。
// 关联：tools/shader_compiler/src/ShaderCompileRegistry.h
// ============================================================================
#include "ShaderCompileRegistry.h"

namespace MiniEngine::ShaderCompiler
{
namespace
{
// D3D12 入口清单（08 篇交付 5 个 pass shader + 1 个 smoke）。
// 说明：`ShadowDepth.hlsl` 只有 VS（depth-only，PSO 的 PS 为空）；其余 pass 成对。
constexpr ShaderEntry kEntries[]{
    // M6-10：逐级迁移使用的固定彩色三角形。
    {"shaders/d3d12/GraphTriangle.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/GraphTriangle.hlsl", "PSMain", "ps_6_0"},
    // M5-09：生成期五个 pass，保留 M4 profile 与确定性 mip 链。
    {"shaders/d3d12/EquirectToCube.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/EquirectToCube.hlsl", "PSMain", "ps_6_0"},
    {"shaders/d3d12/IrradianceConvolution.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/IrradianceConvolution.hlsl", "PSMain", "ps_6_0"},
    {"shaders/d3d12/IrradianceConvolution.hlsl", "PSDownsample", "ps_6_0"},
    {"shaders/d3d12/PrefilterEnvironment.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/PrefilterEnvironment.hlsl", "PSMain", "ps_6_0"},
    {"shaders/d3d12/IntegrateBrdf.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/IntegrateBrdf.hlsl", "PSMain", "ps_6_0"},

    // 工具链闭环（05 篇模板 → 08 篇接入 DXC）
    {"shaders/d3d12/TriangleSmoke.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/TriangleSmoke.hlsl", "PSMain", "ps_6_0"},

    // Forward pass（PBR + shadow + IBL）
    {"shaders/d3d12/PbrForward.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/PbrForward.hlsl", "PSMain", "ps_6_0"},

    // Shadow pass（depth-only）
    {"shaders/d3d12/ShadowDepth.hlsl", "VSMain", "vs_6_0"},

    // Skybox（HDR target，线性 radiance）
    {"shaders/d3d12/Skybox.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/Skybox.hlsl", "PSMain", "ps_6_0"},

    // Tone map（全屏三角形 + sRGB 编码）
    {"shaders/d3d12/ToneMap.hlsl", "VSMain", "vs_6_0"},
    {"shaders/d3d12/ToneMap.hlsl", "PSMain", "ps_6_0"},
};
} // namespace

std::span<const ShaderEntry> Entries() noexcept
{
    return kEntries;
}
} // namespace MiniEngine::ShaderCompiler
