// ============================================================================
// Skybox.hlsl — 环境天空盒（D3D12 / SM6 侧；写入 scene-linear HDR target）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；与 M4 同源）
// 职责：用单位立方体把环境 cubemap 铺满背景，输出**线性 radiance**到 HDR target，
//       与场景一起进入 tone map（曝光不在此施加）。三条关键契约：
//   1. 去相机平移的 View-Projection：立方体顶点即方向向量。
//   2. 远深度技巧 `clip.xyww`：ndc.z = 1（远平面），配合 DepthFunc=LESS_EQUAL +
//      DepthWriteMask=ZERO，天空只在 depth==1 的像素通过且不污染深度。
//   3. SampleLevel(..., 0)：固定 LOD，保证截图可复现。
// 与 M4 的关系：逐行同源移植；寄存器改用契约宏（b0 / t0 / s1）。
// 关联：shaders/d3d12/ToneMap.hlsl（下游：同一 HDR target 的曝光与编码）
//       shaders/d3d11/Skybox.hlsl（M4 同源唯一实现）
// ============================================================================
#include "BindingContract.hlsli"

// b0：去平移的 view * projection（已转置的行主序；64B）。
cbuffer SkyboxConstants : register(ME_FRAME_CB_REGISTER)
{
    float4x4 ViewProjectionWithoutTranslation;
};

// t0 = environment cube（RGBA16F）；s1 = 线性 clamp（与 IBL 同槽位语义）。
TextureCube<float4> EnvironmentCube : register(ME_BASE_COLOR_REGISTER);
SamplerState LinearClampSampler : register(ME_IBL_SAMPLER_REGISTER);

struct VertexInput
{
    float3 position : POSITION;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 direction : DIRECTION;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 clip = mul(float4(input.position, 1.0), ViewProjectionWithoutTranslation);
    // z = w：透视除法后深度恒为 1（远平面）。相机后方的顶点会被近平面裁掉，
    // 跨近平面的三角形裁剪后仍覆盖该屏幕区域，故不会出现破洞。
    output.position = clip.xyww;
    // 顶点位置即方向（单位立方体以相机为中心）；PS 内再归一化以抵消插值。
    output.direction = input.position;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    // 输出线性 radiance（可能 >1：HDRI 天顶可达数倍亮度）——这正是"HDR target 中
    // 存在 >1 亮度"的来源之一。
    const float3 radiance = EnvironmentCube.SampleLevel(LinearClampSampler, normalize(input.direction), 0.0).rgb;
    return float4(radiance, 1.0);
}
