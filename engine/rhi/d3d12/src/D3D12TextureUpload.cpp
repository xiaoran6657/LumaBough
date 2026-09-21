// ============================================================================
// D3D12TextureUpload.cpp — footprints 计划、逐行打包与 CopyTextureRegion 录制
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 4 条）
// 职责：实现 D3D12TextureUpload.h。三条纪律：
//   1) 行距/对齐一律来自 GetCopyableFootprints，本文件不出现 256/512 的字面量猜测
//      （placement 对齐检查除外，那是 06 篇冻结的 512 档位）；
//   2) 逐行复制：源 rowPitch 与目标 Footprint.RowPitch 分开，绝不整块 memcpy；
//   3) 越界即失败：上传缓冲是有限的，打包前先把范围算清楚。
// 关联：docs/architecture/README.md（Texture upload）
// ============================================================================
#include "D3D12TextureUpload.h"

#include "D3D12UploadRingAllocator.h"

#include <MiniEngine/Core/Log.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// 2D 纹理的子资源总数 = mipLevels × arraySize（depth 针对 3D；M5 只用 2D/cube）。
std::uint32_t SubresourceCount(const D3D12_RESOURCE_DESC& description)
{
    return static_cast<std::uint32_t>(description.MipLevels) * static_cast<std::uint32_t>(description.DepthOrArraySize);
}
} // namespace

TextureUploadPlan PlanTextureUpload(ID3D12Device& device, const D3D12_RESOURCE_DESC& description,
                                    const std::uint64_t baseOffset)
{
    // 06 篇「对齐」表：纹理放置必须 512 字节对齐。
    if (baseOffset % UploadRingAllocator::kTexturePlacementAlignment != 0U)
    {
        throw std::invalid_argument{"texture upload placement offset must be 512-byte aligned"};
    }
    if (description.MipLevels == 0U || description.DepthOrArraySize == 0U)
    {
        throw std::invalid_argument{"texture description must have mips and at least one slice"};
    }

    const std::uint32_t count = SubresourceCount(description);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placements(count);
    std::vector<UINT> rowCounts(count, 0U);
    std::vector<UINT64> rowSizes(count, 0U);
    UINT64 totalBytes = 0U;

    device.GetCopyableFootprints(&description, 0U, count, baseOffset, placements.data(), rowCounts.data(),
                                 rowSizes.data(), &totalBytes);

    TextureUploadPlan plan;
    plan.subresourceCount = count;
    plan.totalBytes = totalBytes;
    plan.subresources.resize(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        TextureSubresourcePlan& entry = plan.subresources[index];
        entry.placement = placements[index];
        entry.rowCount = rowCounts[index];
        entry.rowSize = rowSizes[index];
        // 该 subresource 占用的上传字节 = 下一段起点 − 本段起点（最后一段用总量兜底），
        // 这正是 DXGI 自己排布的结果，避免另算一套 RowPitch*rows 导致差一列。
        const std::uint64_t nextOffset = index + 1U < count ? placements[index + 1U].Offset : baseOffset + totalBytes;
        entry.totalBytes = nextOffset - placements[index].Offset;
    }

    if (plan.totalBytes == 0U)
    {
        throw std::runtime_error{"GetCopyableFootprints returned zero bytes for the texture description"};
    }
    return plan;
}

void PackTextureRows(const TextureUploadPlan& plan, const std::uint32_t subresourceIndex, std::byte* const uploadBase,
                     const std::uint64_t uploadByteSize, const TextureUploadSource& source)
{
    if (subresourceIndex >= plan.subresourceCount)
    {
        throw std::out_of_range{"texture subresource index out of range"};
    }
    if (uploadBase == nullptr || source.data == nullptr)
    {
        throw std::invalid_argument{"texture upload requires non-null source and destination pointers"};
    }

    const TextureSubresourcePlan& entry = plan.subresources[subresourceIndex];
    if (source.rowPitch < entry.rowSize)
    {
        // 源行距小于有效行字节：逐行复制会把两行拼在一起（串行）。
        throw std::invalid_argument{"texture source row pitch is smaller than the footprint row size"};
    }
    if (entry.rowCount == 0U || entry.rowSize == 0U)
    {
        throw std::invalid_argument{"texture subresource has no rows (degenerate description)"};
    }

    const std::uint64_t destinationRowPitch = entry.placement.Footprint.RowPitch;
    // 目标范围上界（最后一行只占 rowSize，不含行尾 padding）。
    const std::uint64_t destinationEnd =
        entry.placement.Offset + (entry.rowCount - 1U) * destinationRowPitch + entry.rowSize;
    if (destinationEnd > uploadByteSize)
    {
        throw std::out_of_range{"texture upload would write past the upload buffer"};
    }

    // 源侧同样要校验：只有 data + rowPitch 时无法发现"源缓冲比需要的短"，
    // 那会变成越界读（比越界写更难排查）。
    const std::uint64_t sourceEnd = (entry.rowCount - 1U) * source.rowPitch + entry.rowSize;
    if (sourceEnd > source.byteSize)
    {
        throw std::out_of_range{"texture upload would read past the source buffer"};
    }

    for (std::uint32_t row = 0; row < entry.rowCount; ++row)
    {
        const std::byte* const sourceRow = source.data + static_cast<std::uint64_t>(row) * source.rowPitch;
        std::byte* const destinationRow =
            uploadBase + entry.placement.Offset + static_cast<std::uint64_t>(row) * destinationRowPitch;
        std::memcpy(destinationRow, sourceRow, static_cast<std::size_t>(entry.rowSize));
    }
}

void RecordTextureCopies(ID3D12GraphicsCommandList& commandList, ID3D12Resource& destination,
                         const TextureUploadPlan& plan, ID3D12Resource& uploadBuffer)
{
    const D3D12_RESOURCE_DESC destinationDescription = destination.GetDesc();
    const UINT mipLevels = destinationDescription.MipLevels;
    const UINT arraySize = destinationDescription.DepthOrArraySize;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = &uploadBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    for (std::uint32_t index = 0; index < plan.subresourceCount; ++index)
    {
        // 从线性下标反推 (mip, slice)：与 D3D12CalcSubresource 的排序一致。
        const UINT mip = index % mipLevels;
        const UINT slice = index / mipLevels;

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = &destination;
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = CalcSubresourceIndex(mip, slice, 0U, mipLevels, arraySize);

        source.PlacedFootprint = plan.subresources[index].placement;
        commandList.CopyTextureRegion(&destinationLocation, 0U, 0U, 0U, &source, nullptr);
    }
}

void RecordResourceTransition(ID3D12GraphicsCommandList& commandList, ID3D12Resource& resource,
                              const D3D12_RESOURCE_STATES before, const D3D12_RESOURCE_STATES after)
{
    // before == after：**静默跳过**而不是抛异常。理由（审查意见 M5-06 次要项）：
    // 04 篇的 D3D12SwapChain::Transition 对同状态就是 no-op，两个 helper 语义必须一致，
    // 否则调用方在"状态跟踪器已经去重"的场景会踩到假失败。真正的状态错误由 07 篇的
    // ResourceStateTracker 负责发现（它掌握每个资源的记账），不由本工具函数代劳。
    if (before == after)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = &resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList.ResourceBarrier(1U, &barrier);
}
} // namespace MiniEngine::Rhi::D3D12
