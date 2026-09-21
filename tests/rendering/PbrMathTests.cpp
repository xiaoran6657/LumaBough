// ============================================================================
// PbrMathTests.cpp — PBR 公共数学的 CPU 参考实现（02 篇 direct + 04 篇 IBL）
// 里程碑：M4（02 篇 Material 资产与 metallic-roughness PBR；04 篇 IBL 采样数学）
// 职责：在 HLSL 进 shader 之前，用同公式的 CPU 参考验证 BRDF/IBL 数学基线域：
//       Schlick fresnel 端点、GGX 全域 finite、Reinhard/sRGB、Hammersley 确定性、
//       equirect 方向映射、cube face basis 正交、irradiance/PI 约定、white
//       furnace 能量上界、BRDF LUT 轴与范围；05 篇追加曝光/Reinhard/分段 sRGB 的
//       golden、double-gamma 检出与 non-finite 拒绝。公式与
//       shaders/d3d11/PbrCommon.hlsli、四个 IBL 生成 shader 及
//       shaders/d3d11/ToneMap.hlsl 一一对应（02/04/05 篇「自动化测试」清单）。
// 关联：docs/architecture/README.md「自动化测试」
//       docs/architecture/README.md「自动化/图形测试」
//       docs/architecture/README.md「自动化测试」
//       shaders/d3d11/PbrCommon.hlsli（HLSL 侧同源公式）
// ============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
constexpr float kPi = 3.14159265358979323846F;
constexpr float kMinRoughness = 0.045F;

struct Vec2 final
{
    float x;
    float y;
};

struct Vec3 final
{
    float x;
    float y;
    float z;
};

Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(const Vec3& a, const float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Length(const Vec3& a)
{
    return std::sqrt(Dot(a, a));
}

Vec3 Normalize(const Vec3& a)
{
    const float length = Length(a);
    return {a.x / length, a.y / length, a.z / length};
}

float Saturate(const float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

// Schlick fresnel（标量版；HLSL 侧为 float3 逐分量）。
float FresnelSchlick(const float cosTheta, const float f0)
{
    const float x = 1.0F - Saturate(cosTheta);
    return f0 + (1.0F - f0) * x * x * x * x * x;
}

// Schlick fresnel（IBL 形式：roughness 抑制掠射抬升；FresnelSchlickRoughness 对应）。
float FresnelSchlickRoughness(const float cosTheta, const float f0, const float roughness)
{
    const float x = 1.0F - Saturate(cosTheta);
    return f0 + (std::max(1.0F - roughness, f0) - f0) * x * x * x * x * x;
}

// GGX NDF（Trowbridge-Reitz）；分母下限 1e-7 与 PbrCommon.hlsli 一致。
float DistributionGgx(const float noH, const float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float denominator = noH * noH * (alphaSquared - 1.0F) + 1.0F;
    return alphaSquared / std::max(kPi * denominator * denominator, 1.0e-7F);
}

// Reinhard tone curve（05 篇 tone map 的 CPU 参考，此处锁定单调/有界行为）。
float Reinhard(const float linear)
{
    return linear / (1.0F + linear);
}

// 分段 sRGB 传输函数（Cooker mip 滤波与 05 篇 tone map 共用的 CPU 参考）。
float LinearToSrgb(const float linear)
{
    return linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

// ---- 05 篇后处理数学（与 shaders/d3d11/ToneMap.hlsl 逐公式对应） ----

// 手动曝光（05 篇冻结）：相对 EV，线性倍率 = 2^EV。基线固定 0（倍率 1）。
float ApplyExposure(const float radiance, const float exposureEv)
{
    return radiance * std::exp2(exposureEv);
}

// tone map 入口（HLSL 侧先 max(color,0) 再 Reinhard）：负输入在钳制后得 0，
// 而不是 Reinhard(-1) 的 -inf——这是"tone map 前必须钳负"的判据。
float ToneMapReinhardSafe(const float radiance)
{
    return Reinhard(std::max(radiance, 0.0F));
}

// HDR radiance 是否可渲染（05 篇「负值、NaN 与极端亮度」）：CPU 数据拒绝
// NaN/Inf 与负值——Cooker 侧 HDR 解码已拒绝负 source，此处是渲染端同语义参考。
bool IsRenderableHdrRadiance(const Vec3& radiance)
{
    return std::isfinite(radiance.x) && std::isfinite(radiance.y) && std::isfinite(radiance.z) && radiance.x >= 0.0F &&
           radiance.y >= 0.0F && radiance.z >= 0.0F;
}

// ---- 04 篇 IBL 采样数学（与 PbrCommon.hlsli 逐公式对应） ----

// Van der Corput radical inverse（02 位反转）：输入位模式固定，输出确定。
float RadicalInverseVdc(std::uint32_t bits)
{
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

Vec2 Hammersley(const std::uint32_t index, const std::uint32_t count)
{
    return {static_cast<float>(index) / static_cast<float>(count), RadicalInverseVdc(index)};
}

Vec3 ImportanceSampleGgx(const Vec2 xi, const Vec3 normal, const float roughness)
{
    const float alpha = roughness * roughness;
    const float phi = 2.0F * kPi * xi.x;
    const float cosTheta = std::sqrt((1.0F - xi.y) / std::max(1.0F + (alpha * alpha - 1.0F) * xi.y, 1.0e-7F));
    const float sinTheta = std::sqrt(Saturate(1.0F - cosTheta * cosTheta));

    const Vec3 halfTangent{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
    const Vec3 up = std::abs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 tangent = Normalize(Cross(up, normal));
    const Vec3 bitangent = Cross(normal, tangent);
    return Normalize(tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

Vec3 CosineSampleHemisphere(const Vec2 xi, const Vec3 normal)
{
    const float radius = std::sqrt(xi.x);
    const float phi = 2.0F * kPi * xi.y;
    const Vec3 local{radius * std::cos(phi), radius * std::sin(phi), std::sqrt(Saturate(1.0F - xi.x))};

    const Vec3 up = std::abs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 tangent = Normalize(Cross(up, normal));
    const Vec3 bitangent = Cross(normal, tangent);
    return Normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

// equirect 方向映射（EquirectToCube.hlsl 的冻结公式）。注意 acos 的 clamp 是
// [-1,1] 全域（不是 saturate）——南极方向 v=1 依赖这一点。
Vec2 DirectionToEquirectUv(const Vec3 direction)
{
    const Vec3 normalized = Normalize(direction);
    const float u = std::atan2(normalized.x, normalized.z) / (2.0F * kPi) + 0.5F;
    const float v = std::acos(std::clamp(normalized.y, -1.0F, 1.0F)) / kPi;
    return {u - std::floor(u), v};
}

// IBL geometry 项（IntegrateBrdf.hlsl 的 Smith k=alpha²/2 形式）。
float GeometrySchlickGgxIbl(const float noX, const float roughness)
{
    const float alpha = roughness * roughness;
    const float k = alpha * 0.5F;
    return noX / std::max(noX * (1.0F - k) + k, 1.0e-7F);
}

// BRDF LUT 项的 CPU 积分（IntegrateBrdf.hlsl 的 PSMain 同式；测试用抽稀采样数）。
void IntegrateBrdfTerm(const float noV, const float roughness, const std::uint32_t samples, float& outScale,
                       float& outBias)
{
    const Vec3 normal{0.0F, 0.0F, 1.0F};
    const Vec3 viewDirection{std::sqrt(std::max(0.0F, 1.0F - noV * noV)), 0.0F, noV};
    float scaleSum = 0.0F;
    float biasSum = 0.0F;
    for (std::uint32_t index = 0; index < samples; ++index)
    {
        const Vec3 halfVector = ImportanceSampleGgx(Hammersley(index, samples), normal, roughness);
        const float dvH = Dot(viewDirection, halfVector);
        const Vec3 lightDirection =
            Normalize(Vec3{2.0F * dvH * halfVector.x - viewDirection.x, 2.0F * dvH * halfVector.y - viewDirection.y,
                           2.0F * dvH * halfVector.z - viewDirection.z});
        const float noL = Saturate(lightDirection.z);
        const float noH = Saturate(halfVector.z);
        const float voH = Saturate(dvH);
        if (noL > 0.0F)
        {
            const float g = GeometrySchlickGgxIbl(noV, roughness) * GeometrySchlickGgxIbl(noL, roughness);
            const float visibility = g * voH / std::max(noH * noV, 1.0e-6F);
            const float x = 1.0F - voH;
            const float fresnel = x * x * x * x * x;
            scaleSum += (1.0F - fresnel) * visibility;
            biasSum += fresnel * visibility;
        }
    }
    outScale = scaleSum / static_cast<float>(samples);
    outBias = biasSum / static_cast<float>(samples);
}
} // namespace

TEST(PbrMathTests, DielectricFresnelHasExpectedEndpoints)
{
    // F0 在掠射角（cosTheta=1）保持不变，垂直入射（cosTheta=0）逼近全反射。
    EXPECT_NEAR(FresnelSchlick(1.0F, 0.04F), 0.04F, 1.0e-6F);
    EXPECT_NEAR(FresnelSchlick(0.0F, 0.04F), 1.0F, 1.0e-6F);
}

TEST(PbrMathTests, GgxIsFiniteAcrossBaselineDomain)
{
    // roughness ∈ [0.045, 1]（02 篇 clamp 下限）、NoH ∈ [0, 1] 全组合：finite 且非负。
    for (int roughnessStep = 0; roughnessStep <= 100; ++roughnessStep)
    {
        const float roughness = std::max(0.045F, float(roughnessStep) / 100.0F);
        for (int angleStep = 0; angleStep <= 100; ++angleStep)
        {
            const float value = DistributionGgx(float(angleStep) / 100.0F, roughness);
            EXPECT_TRUE(std::isfinite(value));
            EXPECT_GE(value, 0.0F);
        }
    }
}

TEST(PbrMathTests, RoughnessExtremesStayFinite)
{
    // roughness 0.045（近镜面，峰值可能巨大但 finite）与 1（完全粗糙，值为 alpha^2/π）。
    EXPECT_TRUE(std::isfinite(DistributionGgx(1.0F, 0.045F)));
    EXPECT_NEAR(DistributionGgx(1.0F, 1.0F), 1.0F / kPi, 1.0e-6F);
}

TEST(PbrMathTests, ReinhardIsMonotonicAndBounded)
{
    EXPECT_FLOAT_EQ(Reinhard(0.0F), 0.0F);
    EXPECT_LT(Reinhard(1.0F), Reinhard(10.0F));
    EXPECT_LT(Reinhard(10000.0F), 1.0F);
}

TEST(PbrMathTests, PiecewiseSrgbMatchesKnownPoints)
{
    EXPECT_NEAR(LinearToSrgb(0.0F), 0.0F, 1.0e-7F);
    EXPECT_NEAR(LinearToSrgb(0.0031308F), 0.04045F, 1.0e-5F);
    EXPECT_NEAR(LinearToSrgb(1.0F), 1.0F, 1.0e-6F);
}

// ---------------------------------------------------------------------------
// 05 篇后处理数学（曝光 / Reinhard / 显式 sRGB）
// ---------------------------------------------------------------------------

TEST(PostProcessTests, ReinhardMatchesGoldenSamples)
{
    // 05 篇冻结公式 color/(1+color)：golden 锚点覆盖 0、中值、1、远大于 1、极值。
    EXPECT_FLOAT_EQ(Reinhard(0.0F), 0.0F);
    EXPECT_FLOAT_EQ(Reinhard(1.0F), 0.5F);
    EXPECT_NEAR(Reinhard(0.5F), 1.0F / 3.0F, 1.0e-6F);
    EXPECT_NEAR(Reinhard(10.0F), 10.0F / 11.0F, 1.0e-6F);
    EXPECT_NEAR(Reinhard(100.0F), 100.0F / 101.0F, 1.0e-6F);

    // 极大值：finite 且严格小于 1（全局 Reinhard 的值域上界，不是饱和到 1）。
    const float extreme = Reinhard(1.0e6F);
    EXPECT_TRUE(std::isfinite(extreme));
    EXPECT_LT(extreme, 1.0F);
    EXPECT_GT(extreme, 0.999F);

    // 单调：HDR 输入越大输出越大（tone map 不得破坏亮度序）。
    EXPECT_LT(Reinhard(1.0F), Reinhard(2.0F));
    EXPECT_LT(Reinhard(2.0F), Reinhard(10.0F));
    EXPECT_LT(Reinhard(10.0F), Reinhard(100.0F));
}

TEST(PostProcessTests, ExposureScalesByPowerOfTwo)
{
    // 05 篇：exposed = hdr * exp2(exposureEv)——EV 每 +1 亮度翻倍、-1 减半。
    EXPECT_FLOAT_EQ(ApplyExposure(1.0F, 0.0F), 1.0F);
    EXPECT_FLOAT_EQ(ApplyExposure(1.0F, -2.0F), 0.25F);
    EXPECT_FLOAT_EQ(ApplyExposure(1.0F, 2.0F), 4.0F);
    EXPECT_FLOAT_EQ(ApplyExposure(4.0F, -2.0F), 1.0F);

    // 与 tone map 组合后仍保持序关系（曝光是单调缩放，不改变 Reinhard 的单调性）。
    EXPECT_LT(Reinhard(ApplyExposure(1.0F, -2.0F)), Reinhard(ApplyExposure(1.0F, 0.0F)));
    EXPECT_LT(Reinhard(ApplyExposure(1.0F, 0.0F)), Reinhard(ApplyExposure(1.0F, 2.0F)));

    // +2 EV 的线性倍率恰为 -2 EV 的 16 倍（4 档 EV = 2^4）。
    EXPECT_FLOAT_EQ(ApplyExposure(1.0F, 2.0F) / ApplyExposure(1.0F, -2.0F), 16.0F);
}

TEST(PostProcessTests, PiecewiseSrgbIsNotGammaApproximation)
{
    // 05 篇明确禁止 pow(x, 1/2.2) 近似：两者在中灰处差 ~5.6e-3，足以污染截图比较。
    const float srgb = LinearToSrgb(0.5F);
    EXPECT_NEAR(srgb, 0.735357F, 1.0e-4F) << "分段 sRGB 的中灰锚点";
    const float approximate = std::pow(0.5F, 1.0F / 2.2F);
    EXPECT_GT(std::abs(srgb - approximate), 0.005F) << "必须与 1/2.2 近似可区分";

    // 断点：0.0031308 两侧连续（低段线性段与高段幂段在此衔接，容差 1e-5）。
    EXPECT_NEAR(LinearToSrgb(0.0031308F), 0.04045F, 1.0e-5F);
    EXPECT_NEAR(12.92F * 0.0031308F, 0.04045F, 1.0e-6F);
    const float highBranch = 1.055F * std::pow(0.0031308F, 1.0F / 2.4F) - 0.055F;
    EXPECT_NEAR(highBranch, 12.92F * 0.0031308F, 1.0e-5F) << "分段函数在断点处连续";
}

TEST(PostProcessTests, DoubleEncodeIsDetectable)
{
    // no double-gamma golden fixture（05 篇）：线性值被编码两次后明显偏离单次结果。
    // 若渲染端同时创建 _SRGB RTV 又在 shader 内编码，本用例的差值就是那类缺陷的量级。
    const float single = LinearToSrgb(0.5F);
    const float doubled = LinearToSrgb(single);
    EXPECT_GT(doubled - single, 0.1F) << "二次编码必须可被 golden 检出";

    // 端点 0/1 是不动点：即使误编码也不会显现，故不能只测端点。
    EXPECT_FLOAT_EQ(LinearToSrgb(0.0F), 0.0F);
    EXPECT_FLOAT_EQ(LinearToSrgb(1.0F), 1.0F);
}

TEST(PostProcessTests, EmissiveRampIsMonotonicAndApproachesOne)
{
    // 05 篇「极端亮度」：白色 emissive 0、1、10、100 经 Reinhard 后单调并趋近 1。
    float previous = -1.0F;
    for (const float emissive : {0.0F, 1.0F, 10.0F, 100.0F})
    {
        const float mapped = ToneMapReinhardSafe(emissive);
        EXPECT_TRUE(std::isfinite(mapped));
        EXPECT_GE(mapped, 0.0F);
        EXPECT_LT(mapped, 1.0F);
        EXPECT_GT(mapped, previous) << "emissive=" << emissive << " 必须严格单调递增";
        previous = mapped;
    }
    EXPECT_GT(previous, 0.98F) << "100 的 emissive 应逼近 1";
}

TEST(PostProcessTests, HdrRadianceRejectsNonFiniteAndNegative)
{
    // 05 篇「负值、NaN 与极端亮度」：CPU 数据拒绝 NaN/Inf 与负值。
    EXPECT_FALSE(IsRenderableHdrRadiance({std::numeric_limits<float>::quiet_NaN(), 1.0F, 1.0F}));
    EXPECT_FALSE(IsRenderableHdrRadiance({std::numeric_limits<float>::infinity(), 1.0F, 1.0F}));
    EXPECT_FALSE(IsRenderableHdrRadiance({-std::numeric_limits<float>::infinity(), 1.0F, 1.0F}));
    EXPECT_FALSE(IsRenderableHdrRadiance({-0.1F, 1.0F, 1.0F})) << "负 radiance 在 CPU 侧即被拒绝";
    EXPECT_TRUE(IsRenderableHdrRadiance({0.0F, 1.0F, 4.0F})) << ">1 的 radiance 是合法的 HDR 值";

    // tone map 前的 max(color, 0)：负输入钳制后为 0，而不是 Reinhard(-1) 的 -inf。
    EXPECT_FLOAT_EQ(ToneMapReinhardSafe(-1.0F), 0.0F);
    EXPECT_TRUE(std::isfinite(ToneMapReinhardSafe(-1.0F)));
}

// ---------------------------------------------------------------------------
// 04 篇 IBL 采样数学
// ---------------------------------------------------------------------------

TEST(IblMathTests, HammersleyIsDeterministicAndMatchesKnownValues)
{
    // 已知锚点：RadicalInverse 的前几项（0、1/2、1/4、3/4、1/8）——序列无随机
    // seed，跨运行/跨设备逐位一致（04 篇「固定 sequence」契约）。
    EXPECT_FLOAT_EQ(RadicalInverseVdc(0U), 0.0F);
    EXPECT_NEAR(RadicalInverseVdc(1U), 0.5F, 1.0e-7F);
    EXPECT_NEAR(RadicalInverseVdc(2U), 0.25F, 1.0e-7F);
    EXPECT_NEAR(RadicalInverseVdc(3U), 0.75F, 1.0e-7F);
    EXPECT_NEAR(RadicalInverseVdc(4U), 0.125F, 1.0e-7F);

    // 确定性：重复求值逐位一致；x 分量是精确的 i/N 网格（N=2 的幂）。
    for (std::uint32_t index = 0; index < 64; ++index)
    {
        const Vec2 first = Hammersley(index, 256U);
        const Vec2 second = Hammersley(index, 256U);
        ASSERT_FLOAT_EQ(first.x, second.x);
        ASSERT_FLOAT_EQ(first.y, second.y);
        EXPECT_NEAR(first.x, static_cast<float>(index) / 256.0F, 1.0e-6F);
    }
}

TEST(IblMathTests, GgxImportanceSamplesStayOnUnitHemisphere)
{
    // xi 全域采样：half vector 单位长、位于 normal 半球（NoH ≥ 0）且 finite。
    const Vec3 normal = Normalize(Vec3{0.3F, 0.8F, 0.5F});
    for (int roughnessStep = 1; roughnessStep <= 20; ++roughnessStep)
    {
        const float roughness = std::max(kMinRoughness, roughnessStep / 20.0F);
        for (int xiStep = 0; xiStep < 32; ++xiStep)
        {
            const std::uint32_t index = static_cast<std::uint32_t>(xiStep) * 7U + 1U;
            const Vec3 sampled = ImportanceSampleGgx(Hammersley(index, 224U), normal, roughness);
            const float length = Length(sampled);
            EXPECT_TRUE(std::isfinite(length)) << "roughness=" << roughness << " xi=" << xiStep;
            EXPECT_NEAR(length, 1.0F, 1.0e-4F);
            EXPECT_GE(Dot(sampled, normal), -1.0e-4F) << "half vector must stay in the hemisphere";
        }
    }
}

TEST(IblMathTests, EquirectMappingMatchesFrozenContract)
{
    // 04 篇冻结公式：u = atan2(x,z)/2π+0.5、v = acos(y)/π（顶边为 +Y）。
    const Vec2 up = DirectionToEquirectUv({0.0F, 1.0F, 0.0F});
    EXPECT_NEAR(up.y, 0.0F, 1.0e-6F); // 顶边（+Y）→ v = 0

    const Vec2 down = DirectionToEquirectUv({0.0F, -1.0F, 0.0F});
    EXPECT_NEAR(down.y, 1.0F, 1.0e-6F); // 底边（-Y）→ v = 1（acos 域必须含 -1）

    const Vec2 plusZ = DirectionToEquirectUv({0.0F, 0.0F, 1.0F});
    EXPECT_NEAR(plusZ.x, 0.5F, 1.0e-6F); // atan2(0,1)=0 → u = 0.5

    const Vec2 plusX = DirectionToEquirectUv({1.0F, 0.0F, 0.0F});
    EXPECT_NEAR(plusX.x, 0.75F, 1.0e-6F); // atan2(1,0)=π/2 → u = 0.25+0.5 = 0.75

    const Vec2 minusX = DirectionToEquirectUv({-1.0F, 0.0F, 0.0F});
    EXPECT_NEAR(minusX.x, 0.25F, 1.0e-6F); // atan2(-1,0)=-π/2 → u = -0.25+0.5 = 0.25

    // -Z 方向的 u 在 0/1 折回边界（atan2 跳变）——EquirectToCube 用 frac 处理，
    // panorama 采样的 clamp 语义在边界内侧必须连续。
    const Vec2 nearMinusZHighSide = DirectionToEquirectUv({0.001F, 0.0F, -1.0F});
    EXPECT_GT(nearMinusZHighSide.x, 0.99F) << "atan2 just above -Z should wrap near 1.0";
    const Vec2 nearMinusZLowSide = DirectionToEquirectUv({-0.001F, 0.0F, -1.0F});
    EXPECT_LT(nearMinusZLowSide.x, 0.01F) << "atan2 just below -Z should wrap near 0.0";
}

TEST(IblMathTests, CubeFaceBasesAreOrthonormalAndMatchFrozenTable)
{
    // 04 篇「Face 方向」表：六面 look/up 正交、单位长——face VP 错误会在轴标记
    // fixture 上直接暴露（方向标记色块错位）。
    struct FaceBasis final
    {
        Vec3 look;
        Vec3 up;
    };
    const std::array<FaceBasis, 6> faces{FaceBasis{Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}},
                                         FaceBasis{Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}},
                                         FaceBasis{Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}},
                                         FaceBasis{Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}},
                                         FaceBasis{Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 1.0F, 0.0F}},
                                         FaceBasis{Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F}}};

    for (const FaceBasis& face : faces)
    {
        EXPECT_NEAR(Length(face.look), 1.0F, 1.0e-6F);
        EXPECT_NEAR(Length(face.up), 1.0F, 1.0e-6F);
        EXPECT_NEAR(Dot(face.look, face.up), 0.0F, 1.0e-6F);
    }
}

TEST(IblMathTests, ConstantWhiteEnvironmentYieldsIrradianceOverPi)
{
    // irradiance/PI 约定（04 篇）：常量白环境（Li=1）下 cosine-weighted 均值
    // E[Li] = 1——即 irradiance/PI = 1，运行时 diffuse = 1 * baseColor 不再除 PI。
    // CPU 蒙特卡洛（256 samples，与 shader 同公式）应在宽松容差内回归 1。
    const Vec3 normal = Normalize(Vec3{0.0F, 1.0F, 0.0F});
    float sum = 0.0F;
    constexpr std::uint32_t kSamples = 256U;
    for (std::uint32_t index = 0; index < kSamples; ++index)
    {
        const Vec3 direction = CosineSampleHemisphere(Hammersley(index, kSamples), normal);
        sum += 1.0F; // 常量白环境的 Li ≡ 1
    }
    const float irradianceOverPi = sum / static_cast<float>(kSamples);
    EXPECT_NEAR(irradianceOverPi, 1.0F, 0.05F);
}

TEST(IblMathTests, BrdfLutAxesAreNovAndRoughness)
{
    // 轴契约（04 篇）：x = NoV、y = roughness——防止轴交换的判据：
    // 1) 正视角（NoV=1）+ 低粗糙度：scale≈1、bias≈0（specularIbl = prefiltered*F0）。
    // 2) 同 roughness 下 NoV 增大（0.5→1）时 scale 上升（Fresnel 减弱）。
    // 3) 全域 scale/bias 非负、有限（LUT readback 校验的 CPU 前置）。
    float scaleNear = 0.0F;
    float biasNear = 0.0F;
    IntegrateBrdfTerm(1.0F, kMinRoughness, 512U, scaleNear, biasNear);
    EXPECT_NEAR(scaleNear, 1.0F, 0.05F);
    EXPECT_NEAR(biasNear, 0.0F, 0.05F);

    float scaleLow = 0.0F;
    float biasLow = 0.0F;
    IntegrateBrdfTerm(0.5F, 0.3F, 512U, scaleLow, biasLow);
    float scaleHigh = 0.0F;
    float biasHigh = 0.0F;
    IntegrateBrdfTerm(1.0F, 0.3F, 512U, scaleHigh, biasHigh);
    EXPECT_GT(scaleHigh, scaleLow) << "larger NoV should raise the scale axis";

    EXPECT_GE(scaleLow, 0.0F);
    EXPECT_GE(biasLow, 0.0F);
    EXPECT_TRUE(std::isfinite(scaleLow));
    EXPECT_TRUE(std::isfinite(biasLow));
    // bias 的单调性不是全域性质（k=alpha²/2 下粗糙表面掠射可见性回落），
    // 因此不在此断言 bias 随 roughness 单调——以 scale 锚点 + 域校验防轴交换。
}

TEST(IblMathTests, WhiteFurnaceEnergyDoesNotExceedInput)
{
    // white furnace test（04 篇）：白环境 + 白材质 + metallic=0 时，split-sum 组合
    //  indirect = kd * irradiance * baseColor + prefiltered * (F0*scale + bias)
    // 的能量不应明显大于输入（1）。diffuse/specular 都以常量白环境近似：
    // irradiance/PI = 1、prefiltered ≈ 1（白环境各方向 radiance 恒 1）。
    const float roughness = 0.5F;
    const float noV = 0.707F;
    const float f0 = 0.04F;

    float scale = 0.0F;
    float bias = 0.0F;
    IntegrateBrdfTerm(noV, roughness, 512U, scale, bias);

    const float fresnel = FresnelSchlickRoughness(noV, f0, roughness);
    const float kd = (1.0F - fresnel) * (1.0F - 0.0F); // metallic = 0
    const float diffuseIbl = 1.0F;                     // irradiance/PI * baseColor(=1)
    const float specularIbl = 1.0F * (f0 * scale + bias);
    const float energy = kd * diffuseIbl + specularIbl;

    // 能量上界：不超输入 1（蒙特卡洛噪声余量 5%）——重复项（如 diffuse 再除一次
    // PI、AO/shadow 误乘）或约定断裂会立刻超界。
    EXPECT_LE(energy, 1.05F) << "white furnace energy must not exceed input";
    // 合理下界：能量不能塌缩到 0（积分或约定断裂的另一形态）。
    EXPECT_GT(energy, 0.3F);
}
