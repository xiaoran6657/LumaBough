// ============================================================================
// D3D12RootSignature.h — Root Signature 1.0 的 C++ 程序化创建
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 4 条）
// 职责：按 05 篇 baseline 表创建 Root Signature 1.0：root parameter 0—3 为
//       root CBV（b0—b3，b2/b3 仅 PIXEL 可见）、4—5 为两个 SRV descriptor table
//       （t0—t4 共 5 个、t5—t8 共 4 个），加 3 个 static sampler（s0/s1/s2，
//       不进 descriptor heap）。所有编号取自 D3D12RootBindings.h（唯一真源），
//       本文件不写任何字面量寄存器号。
// 与 M4 的对应：b/t/s 编号与 D3D11 侧一致（M5-09 parity 前提）；adapter/
//       feature level 与 root signature 版本要求见 ADR-0006 与 02 篇。
// 内部性说明：后端内部类型（src/），直接暴露 ID3D12RootSignature，不进公共头。
// 关联：docs/architecture/README.md（Root Signature baseline）
//       engine/rhi/d3d12/include/MiniEngine/Rhi/D3D12/D3D12RootBindings.h（寄存器契约）
//       shaders/d3d12/BindingContract.hlsli（HLSL 侧镜像）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "MiniEngine/Rhi/D3D12/D3D12RootBindings.h"

namespace MiniEngine::Rhi::D3D12
{
// 创建结果的可核对事实（进 metadata 与验收记录）。
struct RootSignatureFacts final
{
    std::uint32_t rootParameterCount = 0; // 必须等于 RootParameter::Count
    std::uint32_t staticSamplerCount = 0; // 必须等于 kStaticSamplerCount
    std::uint32_t serializedBytes = 0;    // D3D12SerializeRootSignature 产出大小
    std::uint32_t flags = 0;              // D3D12_ROOT_SIGNATURE_FLAG_*
};

// 05 篇静态采样器的冻结参数（与 M4 D3D11 侧同语义；差异见 s0 注释）。
//
// 说明（与 05 篇表格的偏差，已记入验收记录）：05 篇表写「s0 anisotropic wrap」，
// 而 M4 D3D11 冻结实现是 min/mag/mip **linear** + wrap（D3D11Renderer.cpp 的
// s0）。M5 的首要目标是输出与 D3D11 基本一致（M5-09 parity），因此这里按 M4
// 的 linear wrap 落地；改 anisotropic 必须两端同时改并重采 golden（属 M11 材质篇）。
D3D12_STATIC_SAMPLER_DESC BuildMaterialStaticSampler();         // s0 linear wrap
D3D12_STATIC_SAMPLER_DESC BuildIblStaticSampler();              // s1 linear clamp
D3D12_STATIC_SAMPLER_DESC BuildShadowComparisonStaticSampler(); // s2 comparison border white, LESS_EQUAL

// 创建 Root Signature 1.0。
//
// 失败：D3D12SerializeRootSignature 失败时把 FXC/DXC 风格的错误 blob 文本一并
//   写进日志后抛 HResultError；CreateRootSignature 失败同样抛 HResultError。
[[nodiscard]] Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateM5RootSignature(ID3D12Device& device,
                                                                                RootSignatureFacts& facts);

// 布局存储：descriptor description 里的指针指向它，因此它必须比 description 活得久。
// 单独暴露出来是为了让测试能"序列化 production 的同一份 description 再反序列化
// 校验布局"——而不是在测试里另抄一份期望值（那只能证明抄写一致）。
struct RootSignatureLayoutStorage final
{
    std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
    std::array<D3D12_ROOT_PARAMETER, static_cast<std::size_t>(RootParameter::Count)> parameters{};
    std::array<D3D12_STATIC_SAMPLER_DESC, kStaticSamplerCount> staticSamplers{};
};

// 按 05 篇 baseline 填充 storage 并返回指向它的 description（生产创建与测试校验
// 共用同一份装配代码，避免"实现改了测试没改"的漂移）。
[[nodiscard]] D3D12_ROOT_SIGNATURE_DESC BuildM5RootSignatureDescription(RootSignatureLayoutStorage& storage);
} // namespace MiniEngine::Rhi::D3D12
