// ============================================================================
// ShadowSampling.hlsli — 方向光 shadow map 采样（D3D12 / SM6 侧）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；与 M4 同源）
// 职责：t8 shadow map（R32_FLOAT SRV）+ s2 comparison sampler 的 3×3 PCF 采样。
//       契约：D3D clip depth 是 [0,1]（不套用 OpenGL [-1,1] remap）；UV 做 Y flip
//      （uv.y = -ndc.y*0.5+0.5）；shadow volume 外 / lightClip.w<=0 按完全受光
//       处理（border=1 的同款语义）；PCF kernel 与 texel size 固定，无随机旋转。
// 与 M4 的关系：逐行同源移植（公式与守卫形状一字不改）；寄存器改用
//       BindingContract.hlsli 的宏，使 M5-D3D12 与 M4-D3D11 的 t8/s2 由同一份
//       契约文字约束（drift 由 BindingContractDriftTests 守住）。
// 关联：shaders/d3d12/PbrForward.hlsl（消费方；direct light 乘 factor）
//       shaders/d3d11/ShadowSampling.hlsli（M4 同源唯一实现）
// ============================================================================
#ifndef MINIENGINE_D3D12_SHADOW_SAMPLING_HLSLI
#define MINIENGINE_D3D12_SHADOW_SAMPLING_HLSLI

#include "BindingContract.hlsli"

Texture2D<float> ShadowMap : register(ME_SHADOW_REGISTER);
SamplerComparisonState ShadowSampler : register(ME_SHADOW_SAMPLER_REGISTER);

// 3×3 comparison PCF。lightClipPosition 是 b1 的 LightWorldViewProjection 输出
//（未透视除法的 clip 坐标）；shadowMapSize 为 shadow map 分辨率（b0 提供）。
float SampleDirectionalShadow(float4 lightClipPosition, float2 shadowMapSize)
{
    // receiver policy：无效坐标 / volume 外一律 factor=1（完全受光）；
    // direct light 之外（IBL/emissive）的项由调用方保证不乘 factor。
    float visibility = 1.0;

    if (lightClipPosition.w > 0.0)
    {
        const float3 ndc = lightClipPosition.xyz / lightClipPosition.w;
        // D3D clip depth [0,1]；UV Y flip（贴图行方向与 NDC y 相反）。
        const float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
        const bool insideVolume = all(uv >= 0.0) && all(uv <= 1.0) && ndc.z >= 0.0 && ndc.z <= 1.0;

        if (insideVolume)
        {
            const float2 texel = 1.0 / shadowMapSize;
            visibility = 0.0;
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    // ComparisonFunc=LESS_EQUAL：depth <= stored → 受光（1）。
                    visibility += ShadowMap.SampleCmpLevelZero(
                        ShadowSampler,
                        uv + float2(float(x), float(y)) * texel,
                        ndc.z);
                }
            }
            visibility /= 9.0;
        }
    }
    return visibility;
}

#endif
