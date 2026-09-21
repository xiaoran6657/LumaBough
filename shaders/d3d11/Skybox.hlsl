// ============================================================================
// Skybox.hlsl — M4-05 环境天空盒（写入同一 scene-linear HDR target）
// 里程碑：M4（05 篇「PBR 和 skybox」；手抄清单第 4 条）
// 职责：用单位立方体把环境 cubemap 铺满背景，输出**线性 radiance**到 HDR target，
//       与场景一起进入 tone map（曝光不在此施加——05 篇：exposure 由 tone map 统一
//       应用，skybox 与场景共享同一个曝光值）。三条关键契约：
//   1. 去相机平移的 View-Projection：立方体顶点即方向向量，去掉平移后天空永远以
//      相机为中心，不会随相机移动"穿帮"。
//   2. 远深度技巧 `clip.xyww`：把 z 写成 w → 透视除法后 ndc.z = 1（远平面），
//      配合 CPU 侧的 DepthFunc=LESS_EQUAL + DepthWriteMask=ZERO，天空只在没有
//      几何写过深度（depth==1）的像素通过，且不污染深度缓冲。
//   3. SampleLevel(..., 0) 而非 Sample：skybox 不参与 mip 反馈，固定 LOD 保证
//      截图可复现（04/05 篇同款确定性纪律）。
// 关联：docs/architecture/README.md「PBR 和 skybox」
//       shaders/d3d11/ToneMap.hlsl（下游：同一 HDR target 的曝光与编码）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（pass 3 的 CPU 侧装配）
// ============================================================================

// b0：去平移的 view * projection（已转置的行主序；64B）。
cbuffer SkyboxConstants : register(b0)
{
    float4x4 ViewProjectionWithoutTranslation;
};

// t0 = environment cube（RGBA16F，04 篇生成）；s1 = 线性 clamp（与 IBL 同槽位语义）。
TextureCube<float4> EnvironmentCube : register(t0);
SamplerState LinearClampSampler : register(s1);

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
    // 输出线性 radiance（可能 >1：HDRI 天顶可达数倍亮度）——这正是 05 篇要求
    // 验证"HDR target 中存在 >1 亮度"的来源之一。
    const float3 radiance = EnvironmentCube.SampleLevel(LinearClampSampler, normalize(input.direction), 0.0).rgb;
    return float4(radiance, 1.0);
}
