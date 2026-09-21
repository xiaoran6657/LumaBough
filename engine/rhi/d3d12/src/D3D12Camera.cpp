// ============================================================================
// D3D12Camera.cpp — 左手系视角相机实现（D3D12 侧）
// 里程碑：M5（09 篇 迁移 M4 Pass 与输出一致性；迁移顺序第 3 步的"camera"）
// 职责：实现 D3D12Camera.h。逐表达式与 D3D11Camera.cpp 同源——这不是"抄一份
//       算了"，而是 parity 的前提：两侧的 view/projection 必须逐位相同，否则
//       视锥剔除会在边界情形给出不同的分类，RenderPacket 的可见序列随之不同。
//       等价性由 D3D12CameraParityTests 直接比较两份实现来锁定。
// 关联：engine/rhi/d3d11/src/D3D11Camera.cpp（同源唯一参照）
// ============================================================================
#include <MiniEngine/Rhi/D3D12/D3D12Camera.h>

#include <MiniEngine/Core/Assert.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// 俯仰角钳制上限：π/2 减 0.01 弧度，避免朝向恰好指向正上方/正下方。
constexpr float kPitchLimit = std::numbers::pi_v<float> * 0.5F - 0.01F;

// 判断单精度值是否为有限值（拒绝 NaN 与无穷）。
bool IsFinite(const float value) noexcept
{
    return std::isfinite(value);
}
} // namespace

D3D12Camera::D3D12Camera() = default;

void D3D12Camera::SetPosition(const float x, const float y, const float z) noexcept
{
    // 位置任何分量非有限都会让后续矩阵运算产生 NaN，因此入参先做断言校验。
    ME_VERIFY(IsFinite(x) && IsFinite(y) && IsFinite(z), "camera position must be finite");
    m_position = {x, y, z};
}

// 一次性设置透视投影参数；每个参数都在写入成员前校验合法性，避免非法值污染后续矩阵。
void D3D12Camera::SetLens(const float verticalFovRadians, const float aspect, const float nearZ, const float farZ)
{
    // 垂直 FOV 必须是 (0, π) 内的有限值，纵横比、近平面须为正，远平面须大于近平面。
    ME_VERIFY(IsFinite(verticalFovRadians) && verticalFovRadians > 0.0F &&
                  verticalFovRadians < std::numbers::pi_v<float>,
              "vertical FOV must be finite and between zero and pi");
    ME_VERIFY(IsFinite(aspect) && aspect > 0.0F, "aspect must be finite and positive");
    ME_VERIFY(IsFinite(nearZ) && nearZ > 0.0F, "near plane must be finite and positive");
    ME_VERIFY(IsFinite(farZ) && farZ > nearZ, "far plane must be finite and greater than near plane");

    m_verticalFovRadians = verticalFovRadians;
    m_aspect = aspect;
    m_nearZ = nearZ;
    m_farZ = farZ;
}

void D3D12Camera::SetAspect(const float aspect)
{
    // 纵横比为正才构成合法投影；窗口尺寸异常时由调用方过滤，此处仍做防御。
    ME_VERIFY(IsFinite(aspect) && aspect > 0.0F, "aspect must be finite and positive");
    m_aspect = aspect;
}

// 累加偏航/俯仰增量，并分别做范围规范化与钳制，保证朝向向量始终可 normalize。
void D3D12Camera::AddYawPitch(const float yawDelta, const float pitchDelta) noexcept
{
    ME_VERIFY(IsFinite(yawDelta) && IsFinite(pitchDelta), "camera rotation delta must be finite");
    // 偏航角取模回落到 [−π, π]，避免长期累积漂移；俯仰角钳制防止翻转与朝向退化。
    m_yaw = std::remainder(m_yaw + yawDelta, std::numbers::pi_v<float> * 2.0F);
    m_pitch = std::clamp(m_pitch + pitchDelta, -kPitchLimit, kPitchLimit);
}

// 沿相机局部坐标轴（右/上/前）移动，位移量 = 轴输入 × 单位；右轴由 worldUp 与前向叉积求得。
void D3D12Camera::MoveLocal(const float right, const float up, const float forward) noexcept
{
    using namespace DirectX;

    ME_VERIFY(IsFinite(right) && IsFinite(up) && IsFinite(forward), "camera movement must be finite");

    // 右轴 = cross(worldUp, forward)，仅保留水平方向的侧向分量，再与上下、前后位移合成。
    const XMVECTOR forwardVector = ForwardVector();
    const XMVECTOR worldUp = XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F);
    // cross(worldUp, forward) 在 pitch 恰为 ±90° 时为零向量，normalize 会产生
    // NaN；AddYawPitch 已把 pitch clamp 到 ±(π/2 - 0.01)，此处必然非零。
    const XMVECTOR rightVector = XMVector3Normalize(XMVector3Cross(worldUp, forwardVector));
    const XMVECTOR displacement =
        XMVectorScale(rightVector, right) + XMVectorScale(worldUp, up) + XMVectorScale(forwardVector, forward);

    XMVECTOR position = XMLoadFloat3(&m_position);
    position += displacement;
    XMStoreFloat3(&m_position, position);
}

DirectX::XMVECTOR D3D12Camera::PositionVector() const noexcept
{
    return DirectX::XMLoadFloat3(&m_position);
}

// 由 yaw/pitch 展开为球面坐标单位方向，即相机前向。
DirectX::XMVECTOR D3D12Camera::ForwardVector() const noexcept
{
    // 由 yaw/pitch 展开为球面坐标方向：x=sin(yaw)*cos(pitch)，y=sin(pitch)，
    // z=cos(yaw)*cos(pitch)；取模后即单位前向。yaw=0、pitch=0 时指向 +Z。
    const float cosPitch = std::cos(m_pitch);
    return DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(std::sin(m_yaw) * cosPitch, std::sin(m_pitch), std::cos(m_yaw) * cosPitch, 0.0F));
}

// 用当前位置与前向构造左手观察矩阵（XMMatrixLookToLH）。
DirectX::XMMATRIX D3D12Camera::ViewMatrix() const noexcept
{
    // 左手观察矩阵：以位置为视点、前向为视线、世界 +Y 为向上。
    const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F);
    return DirectX::XMMatrixLookToLH(PositionVector(), ForwardVector(), up);
}

// 按当前投影参数构造左手透视投影矩阵（XMMatrixPerspectiveFovLH）。
DirectX::XMMATRIX D3D12Camera::ProjectionMatrix() const noexcept
{
    // 左手透视投影，使用最新 FOV/纵横比/近远平面。
    return DirectX::XMMatrixPerspectiveFovLH(m_verticalFovRadians, m_aspect, m_nearZ, m_farZ);
}

float D3D12Camera::Aspect() const noexcept
{
    return m_aspect;
}

float D3D12Camera::Yaw() const noexcept
{
    return m_yaw;
}

float D3D12Camera::Pitch() const noexcept
{
    return m_pitch;
}
} // namespace MiniEngine::Rhi::D3D12
