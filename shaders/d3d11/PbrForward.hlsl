// ============================================================================
// PbrForward.hlsl — M4 forward opaque PBR（02 篇 direct-light + 03 篇 shadow 接入）
// 里程碑：M4（02 篇 Material 资产与 metallic-roughness PBR；03 篇 Directional Shadow）
// 职责：VS 输出 world-space N/T（normal 走 inverse-transpose，tangent 走 world 3×3
//       后 normalize，手性 = tangent.w * WorldHandedness）与光空间 clip 坐标；
//       PS 采样 t0–t4 五个贴图角色 + t8 shadow map（3×3 comparison PCF）计算
//       direct-light PBR，shadow factor 只乘 direct light（IBL/emissive 不乘）；
//       04 篇接入 split-sum IBL（t5 irradiance / t6 prefiltered / t7 BRDF LUT +
//       b3 IblConstants + s1 clamp 采样器）。tone mapping 不在此做（05 篇）。
// 关联：shaders/d3d11/PbrCommon.hlsli（BRDF 唯一实现）
//       shaders/d3d11/ShadowSampling.hlsli（t8/s2 采样；b1 与 ShadowDepth.hlsl
//       逐字节一致——增删字段两侧同改并重跑 HLSL gate）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（CPU 侧常量布局与 slot 绑定一一对应）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11RenderBindings.h（slot 表）
// ============================================================================
#include "PbrCommon.hlsli"
#include "ShadowSampling.hlsli"

// b0 FrameConstants：每帧一次，128B。**全部字段 float4**——fxc 对 float3/float2
// 混合字段的隐式打包与 CPU struct 错位（M4-03 实测 DebugMode 读到 0 的事故，
// 多偏移探针取证），float4-only 是与 b2 同款的唯一可靠布局。
cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;              // view * projection（已转置的行主序）
    float4 CameraPositionAndDebugMode;    // xyz = 相机世界位置，w = debug view 模式
    float4 DirectionAndIntensity;         // xyz = DirectionToLight（L），w = LightIntensity
    float4 LightColorAndPadding;          // xyz = 线性 RGB，w = 0
    float4 ShadowMapSizeAndPadding;       // xy = 分辨率（2048²），zw = 0
};

// b1 ObjectConstants：每 Draw 一次，208B。与 ShadowDepth.hlsl 的 b1 声明
// 逐字节一致（03 篇硬约束）；float4-only 同上（ReceivesShadow 走 w 分量）。
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;                     // world 矩阵（已转置的行主序）
    float4x4 NormalMatrix;              // 3×3 inverse-transpose（非均匀缩放下 ≠ world）
    float4x4 LightWorldViewProjection;  // 光空间 VP（固定 shadow volume）
    float4 HandednessAndReceivesShadow; // x = WorldHandedness（mirrored? -1:+1），
                                        // y = ReceivesShadow（1/0），zw = 0
};

// b2 MaterialConstants：每 Draw 一次，48B（02 篇「Material constants」契约，
// static_assert(sizeof(MaterialConstants) % 16 == 0) 在 CPU 侧锁定）。
cbuffer MaterialConstants : register(b2)
{
    float4 BaseColorFactor;                // 线性 RGBA 乘子
    float4 EmissiveAndMetallic;            // rgb = emissiveFactor，a = metallicFactor
    float4 RoughnessNormalOcclusionFlags;  // x = roughness，y = normalScale，
                                           // z = occlusionStrength，w = flags（恒 0）
};

// b3 IblConstants：environment 变化时更新（16B；float4-only 原则）。
cbuffer IblConstants : register(b3)
{
    float4 PrefilterMipCountAndFlags; // x = prefiltered mip 数，yzw = 0
}

// 五个贴图角色（t0–t4，D3D11RenderBindings::PbrSrvSlot 契约；缺省槽位由
// 渲染端绑定真实 fallback SRV，禁止留空依赖上一材质残留）。
Texture2D BaseColorTexture : register(t0);            // sRGB
Texture2D NormalTexture : register(t1);               // linear，tangent-space XYZ
Texture2D MetallicRoughnessTexture : register(t2);    // linear，g = roughness，b = metallic
Texture2D OcclusionTexture : register(t3);            // linear，r = AO
Texture2D EmissiveTexture : register(t4);             // sRGB

// IBL 三资源（t5–t7，04 篇）：IblState 未 Ready 时渲染端绑定 fallback——
// 黑 cube ×2 + 中性 LUT（scale=1,bias=0），indirect 退化为 0 而非 NaN。
TextureCube DiffuseIrradiance : register(t5);         // RGBA16F，32²×6，已预除 PI
TextureCube PrefilteredSpecular : register(t6);       // RGBA16F，128²×6×full chain
Texture2D BrdfLut : register(t7);                     // RG16F，256²，(scale, bias)

SamplerState MaterialSampler : register(s0);          // 线性过滤 + wrap
SamplerState IblSampler : register(s1);               // IBL 线性 clamp（04 篇契约）

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;    // xyz = 单位切线，w = bitangent 手性（±1）
    float2 uv0 : TEXCOORD0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : WORLDPOS;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float4 lightClipPosition : LIGHTCLIP; // 光空间 clip 坐标（shadow 采样入口）
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    float4 worldPosition = mul(float4(input.position, 1.0), World);
    output.position = mul(worldPosition, ViewProjection);
    output.worldPosition = worldPosition.xyz;

    // normal 走 inverse-transpose（NormalMatrix）；tangent 走 world 3×3 后 normalize
    //（02 篇「Normal mapping」：两者是非均匀缩放下不同的变换）。
    output.normal = normalize(mul(float4(input.normal, 0.0), NormalMatrix).xyz);
    output.tangent.xyz = normalize(mul(float4(input.tangent.xyz, 0.0), World).xyz);
    output.tangent.w = input.tangent.w * HandednessAndReceivesShadow.x;
    output.uv0 = input.uv0;

    // 光空间 clip 坐标：透视除法与 UV/depth 映射在 ShadowSampling.hlsli 内完成
    //（D3D depth [0,1] + Y flip 契约集中在采样处，便于 debug 定位）。
    output.lightClipPosition = mul(mul(float4(input.position, 1.0), World), LightWorldViewProjection);
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    // glTF 传输函数契约：baseColor/emissive 的 SRV 是 sRGB 视图，硬件解码到线性；
    // metallic-roughness/normal/occlusion 是 linear 视图，通道语义固定。
    float4 baseSample = BaseColorTexture.Sample(MaterialSampler, input.uv0);
    float3 baseColor = baseSample.rgb * BaseColorFactor.rgb; // OPAQUE 时忽略贴图 alpha

    float4 mr = MetallicRoughnessTexture.Sample(MaterialSampler, input.uv0);
    float metallic = saturate(mr.b * EmissiveAndMetallic.a);
    float roughness = max(saturate(mr.g * RoughnessNormalOcclusionFlags.x), MIN_ROUGHNESS);

    // TBN 正交化 + 手性修正：T 先去掉 N 分量，B = cross(N, T) * w。
    float3 geometricNormal = normalize(input.normal);
    float3 tangent = normalize(input.tangent.xyz - geometricNormal * dot(input.tangent.xyz, geometricNormal));
    float3 bitangent = cross(geometricNormal, tangent) * input.tangent.w;

    // normal map：map = tex.rgb*2-1，XY 乘 normalScale 后 normalize（只乘 XY）。
    float3 normalSample = NormalTexture.Sample(MaterialSampler, input.uv0).xyz * 2.0 - 1.0;
    normalSample.xy *= RoughnessNormalOcclusionFlags.y;
    float3 normal = normalize(tangent * normalSample.x + bitangent * normalSample.y + geometricNormal * normalSample.z);

    // AO：lerp(1, ao, strength)；direct-only 阶段不压黑 direct light（02 篇规则）。
    float aoSample = OcclusionTexture.Sample(MaterialSampler, input.uv0).r;
    float ao = lerp(1.0, aoSample, saturate(RoughnessNormalOcclusionFlags.z));

    // emissive：sRGB 解码后乘 factor，最终着色时相加。
    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.uv0).rgb * EmissiveAndMetallic.rgb;

    float3 viewDirection = normalize(CameraPositionAndDebugMode.xyz - input.worldPosition);
    float3 lightDirection = normalize(DirectionAndIntensity.xyz);

    // shadow factor（03 篇）：3×3 comparison PCF；receiver policy 在采样函数内
    //（volume 外 / w<=0 → 1）。factor 只乘 direct light；emissive/IBL 不受影响。
    const float sampledShadow =
        SampleDirectionalShadow(input.lightClipPosition, ShadowMapSizeAndPadding.xy);
    const float shadow = lerp(1.0, sampledShadow, saturate(HandednessAndReceivesShadow.y));

    float3 direct = EvaluateDirectBrdf(
        baseColor, metallic, roughness, normal, viewDirection, lightDirection,
        LightColorAndPadding.xyz * DirectionAndIntensity.w) * shadow;

    // IBL 组合（04 篇「IBL 组合」，split-sum）：
    //   diffuseIbl = irradiance * diffuseColor（irradiance 已预除 PI，不重复除）；
    //   specularIbl = prefiltered * (F0 * LUT.x + LUT.y)；
    //   indirect = (kd * diffuseIbl + specularIbl) * ao。
    // 规则（04 篇）：metallic 不产生 diffuse IBL（kd 含 (1-metallic)）；AO 不乘
    // emissive；shadow 不乘 IBL；无任意 ambient constant。
    const float noV = saturate(dot(normal, viewDirection));
    const float3 f0 = lerp(float3(DIELECTRIC_F0, DIELECTRIC_F0, DIELECTRIC_F0), baseColor, metallic);
    const float3 fresnel = FresnelSchlickRoughness(noV, f0, roughness);
    const float3 kd = (1.0 - fresnel) * (1.0 - metallic);
    const float3 irradiance = DiffuseIrradiance.Sample(IblSampler, normal).rgb;
    const float3 reflection = reflect(-viewDirection, normal);
    const float lod = roughness * max(PrefilterMipCountAndFlags.x - 1.0, 0.0);
    const float3 prefiltered = PrefilteredSpecular.SampleLevel(IblSampler, reflection, lod).rgb;
    const float2 brdf = BrdfLut.Sample(IblSampler, float2(noV, roughness)).rg;
    const float3 specularIbl = prefiltered * (f0 * brdf.x + brdf.y);
    const float3 indirect = (kd * irradiance * baseColor + specularIbl) * ao;

    // debug views（1–6/8 同前；04 篇调整：7 = indirect（IBL 诊断主入口）、
    // 9 = shadow map 灰度（03 篇原 7，让位后顺延，判读口径不变））。
    if (CameraPositionAndDebugMode.w == 1) return float4(baseColor, 1.0);
    if (CameraPositionAndDebugMode.w == 2) return float4(normal * 0.5 + 0.5, 1.0);
    if (CameraPositionAndDebugMode.w == 3) return metallic.xxxx;
    if (CameraPositionAndDebugMode.w == 4) return roughness.xxxx;
    if (CameraPositionAndDebugMode.w == 5) return ao.xxxx;
    if (CameraPositionAndDebugMode.w == 6) return float4(direct, 1.0);
    if (CameraPositionAndDebugMode.w == 7) return float4(indirect, 1.0);
    if (CameraPositionAndDebugMode.w == 8) return shadow.xxxx;
    if (CameraPositionAndDebugMode.w == 9)
    {
        // shadow map 灰度可视化：在 receiver 的光空间 UV 处重采样 depth。
        // 03 篇「Debug 与 RenderDoc」要求的 shadow map 可视化（诊断 Y flip /
        // [0,1] depth 误用——镜像或越界在本视图立即暴露）。
        float mapDepth = 0.0;
        if (input.lightClipPosition.w > 0.0)
        {
            const float3 ndc = input.lightClipPosition.xyz / input.lightClipPosition.w;
            const float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
            if (all(uv >= 0.0) && all(uv <= 1.0))
            {
                mapDepth = ShadowMap.SampleLevel(MaterialSampler, uv, 0.0);
            }
        }
        return mapDepth.xxxx;
    }

    // emissive 不受 AO/shadow/IBL 影响；tone mapping 不在此做（05 篇 HDR target）。
    float3 color = direct + indirect + emissive;
    return float4(color, 1.0);
}
