// D3D12 迁移：保留 M4 数学、采样次数、寄存器和面方向，离线 DXC 编译。
// ============================================================================
// IntegrateBrdf.hlsl — BRDF LUT（split-sum 后半，环境无关项）
// 里程碑：M4（04 篇「BRDF LUT」；手抄清单第 4 条）
// 职责：256² R16G16_FLOAT 全屏 pass，逐 texel 做 1024-sample GGX 积分，输出
//       (scale, bias)。轴契约（04 篇冻结）：x = NoV（u 轴）、y = roughness
//       （v 轴）——与 FullscreenTriangle.hlsli 的 UV 同向，无轴交换；与
//       PrefilterEnvironment 共用同一 GGX/roughness 约定（Disney alpha=r²）。
//       运行时：specularIbl = prefiltered * (F0 * brdf.x + brdf.y)。
// 关联：docs/architecture/README.md「BRDF LUT」
//       shaders/d3d11/FullscreenTriangle.hlsli（VS；UV 生成）
//       shaders/d3d11/PbrCommon.hlsli（Hammersley/ImportanceSampleGgx）
// ============================================================================
#include "FullscreenTriangle.hlsli"
#include "PbrCommon.hlsli"

// IBL 线性 clamp 采样器（s1 契约；LUT 采样越界由 clamp 兜底到边缘 texel）。
SamplerState LinearClampSampler : register(s1);

// VS 入口包装：FullscreenTriangleVS 是共享实现（05 篇 ToneMap 复用），本文件
// 暴露运行时与 gate 统一引用的 VSMain 入口（无 VB、SV_VertexID 生成）。
FullscreenOutput VSMain(uint vertexId : SV_VertexID)
{
    return FullscreenTriangleVS(vertexId);
}

// Geometry 项的 IBL 形式：k = alpha²/2（与 direct-light 的 (r+1)²/8 不同——
// IBL 积分的 Smith 近似用另一 k，04 篇与 UE4 split-sum 同款；不要混用）。
float GeometrySchlickGgxIbl(float noX, float roughness)
{
    const float alpha = roughness * roughness;
    const float k = alpha * 0.5;
    return noX / max(noX * (1.0 - k) + k, 1.0e-7);
}

float GeometrySmithIbl(float noV, float noL, float roughness)
{
    return GeometrySchlickGgxIbl(noV, roughness) * GeometrySchlickGgxIbl(noL, roughness);
}

float2 PSMain(FullscreenOutput input) : SV_TARGET
{
    // baseline profile：1024 samples/texel（04 篇表格）。
    static const uint SAMPLE_COUNT = 1024;
    // NoV 下限 1e-4：掠射角下 viewDirection 退化会放大数值噪声；roughness 下限同 MIN_ROUGHNESS。
    const float noV = max(input.uv.x, 1.0e-4);
    const float roughness = max(input.uv.y, MIN_ROUGHNESS);
    const float3 viewDirection = float3(sqrt(saturate(1.0 - noV * noV)), 0.0, noV);
    const float3 normal = float3(0.0, 0.0, 1.0);

    float scale = 0.0;
    float bias = 0.0;
    [loop]
    for (uint index = 0; index < SAMPLE_COUNT; ++index)
    {
        const float3 halfVector = ImportanceSampleGgx(Hammersley(index, SAMPLE_COUNT), normal, roughness);
        const float3 lightDirection = normalize(2.0 * dot(viewDirection, halfVector) * halfVector - viewDirection);

        const float noL = saturate(lightDirection.z);
        const float noH = saturate(halfVector.z);
        const float voH = saturate(dot(viewDirection, halfVector));
        if (noL > 0.0)
        {
            // 解析积分的解析近似（Karis 2013）：scale/bias 分别对应 Fresnel 的
            // (1-F) 与 F 部分；visibility = G*VoH/(NoH*NoV)。
            const float g = GeometrySmithIbl(noV, noL, roughness);
            const float visibility = g * voH / max(noH * noV, 1.0e-6);
            const float fresnel = Pow5(1.0 - voH);
            scale += (1.0 - fresnel) * visibility;
            bias += fresnel * visibility;
        }
    }
    return float2(scale, bias) / float(SAMPLE_COUNT);
}
