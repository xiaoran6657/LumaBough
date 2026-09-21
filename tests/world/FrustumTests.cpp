// ============================================================================
// FrustumTests.cpp — Frustum 平面提取 / 球分类 / 保守球变换的单元测试
// 里程碑：M4（01 篇手抄清单第 3 条；06 篇 culling baseline 前的数学地基）
// 职责：用 identity 与手算矩阵做 golden 对照，覆盖：D3D 裁剪体分类、
//       切平面 epsilon 行为、非法输入拒绝、非均匀缩放的保守球、平面归一化。
// 关联：engine/world/src/Frustum.cpp（被测实现）
// ============================================================================

#include <MiniEngine/World/Frustum.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

using MiniEngine::World::Float3;
using MiniEngine::World::Frustum;
using MiniEngine::World::Matrix4;
using MiniEngine::World::Sphere;
using MiniEngine::World::TransformSphereConservative;
using MiniEngine::World::VolumeRelation;

namespace
{
Matrix4 Identity()
{
    return {};
}
} // namespace

// identity 的 view-projection 即 D3D 单位裁剪体（x/y ∈ [-1,1]，z ∈ [0,1]）：
// 用它做分类的 golden 依据——体内、切 x 平面、近/远平面外四个方向各一例。
TEST(FrustumTests, ClassifiesDirect3DIdentityClipVolume)
{
    const Frustum frustum = Frustum::FromRowVectorDirect3D(Identity());

    EXPECT_EQ(frustum.Classify(Sphere{{0.0F, 0.0F, 0.5F}, 0.1F}), VolumeRelation::Inside);
    EXPECT_EQ(frustum.Classify(Sphere{{-1.0F, 0.0F, 0.5F}, 0.1F}), VolumeRelation::Intersecting);
    EXPECT_EQ(frustum.Classify(Sphere{{0.0F, 0.0F, -0.2F}, 0.05F}), VolumeRelation::Outside);
    EXPECT_EQ(frustum.Classify(Sphere{{0.0F, 0.0F, 1.2F}, 0.05F}), VolumeRelation::Outside);
}

// 切近平面的球不应被剔除（浮点残差 + epsilon 保守语义）。
TEST(FrustumTests, KeepsSphereTouchingNearPlane)
{
    const Frustum frustum = Frustum::FromRowVectorDirect3D(Identity());
    EXPECT_NE(frustum.Classify(Sphere{{0.0F, 0.0F, -0.1F}, 0.1F}), VolumeRelation::Outside);
}

// 半径量级的 epsilon 覆盖：距离比半径小 5e-5 仍在容差带内，不判 Outside。
TEST(FrustumTests, KeepsSphereInsideClassificationEpsilon)
{
    const Frustum frustum = Frustum::FromRowVectorDirect3D(Identity());
    EXPECT_NE(frustum.Classify(Sphere{{0.0F, 0.0F, -0.10005F}, 0.1F}), VolumeRelation::Outside);
}

// NaN 球心必须 fail-fast：病态输入进 culling 会产出不可复现的剔除结果。
TEST(FrustumTests, RejectsNonFiniteSphere)
{
    const Frustum frustum = Frustum::FromRowVectorDirect3D(Identity());
    EXPECT_THROW(frustum.Classify(Sphere{{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.5F}, 0.1F}),
                 std::runtime_error);
}

// 非均匀缩放 (2,3,4) 手算 golden：center = (1,2,3)*world + 平移 = (12,6,-12)，
// 纯 TRS 半径 = 最大基长 4 × 0.5 = 2（不是 Frobenius √(4+9+16) 的保守值）。
TEST(FrustumTests, UsesLargestScaleForOrthogonalNonUniformTransform)
{
    Matrix4 world = Identity();
    world.values[0] = 2.0F;
    world.values[5] = 3.0F;
    world.values[10] = -4.0F;
    world.values[12] = 10.0F;

    const Sphere transformed = TransformSphereConservative(Sphere{{1.0F, 2.0F, 3.0F}, 0.5F}, world);
    EXPECT_FLOAT_EQ(transformed.center.x, 12.0F);
    EXPECT_FLOAT_EQ(transformed.center.y, 6.0F);
    EXPECT_FLOAT_EQ(transformed.center.z, -12.0F);
    EXPECT_FLOAT_EQ(transformed.radius, 2.0F);
}

// 六平面归一化是"dot(n,p)+w = 欧氏距离"语义的前提，逐平面断言 |n| = 1。
TEST(FrustumTests, ExtractedPlanesAreNormalized)
{
    const Frustum frustum = Frustum::FromRowVectorDirect3D(Identity());
    for (const auto plane : frustum.Planes())
    {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        EXPECT_NEAR(length, 1.0F, 1.0e-6F);
    }
}

// ---------------------------------------------------------------------------
// 06 篇：生产级相机矩阵下的分类（手写 LH 矩阵做 CPU 参考，不依赖 RHI/DirectXMath）
// ---------------------------------------------------------------------------

namespace
{
// 手写 PerspectiveFovLH（D3D 深度 [0,1]）：竖直 fov=90°（tan=1）、aspect=1、
// near=0.1、far=100。与 DirectXMath 同式（row-major row-vector）：
//   m00 = xScale = 1/tan(fov/2)/aspect = 1，m11 = yScale = 1
//   m22 = far/(far-near) = 100/99.9
//   m32 = -near*far/(far-near) = -10/99.9（深度偏移在 col3 的 w 分量）
//   m23 = 1（w = z_view，透视除法来源），m33 = 0
// 数值锚点：z_view=near → ndc.z=0；z_view=far → ndc.z=1。
Matrix4 PerspectiveFovLh90()
{
    Matrix4 matrix{};
    matrix.values[0] = 1.0F;
    matrix.values[5] = 1.0F;
    matrix.values[10] = 100.0F / 99.9F; // far / (far - near)
    matrix.values[11] = 1.0F;           // w = z_view
    matrix.values[14] = -10.0F / 99.9F; // -near * far / (far - near)
    matrix.values[15] = 0.0F;
    return matrix;
}

// 手写 LookAtLH：eye=(0,0,-5) 看向 +Z（up=+Y）。视线前方 z_world=-5+距离，
// 故 z_view = z_world + 5；相机前方为 +Z 视空间（LH 约定）。
Matrix4 LookAtLhFromZMinus5()
{
    // view = R（纯旋转）+ 平移行：平移 = -eye * R。R=identity 时即 -eye 行。
    Matrix4 matrix{};
    matrix.values[12] = 0.0F;
    matrix.values[13] = 0.0F;
    matrix.values[14] = 5.0F; // p_view.z = p_world.z + 5
    matrix.values[15] = 1.0F;
    return matrix;
}

// row-vector 矩阵乘法（p * (A*B) = (p*A)*B），供组装 view*projection。
Matrix4 MultiplyRows(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k)
            {
                sum += left.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(k)] *
                       right.values[static_cast<std::size_t>(k) * 4U + static_cast<std::size_t>(column)];
            }
            result.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(column)] = sum;
        }
    }
    return result;
}

// row-vector 变换一个点的 xyz（与引擎 Matrix4 布局一致）。
MiniEngine::World::Float3 Transform(const Matrix4& matrix, const MiniEngine::World::Float3& point)
{
    const float p[4] = {point.x, point.y, point.z, 1.0F};
    MiniEngine::World::Float3 out{};
    out.x = p[0] * matrix.values[0] + p[1] * matrix.values[4] + p[2] * matrix.values[8] + p[3] * matrix.values[12];
    out.y = p[0] * matrix.values[1] + p[1] * matrix.values[5] + p[2] * matrix.values[9] + p[3] * matrix.values[13];
    out.z = p[0] * matrix.values[2] + p[1] * matrix.values[6] + p[2] * matrix.values[10] + p[3] * matrix.values[14];
    return out;
}

// row-vector 变换一个点的 w 分量（透视除法的分母）。
float TransformW(const Matrix4& matrix, const MiniEngine::World::Float3& point)
{
    const float p[4] = {point.x, point.y, point.z, 1.0F};
    return p[0] * matrix.values[3] + p[1] * matrix.values[7] + p[2] * matrix.values[11] + p[3] * matrix.values[15];
}
} // namespace

// 生产相机（LookAtLH × PerspectiveFovLH，fov 90°、near 0.1、far 100、eye z=-5）：
// 分类必须与"点的 clip 坐标"手算一致，而不是只对 identity 矩阵成立。
//   点 (0,0,0)：z_view = 5，ndc.z = (5-0.1)/99 × ... 手算 clip.z/w = (5·f - near·f/(f-near)) / 5
//   其中 f = far/(far-near) = 100/99 → z_ndc = (5·100/99 - 10/99)/5 = (500-10)/495 ≈ 0.9899
//   → 在 [0,1] 内、x/y = 0 → Inside。
TEST(FrustumTests, ClassifiesProductionLookAtPerspectiveCamera)
{
    const Matrix4 viewProjection = MultiplyRows(LookAtLhFromZMinus5(), PerspectiveFovLh90());
    const Frustum frustum = Frustum::FromRowVectorDirect3D(viewProjection);

    // 相机正前方 5 单位处的点在视锥中心 → Inside。
    EXPECT_EQ(frustum.Classify(Sphere{{0.0F, 0.0F, 0.0F}, 0.2F}), VolumeRelation::Inside);
    // 相机后方（z_view < near，w 为负）→ Outside。
    EXPECT_EQ(frustum.Classify(Sphere{{0.0F, 0.0F, -20.0F}, 0.5F}), VolumeRelation::Outside);
    // 深度远超 far（z_view ≈ 205 → ndc.z ≈ 1.0005）→ Outside。
    EXPECT_EQ(frustum.Classify(Sphere{{0.0F, 0.0F, 200.0F}, 0.5F}), VolumeRelation::Outside);
    // fov 90° → 视空间 ±45° 为边界：z_view=100 时 x=100 恰在右边界上 → Intersecting。
    EXPECT_EQ(frustum.Classify(Sphere{{100.0F, 0.0F, 95.0F}, 0.2F}), VolumeRelation::Intersecting);
}

// near 平面来自 col3（06 篇硬契约）：近裁剪面法线必须指向 +Z 视方向。
// 用错误公式（如 OpenGL 的 -col3 或 col4+col3）会在透视下直接翻转 near 语义。
TEST(FrustumTests, NearPlaneComesFromThirdColumn)
{
    const Matrix4 viewProjection = MultiplyRows(LookAtLhFromZMinus5(), PerspectiveFovLh90());
    const Frustum frustum = Frustum::FromRowVectorDirect3D(viewProjection);
    const auto& planes = frustum.Planes();

    // 平面序固定为 Left, Right, Bottom, Top, Near, Far（Frustum.h 契约）。
    // 视锥平面由 world-view-projection 提取，因此平面方程在**世界空间**：
    //   near = col3 归一化后法线 (0,0,1)，平面位于 z_world = eye.z + near = -4.9
    //   → dot(n,p)+w = p.z + 4.9 ≥ 0 ⇔ p.z ≥ -4.9 ⇔ z_view = p.z + 5 ≥ near ✓
    EXPECT_NEAR(planes[4].x, 0.0F, 1.0e-5F);
    EXPECT_NEAR(planes[4].y, 0.0F, 1.0e-5F);
    EXPECT_GT(planes[4].z, 0.99F) << "near 法线必须指向 +Z（视方向），col3 契约";
    EXPECT_NEAR(planes[4].w, 4.9F, 1.0e-3F) << "near 平面世界位置 = eye.z + near = -5 + 0.1";
    // far = col4 - col3：归一化后法线 (0,0,-1)，平面位于 z_world = eye.z + far = 95
    //   → dot(n,p)+w = -p.z + 95 ≥ 0 ⇔ p.z ≤ 95 ⇔ z_view ≤ far ✓
    EXPECT_LT(planes[5].z, -0.99F);
    EXPECT_NEAR(planes[5].w, 95.0F, 1.0e-3F) << "far 平面世界位置 = eye.z + far = -5 + 100";

    // 数值黄金：z_view = near 的点 ndc.z = 0，z_view = far 的点 ndc.z = 1
    // （D3D 深度 [0,1] 映射的端到端确认；错用 OpenGL [-1,1] 会在近处得到 -1）。
    const Float3 clipNear = Transform(viewProjection, Float3{0.0F, 0.0F, -4.9F});
    const float wNear = TransformW(viewProjection, Float3{0.0F, 0.0F, -4.9F});
    EXPECT_NEAR(clipNear.z / wNear, 0.0F, 1.0e-4F);
    const Float3 clipFar = Transform(viewProjection, Float3{0.0F, 0.0F, 95.0F});
    const float wFar = TransformW(viewProjection, Float3{0.0F, 0.0F, 95.0F});
    EXPECT_NEAR(clipFar.z / wFar, 1.0F, 1.0e-4F);
}

// 镜像（负 determinant）与剪切变换下的包围球只要求保守上界：半径可能偏大
// （false positive），但绝不允许偏小（false negative 会漏剔除）。
TEST(FrustumTests, ConservativeSphereUnderMirrorAndShear)
{
    // 镜像：x 基取反。
    Matrix4 mirrored = Identity();
    mirrored.values[0] = -2.0F;
    mirrored.values[5] = 1.0F;
    mirrored.values[10] = 1.0F;
    const Sphere mirroredSphere = TransformSphereConservative(Sphere{{0.0F, 0.0F, 0.0F}, 1.0F}, mirrored);
    EXPECT_GE(mirroredSphere.radius, 2.0F) << "镜像 + 缩放 2 的基长为 2，半径不得小于它";

    // 剪切：world row1 = [1,1,0]（y 基含 x 分量）→ 基不正交，走 Frobenius 上界。
    // Frobenius = sqrt(|row0|²+|row1|²+|row2|²) = sqrt(1 + 2 + 1) = 2，
    // ≥ 真实最大基长 √2 ≈ 1.414（y 基 (1,1,0)）——可能 false positive，不会 false negative。
    Matrix4 shear = Identity();
    shear.values[4] = 1.0F;
    const Sphere shearSphere = TransformSphereConservative(Sphere{{0.0F, 0.0F, 0.0F}, 1.0F}, shear);
    EXPECT_GE(shearSphere.radius, 1.414F) << "上界不得小于真实最大基长 √2";
    EXPECT_LE(shearSphere.radius, 2.001F) << "Frobenius 上界 = √4 = 2";
}

// 相机位于大球内部：任意视锥平面距离都大于 -radius → 必须 Inside/Intersecting，
// 绝不允许 Outside（06 篇「边界与失效」显式条目）。
TEST(FrustumTests, CameraInsideLargeSphereStaysVisible)
{
    const Frustum frustum = Frustum::FromRowVectorDirect3D(Identity());
    // 单位裁剪体中心即"相机"，半径 10 的大球包住整个裁剪体。
    EXPECT_NE(frustum.Classify(Sphere{{0.0F, 0.0F, 0.5F}, 10.0F}), VolumeRelation::Outside);
}
