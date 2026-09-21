// D3D12 迁移：保留 M4 数学、采样次数、寄存器和面方向，离线 DXC 编译。
// ============================================================================
// EquirectToCube.hlsl — equirectangular panorama → environment cube 转换
// 里程碑：M4（04 篇「Equirectangular → Environment Cube」；手抄清单第 1 条）
// 职责：从单位立方体面几何渲染六面，每像素把 face direction 映射回 panorama
//       UV 采样线性 HDR。方向公式是 04 篇冻结契约：u = atan2(x,z)/2π+0.5、
//       v = acos(clamp(y,-1,1))/π——「图像顶边为 +Y、Cooker 不做垂直翻转」。
//       公式必须先经轴标记 fixture 验证（CPU 参考 + RenderDoc 六面判读），
//       不能只看"天空大致正常"。
// 关联：docs/architecture/README.md「Face 方向」「Equirectangular → Environment Cube」
//       engine/rhi/d3d11/src/D3D11IblResources.cpp（FaceViewProjection 与 cube 几何的提供方）
//       tests/rendering/PbrMathTests.cpp（DirectionToEquirectUv 的 CPU 参考）
// ============================================================================
#include "PbrCommon.hlsli"

// b0：当前 face 的 look-at * 90° 透视矩阵（行主序转置，与 PbrForward 同款约定）。
cbuffer CubeFaceConstants : register(b0)
{
    float4x4 FaceViewProjection;
};

// t0：baked RGBA16F panorama（.metex HdrEnvironment 或 procedural fixture）。
Texture2D<float4> EquirectangularPanorama : register(t0);
SamplerState LinearClampSampler : register(s1); // IBL 线性 clamp（s1 契约）

struct VertexInput
{
    float3 position : POSITION; // 单位立方体顶点（±1）——同时是方向向量
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 direction : DIRECTION; // 插值后的面内方向（PS 里 normalize）
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0), FaceViewProjection);
    output.direction = input.position;
    return output;
}

// 方向 → panorama UV（04 篇冻结公式；CPU 参考同式）。
// u 的 atan2(x,z) 使 -Z 方向为 u=0（可被 frac 折回）；v=0 对应顶边（+Y）。
float2 DirectionToEquirectUv(float3 direction)
{
    direction = normalize(direction);
    const float u = atan2(direction.x, direction.z) / (2.0 * PI) + 0.5;
    // 固定 source 契约：图像顶边为 +Y，Cooker 不做垂直翻转——v 用 acos(y)/π
    // 而非 (1-y)/2 翻转；方向标记 fixture 会立即暴露翻转错误。
    const float v = acos(clamp(direction.y, -1.0, 1.0)) / PI;
    return float2(frac(u), v);
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    // 顶层 mip：转换只吃全景图最高分辨率（cube mip 链由后续 pass 生成）。
    return EquirectangularPanorama.SampleLevel(LinearClampSampler, DirectionToEquirectUv(input.direction), 0.0);
}
