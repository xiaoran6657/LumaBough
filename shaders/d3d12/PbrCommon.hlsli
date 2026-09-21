// ============================================================================
// PbrCommon.hlsli — PBR 公共函数库（D3D12 / SM6 侧）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；与 M4 同源）
// 职责：Trowbridge-Reitz GGX NDF、Smith with Schlick-GGX geometry、Schlick
//       fresnel 与 Lambert diffuse 的 direct-light BRDF；以及 04 篇固定 IBL profile
//       的低差异序列与重要性采样。全部计算在线性空间。
// 与 M4 的关系（本步的硬契约）：本文件是 shaders/d3d11/PbrCommon.hlsli 的**逐公式
//       同源移植**——函数体一行不改，因此在 M5-09 parity 之前两边公式不可能分叉。
//       M5-09 会用 CPU fixture（tests/rendering/PbrMathTests.cpp 的参考实现）在
//       D3D12 侧再证明一次数值一致。
// 为什么不做"同一文件两端编译"：D3D11=FXC SM5、D3D12=DXC SM6，条件编译会让两端
//       的编译口径互相牵制（08 篇明确要求"不为同一文件编译引入大量条件分支"）。
// 关联：shaders/d3d12/PbrForward.hlsl（调用方）
//       shaders/d3d11/PbrCommon.hlsli（M4 同源唯一实现）
//       engine/rhi/d3d12/include/MiniEngine/Rhi/D3D12/D3D12RootBindings.h（寄存器契约）
// ============================================================================
#ifndef MINIENGINE_D3D12_PBR_COMMON_HLSLI
#define MINIENGINE_D3D12_PBR_COMMON_HLSLI

static const float PI = 3.14159265358979323846;
static const float DIELECTRIC_F0 = 0.04;
static const float MIN_ROUGHNESS = 0.045;

float Pow5(float value)
{
    float squared = value * value;
    return squared * squared * value;
}

// Schlick fresnel：cosTheta 先 saturate，杜绝 >1 的负指数路径。
float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * Pow5(1.0 - saturate(cosTheta));
}

// Schlick fresnel（IBL 形式，04 篇「IBL 组合」）：roughness 越大掠射角抬升越少。
float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(1.0 - roughness, f0) - f0) * Pow5(1.0 - saturate(cosTheta));
}

// GGX NDF（Trowbridge-Reitz）：分母下限 1e-7，粗糙度域内恒 finite。
float DistributionGgx(float noH, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = noH * noH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 1.0e-7);
}

// Schlick-GGX（direct-light 形式，k = (r+1)^2 / 8）：除零由 max 下限吸收。
float GeometrySchlickGgx(float noX, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return noX / max(noX * (1.0 - k) + k, 1.0e-7);
}

float GeometrySmith(float noV, float noL, float roughness)
{
    return GeometrySchlickGgx(noV, roughness) * GeometrySchlickGgx(noL, roughness);
}

// Direct-light BRDF 核心（02 篇固定公式）：
//   NoV/NoL 任一 <= epsilon 直接返回 0；V+L 长度近零（对零 half-vector 做
//   normalize）同样返回 0——禁止对退化输入产生 NaN。
// 返回值已含 radiance 与 NoL（即 (diffuse + specular) * radiance * NoL）。
// 单出口写法：FXC 在 /Od（SKIP_OPTIMIZATION）下对"提前 return + 后续使用"
// 会误报 X4000（use of potentially uninitialized variable），被 /WX 提升为
// 编译错误——DXC 无该误报，但保持同一形状让两端公式可逐行对照。
float3 EvaluateDirectBrdf(
    float3 baseColor,
    float metallic,
    float roughness,
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 radiance)
{
    float3 result = float3(0.0, 0.0, 0.0);

    const float noV = saturate(dot(normal, viewDirection));
    const float noL = saturate(dot(normal, lightDirection));
    if (noV > 1.0e-6 && noL > 1.0e-6)
    {
        const float3 halfSum = viewDirection + lightDirection;
        const float halfLengthSquared = dot(halfSum, halfSum);
        if (halfLengthSquared > 1.0e-8)
        {
            const float3 halfVector = halfSum * rsqrt(halfLengthSquared);
            const float noH = saturate(dot(normal, halfVector));
            const float voH = saturate(dot(viewDirection, halfVector));

            const float3 f0 = lerp(DIELECTRIC_F0.xxx, baseColor, metallic);
            const float3 f = FresnelSchlick(voH, f0);
            const float d = DistributionGgx(noH, roughness);
            const float g = GeometrySmith(noV, noL, roughness);
            const float3 specular = d * g * f / max(4.0 * noV * noL, 1.0e-5);

            const float3 kd = (1.0 - f) * (1.0 - metallic);
            const float3 diffuse = kd * baseColor / PI;
            result = (diffuse + specular) * radiance * noL;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 04 篇「固定 IBL profile」的采样数学：低差异序列与重要性采样。
// 全部函数无随机 seed、无状态——CPU 参考（tests/rendering/PbrMathTests.cpp）
// 与此处逐公式对应，保证 irradiance/prefilter/LUT 三处约定一致。
// ---------------------------------------------------------------------------

// Van der Corput radical inverse（02 位反转）：Hammersley 序列的第二维。
float RadicalInverseVdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    // 1 / 2^32：bit-reversed 整数映射到 (0,1)。
    return float(bits) * 2.3283064365386963e-10;
}

// Hammersley 点集：x = i/N（均匀）、y = radical inverse（低差异）。
float2 Hammersley(uint index, uint count)
{
    return float2(float(index) / float(count), RadicalInverseVdc(index));
}

// GGX NDF 重要性采样：xi → half vector（世界空间，绕 normal 正交基）。
float3 ImportanceSampleGgx(float2 xi, float3 normal, float roughness)
{
    const float alpha = roughness * roughness;
    const float phi = 2.0 * PI * xi.x;
    const float cosTheta = sqrt((1.0 - xi.y) / max(1.0 + (alpha * alpha - 1.0) * xi.y, 1.0e-7));
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));

    const float3 halfTangent = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

// Cosine-weighted hemisphere 采样（irradiance 卷积用）：pdf 与 cos(theta) 成正比，
// 权重抵消后被积函数只剩 Li——均值即 irradiance / PI（04 篇约定，运行时不再除 PI）。
float3 CosineSampleHemisphere(float2 xi, float3 normal)
{
    const float radius = sqrt(xi.x);
    const float phi = 2.0 * PI * xi.y;
    const float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(saturate(1.0 - xi.x)));

    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

#endif
