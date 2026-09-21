// ============================================================================
// D3D12RootSignatureDeviceTests.cpp — Root Signature 布局与绑定契约一致性
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature）
// 职责：把"Root Signature 与 b/t/s contract 一致"变成可执行断言。做法不是抄一份
//       期望值，而是**序列化生产用的同一份 description 再反序列化**，逐项核对
//       root parameter 的类型/可见性/寄存器、两张 SRV table 的 range 与起点、
//       三个 static sampler 的寄存器与关键参数、以及 root signature flags。
//       任何"实现改了但契约没跟上"（或反之）都会在这里失败。
// 环境：需要 D3D12 硬件或 WARP。
// 关联：docs/architecture/README.md（Root Signature baseline）
//       engine/rhi/d3d12/include/MiniEngine/Rhi/D3D12/D3D12RootBindings.h（契约真源）
// ============================================================================
#include "D3D12RootSignature.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <memory>

namespace D12 = MiniEngine::Rhi::D3D12;

namespace
{
struct RootSignatureHarness final
{
    std::unique_ptr<D12::D3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    D12::RootSignatureFacts facts;

    RootSignatureHarness()
    {
        D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = D12::D3D12Device::Create(options);
        rootSignature = D12::CreateM5RootSignature(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()), facts);
    }
};
} // namespace

// 创建 Gate：root parameter / static sampler 数量与 flags 与契约一致。
TEST(D3D12RootSignatureDeviceTests, CreatesBaselineRootSignatureWithExpectedShape)
{
    RootSignatureHarness harness;
    ASSERT_NE(harness.rootSignature.Get(), nullptr);

    EXPECT_EQ(harness.facts.rootParameterCount, D12::RootIndex(D12::RootParameter::Count));
    EXPECT_EQ(harness.facts.rootParameterCount, 6U);
    EXPECT_EQ(harness.facts.staticSamplerCount, D12::kStaticSamplerCount);
    EXPECT_GT(harness.facts.serializedBytes, 0U);
    EXPECT_EQ(harness.facts.flags,
              static_cast<std::uint32_t>(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                         D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                         D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS));
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "创建 root signature 不得产生调试层消息";
}

// 反序列化布局校验：这是"root signature 与 b/t/s contract 一致"的直接证据。
TEST(D3D12RootSignatureDeviceTests, SerializedLayoutMatchesBindingContract)
{
    RootSignatureHarness harness;

    // 用生产装配函数重建同一份 description（与 CreateM5RootSignature 共用代码）。
    D12::RootSignatureLayoutStorage storage{};
    const D3D12_ROOT_SIGNATURE_DESC description = D12::BuildM5RootSignatureDescription(storage);

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    ASSERT_TRUE(
        SUCCEEDED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)));

    Microsoft::WRL::ComPtr<ID3D12RootSignatureDeserializer> deserializer;
    ASSERT_TRUE(SUCCEEDED(D3D12CreateRootSignatureDeserializer(
        serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&deserializer))));
    const D3D12_ROOT_SIGNATURE_DESC* const layout = deserializer->GetRootSignatureDesc();
    ASSERT_NE(layout, nullptr);

    // 1) 总量与 flags 一致。
    ASSERT_EQ(layout->NumParameters, D12::RootIndex(D12::RootParameter::Count));
    ASSERT_EQ(layout->NumStaticSamplers, D12::kStaticSamplerCount);
    EXPECT_EQ(layout->Flags, description.Flags);

    // 2) root CBV b0—b3：类型、寄存器、可见性（b0/b1 = ALL，b2/b3 = PIXEL）。
    for (std::uint32_t index = 0; index < D12::kRootConstantBufferCount; ++index)
    {
        const D3D12_ROOT_PARAMETER& parameter = layout->pParameters[index];
        EXPECT_EQ(parameter.ParameterType, D3D12_ROOT_PARAMETER_TYPE_CBV) << "root " << index;
        EXPECT_EQ(parameter.Descriptor.ShaderRegister, index) << "root " << index << " 必须是 b" << index;
        EXPECT_EQ(parameter.Descriptor.RegisterSpace, 0U);
        const D3D12_SHADER_VISIBILITY expected =
            index < 2U ? D3D12_SHADER_VISIBILITY_ALL : D3D12_SHADER_VISIBILITY_PIXEL;
        EXPECT_EQ(parameter.ShaderVisibility, expected) << "root " << index;
    }

    // 3) 材质表：root 4，单个 range，SRV × 5 从 t0 起、表内偏移 0。
    const D3D12_ROOT_PARAMETER& materialRoot = layout->pParameters[D12::RootIndex(D12::RootParameter::MaterialSrvs)];
    EXPECT_EQ(materialRoot.ParameterType, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    EXPECT_EQ(materialRoot.ShaderVisibility, D3D12_SHADER_VISIBILITY_PIXEL);
    ASSERT_EQ(materialRoot.DescriptorTable.NumDescriptorRanges, 1U);
    EXPECT_EQ(materialRoot.DescriptorTable.pDescriptorRanges[0].RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
    EXPECT_EQ(materialRoot.DescriptorTable.pDescriptorRanges[0].NumDescriptors, D12::kMaterialSrvCount);
    EXPECT_EQ(materialRoot.DescriptorTable.pDescriptorRanges[0].BaseShaderRegister, 0U) << "材质表必须从 t0 开始";
    // 表内偏移用 OFFSET_APPEND：序列化后保留哨兵值（0xFFFFFFFF）而非 0；每个 table
    // 只有一个 range，APPEND 在语义上就是表内偏移 0（HLSL 的 t 号由 BaseShaderRegister
    // 决定），因此这里接受"0 或 APPEND"两种表示。
    const UINT materialOffset = materialRoot.DescriptorTable.pDescriptorRanges[0].OffsetInDescriptorsFromTableStart;
    EXPECT_TRUE(materialOffset == 0U || materialOffset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
        << "材质表 range 的表内偏移必须是 0（或等价的 APPEND），实际 " << materialOffset;

    // 4) 全局表：root 5，单个 range，SRV × 4 从 t5 起（紧接材质表）。
    const D3D12_ROOT_PARAMETER& globalRoot = layout->pParameters[D12::RootIndex(D12::RootParameter::GlobalSrvs)];
    EXPECT_EQ(globalRoot.ParameterType, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    EXPECT_EQ(globalRoot.ShaderVisibility, D3D12_SHADER_VISIBILITY_PIXEL);
    ASSERT_EQ(globalRoot.DescriptorTable.NumDescriptorRanges, 1U);
    EXPECT_EQ(globalRoot.DescriptorTable.pDescriptorRanges[0].NumDescriptors, D12::kGlobalSrvCount);
    EXPECT_EQ(globalRoot.DescriptorTable.pDescriptorRanges[0].BaseShaderRegister, D12::kMaterialSrvCount)
        << "全局表必须从 t5 开始";
    const UINT globalOffset = globalRoot.DescriptorTable.pDescriptorRanges[0].OffsetInDescriptorsFromTableStart;
    EXPECT_TRUE(globalOffset == 0U || globalOffset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
        << "全局表 range 的表内偏移必须是 0（或等价的 APPEND），实际 " << globalOffset;

    // 5) static samplers：s0 linear wrap、s1 linear clamp、s2 comparison border white LESS_EQUAL。
    for (std::uint32_t index = 0; index < layout->NumStaticSamplers; ++index)
    {
        EXPECT_EQ(layout->pStaticSamplers[index].ShaderRegister, index) << "static sampler 顺序必须与 s0—s2 一致";
        EXPECT_EQ(layout->pStaticSamplers[index].RegisterSpace, 0U);
        EXPECT_EQ(layout->pStaticSamplers[index].ShaderVisibility, D3D12_SHADER_VISIBILITY_PIXEL);
    }
    const D3D12_STATIC_SAMPLER_DESC& s0 = layout->pStaticSamplers[0];
    EXPECT_EQ(s0.Filter, D3D12_FILTER_MIN_MAG_MIP_LINEAR) << "s0 与 M4 D3D11 的材质采样器同语义（linear）";
    EXPECT_EQ(s0.AddressU, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const D3D12_STATIC_SAMPLER_DESC& s1 = layout->pStaticSamplers[1];
    EXPECT_EQ(s1.Filter, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    EXPECT_EQ(s1.AddressU, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    // s0/s1 的 MaxLOD 与 M4 一致（FLOAT32_MAX：材质贴图与 IBL 都用完整 mip 链）。
    EXPECT_EQ(s0.MinLOD, 0.0F);
    EXPECT_EQ(s0.MaxLOD, D3D12_FLOAT32_MAX);
    EXPECT_EQ(s1.MinLOD, 0.0F);
    EXPECT_EQ(s1.MaxLOD, D3D12_FLOAT32_MAX);

    const D3D12_STATIC_SAMPLER_DESC& s2 = layout->pStaticSamplers[2];
    EXPECT_EQ(s2.Filter, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    EXPECT_EQ(s2.AddressU, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    EXPECT_EQ(s2.BorderColor, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE) << "阴影图外必须返回完全照亮（1.0）";
    EXPECT_EQ(s2.ComparisonFunc, D3D12_COMPARISON_FUNC_LESS_EQUAL);
    // LOD 与 M4 D3D11 的 shadow sampler 对齐（MinLOD=0、MaxLOD=0）：1 mip 下行为等价，
    // 但 M5-09 parity 要求同一采样参数；此处显式断言，防止"改了 MaxLOD 没人发现"。
    EXPECT_EQ(s2.MinLOD, 0.0F);
    EXPECT_EQ(s2.MaxLOD, 0.0F) << "必须与 M4 D3D11 的 shadow sampler 一致（不是 FLOAT32_MAX）";

    // 6) 反序列化布局与创建时的事实一致（防止两条路径跑偏）。
    EXPECT_EQ(layout->NumParameters, harness.facts.rootParameterCount);
    EXPECT_EQ(layout->NumStaticSamplers, harness.facts.staticSamplerCount);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 运行时去重行为（实测，2026-09-10）：**内容相同**的 root signature 两次创建会返回
// 同一个对象（指针相等），这是 D3D12 runtime 的内部缓存，不是我们的缺陷。
//
// 为什么必须锁定它：与 M4-03 的栅格化状态去重同型——同一对象被设置**不同**的调试名
// 会触发 SETPRIVATEDATA_CHANGINGPARAMS warning。因此 M5 的 root signature 名必须是
// 稳定常量（当前为 M5.D3D12.RootSignature），不允许按"第 N 个"命名；本用例同时证明
// 重复创建（含重复设置同名）不产生任何调试层消息。
TEST(D3D12RootSignatureDeviceTests, IdenticalDescriptionsAreDeduplicatedByRuntime)
{
    RootSignatureHarness first;
    D12::RootSignatureFacts facts{};
    const Microsoft::WRL::ComPtr<ID3D12RootSignature> second =
        D12::CreateM5RootSignature(*static_cast<ID3D12Device*>(first.device->NativeDeviceHandle()), facts);

    ASSERT_NE(second.Get(), nullptr);
    EXPECT_EQ(second.Get(), first.rootSignature.Get())
        << "内容相同的 root signature 由运行时去重为同一对象（因此调试名必须稳定）";
    EXPECT_EQ(facts.rootParameterCount, first.facts.rootParameterCount);
    EXPECT_EQ(facts.staticSamplerCount, first.facts.staticSamplerCount);
    EXPECT_EQ(facts.serializedBytes, first.facts.serializedBytes) << "同一描述必须序列化出同样大小";
    EXPECT_FALSE(first.device->DrainInfoQueue().HasFailure())
        << "重复创建 + 重复设置同一调试名不得产生消息（若有，说明命名不再稳定）";
}