// ============================================================================
// ToneMap.hlsl — M4-05 后处理：手动曝光 + Reinhard + 显式 Linear→sRGB
// 里程碑：M4（05 篇「手动曝光」「Tone mapping」「Linear → sRGB」；手抄清单第 3 条）
// 职责：以 SV_VertexID 全屏三角形采样 HDR scene color（t0），按 exp2(ExposureEv)
//       施加固定曝光、全局 Reinhard 压缩到 [0,1)、再按 IEC sRGB 分段函数显式编码，
//       写入 UNORM 后备缓冲 RTV。三条硬契约：
//   1. **只编码一次**：后备缓冲 RTV 是 `R8G8B8A8_UNORM`（非 `_SRGB` 视图），sRGB
//      编码在本 shader 内显式完成；再建一个 _SRGB RTV 会 double encode（05 篇
//      RenderDoc 检查第 6 项）。
//   2. **顺序固定**：exposure → Reinhard → sRGB。任何阶段都不 clamp 到 [0,1]
//      之前的亮度（HDR 值 >1 必须能进入本 pass，否则 tone mapping 未被验证）。
//   3. **不使用 pow(x, 1/2.2) 近似**：分段 sRGB 在断点 0.0031308 处与近似公式
//      差异可达 1e-2 量级，会污染 golden 截图比较。
// 关联：docs/architecture/README.md
//       shaders/d3d11/FullscreenTriangle.hlsli（VS；UV 生成）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11HdrTarget.h（b0 布局）
// ============================================================================
#include "FullscreenTriangle.hlsli"

// b0 与 CPU 侧 PostProcessConstants 逐字节对应（16B；static_assert 在 CPU 侧锁定）。
cbuffer ToneMapConstants : register(b0)
{
    float ExposureEv;
    uint DebugHdr;
    float2 InverseOutputSize;
};

// t0 = HDR scene color（R16G16B16A16_FLOAT）；s1 = 线性 clamp（05 篇 slot 契约，
// 与 IBL 共用同一个 sampler 槽位语义）。
Texture2D<float4> HdrScene : register(t0);
SamplerState LinearClampSampler : register(s1);

// VS：无顶点缓冲，SV_VertexID 0..2 生成覆盖全屏的超屏三角形。
FullscreenOutput VSMain(uint vertexId : SV_VertexID)
{
    return FullscreenTriangleVS(vertexId);
}

// 全局 Reinhard（05 篇冻结公式）：逐通道单调、finite、无 LUT。
// max(color, 0) 先行：负亮度（数值噪声/错误数据）不得翻转成亮值。
float3 ToneMapReinhard(float3 color)
{
    color = max(color, 0.0);
    return color / (1.0 + color);
}

// IEC 61966-2-1 分段 sRGB 传输函数。**不要改成 pow(x, 1/2.2)**——05 篇明确禁止
// 近似 baseline；CPU 参考见 tests/rendering/PbrMathTests.cpp。
float3 LinearToSrgb(float3 linearColor)
{
    linearColor = saturate(linearColor);
    const float3 low = 12.92 * linearColor;
    const float3 high = 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(linearColor, 0.0031308));
}

// SceneLuminance debug view（05 篇「HDR pass」要求的可验证亮度入口）：
//   luminance < 1  → 线性灰（**不做 tone map、不做 sRGB encode**，故本视图输出
//                    的是 linear quantity，判读时必须按线性值理解）
//   luminance >= 1 → 洋红饱和标记：任何洋红像素即证明 HDR 中存在 >1 的亮度
//                    （若整屏无洋红，说明 tone mapping 从未被真正验证）。
float3 SceneLuminanceFalseColor(float3 hdrColor)
{
    const float luminance = dot(max(hdrColor, 0.0), float3(0.2126, 0.7152, 0.0722));
    return luminance < 1.0 ? float3(luminance, luminance, luminance) : float3(1.0, 0.0, 1.0);
}

float4 PSMain(FullscreenOutput input) : SV_TARGET
{
    const float3 hdr = HdrScene.SampleLevel(LinearClampSampler, input.uv, 0.0).rgb;

    // 单出口（M4-02 教训：/Od + 提前 return 会触发 FXC X4000 误报，被 /WX 提升为
    // 编译错误）：两个分支都写 result，且预置初值避免任何未初始化分析歧义。
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

    // alpha 固定 1：M4 截图比较忽略 alpha，但 UNORM 后备缓冲仍按不透明写入。
    return float4(result, 1.0);
}
