// ============================================================================
// D3D12Camera.h — 纯 CPU 的左手系视角相机（D3D12 侧）
// 里程碑：M5（09 篇 迁移 M4 Pass 与输出一致性；迁移顺序第 3 步的"camera"）
// 职责：与 Rhi::D3D11::D3D11Camera **同源**的相机：维护位置、偏航/俯仰角与透视
//       投影参数，导出观察矩阵、投影矩阵与前向向量。纯 CPU value object，不依赖
//       GPU，因此可被单元测试直接验证。
// 为什么要有第二份而不是共用 D3D11 的那一份：本仓库的既有裁决是"两个后端各自
//       concrete、命名与目录同形"（M5-01：D3D12 后端不引入 IRenderDevice，也不
//       反向依赖 D3D11 目录）。相机虽为纯 CPU，但它是**渲染语义**的一部分
//       （左手系、D3D 深度 0..1、+Z 前向），把它塞进 Core/World 会改变既有分层。
//       因此这里做同形镜像，并新增 D3D12CameraParityTests 把"两份必须逐位等价"
//       变成可执行断言——重复本身不是风险，"未被检查的重复"才是。
// 为什么 parity 必须锁：相机的 view/projection 直接决定视锥剔除结果，进而决定
//       RenderPacket 的可见 draw 序列；两侧差一个 1e-7 就可能在球体恰好贴住
//       视锥平面时给出不同的 inside/intersecting 分类，让 sequence hash 对不上。
// 关联：docs/architecture/README.md（Parity 步骤：camera 必须相同）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11Camera.h（同源唯一参照）
// ============================================================================
#pragma once

#include <DirectXMath.h>

namespace MiniEngine::Rhi::D3D12
{
// 视角相机：采用左手坐标系，+Z 为前向，+Y 为世界向上。
//
// 方位角（yaw，绕 Y 轴）与俯仰角（pitch，绕局部 X 轴）共同决定朝向；pitch 被钳制在
// ±(π/2−0.01) 内，既避免俯仰到正上方/正下方导致朝向向量退化，也防止据此求右向量时
// 发生 normalize 除零。修改投影相关参数（SetLens / SetAspect）只会更新成员，投影矩阵
// 在每次 ProjectionMatrix() 调用时按最新参数重建，因此不存在缓存失效问题。
class D3D12Camera
{
  public:
    // 构造默认视角相机；成员默认值即 M2 初始视角（位置 (0, 1.5, -5)，60 度垂直 FOV，
    // 16:9 纵横比，近/远平面 0.1/100，偏航与俯仰均为 0，即朝向 +Z）。
    D3D12Camera();

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
} // namespace MiniEngine::Rhi::D3D12
