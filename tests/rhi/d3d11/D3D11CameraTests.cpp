// ============================================================================
// D3D11CameraTests.cpp — D3D11Camera 纯数学行为的单元测试
// 里程碑：M2
// 职责：在不依赖 GPU 的前提下验证 D3D11Camera 的默认朝向、移动、旋转钳制、观察矩阵
//   与投影矩阵等契约，含非法入参的死亡测试（依赖 ME_VERIFY 在 Debug/Release 均崩溃）。
// 关联：docs/architecture/README.md、docs/architecture/DECISIONS.md
// ============================================================================
#include <MiniEngine/Rhi/D3D11/D3D11Camera.h>

#include <DirectXMath.h>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

// 死亡测试依赖 M1 契约 ME_VERIFY（MiniEngine/Core/Assert.h）在 Debug/Release
// 均触发崩溃；M1 实现满足该契约（ME_VERIFY 无条件 FailAssertion）。
namespace
{
// 向量近似比较的容差，覆盖浮点累积误差。
constexpr float kEpsilon = 1.0e-4F;

// 断言两个三维向量各分量近似相等（忽略 w 分量）。
void ExpectVector3Near(const DirectX::XMVECTOR actual, const DirectX::XMVECTOR expected)
{
    EXPECT_NEAR(DirectX::XMVectorGetX(actual), DirectX::XMVectorGetX(expected), kEpsilon);
    EXPECT_NEAR(DirectX::XMVectorGetY(actual), DirectX::XMVectorGetY(expected), kEpsilon);
    EXPECT_NEAR(DirectX::XMVectorGetZ(actual), DirectX::XMVectorGetZ(expected), kEpsilon);
}
} // namespace

// 默认视角（yaw=pitch=0）下相机应朝向 +Z。
TEST(D3D11CameraTests, DefaultForwardPointsAlongPositiveZ)
{
    const MiniEngine::D3D11Camera camera;
    ExpectVector3Near(camera.ForwardVector(), DirectX::XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F));
}

// MoveLocal 的 forward 应沿当前前向位移，而非固定世界方向。
TEST(D3D11CameraTests, MoveLocalForwardUsesCurrentForward)
{
    MiniEngine::D3D11Camera camera;
    camera.SetPosition(0.0F, 0.0F, 0.0F);
    camera.AddYawPitch(std::numbers::pi_v<float> * 0.5F, 0.0F);
    camera.MoveLocal(0.0F, 0.0F, 2.0F);

    // 偏航 90 度后前向指向 +X，前进 2 单位应落到 (2, 0, 0)。
    ExpectVector3Near(camera.PositionVector(), DirectX::XMVectorSet(2.0F, 0.0F, 0.0F, 0.0F));
}

// 偏航 90 度（π/2 弧度）后前向应指向 +X。
TEST(D3D11CameraTests, YawNinetyDegreesPointsAlongPositiveX)
{
    MiniEngine::D3D11Camera camera;
    camera.AddYawPitch(std::numbers::pi_v<float> * 0.5F, 0.0F);
    ExpectVector3Near(camera.ForwardVector(), DirectX::XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F));
}

// 俯仰角被钳制在 ±(π/2−0.01) 内，且观察矩阵在极端输入下仍保持有限值。
TEST(D3D11CameraTests, PitchIsClampedAndViewMatrixRemainsFinite)
{
    MiniEngine::D3D11Camera camera;
    camera.AddYawPitch(0.0F, 100.0F);

    DirectX::XMFLOAT4X4 view{};
    DirectX::XMStoreFloat4x4(&view, camera.ViewMatrix());

    // 矩阵所有元素均须有限，验证钳制有效避免了朝向退化导致的 NaN。
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            EXPECT_TRUE(std::isfinite(view.m[row][column]));
        }
    }
    EXPECT_LT(camera.Pitch(), std::numbers::pi_v<float> * 0.5F);
}

// 观察矩阵应把相机位置映射到视图空间原点。
TEST(D3D11CameraTests, ViewMatrixMapsCameraPositionToOrigin)
{
    MiniEngine::D3D11Camera camera;
    camera.SetPosition(2.0F, 3.0F, -4.0F);

    const DirectX::XMVECTOR viewSpacePosition =
        DirectX::XMVector3TransformCoord(camera.PositionVector(), camera.ViewMatrix());
    ExpectVector3Near(viewSpacePosition, DirectX::XMVectorZero());
}

// 更宽的纵横比应减小水平投影缩放（矩阵 _11），垂直方向不变（_22）。
TEST(D3D11CameraTests, WiderAspectReducesHorizontalProjectionScale)
{
    MiniEngine::D3D11Camera camera;
    camera.SetAspect(4.0F / 3.0F);
    DirectX::XMFLOAT4X4 fourByThree{};
    DirectX::XMStoreFloat4x4(&fourByThree, camera.ProjectionMatrix());

    camera.SetAspect(16.0F / 9.0F);
    DirectX::XMFLOAT4X4 sixteenByNine{};
    DirectX::XMStoreFloat4x4(&sixteenByNine, camera.ProjectionMatrix());

    EXPECT_LT(sixteenByNine._11, fourByThree._11);
    EXPECT_NEAR(sixteenByNine._22, fourByThree._22, kEpsilon);
}

// 非正纵横比应触发断言崩溃。
TEST(D3D11CameraDeathTests, RejectsInvalidAspect)
{
    EXPECT_DEATH(
        {
            MiniEngine::D3D11Camera camera;
            camera.SetAspect(0.0F);
        },
        "aspect");
}

// 非有限纵横比（NaN）应触发断言崩溃。
TEST(D3D11CameraDeathTests, RejectsNonFiniteAspect)
{
    EXPECT_DEATH(
        {
            MiniEngine::D3D11Camera camera;
            camera.SetAspect(std::numeric_limits<float>::quiet_NaN());
        },
        "aspect");
}

// farZ 不大于 nearZ 的投影参数应触发断言崩溃。
TEST(D3D11CameraDeathTests, RejectsInvalidLensPlanes)
{
    EXPECT_DEATH(
        {
            MiniEngine::D3D11Camera camera;
            camera.SetLens(std::numbers::pi_v<float> / 3.0F, 1.0F, 1.0F, 1.0F);
        },
        "far plane");
}

// 非有限移动增量应触发断言崩溃。
TEST(D3D11CameraDeathTests, RejectsNonFiniteMovement)
{
    EXPECT_DEATH(
        {
            MiniEngine::D3D11Camera camera;
            camera.MoveLocal(std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F);
        },
        "movement");
}
