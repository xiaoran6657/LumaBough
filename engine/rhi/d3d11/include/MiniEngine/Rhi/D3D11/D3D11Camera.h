// ============================================================================
// D3D11Camera.h — 纯 CPU 的左手系视角相机（面向 M2 渲染器）
// 里程碑：M2
// 职责：维护相机位置、偏航/俯仰角与透视投影参数，并导出观察矩阵、投影矩阵和
//   前向向量。本类型是纯 CPU value object，不依赖 GPU，因此可被单元测试直接验证。
//   依赖 DirectXMath 进行向量/矩阵运算。
// 关联：docs/architecture/README.md、docs/architecture/DECISIONS.md
// ============================================================================
#pragma once

#include <DirectXMath.h>

namespace MiniEngine
{
// 视角相机：采用左手坐标系，+Z 为前向，+Y 为世界向上。
//
// 方位角（yaw，绕 Y 轴）与俯仰角（pitch，绕局部 X 轴）共同决定朝向；pitch 被钳制在
// ±(π/2−0.01) 内，既避免俯仰到正上方/正下方导致朝向向量退化，也防止据此求右向量时
// 发生 normalize 除零。修改投影相关参数（SetLens / SetAspect）只会更新成员，投影矩阵
// 在每次 ProjectionMatrix() 调用时按最新参数重建，因此不存在缓存失效问题。
class D3D11Camera
{
  public:
    // 构造默认视角相机；成员默认值即 M2 初始视角（位置 (0, 1.5, -5)，60 度垂直 FOV，
    // 16:9 纵横比，近/远平面 0.1/100，偏航与俯仰均为 0，即朝向 +Z）。
    D3D11Camera();

    // 设置相机世界坐标位置（三个分量均须为有限值）。
    //
    // 参数：
    //   x / y / z —— 世界空间位置分量
    // 失败：任一分量非有限值时由 ME_VERIFY 触发断言崩溃（Debug/Release 均触发）。
    void SetPosition(float x, float y, float z) noexcept;
    // 设置透视投影参数；垂直 FOV、纵横比、近/远平面须满足合法性约束。
    //
    // 参数：
    //   verticalFovRadians —— 垂直视场角（弧度），须在 (0, π) 内
    //   aspect              —— 宽高比（宽度/高度），须为正
    //   nearZ / farZ        —— 近/远平面距离，须为正且 farZ > nearZ
    // 失败：任一参数不合法时由 ME_VERIFY 触发断言崩溃。
    void SetLens(float verticalFovRadians, float aspect, float nearZ, float farZ);
    // 仅更新纵横比（窗口 Resize 时由调用方同步调用），其余投影参数保持不变。
    //
    // 参数：
    //   aspect —— 宽高比（宽度/高度），须为正
    // 失败：aspect 非正或非有限时由 ME_VERIFY 触发断言崩溃。
    void SetAspect(float aspect);
    // 累加偏航角与俯仰角增量，并做规范化与钳制。
    //
    // 偏航角取模回落到 [−π, π)，俯仰角被钳制到 ±(π/2−0.01)，保证朝向向量与
    // 右向量始终可被 normalize。增量须为有限值。
    //
    // 参数：
    //   yawDelta   —— 偏航角增量（弧度）
    //   pitchDelta —— 俯仰角增量（弧度）
    // 失败：任一增量非有限时由 ME_VERIFY 触发断言崩溃。
    void AddYawPitch(float yawDelta, float pitchDelta) noexcept;
    // 沿相机局部坐标轴移动，位移量 = 各轴输入 × 单位（如固定步长内的秒数）。
    //
    // right/up/forward 分别为沿局部右、上、前轴的有符号距离；右轴由
    // cross(worldUp, forward) 归一化得到，pitch 已钳制故该向量非零。增量须为有限值。
    //
    // 参数：
    //   right / up / forward —— 各局部轴方向的移动距离
    // 失败：任一增量非有限时由 ME_VERIFY 触发断言崩溃。
    void MoveLocal(float right, float up, float forward) noexcept;

    // 返回相机世界位置向量。
    [[nodiscard]] DirectX::XMVECTOR PositionVector() const noexcept;
    // 返回相机前向单位向量（由 yaw/pitch 推导，指向 +Z 时（0,0,1））。
    [[nodiscard]] DirectX::XMVECTOR ForwardVector() const noexcept;
    // 返回左手系观察矩阵（XMMatrixLookToLH，基于位置与前向）。
    [[nodiscard]] DirectX::XMMATRIX ViewMatrix() const noexcept;
    // 返回左手系透视投影矩阵（XMMatrixPerspectiveFovLH，基于最新投影参数）。
    [[nodiscard]] DirectX::XMMATRIX ProjectionMatrix() const noexcept;

    // 返回当前纵横比。
    [[nodiscard]] float Aspect() const noexcept;
    // 返回当前偏航角（弧度）。
    [[nodiscard]] float Yaw() const noexcept;
    // 返回当前俯仰角（弧度，已被钳制）。
    [[nodiscard]] float Pitch() const noexcept;

  private:
    // 相机世界位置，默认 (0, 1.5, -5)：位于原点前方 Z 轴上、略高于地面。
    DirectX::XMFLOAT3 m_position{0.0F, 1.5F, -5.0F};
    // 垂直视场角，默认 60 度（弧度）。
    float m_verticalFovRadians = DirectX::XMConvertToRadians(60.0F);
    // 宽高比，默认 16:9。
    float m_aspect = 16.0F / 9.0F;
    // 近平面距离。
    float m_nearZ = 0.1F;
    // 远平面距离。
    float m_farZ = 100.0F;
    // 偏航角（弧度，绕 Y 轴）。
    float m_yaw = 0.0F;
    // 俯仰角（弧度，绕局部 X 轴），钳制在 ±(π/2−0.01)。
    float m_pitch = 0.0F;
};
} // namespace MiniEngine
