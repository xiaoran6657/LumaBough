// ============================================================================
// RootBindingsContractTests.cpp — b/t/s 绑定契约的纯 CPU 对照测试
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 3 条）
// 职责：把"两个后端寄存器编号必须一致"这条 parity 前提变成可执行断言：
//       D3D12 侧 D3D12RootBindings.h 与 M4 冻结的 D3D11RenderBindings.h 逐项
//       比较（常量缓冲、SRV、采样器三组），任何一侧改动而另一侧没跟上都会
//       在本用例失败——这正是 M5-09 parity 的第一道防线。
// 说明：两个头都是纯常量/枚举（无 D3D 类型），因此本用例不需要设备。
// 关联：docs/architecture/README.md（与 M4 逐寄存器一致）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11RenderBindings.h（M4 对照）
// ============================================================================
#include <MiniEngine/Rhi/D3D11/D3D11RenderBindings.h>
#include <MiniEngine/Rhi/D3D12/D3D12RootBindings.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace D11 = MiniEngine::Rhi::D3D11;
namespace D12 = MiniEngine::Rhi::D3D12;

// name="b0—b3"：常量缓冲槽位在两个后端必须同号同名（同一份数据落同一槽位）。
TEST(RootBindingsContractTests, ConstantBufferSlotsMatchD3D11)
{
    EXPECT_EQ(D12::Register(D12::ConstantBufferRegister::Frame), D11::Slot(D11::ConstantBufferSlot::Frame));
    EXPECT_EQ(D12::Register(D12::ConstantBufferRegister::Object), D11::Slot(D11::ConstantBufferSlot::Object));
    EXPECT_EQ(D12::Register(D12::ConstantBufferRegister::Material), D11::Slot(D11::ConstantBufferSlot::Material));
    EXPECT_EQ(D12::Register(D12::ConstantBufferRegister::Ibl), D11::Slot(D11::ConstantBufferSlot::Ibl));
}

// name="t0—t8"：九个 SRV 槽位逐一对照（材质表 5 + 全局表 4）。
TEST(RootBindingsContractTests, SrvSlotsMatchD3D11)
{
    EXPECT_EQ(D12::Register(D12::MaterialSrvRegister::BaseColor), D11::Slot(D11::PbrSrvSlot::BaseColor));
    EXPECT_EQ(D12::Register(D12::MaterialSrvRegister::Normal), D11::Slot(D11::PbrSrvSlot::Normal));
    EXPECT_EQ(D12::Register(D12::MaterialSrvRegister::MetallicRoughness),
              D11::Slot(D11::PbrSrvSlot::MetallicRoughness));
    EXPECT_EQ(D12::Register(D12::MaterialSrvRegister::Occlusion), D11::Slot(D11::PbrSrvSlot::Occlusion));
    EXPECT_EQ(D12::Register(D12::MaterialSrvRegister::Emissive), D11::Slot(D11::PbrSrvSlot::Emissive));
    EXPECT_EQ(D12::Register(D12::GlobalSrvRegister::DiffuseIrradiance), D11::Slot(D11::PbrSrvSlot::DiffuseIrradiance));
    EXPECT_EQ(D12::Register(D12::GlobalSrvRegister::PrefilteredSpecular),
              D11::Slot(D11::PbrSrvSlot::PrefilteredSpecular));
    EXPECT_EQ(D12::Register(D12::GlobalSrvRegister::BrdfLut), D11::Slot(D11::PbrSrvSlot::BrdfLut));
    EXPECT_EQ(D12::Register(D12::GlobalSrvRegister::ShadowMap), D11::Slot(D11::PbrSrvSlot::ShadowMap));
}

// name="s0—s2"：三组采样器槽位对照（D3D12 侧用 static sampler，编号不变）。
TEST(RootBindingsContractTests, SamplerSlotsMatchD3D11)
{
    EXPECT_EQ(D12::Register(D12::SamplerRegister::Material), D11::Slot(D11::SamplerSlot::Material));
    EXPECT_EQ(D12::Register(D12::SamplerRegister::IblAndPost), D11::Slot(D11::SamplerSlot::IblAndPost));
    EXPECT_EQ(D12::Register(D12::SamplerRegister::ShadowComparison), D11::Slot(D11::SamplerSlot::ShadowComparison));
}

// table 的连续性与起点：material 表 5 个从 t0 起、global 表 4 个从 t5 起，
// 两段必须首尾相接（不能有洞，否则 HLSL 里的 t 号与 range 定义不一致）。
TEST(RootBindingsContractTests, SrvTablesAreContiguousAndStartWhereDeclared)
{
    EXPECT_EQ(D12::kMaterialSrvCount, 5U);
    EXPECT_EQ(D12::kGlobalSrvCount, 4U);
    EXPECT_EQ(D12::kMaterialSrvBaseRegister, 0U) << "material 表必须从 t0 开始";
    EXPECT_EQ(D12::kGlobalSrvBaseRegister, D12::kMaterialSrvCount) << "global 表必须紧接 material 表（t5）";
    EXPECT_EQ(D12::Register(D12::MaterialSrvRegister::Count), D12::kMaterialSrvCount);
    EXPECT_EQ(D12::Register(D12::GlobalSrvRegister::Count), D12::kMaterialSrvCount + D12::kGlobalSrvCount)
        << "t 槽位总数必须等于两表之和（9）";
}

// root parameter 索引与两类资源的对应：0—3 是根 CBV，4/5 是两张 SRV table。
TEST(RootBindingsContractTests, RootParameterIndicesMatchDeclaredResources)
{
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::FrameCbv), D12::Register(D12::ConstantBufferRegister::Frame));
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::ObjectCbv), D12::Register(D12::ConstantBufferRegister::Object));
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::MaterialCbv), D12::Register(D12::ConstantBufferRegister::Material));
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::IblCbv), D12::Register(D12::ConstantBufferRegister::Ibl));
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::MaterialSrvs), D12::kRootConstantBufferCount);
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::GlobalSrvs), D12::kRootConstantBufferCount + 1U);
    EXPECT_EQ(D12::RootIndex(D12::RootParameter::Count), D12::kRootConstantBufferCount + 2U);
}

// 采样器数量与 static sampler 数量一致（M5 不创建 SAMPLER heap，见 heap profile 测试）。
TEST(RootBindingsContractTests, StaticSamplerCountMatchesSamplerSlots)
{
    EXPECT_EQ(D12::kStaticSamplerCount, 3U);
    EXPECT_EQ(D12::Register(D12::SamplerRegister::Count), D12::kStaticSamplerCount);
}
