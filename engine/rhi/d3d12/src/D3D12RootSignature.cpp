// ============================================================================
// D3D12RootSignature.cpp — Root Signature 1.0 创建与 static sampler 参数
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 4 条）
// 职责：实现 D3D12RootSignature.h。所有寄存器号来自 D3D12RootBindings.h，本文件
//       只负责"装配"：root parameter 类型/可见性、descriptor range 的
//       offsetInDescriptorsFromTableStart（表内偏移必须从 0 起，见下）、static
//       sampler 参数与 root signature flags。
// 关键约定：
//   1) 两个 table 各自的 range 用 OFFSET_APPEND，且每个 table 只有一个 range，
//      因此表内偏移恒为 0——HLSL 里的 t 寄存器号由 range 的 BaseShaderRegister 决定；
//   2) b2/b3 只对 PIXEL 可见（与 HLSL 只在 PS 里用一致，避免无谓的版本化开销）；
//   3) flags = ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | DENY_{HULL,DOMAIN,GEOMETRY}_ROOT_ACCESS
//      （M5 不用这些阶段；deny 让误用立即失败而不是静默绑定）；
//   4) 不用 Root Signature 1.1 volatility flags（M6 调研项）。
// 关联：docs/architecture/README.md
//       Microsoft Learn：Creating a Root Signature
// ============================================================================
#include "D3D12RootSignature.h"

#include "D3D12Diagnostics.h"

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <array>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// 序列化失败时把编译器风格错误 blob 写进日志：只有 HRESULT 无法定位布局问题。
void LogSerializeErrors(ID3DBlob* errors)
{
    if (errors == nullptr || errors->GetBufferSize() == 0U)
    {
        return;
    }
    const char* const text = static_cast<const char*>(errors->GetBufferPointer());
    MiniEngine::WriteLog(MiniEngine::LogLevel::Error,
                         "d3d12 root signature serialize errors: " + std::string{text, errors->GetBufferSize()});
}
} // namespace

D3D12_STATIC_SAMPLER_DESC BuildMaterialStaticSampler()
{
    // s0：与 M4 D3D11 的 s0 同语义（min/mag/mip linear + 三向 wrap + MaxLOD 无上限）。
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1U;                           // 非各向异性滤波时该字段必须为 1（D3D12 校验要求）
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER; // 非比较采样器：该字段未使用
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = Register(SamplerRegister::Material); // s0
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return sampler;
}

D3D12_STATIC_SAMPLER_DESC BuildIblStaticSampler()
{
    // s1：IBL/后处理线性 clamp——wrap 采样对 cube/LUT 会跨面混绕（04 篇契约）。
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1U;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = Register(SamplerRegister::IblAndPost); // s1
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return sampler;
}

D3D12_STATIC_SAMPLER_DESC BuildShadowComparisonStaticSampler()
{
    // s2：shadow comparison——border=opaque white 让"采样在阴影图外"返回 1.0（完全照亮），
    // 与 M4 D3D11 的 comparison sampler（LESS_EQUAL / border=1）逐项一致。
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1U;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MinLOD = 0.0F;
    // MaxLOD = 0：与 M4 D3D11 的 shadow sampler（D3D11ShadowMap.cpp：MinLOD=0、MaxLOD=0）
    // 逐项一致。阴影图只有 1 个 mip，因此 0 与 FLOAT32_MAX 在当前行为等价——
    // 但 M5-09 parity 的前提是"同一采样参数"，且一旦将来给阴影图加 mip，
    // MaxLOD=FLOAT32_MAX 会立刻改变采样结果（审查意见 P2-2 指出此处原先与 M4 不一致）。
    sampler.MaxLOD = 0.0F;
    sampler.ShaderRegister = Register(SamplerRegister::ShadowComparison); // s2
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return sampler;
}

D3D12_ROOT_SIGNATURE_DESC BuildM5RootSignatureDescription(RootSignatureLayoutStorage& storage)
{
    // 两个 descriptor range：材质表 t0—t4（5 个）与全局表 t5—t8（4 个）。
    // 每个 table 只含一个 range，因此 offsetInDescriptorsFromTableStart 恒为 0；
    // BaseShaderRegister 决定 HLSL 侧的 t 号（与 BindingContract.hlsli 对应）。
    storage.ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    storage.ranges[0].NumDescriptors = kMaterialSrvCount;
    storage.ranges[0].BaseShaderRegister = kMaterialSrvBaseRegister; // t0
    storage.ranges[0].RegisterSpace = 0U;
    storage.ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    storage.ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    storage.ranges[1].NumDescriptors = kGlobalSrvCount;
    storage.ranges[1].BaseShaderRegister = kGlobalSrvBaseRegister; // t5
    storage.ranges[1].RegisterSpace = 0U;
    storage.ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // root parameter 0—3：root CBV b0—b3；b0/b1 对全部阶段可见，b2/b3 仅 PIXEL。
    for (std::uint32_t index = 0; index < kRootConstantBufferCount; ++index)
    {
        storage.parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        storage.parameters[index].Descriptor.ShaderRegister = index; // b0—b3
        storage.parameters[index].Descriptor.RegisterSpace = 0U;
        // b0/b1 供 VS/PS 共用（Frame/Object）；b2/b3 只在 PS 用。
        storage.parameters[index].ShaderVisibility =
            index < 2U ? D3D12_SHADER_VISIBILITY_ALL : D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // root parameter 4/5：两个 SRV table。
    const std::uint32_t materialRoot = RootIndex(RootParameter::MaterialSrvs);
    storage.parameters[materialRoot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    storage.parameters[materialRoot].DescriptorTable.NumDescriptorRanges = 1U;
    storage.parameters[materialRoot].DescriptorTable.pDescriptorRanges = &storage.ranges[0];
    storage.parameters[materialRoot].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const std::uint32_t globalRoot = RootIndex(RootParameter::GlobalSrvs);
    storage.parameters[globalRoot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    storage.parameters[globalRoot].DescriptorTable.NumDescriptorRanges = 1U;
    storage.parameters[globalRoot].DescriptorTable.pDescriptorRanges = &storage.ranges[1];
    storage.parameters[globalRoot].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // static sampler：显式构造三条（绝不 memcpy 一份 D3D11 sampler 描述）。
    storage.staticSamplers[0] = BuildMaterialStaticSampler();
    storage.staticSamplers[1] = BuildIblStaticSampler();
    storage.staticSamplers[2] = BuildShadowComparisonStaticSampler();

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(storage.parameters.size());
    description.pParameters = storage.parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(storage.staticSamplers.size());
    description.pStaticSamplers = storage.staticSamplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    return description;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateM5RootSignature(ID3D12Device& device, RootSignatureFacts& facts)
{
    RootSignatureLayoutStorage storage{};
    const D3D12_ROOT_SIGNATURE_DESC description = BuildM5RootSignatureDescription(storage);

    // 05 篇要求 version 1.0（不用 1.1 volatility flags）。序列化后创建。
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult))
    {
        LogSerializeErrors(errors.Get());
        ThrowIfFailed(serializeResult, "D3D12SerializeRootSignature");
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSignature)),
                  "ID3D12Device::CreateRootSignature");
    Internal::SetDebugName(rootSignature.Get(), L"M5.D3D12.RootSignature");

    facts.rootParameterCount = static_cast<std::uint32_t>(storage.parameters.size());
    facts.staticSamplerCount = static_cast<std::uint32_t>(storage.staticSamplers.size());
    facts.serializedBytes = static_cast<std::uint32_t>(serialized->GetBufferSize());
    facts.flags = description.Flags;
    return rootSignature;
}
} // namespace MiniEngine::Rhi::D3D12
