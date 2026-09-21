// ============================================================================
// D3D12StabilityPressure.cpp — 上传与描述符容量压力实现
// 职责：使用真实提交 fence 验证 wrap、stall、dedicated 和延迟回收。
// 验证 GPU 回读字节，禁止用分配计数代替上传内容正确性。
// ============================================================================
#include "D3D12StabilityPressure.h"
#include "D3D12DescriptorHeap.h"
#include "D3D12Queue.h"
#include "D3D12UploadManager.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

using namespace MiniEngine::Rhi::D3D12;
D3D12PressureFacts RunD3D12Pressure(D3D12Device& owner)
{
    auto& device = *static_cast<ID3D12Device*>(owner.NativeDeviceHandle());
    D3D12PressureFacts facts{};
    facts.capacity = 1024U * 1024U;
    D3D12Queue queue;
    queue.Initialize(device);
    D3D12UploadRing ring;
    ring.Initialize(device, facts.capacity);
    D3D12UploadManager uploads;
    uploads.Initialize(
        &device, ring,
        [&](std::uint64_t fence)
        {
            ++facts.waits;
            queue.WaitForSubmittedFence(fence, "M5-10 upload pressure");
        },
        4U * facts.capacity);
    D3D12DescriptorHeap heap;
    heap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8U, true, L"M5-10.SmallHeap");
    auto range = heap.Allocate(8U);
    const auto requireExhaustion = [&]()
    {
        bool exhausted = false;
        try
        {
            static_cast<void>(heap.Allocate(1U));
        }
        catch (const std::bad_alloc&)
        {
            exhausted = true;
        }
        if (!exhausted)
            throw std::runtime_error("small heap unexpectedly allocated an occupied slot");
        ++facts.descriptorExhaustions;
    };
    requireExhaustion();
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 2U * facts.capacity;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
                  "pressure readback");
    // Guard 比被引用资源晚析构：异常路径也先等待已提交工作。
    struct FlushGuard final
    {
        D3D12Queue& queue;
        ~FlushGuard()
        {
            try
            {
                queue.FlushGpu("pressure scope exit");
            }
            catch (...)
            {
            }
        }
    } guard{queue};
    auto& first = queue.BeginFrame(0);
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        const auto allocation = uploads.Allocate(facts.capacity / 4U, 256U, "fill ring");
        std::memset(allocation.cpu, static_cast<int>(index + 1U), static_cast<std::size_t>(allocation.size));
        queue.CommandList().CopyBufferRegion(readback.Get(), index * allocation.size, allocation.source,
                                             allocation.offset, allocation.size);
    }
    const auto firstFence = queue.ExecuteAndSignal(first);
    uploads.CommitFrame(firstFence);
    heap.Retire(range, firstFence);
    heap.Reclaim(firstFence - 1U);
    requireExhaustion();
    auto& second = queue.BeginFrame(1);
    // 不提前 Reclaim：此分配必须走 wait-oldest 分支，完成后从环首复用。
    const auto wrapped = uploads.Allocate(facts.capacity / 4U, 256U, "wrap ring");
    if (wrapped.offset != 0U)
        throw std::runtime_error("ring did not wrap to offset zero");
    ++facts.wraps;
    std::memset(wrapped.cpu, 5, static_cast<std::size_t>(wrapped.size));
    queue.CommandList().CopyBufferRegion(readback.Get(), facts.capacity, wrapped.source, wrapped.offset, wrapped.size);
    const auto dedicated = uploads.Allocate(facts.capacity / 2U, 256U, "dedicated pressure");
    std::memset(dedicated.cpu, 6, static_cast<std::size_t>(dedicated.size));
    queue.CommandList().CopyBufferRegion(readback.Get(), facts.capacity + wrapped.size, dedicated.source,
                                         dedicated.offset, dedicated.size);
    const auto secondFence = queue.ExecuteAndSignal(second);
    uploads.CommitFrame(secondFence);
    if (uploads.RetiredDedicatedCount() != 1U)
        throw std::runtime_error("dedicated upload not retired");
    queue.WaitForSubmittedFence(secondFence, "pressure readback complete");
    uploads.Reclaim(queue.CompletedValue());
    heap.Reclaim(queue.CompletedValue());
    range = heap.Allocate(8U);
    ++facts.descriptorReclaims;
    heap.Retire(range, secondFence);
    heap.Reclaim(queue.CompletedValue());
    facts.comparedBytes = facts.capacity + wrapped.size + dedicated.size;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(facts.comparedBytes)};
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, &readRange, &mapped), "pressure Map");
    const auto* bytes = static_cast<const unsigned char*>(mapped);
    bool equal = true;
    for (std::uint64_t index = 0; index < facts.comparedBytes; ++index)
    {
        const auto expected = index < facts.capacity ? index / (facts.capacity / 4U) + 1U
                                                     : (index < facts.capacity + wrapped.size ? 5U : 6U);
        equal = equal && bytes[index] == expected;
    }
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    if (!equal)
        throw std::runtime_error("pressure GPU readback mismatch");
    facts.ringAllocations = uploads.Stats().ringAllocations;
    facts.noSpanEvents = uploads.Stats().ringNoSpanEvents;
    facts.dedicated = uploads.Stats().dedicatedAllocations;
    if (facts.waits != 1U || facts.noSpanEvents != 1U || facts.dedicated != 1U ||
        ring.Allocator().OccupiedBytes() != 0U || uploads.RetiredDedicatedCount() != 0U ||
        heap.RetiredRangeCount() != 0U)
        throw std::runtime_error("pressure retirement or path counter mismatch");
    return facts;
}
