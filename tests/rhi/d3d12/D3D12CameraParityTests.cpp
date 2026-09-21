// ============================================================================
// D3D12CameraParityTests.cpp — D3D11 / D3D12 两份相机实现的逐元素等价性
// 里程碑：M5（09 篇 迁移 M4 Pass 与输出一致性；迁移顺序第 3 步的"camera"）
// 职责：把"两个后端的相机必须产出相同的 view/projection"从口头约定变成可执行
//       断言。D3D12Camera 是 D3D11Camera 的同形镜像（本仓库"两个 concrete 后端、
//       不引入共享 RHI 抽象"的既有裁决），镜像本身没问题——**未被检查的镜像**
//       才是风险：一旦有人只改了一侧（例如把 pitch 钳制从 0.01 改成 0.02），
//       表现会是"某个角度下剔除结果不同"，而像素比较只会给出一处说不清来源的差异。
//
// 为什么必须逐元素比较而不是"能画出东西"：相机的 view/projection 直接决定视锥
//       剔除分类（inside / intersecting / outside）。两侧差一个极小量，就可能在
//       包围球恰好贴住视锥平面时给出不同分类，让 RenderPacket 的可见 draw 序列
//       与 sequence hash 对不上——那正是 09 篇的验收项之一。
//
// 口径说明：容差取 EXPECT_FLOAT_EQ（约 4 ULP）。两份实现是逐表达式同源、同一套
//       DirectXMath，实测逐位相同；留 4 ULP 是为了不把"编译器跨 TU 的浮点重结合"
//       误判成缺陷，同时仍足以抓住任何真实的公式漂移（真实漂移远大于 4 ULP）。
// 命名空间偏差（有意）：D3D11Camera 位于 MiniEngine::（M2 时期），本仓库 M5 起
//       D3D12 类型统一在 MiniEngine::Rhi::D3D12。此处不为了对称去改动 M2 的既有
//       命名（属前序代码），偏差记入 09 篇交付报告。
// 关联：docs/architecture/README.md（Parity 步骤）
//       engine/rhi/d3d11/src/D3D11Camera.cpp、engine/rhi/d3d12/src/D3D12Camera.cpp
// ============================================================================
#include <MiniEngine/Rhi/D3D11/D3D11Camera.h>
#include <MiniEngine/Rhi/D3D12/D3D12Camera.h>

#include <DirectXMath.h>

#include <gtest/gtest.h>

#include <cmath>

namespace
{
// 逐元素比较两个 XMMATRIX。先落到 XMFLOAT4X4 再比，避免把 SIMD 寄存器当成可比较值。
void ExpectMatricesNear(const DirectX::XMMATRIX& actual, const DirectX::XMMATRIX& expected, const char* label)
{
    DirectX::XMFLOAT4X4 a{};
    DirectX::XMFLOAT4X4 b{};
    DirectX::XMStoreFloat4x4(&a, actual);
    DirectX::XMStoreFloat4x4(&b, expected);
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            EXPECT_FLOAT_EQ(a.m[row][column], b.m[row][column])
                << label << " 第 (" << row << "," << column << ") 个元素：两侧相机公式出现漂移";
        }
    }
}

void ExpectVectorsNear(const DirectX::XMVECTOR& actual, const DirectX::XMVECTOR& expected, const char* label)
{
    DirectX::XMFLOAT4 a{};
    DirectX::XMFLOAT4 b{};
    DirectX::XMStoreFloat4(&a, actual);
    DirectX::XMStoreFloat4(&b, expected);
    for (int index = 0; index < 4; ++index)
    {
        EXPECT_FLOAT_EQ((&a.x)[index], (&b.x)[index]) << label << " 第 " << index << " 个分量不一致";
    }
}

// 给两份相机施加完全相同的操作序列，然后逐项比较。
struct PoseScript final
{
    float positionX = 0.0F;
    float positionY = 0.0F;
    float positionZ = 0.0F;
    float fovRadians = 0.0F;
    float aspect = 1.0F;
    float nearZ = 0.1F;
    float farZ = 100.0F;
    float yawDelta = 0.0F;
    float pitchDelta = 0.0F;
    float moveRight = 0.0F;
    float moveUp = 0.0F;
    float moveForward = 0.0F;
};

void ApplyScript(MiniEngine::D3D11Camera& camera, const PoseScript& script)
{
    camera.SetPosition(script.positionX, script.positionY, script.positionZ);
    camera.SetLens(script.fovRadians, script.aspect, script.nearZ, script.farZ);
    camera.AddYawPitch(script.yawDelta, script.pitchDelta);
    camera.MoveLocal(script.moveRight, script.moveUp, script.moveForward);
}

void ApplyScript(MiniEngine::Rhi::D3D12::D3D12Camera& camera, const PoseScript& script)
{
    camera.SetPosition(script.positionX, script.positionY, script.positionZ);
    camera.SetLens(script.fovRadians, script.aspect, script.nearZ, script.farZ);
    camera.AddYawPitch(script.yawDelta, script.pitchDelta);
    camera.MoveLocal(script.moveRight, script.moveUp, script.moveForward);
}

// 对一组姿态脚本同时跑两份相机并比较 view / projection / forward。
void ExpectCamerasAgree(const PoseScript& script, const char* label)
{
    MiniEngine::D3D11Camera reference;
    MiniEngine::Rhi::D3D12::D3D12Camera candidate;
    ApplyScript(reference, script);
    ApplyScript(candidate, script);

    ExpectMatricesNear(candidate.ViewMatrix(), reference.ViewMatrix(), label);
    ExpectMatricesNear(candidate.ProjectionMatrix(), reference.ProjectionMatrix(), label);
    ExpectVectorsNear(candidate.ForwardVector(), reference.ForwardVector(), label);
    EXPECT_FLOAT_EQ(candidate.Aspect(), reference.Aspect()) << label << "：纵横比不一致";
}
} // namespace

TEST(D3D12CameraParityTests, DefaultConstructionAgrees)
{
    // 默认构造即 M2 初始视角：两份实现必须从同一姿态出发。
    ExpectCamerasAgree(PoseScript{0.0F, 1.5F, -5.0F, DirectX::XMConvertToRadians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F,
                                  0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
                       "默认姿态");
}

TEST(D3D12CameraParityTests, M4FixedBaselinePoseAgrees)
{
    // M4-09 固定场景的确定性相机位姿：固定截图与 A/B 对照都基于它。
    // 这一条是 09 篇"RenderPacket sequence hash 相同"的直接前提。
    ExpectCamerasAgree(PoseScript{0.0F, 3.0F, -10.0F, DirectX::XMConvertToRadians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F,
                                  0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
                       "M4 固定基线位姿");
}

TEST(D3D12CameraParityTests, RotationAndMovementAgree)
{
    // 非零 yaw/pitch + 三轴移动：覆盖 ForwardVector 的球面展开、右轴叉积与位置累加。
    // pitch 取接近钳制上限的值（1.4 rad < π/2−0.01），顺带验证钳制公式一致。
    ExpectCamerasAgree(PoseScript{-3.5F, 2.25F, 4.75F, DirectX::XMConvertToRadians(75.0F), 21.0F / 9.0F, 0.05F, 250.0F,
                                  1.2345F, 1.4F, -1.5F, 0.75F, 2.5F},
                       "旋转与移动");

    // 负 yaw 与负 pitch：取模方向（std::remainder）在正负两侧必须一致。
    ExpectCamerasAgree(PoseScript{8.0F, -1.0F, -12.0F, DirectX::XMConvertToRadians(45.0F), 4.0F / 3.0F, 0.2F, 80.0F,
                                  -2.9F, -0.6F, 3.0F, -2.0F, -4.0F},
                       "负向旋转与移动");
}

TEST(D3D12CameraParityTests, PitchClampIsIdenticalAtTheLimit)
{
    // 远超钳制上限的 pitch 增量：两份实现必须钳到同一个值——这正是"只改一侧
    // 钳制常量"这类漂移最容易漏掉的用例（表现为同一姿态、不同朝向）。
    ExpectCamerasAgree(PoseScript{1.0F, 1.0F, 1.0F, DirectX::XMConvertToRadians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F,
                                  0.0F, 100.0F, 0.0F, 0.0F, 0.0F},
                       "pitch 正向上钳");
    ExpectCamerasAgree(PoseScript{1.0F, 1.0F, 1.0F, DirectX::XMConvertToRadians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F,
                                  0.0F, -100.0F, 0.0F, 0.0F, 0.0F},
                       "pitch 负向下钳");
}

TEST(D3D12CameraParityTests, ProjectionParameterChangesAgree)
{
    // SetLens 的四种合法参数组合：FOV / 纵横比 / 近远平面各自变化时的投影公式一致。
    const PoseScript wideFov{
        2.0F, 2.0F, 2.0F, DirectX::XMConvertToRadians(30.0F), 16.0F / 9.0F, 0.1F, 100.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    ExpectCamerasAgree(wideFov, "窄 FOV");

    const PoseScript tallAspect{
        2.0F, 2.0F, 2.0F, DirectX::XMConvertToRadians(60.0F), 9.0F / 16.0F, 0.1F, 100.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    ExpectCamerasAgree(tallAspect, "竖向纵横比");

    const PoseScript longRange{2.0F,         2.0F, 2.0F,    DirectX::XMConvertToRadians(60.0F),
                               16.0F / 9.0F, 1.0F, 5000.0F, 0.0F,
                               0.0F,         0.0F, 0.0F,    0.0F};
    ExpectCamerasAgree(longRange, "长近远平面");
}

// ---------------------------------------------------------------------------
// 冻结 golden（审查三-7）：把"两侧一致"升级为"与 M4 基线一致"。
//
// 为什么必须有这一条：`M4FixedBaselinePoseAgrees` 只比较**两份实现**，两侧协同
// 漂移（例如把 FOV 约定、near/far 语义或左手系选择一起改掉）会照样通过；D3D11 侧的
// `D3D11CameraTests` 用的是性质断言（EXPECT_NEAR / EXPECT_LT / isfinite），也没有
// 硬编码期望矩阵。于是"相机数学真的没变"这件事在现有测试里无人看守，只能靠 09 篇
// 末端的图像比较兜住——那时代价已经很大。
//
// 期望值的来源与口径：M4-09 固定相机位姿（位置 (0,3,-10)、朝向 +Z、+Y up、
// FOV 60°、16:9、near 0.1、far 100）下，左手系 `XMMatrixLookToLH` /
// `XMMatrixPerspectiveFovLH` 的**解析解**（row-major、row-vector）：
//   view       —— 姿态无旋转 → 3x3 为单位；平移行 = -dot(axis, eye) = (0,-3,10,1)
//   projection —— m00 = 1/(tan(fov/2)*aspect)、m11 = 1/tan(fov/2)、
//                 m22 = far/(far-near)、m23 = 1、m32 = -near*far/(far-near)、m33 = 0
// 容差用 EXPECT_FLOAT_EQ（约 4 ULP）：解析解与 DirectXMath 的浮点实现只应差末位；
// 真实漂移（改常量、改约定）远大于该量级。
// ---------------------------------------------------------------------------
TEST(D3D12CameraParityTests, M4FixedPoseMatchesFrozenGolden)
{
    MiniEngine::Rhi::D3D12::D3D12Camera camera;
    camera.SetPosition(0.0F, 3.0F, -10.0F);
    camera.SetLens(DirectX::XMConvertToRadians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);

    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMStoreFloat4x4(&view, camera.ViewMatrix());
    DirectX::XMStoreFloat4x4(&projection, camera.ProjectionMatrix());

    // view：无旋转 → 3x3 为单位；平移在**第 3 行**（row-vector 约定：v' = v * M，
    // 平移项落在最后一行，= -dot(axis, eye)）。
    //
    // 初稿曾把平移写进第 3 **列**（`m[1][3]`/`m[2][3]`），被本用例当场抓出：
    // 实测 `m[3][1] = -3`、`m[3][2] = 10`，与 `XMMatrixLookToLH` 的解析解一致。
    // 这说明 golden 用例确实在"独立于两份实现"地看守约定——若只比较两份实现，
    // 这个错误永远不会暴露（两份实现都返回同一个正确矩阵）。
    const float expectedView[4][4] = {
        {1.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 1.0F, 0.0F},
        {0.0F, -3.0F, 10.0F, 1.0F},
    };
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            EXPECT_FLOAT_EQ(view.m[row][column], expectedView[row][column])
                << "view (" << row << "," << column << ")：相机位姿或左手系约定已偏离 M4 基线";
        }
    }

    // 防退化：view 的平移行若被清零，逐元素比较会退化为"只验证一个单位阵"。
    EXPECT_NE(view.m[3][2], 0.0F) << "view 平移行被清零：相机位置未进入观察矩阵";

    // projection：解析解直接写成表达式，避免抄写小数时引入误差。
    const float tangentHalfFov = 0.5773502691896257F; // tan(30°)
    const float expectedProjection[4][4] = {
        {1.0F / (tangentHalfFov * (16.0F / 9.0F)), 0.0F, 0.0F, 0.0F},
        {0.0F, 1.0F / tangentHalfFov, 0.0F, 0.0F},
        {0.0F, 0.0F, 100.0F / (100.0F - 0.1F), 1.0F},
        {0.0F, 0.0F, -(0.1F * 100.0F) / (100.0F - 0.1F), 0.0F},
    };
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            EXPECT_FLOAT_EQ(projection.m[row][column], expectedProjection[row][column])
                << "projection (" << row << "," << column << ")：FOV/纵横比/近远平面约定已偏离 M4 基线";
        }
    }

    // 防退化：上面的逐元素比较若落在"单位阵"上会整体恒真，这里显式排除。
    EXPECT_NE(projection.m[3][2], 0.0F) << "透视投影的平移项为 0 说明退化成了正交投影";
    EXPECT_NE(projection.m[1][1], 1.0F) << "m11 为 1 说明 FOV 约定被改成了 90°";
}
