// ============================================================================
// PrefilterEnvironment.hlsl — prefiltered specular cube（split-sum 前半）
// 里程碑：M4（04 篇「Prefiltered specular」；手抄清单第 3 条）
// 职责：每个 mip 对应一个 roughness（roughness = mip/(mipCount-1)），对 GGX half
//       vector 做固定 Hammersley importance sampling（512/texel baseline），
//       N=V=R 近似。累计 radiance×NoL 除以 total weight；weight 过小输出 0
//       （明确 fallback，不产生 NaN）。M4 参考路径固定采 source LOD0——
//       该近似写入 ADR-0005（04 篇允许项），高频 HDR fixture 下观察 alias。
// 关联：docs/architecture/README.md「Prefiltered specular」
//       shaders/d3d11/PbrCommon.hlsli（Hammersley/ImportanceSampleGgx）
//       engine/rhi/d3d11/src/D3D11IblResources.cpp（per-mip Roughness 常量驱动方）
// ============================================================================
#include "PbrCommon.hlsli"

// b0：FaceViewProjection（16B）+ roughness 与显式 padding（float4-only 原则）。
cbuffer CubeFaceConstants : register(b0)
{
    float4x4 FaceViewProjection;
    float4 RoughnessAndPadding; // x = 当前 mip 的 roughness，yzw = 0
};

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
    output.position = mul(float4(input.position, 1.0), FaceViewProjection);
    output.direction = input.position;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    // baseline profile：512 samples/texel（04 篇表格）。
    static const uint SAMPLE_COUNT = 512;
    const float3 normal = normalize(input.direction);
    const float3 viewDirection = normal; // N=V=R 近似（split-sum 预积分标准形态）
    float3 result = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    [loop]
    for (uint index = 0; index < SAMPLE_COUNT; ++index)
    {
        const float3 halfVector =
            ImportanceSampleGgx(Hammersley(index, SAMPLE_COUNT), normal, max(RoughnessAndPadding.x, MIN_ROUGHNESS));
        const float3 lightDirection = normalize(2.0 * dot(viewDirection, halfVector) * halfVector - viewDirection);
        const float noL = saturate(dot(normal, lightDirection));
        if (noL > 0.0)
        {
            // M4 参考路径固定采 source LOD0（近似记入 ADR-0005；solid-angle LOD
            // 选择属后续优化，不悄悄引入）。
            result += EnvironmentCube.SampleLevel(LinearClampSampler, lightDirection, 0.0).rgb * noL;
            totalWeight += noL;
        }
    }

    // weight 过小（近镜面 + 采样稀疏）输出 0——明确 fallback 而非 NaN。
    return float4(totalWeight > 1.0e-6 ? result / totalWeight : float3(0.0, 0.0, 0.0), 1.0);
}
