// D3D12 迁移：保留 M4 数学、采样次数、寄存器和面方向，离线 DXC 编译。
// ============================================================================
// IrradianceConvolution.hlsl — diffuse irradiance cube 卷积
// 里程碑：M4（04 篇「Diffuse irradiance」；手抄清单第 2 条）
// 职责：对每个输出法线 N 在半球做 cosine-weighted 蒙特卡洛积分
//       （固定 Hammersley 256/texel，baseline contract 不可静默降采样）。
//       输出约定（04 篇）：**存 irradiance / PI**——运行时 diffuse 项直接乘
//       baseColor，不得再除 PI；由 white furnace test 锁定（重复项=能量爆炸）。
// 关联：docs/architecture/README.md「Diffuse irradiance」
//       shaders/d3d11/PbrCommon.hlsli（Hammersley/CosineSampleHemisphere 唯一实现）
//       tests/rendering/PbrMathTests.cpp（常量白环境均匀性 CPU 参考）
// ============================================================================
#include "PbrCommon.hlsli"

// b0：FaceViewProjection（16B）+ 参数 float4（80B；CPU 侧统一布局——
// 本 shader 忽略第二字段；同文件的 PSDownsample 用 x = source mip）。
cbuffer CubeFaceConstants : register(b0)
{
    float4x4 FaceViewProjection;
    float4 PassParameters; // x = sourceMip（仅 PSDownsample 用），yzw = 0
};

// t0：已生成的 environment cube（RGBA16F full chain；irradiance 固定采 LOD0）。
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
    // baseline profile：256 samples/texel（04 篇表格，禁止运行时悄悄降低）。
    static const uint SAMPLE_COUNT = 256;
    const float3 normal = normalize(input.direction);
    float3 sum = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint index = 0; index < SAMPLE_COUNT; ++index)
    {
        // cosine-weighted 采样下 pdf ∝ cos，权重抵消：均值 = E[Li] = irradiance / PI。
        const float3 direction = CosineSampleHemisphere(Hammersley(index, SAMPLE_COUNT), normal);
        sum += EnvironmentCube.SampleLevel(LinearClampSampler, direction, 0.0).rgb;
    }

    return float4(sum / float(SAMPLE_COUNT), 1.0);
}

// 确定性 mip downsample 入口（04 篇「确定的 full-screen face downsample pass」；
// 不使用 GenerateMips——M4 禁止隐式 GPU state 进资产管线）。PassParameters.x =
// source mip；单 tap SampleLevel 双线性在半分辨率像素中心恰取源 4 texel 平均
//（标准 2×2 box），结果确定且无跨帧差异。调用侧：目标 cube 只绑 RTV，采样走
// 内部 mipSource 中转 cube（同一资源 RTV+SRV 并存是 D3D11 hazard）。
float4 PSDownsample(PixelInput input) : SV_TARGET
{
    return EnvironmentCube.SampleLevel(
        LinearClampSampler, normalize(input.direction), max(PassParameters.x, 0.0));
}
