// ============================================================================
// ToneMap.hlsl — 后处理：手动曝光 + Reinhard + 显式 Linear→sRGB（D3D12 / SM6 侧）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；与 M4 同源）
// 职责：以 SV_VertexID 全屏三角形采样 HDR scene color（t0），按 exp2(ExposureEv)
//       施加固定曝光、全局 Reinhard 压缩到 [0,1)、再按 IEC sRGB 分段函数显式编码，
//       写入 UNORM 后备缓冲 RTV。三条硬契约（与 M4 逐字一致）：
//   1. **只编码一次**：后备缓冲 RTV 是 `R8G8B8A8_UNORM`（非 `_SRGB` 视图）。
//   2. **顺序固定**：exposure → Reinhard → sRGB，之前不 clamp 亮度。
//   3. **不使用 pow(x, 1/2.2) 近似**：分段 sRGB 在断点处差异可达 1e-2。
// 与 M4 的关系：逐行同源移植；寄存器改用契约宏（b0 / t0 / s1）。
// 关联：shaders/d3d12/FullscreenTriangle.hlsli（VS；UV 生成）
//       shaders/d3d11/ToneMap.hlsl（M4 同源唯一实现）
// ============================================================================
#include "BindingContract.hlsli"
#include "FullscreenTriangle.hlsli"

// b0 与 CPU 侧 PostProcessConstants 逐字节对应（16B）。
cbuffer ToneMapConstants : register(ME_FRAME_CB_REGISTER)
{
    float ExposureEv;
    uint DebugHdr;
    float2 InverseOutputSize;
};

// t0 = HDR scene color（R16G16B16A16_FLOAT）；s1 = 线性 clamp。
Texture2D<float4> HdrScene : register(ME_BASE_COLOR_REGISTER);
SamplerState LinearClampSampler : register(ME_IBL_SAMPLER_REGISTER);

// VS：无顶点缓冲，SV_VertexID 0..2 生成覆盖全屏的超屏三角形。
FullscreenOutput VSMain(uint vertexId : SV_VertexID)
{
    return FullscreenTriangleVS(vertexId);
}

// 全局 Reinhard（冻结公式）：逐通道单调、finite、无 LUT。
float3 ToneMapReinhard(float3 color)
{
    color = max(color, 0.0);
    return color / (1.0 + color);
}

// IEC 61966-2-1 分段 sRGB 传输函数。**不要改成 pow(x, 1/2.2)**。
float3 LinearToSrgb(float3 linearColor)
{
    linearColor = saturate(linearColor);
    const float3 low = 12.92 * linearColor;
    const float3 high = 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(linearColor, 0.0031308));
}

// SceneLuminance debug view：luminance < 1 → 线性灰（不做 tone map/sRGB encode）；
// >= 1 → 洋红饱和标记（任何洋红像素即证明 HDR 中存在 >1 的亮度）。
float3 SceneLuminanceFalseColor(float3 hdrColor)
{
    const float luminance = dot(max(hdrColor, 0.0), float3(0.2126, 0.7152, 0.0722));
    return luminance < 1.0 ? float3(luminance, luminance, luminance) : float3(1.0, 0.0, 1.0);
}

float4 PSMain(FullscreenOutput input) : SV_TARGET
{
    const float3 hdr = HdrScene.SampleLevel(LinearClampSampler, input.uv, 0.0).rgb;

    // 单出口（与 M4 同形状：两个分支都写 result，预置初值）。
    float3 result = float3(0.0, 0.0, 0.0);
    if (DebugHdr != 0)
    {
        result = SceneLuminanceFalseColor(hdr);
    }
    else
    {
        const float3 exposed = hdr * exp2(ExposureEv);
        result = LinearToSrgb(ToneMapReinhard(exposed));
    }

    // alpha 固定 1：UNORM 后备缓冲按不透明写入。
    return float4(result, 1.0);
}
