// ============================================================================
// D3D11RenderBindings.h — M4 固定 binding table 的唯一真源（slot 契约冻结）
// 里程碑：M4（01 篇架构边界与渲染顺序；手抄清单第 2 条）
// 职责：以枚举 + static_assert 冻结 PBR/shadow/IBL 各 pass 的常量缓冲、
//       SRV 与采样器槽位。C++ 与 HLSL 各自静态对照，不引入 shader
//       reflection framework——slot 编号变化必须同步修改 shaders/d3d11/
//       下的寄存器声明（b/t/s 寄存器），编译期由 01 篇表格人工核对。
// 关联：docs/architecture/README.md（Binding table 表格）
//       shaders/d3d11/PbrCommon.hlsli（HLSL 侧同源声明）
// ============================================================================

#pragma once

#include <cstdint>

namespace MiniEngine::Rhi::D3D11
{
// 常量缓冲槽位（b0–b3），按更新频率分层：
//   b0 Frame    —— 每帧一次：camera、light、exposure、debug mode
//   b1 Object   —— 每 Draw 一次：world、normal matrix、light world-view-projection
//                  （PBR 与 Shadow shader 的 b1 布局必须逐字节一致）
//   b2 Material —— 材质变化时
//   b3 Ibl      —— environment 变化时
enum class ConstantBufferSlot : std::uint32_t
{
    Frame = 0,
    Object = 1,
    Material = 2,
    Ibl = 3
};

// Pixel Shader SRV 槽位（t0–t8），每个槽位必须绑定默认资源避免 unbound 读取：
//   t0 baseColor            —— white sRGB
//   t1 normal               —— (0.5, 0.5, 1) linear
//   t2 metallicRoughness    —— G=1、B=1 linear
//   t3 occlusion            —— white linear
//   t4 emissive             —— black sRGB
//   t5 diffuse irradiance cube —— black cube
//   t6 prefiltered specular cube —— black cube
//   t7 BRDF LUT             —— valid neutral LUT
//   t8 shadow map           —— fully-lit fallback
enum class PbrSrvSlot : std::uint32_t
{
    BaseColor = 0,
    Normal = 1,
    MetallicRoughness = 2,
    Occlusion = 3,
    Emissive = 4,
    DiffuseIrradiance = 5,
    PrefilteredSpecular = 6,
    BrdfLut = 7,
    ShadowMap = 8,
    Count = 9
};

// 采样器槽位（s0–s2）：
//   s0 材质各向异性 wrap；s1 IBL/后处理线性 clamp；s2 comparison + border shadow。
enum class SamplerSlot : std::uint32_t
{
    Material = 0,
    IblAndPost = 1,
    ShadowComparison = 2
};

// 枚举 → 寄存器号的显式转换：调用处统一走它，避免裸 cast 散落
// （M4-02 补全三组槽位的重载；枚举取值本身仍是冻结契约）。
constexpr std::uint32_t Slot(PbrSrvSlot value)
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t Slot(ConstantBufferSlot value)
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t Slot(SamplerSlot value)
{
    return static_cast<std::uint32_t>(value);
}

// 防回归锚点：表格中"语义关键"的编号在这里锁死；
// 若下方断言失败，说明有人改了枚举但没同步 HLSL 寄存器声明。
static_assert(Slot(PbrSrvSlot::ShadowMap) == 8);
static_assert(Slot(PbrSrvSlot::Count) == 9);
} // namespace MiniEngine::Rhi::D3D11
