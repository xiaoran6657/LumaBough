// ============================================================================
// D3D12IblSet.h — 环境生成事务的独占资源集与回读证据。
// 里程碑：M5-09。
// 职责：暂存 Environment/Irradiance/Prefilter/LUT 与 mip 中转资源，按提交 fence
//       发布或回收；每个集合独占 descriptor，旧帧不观察到新环境的半成品。
// 关联：docs/architecture/README.md。
// ============================================================================
#pragma once

#include "D3D12DescriptorHeap.h"
#include "D3D12ResourceStateTracker.h"
#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/TextureAsset.h>
#include <array>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
struct D3D12IblSet final
{
    // 顺序固定：environment、irradiance、prefilter、LUT、mipSource。
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 5> textures;
    std::array<ResourceKey, 5> keys{};
    std::array<bool, 5> registered{};
    D3D12DescriptorHeap staging;
    D3D12DescriptorHeap rtvs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs{};
    std::array<std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>, 5> targets;
    // 前四个生成目标的完整子资源回读，fence 完成前不得 Map。
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> readbacks;
    std::array<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>, 4> footprints;
    std::array<std::uint64_t, 4> readbackBytes{};
    Assets::AssetHandle<Assets::TextureAsset> source{};
    std::uint64_t revision = 0;
    std::uint64_t shaderRevision = 0;
    std::uint64_t fence = 0;
    std::string failure; // 失败批也要等已录 draw 的 submission fence 完成，不能立即析构。
};
} // namespace MiniEngine::Rhi::D3D12
