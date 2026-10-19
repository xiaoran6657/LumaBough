// 固定 AABB 教学场景：GPU 解析相交生成线性光深度，非通用 mesh 光栅 shadow pass。
// PCSS 沿用 E2 的搜索/平均 blocker/变半径比较结构；本实现采用方形 Hammersley
// 核、世界单位线性深度及按接收深度推导的 UV 半径，是新组合，须独立验证。
// 一次 Lambert 反弹沿 F3 的两段可见性积分定义，改为 GPU 确定性半球积分，
// 不使用旧 lightmap/probe，也不加入常数 ambient 冒充反弹。
#include "PbrCommon.hlsli"

struct Box
{
    float4 minimum; // w=materialId
    float4 maximum;
    float4 albedo;
};

cbuffer SceneConstants : register(b0)
{
    uint4 Dimensions; // width,height,mapSize,boxCount
    float4 ShadowParams; // 完整光源边长,世界单位深度 bias,samples,算法 0/1/2
    float4 RenderParams; // bounce flag,最浅 caster 深度,farDepth,tanHalfFov
    float4 CameraPosition; // xyz,tan(verticalFov/2)
    float4 CameraForward; // xyz,aspect
    float4 CameraRight;
    float4 CameraUp;
    float4 LightPosition; // xyz,point-proxy intensity
    Box Boxes[16];
};

cbuffer DispatchConstants : register(b1)
{
    uint Phase; // 0=shadow map，1=可见性/着色
};

RWStructuredBuffer<float> ShadowDepth : register(u0);
RWStructuredBuffer<float4> LinearColor : register(u1);
RWStructuredBuffer<float4> Diagnostics : register(u2);

struct Hit
{
    float distance;
    float3 normal;
    int boxIndex;
};

// 平行轴显式处理，避免面边缘的 0*INF 产生 NaN；盒内起点取出口。
Hit Trace(float3 origin, float3 direction)
{
    Hit best;
    best.distance = 1.0e20;
    best.normal = 0.0;
    best.boxIndex = -1;
    for (uint index = 0; index < Dimensions.w; ++index)
    {
        float nearT = -1.0e20;
        float farT = 1.0e20;
        float3 nearNormal = 0.0;
        float3 farNormal = 0.0;
        bool valid = true;
        [unroll]
        for (uint axis = 0; axis < 3; ++axis)
        {
            if (abs(direction[axis]) < 1.0e-8)
            {
                valid = valid && origin[axis] >= Boxes[index].minimum[axis] &&
                    origin[axis] <= Boxes[index].maximum[axis];
            }
            else
            {
                float t0 = (Boxes[index].minimum[axis] - origin[axis]) / direction[axis];
                float t1 = (Boxes[index].maximum[axis] - origin[axis]) / direction[axis];
                float3 n = 0.0;
                n[axis] = direction[axis] > 0.0 ? -1.0 : 1.0;
                if (min(t0, t1) > nearT)
                {
                    nearT = min(t0, t1);
                    nearNormal = n;
                }
                if (max(t0, t1) < farT)
                {
                    farT = max(t0, t1);
                    farNormal = -n;
                }
            }
        }
        float distance = nearT > 1.0e-4 ? nearT : farT;
        if (valid && nearT <= farT && distance > 1.0e-4 && distance < best.distance)
        {
            best.distance = distance;
            best.normal = nearT > 1.0e-4 ? nearNormal : farNormal;
            best.boxIndex = int(index);
        }
    }
    return best;
}

float LoadDepth(float2 uv)
{
    if (any(uv < 0.0) || any(uv >= 1.0))
    {
        return RenderParams.z;
    }
    uint2 texel = uint2(uv * Dimensions.z);
    return ShadowDepth[texel.y * Dimensions.z + texel.x];
}

float2 SquareOffset(uint index, uint count)
{
    // 包含中心 tap，以免低预算下搜索窗口跨过小遮挡物却漏掉中心。
    return index == 0 ? 0.0 : 2.0 * Hammersley(index - 1, count - 1) - 1.0;
}

float ReceiverDepth(float2 uv, float3 position, float3 normal, float fallback)
{
    // 精确定位实际读取的 texel 中心，再求该光线与局部接收平面的交点深度。
    // 避免斜墙的深度梯度被 blocker search 误认作外部遮挡；bias 仍独立可控。
    float2 center = (floor(uv * Dimensions.z) + 0.5) / Dimensions.z;
    float2 slope = (center * 2.0 - 1.0) * RenderParams.w;
    float3 ray = float3(slope.x, -1.0, slope.y);
    float denominator = dot(normal, ray);
    return abs(denominator) > 1.0e-6 ? dot(normal, position - LightPosition.xyz) / denominator : fallback;
}

float Filter(float2 uv, float receiverDepth, float radius, float3 position, float3 normal)
{
    float sum = 0.0;
    uint count = uint(ShadowParams.z);
    for (uint index = 0; index < count; ++index)
    {
        float2 tapUv = uv + radius * SquareOffset(index, count);
        float compareDepth = ReceiverDepth(tapUv, position, normal, receiverDepth);
        sum += compareDepth - ShadowParams.y <= LoadDepth(tapUv);
    }
    return sum / count;
}

float3 Shadow(float3 position, float3 normal)
{
    float depth = LightPosition.y - position.y;
    if (depth <= 0.0 || depth >= RenderParams.z)
    {
        return float3(1.0, 0.0, 0.0);
    }
    float2 uv = 0.5 + (position.xz - LightPosition.xz) / (2.0 * depth * RenderParams.w);
    if (any(uv < 0.0) || any(uv >= 1.0))
    {
        return float3(1.0, 0.0, 0.0);
    }
    uint mode = uint(ShadowParams.w);
    if (mode == 0)
    {
        return float3(ReceiverDepth(uv, position, normal, depth) - ShadowParams.y <= LoadDepth(uv), 0.0, 0.0);
    }
    float lightRadius = 0.5 * ShadowParams.x;
    if (mode == 1)
    {
        // 固定 UV PCF：全场景同一过滤半径，不随 blocker 距离变化。
        float fixedRadius = lightRadius / (2.0 * LightPosition.y * RenderParams.w);
        return float3(Filter(uv, depth, fixedRadius, position, normal), 0.0, fixedRadius);
    }
    // 最近 caster 的几何上界给出保守搜索窗；远/近距离均用线性灯轴深度。
    float searchRadius = lightRadius * max(0.0, 1.0 / RenderParams.y - 1.0 / depth) /
        (2.0 * RenderParams.w);
    float blockerSum = 0.0;
    uint blockerCount = 0;
    uint samples = uint(ShadowParams.z);
    for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        float2 tapUv = uv + searchRadius * SquareOffset(sampleIndex, samples);
        float stored = LoadDepth(tapUv);
        float compareDepth = ReceiverDepth(tapUv, position, normal, depth);
        if (stored < compareDepth - ShadowParams.y)
        {
            blockerSum += stored;
            ++blockerCount;
        }
    }
    if (blockerCount == 0)
    {
        return float3(1.0, 0.0, 0.0);
    }
    float blocker = blockerSum / blockerCount;
    // 平行接收平面近似：半影世界半径 r*(d-db)/db，投影到接收深度 d 的 UV。
    float penumbra = lightRadius * max(0.0, depth - blocker) /
        (max(blocker, 0.01) * depth * 2.0 * RenderParams.w);
    return float3(Filter(uv, depth, penumbra, position, normal), blocker, penumbra);
}

float3 Direct(float3 position, float3 normal, float3 albedo, float visibility)
{
    float3 toLight = LightPosition.xyz - position;
    float distanceSquared = dot(toLight, toLight);
    return albedo * (LightPosition.w * max(0.0, dot(normal, normalize(toLight))) * visibility /
        (PI * max(distanceSquared, 0.01)));
}

float3 OneBounce(float3 position, float3 normal, float3 albedo)
{
    // 256 个固定余弦加权方向；q→light 用解析线段可见性，独立于显示用 shadow map。
    // 点光代理保持光源总强度不随 light-size 变化，未声称面光源全角积分。
    float3 sum = 0.0;
    const uint count = 256;
    for (uint index = 0; index < count; ++index)
    {
        float2 xi = Hammersley(index, count);
        xi.x = (float(index) + 0.5) / count;
        float3 direction = CosineSampleHemisphere(xi, normal);
        Hit other = Trace(position + normal * 0.001, direction);
        if (other.boxIndex >= 0)
        {
            float3 bouncePosition = position + normal * 0.001 + direction * other.distance;
            float3 toLight = LightPosition.xyz - bouncePosition;
            Hit blocker = Trace(bouncePosition + other.normal * 0.001, normalize(toLight));
            float visibility = blocker.distance >= length(toLight) - 0.002 ? 1.0 : 0.0;
            sum += Direct(bouncePosition, other.normal, Boxes[other.boxIndex].albedo.xyz, visibility);
        }
    }
    // 余弦 PDF 抵消接收端 cos/pi；只允许这一跳，未加 ambient 或高阶 bounce。
    return albedo * sum / count;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    if (Phase == 0)
    {
        if (any(thread.xy >= Dimensions.zz))
        {
            return;
        }
        float2 uv = (float2(thread.xy) + 0.5) / Dimensions.z;
        float2 slope = (uv * 2.0 - 1.0) * RenderParams.w;
        float3 direction = normalize(float3(slope.x, -1.0, slope.y));
        Hit hit = Trace(LightPosition.xyz, direction);
        float depth = hit.boxIndex >= 0 ? -direction.y * hit.distance : RenderParams.z;
        ShadowDepth[thread.y * Dimensions.z + thread.x] = min(depth, RenderParams.z);
        return;
    }
    if (thread.x >= Dimensions.x || thread.y >= Dimensions.y)
    {
        return;
    }
    uint pixel = thread.y * Dimensions.x + thread.x;
    float2 screen = (float2(thread.xy) + 0.5) / float2(Dimensions.xy) * 2.0 - 1.0;
    float3 direction = normalize(CameraForward.xyz + CameraRight.xyz *
        (screen.x * CameraPosition.w * CameraForward.w) - CameraUp.xyz * (screen.y * CameraPosition.w));
    Hit hit = Trace(CameraPosition.xyz, direction);
    if (hit.boxIndex < 0)
    {
        LinearColor[pixel] = float4(0.018, 0.023, 0.032, 1.0);
        Diagnostics[pixel] = float4(1.0, 0.0, 0.0, -1.0);
        return;
    }
    float3 position = CameraPosition.xyz + direction * hit.distance;
    float3 shadow = Shadow(position, hit.normal);
    float3 albedo = Boxes[hit.boxIndex].albedo.xyz;
    float3 color = Direct(position, hit.normal, albedo, shadow.x);
    if (RenderParams.x > 0.5)
    {
        color += OneBounce(position, hit.normal, albedo);
    }
    LinearColor[pixel] = float4(color, 1.0);
    Diagnostics[pixel] = float4(shadow, Boxes[hit.boxIndex].minimum.w);
}
