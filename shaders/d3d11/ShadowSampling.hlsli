// ============================================================================
// ShadowSampling.hlsli — 方向光 shadow map 采样（03 篇「Shadow coordinates / PCF」）
// 里程碑：M4（03 篇 Directional Shadow）
// 职责：t8 shadow map（R32_FLOAT SRV）+ s2 comparison sampler 的 3×3 PCF 采样。
//       契约：D3D clip depth 是 [0,1]（不套用 OpenGL [-1,1] remap）；UV 做 Y flip
//      （uv.y = -ndc.y*0.5+0.5）；shadow volume 外 / lightClip.w<=0 按完全受光
//       处理（border=1 的同款语义）；PCF kernel 与 texel size 固定，无随机旋转。
//       单出口写法：FXC 在 /Od（SKIP_OPTIMIZATION）+ /WX 下对提前 return 有 X4000
//       误报史（M4-02 事故），守卫全部用正向嵌套表达。
// 关联：docs/architecture/README.md「Shadow coordinates」「Receiver policy」
//       engine/rhi/d3d11/src/D3D11ShadowMap.cpp（t8/s2 资源与采样器创建）
//       shaders/d3d11/PbrForward.hlsl（消费方；direct light 乘 factor）
// ============================================================================
#ifndef MINIENGINE_SHADOW_SAMPLING_HLSLI
#define MINIENGINE_SHADOW_SAMPLING_HLSLI

Texture2D<float> ShadowMap : register(t8);
SamplerComparisonState ShadowSampler : register(s2);

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
