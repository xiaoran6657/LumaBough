// ============================================================================
// D3D12RootBindings.h — D3D12 Root Signature 绑定契约的唯一真源（register 冻结）
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 3 条）
// 职责：以枚举 + static_assert 冻结 b0—b3 / t0—t8 / s0—s2 三组寄存器与 root
//       parameter 的对应关系。C++ 侧与 HLSL 侧（shaders/d3d12/BindingContract.hlsli）
//       各自静态检查，不引入 shader reflection——register 变化必须同时改两处，
//       编译期/测试期即失败。本文件是纯值契约：不含任何 D3D12 类型名，可被
//       公共头边界接受（与 M4 的 D3D11RenderBindings.h 同形）。
// 与 M4 的关系：三组寄存器与 D3D11 侧完全一致（M5-09 parity 的前提——同一份
//       RenderPacket/材质数据在两个后端必须落到同一组槽位）；差异只在"怎么绑"：
//       D3D11 用 immediate context 逐槽 Set*，D3D12 用 4 个 root CBV + 2 个
//       descriptor table + static samplers（见 05 篇 Root Signature baseline 表）。
// 关联：docs/architecture/README.md（Root Signature baseline）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11RenderBindings.h（M4 对照）
//       shaders/d3d12/BindingContract.hlsli（HLSL 侧同源声明）
// ============================================================================

#pragma once

#include <cstdint>

namespace MiniEngine::Rhi::D3D12
{
// ---------------------------------------------------------------------------
// Root parameter 索引（05 篇 baseline 表的顺序即 root signature 里的顺序）
// ---------------------------------------------------------------------------
enum class RootParameter : std::uint32_t
{
    FrameCbv = 0,     // b0 FrameConstants   （visibility ALL）
    ObjectCbv = 1,    // b1 ObjectConstants  （visibility ALL）
    MaterialCbv = 2,  // b2 MaterialConstants（visibility PIXEL）
    IblCbv = 3,       // b3 IblConstants     （visibility PIXEL）
    MaterialSrvs = 4, // SRV table: t0—t4    （visibility PIXEL）
    GlobalSrvs = 5,   // SRV table: t5—t8    （visibility PIXEL）
    Count = 6
};

// 两组 SRV table 的大小（必须与 HLSL 侧的槽位序号连续区间一致）。
inline constexpr std::uint32_t kMaterialSrvCount = 5U; // t0—t4
inline constexpr std::uint32_t kGlobalSrvCount = 4U;   // t5—t8

// 根 CBV 数量（b0—b3）：05 篇 baseline 表与 M5 README 冻结项都是 4 个。
inline constexpr std::uint32_t kRootConstantBufferCount = 4U;

// static sampler 数量（s0—s2）：M5 不使用 sampler heap（SAMPLER 容量 0）。
inline constexpr std::uint32_t kStaticSamplerCount = 3U;

// ---------------------------------------------------------------------------
// 根 CBV 寄存器（b0—b3），按更新频率分层——与 M4 D3D11 的 ConstantBufferSlot 相同
// ---------------------------------------------------------------------------
enum class ConstantBufferRegister : std::uint32_t
{
    Frame = 0,
    Object = 1,
    Material = 2,
    Ibl = 3
};

// ---------------------------------------------------------------------------
// 材质 SRV 槽位（t0—t4）：table 内的偏移即这里的取值（连续 5 个）
// ---------------------------------------------------------------------------
enum class MaterialSrvRegister : std::uint32_t
{
    BaseColor = 0,
    Normal = 1,
    MetallicRoughness = 2,
    Occlusion = 3,
    Emissive = 4,
    Count = 5
};

// ---------------------------------------------------------------------------
// 全局 SRV 槽位（t5—t8）：IBL 与环境相关，归入 global table（连续 4 个）
// ---------------------------------------------------------------------------
enum class GlobalSrvRegister : std::uint32_t
{
    DiffuseIrradiance = 5,
    PrefilteredSpecular = 6,
    BrdfLut = 7,
    ShadowMap = 8,
    Count = 9
};

// ---------------------------------------------------------------------------
// 采样器槽位（s0—s2）：M5 全部走 static sampler，不进 descriptor heap
// ---------------------------------------------------------------------------
enum class SamplerRegister : std::uint32_t
{
    Material = 0,
    IblAndPost = 1,
    ShadowComparison = 2,
    Count = 3
};

// 枚举 → 寄存器号 / root index 的显式转换：调用处统一走它，避免裸 cast 散落。
constexpr std::uint32_t Register(ConstantBufferRegister value)
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t Register(MaterialSrvRegister value)
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t Register(GlobalSrvRegister value)
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t Register(SamplerRegister value)
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t RootIndex(RootParameter value)
{
    return static_cast<std::uint32_t>(value);
}

// 材质 SRV 在 t 寄存器空间的起点（t0）；global table 的起点由 Irradiance 给出。
constexpr std::uint32_t kMaterialSrvBaseRegister = Register(MaterialSrvRegister::BaseColor);
constexpr std::uint32_t kGlobalSrvBaseRegister = Register(GlobalSrvRegister::DiffuseIrradiance);

// 防回归锚点：00 篇 baseline 表里"语义关键"的编号在这里锁死。若下方断言失败，
// 说明有人改了枚举却忘了同步 root signature 创建代码与 HLSL 声明。
static_assert(RootIndex(RootParameter::FrameCbv) == 0);
static_assert(RootIndex(RootParameter::MaterialSrvs) == 4);
static_assert(RootIndex(RootParameter::GlobalSrvs) == 5);
static_assert(RootIndex(RootParameter::Count) == 6);

static_assert(kRootConstantBufferCount == 4U, "b0-b3 共 4 个 root CBV");
static_assert(Register(ConstantBufferRegister::Ibl) == kRootConstantBufferCount - 1U,
              "b3 必须是最后一个根 CBV（root parameter 0..3 与 b0..b3 一一对应）");

static_assert(Register(MaterialSrvRegister::BaseColor) == 0U);
static_assert(Register(MaterialSrvRegister::Count) == kMaterialSrvCount);
static_assert(Register(GlobalSrvRegister::DiffuseIrradiance) == kMaterialSrvCount, "t5 必须紧跟 t0-t4");
static_assert(Register(GlobalSrvRegister::ShadowMap) == kMaterialSrvCount + kGlobalSrvCount - 1U);
static_assert(Register(GlobalSrvRegister::Count) == kMaterialSrvCount + kGlobalSrvCount);

static_assert(Register(SamplerRegister::Count) == kStaticSamplerCount);
} // namespace MiniEngine::Rhi::D3D12
