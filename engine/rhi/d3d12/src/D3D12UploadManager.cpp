// ============================================================================
// D3D12UploadManager.cpp — 上传策略实现（ring 复用 / 定向等待 / dedicated）
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期）
// 职责：实现 D3D12UploadManager.h。策略顺序严格按 06 篇：
//   大请求 → dedicated；小请求 → ring（失败等最老 fence 一次并记 stall，再失败即错）。
//   **任何情况下都不覆盖 pending/current span**（由 ring 分配器保证），
//   也**不用 flush 整个队列**来掩盖容量问题。
// 关联：docs/architecture/README.md（M5 policy）
// ============================================================================
#include "D3D12UploadManager.h"

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
D3D12UploadManager::~D3D12UploadManager()
{
    // 兜底：仍未回收的 dedicated staging 先 Unmap 再随 ComPtr 释放，避免调试层告警。
    for (Microsoft::WRL::ComPtr<ID3D12Resource>& resource : m_frameDedicated)
    {
        if (resource != nullptr)
        {
            resource->Unmap(0, nullptr);
        }
    }
}

void D3D12UploadManager::Initialize(ID3D12Device* device, D3D12UploadRing& ring, FenceWaiter waiter,
                                    const std::uint64_t dedicatedBudget)
{
    if (!waiter)
    {
        // 没有等待实现就无法执行"等最老 fence 一次"策略：宁可显式失败，
        // 也不要静默退化成"直接失败"或"覆盖 pending span"。
        throw std::invalid_argument{"D3D12UploadManager requires a fence waiter"};
    }
    m_device = device;
    m_ring = &ring;
    m_waiter = std::move(waiter);
    m_dedicatedBudget = dedicatedBudget;
}

bool D3D12UploadManager::NeedsDedicated(const std::uint64_t size) const noexcept
{
    // 06 篇：大于 ring 1/4 的 mesh/texture upload 走 dedicated，不阻塞 ring。
    return size > m_ring->Capacity() / 4U;
}

UploadAllocation D3D12UploadManager::CreateDedicated(const std::uint64_t size, const std::uint64_t alignment,
                                                     const std::string_view tag)
{
    if (m_device == nullptr)
    {
        throw std::runtime_error{"dedicated upload requested without a device: " + std::string{tag}};
    }
    if (m_dedicatedBudget != 0U && size > m_dedicatedBudget)
    {
        ++m_stats.rejections;
        throw std::runtime_error{"upload request exceeds dedicated budget: " + std::to_string(size) + " > " +
                                 std::to_string(m_dedicatedBudget) + " (" + std::string{tag} + ")"};
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)),
                  "ID3D12Device::CreateCommittedResource(dedicated staging)");

    std::byte* cpuBase = nullptr;
    const D3D12_RANGE readRange{0U, 0U};
    ThrowIfFailed(staging->Map(0, &readRange, reinterpret_cast<void**>(&cpuBase)),
                  "ID3D12Resource::Map(dedicated staging)");

    UploadAllocation allocation;
    allocation.cpu = cpuBase;
    allocation.gpu = staging->GetGPUVirtualAddress();
    allocation.offset = 0U;
    allocation.size = size;
    // 先取裸指针再 move：dedicated staging 由本对象的 m_frameDedicated 持有到该帧
    // fence 完成，因此调用方在本帧内使用该指针是安全的（06 篇的释放契约）。
    allocation.source = staging.Get();

    m_frameDedicated.push_back(std::move(staging));
    ++m_stats.dedicatedAllocations;
    m_stats.dedicatedBytes += size;

    // alignment 的处理方式（审查意见 M5-06 P2-3：原注释称"保留用于日志与断言"，
    // 但实现里被 static_cast<void> 丢弃）：dedicated staging 是独立资源、起点偏移恒为 0，
    // **0 是任何 2 的幂对齐的倍数**，因此无需补齐；参数保留是为了让两条路径同一签名，
    // 并在此记入日志便于核对调用方期望。
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 dedicated upload staging: bytes=" + std::to_string(size) +
                                                         " alignment=" + std::to_string(alignment) +
                                                         " (offset 0 satisfies it) tag=" + std::string{tag});
    return allocation;
}

UploadAllocation D3D12UploadManager::Allocate(const std::uint64_t size, const std::uint64_t alignment,
                                              const std::string_view tag)
{
    if (m_ring == nullptr)
    {
        throw std::logic_error{"D3D12UploadManager::Allocate before Initialize"};
    }
    if (size == 0U)
    {
        throw std::invalid_argument{"upload allocation size must be non-zero"};
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
    {
        throw std::invalid_argument{"upload alignment must be a power of two"};
    }

    // 路径 1：大请求直接走 dedicated（不占用 ring，不阻塞其他帧）。
    if (NeedsDedicated(size))
    {
        return CreateDedicated(size, alignment, tag);
    }

    // 路径 2：先试 ring。
    UploadAllocation allocation = m_ring->TryAllocate(size, alignment);
    if (allocation)
    {
        ++m_stats.ringAllocations;
        return allocation;
    }

    // 路径 3：ring 暂时没有安全 span → 等**最老 pending fence 一次**后重试。
    const std::uint64_t oldestFence = m_ring->Allocator().OldestPendingFence();
    if (oldestFence != 0U)
    {
        ++m_stats.ringNoSpanEvents;
        const auto waitStart = std::chrono::steady_clock::now();
        m_waiter(oldestFence);
        m_stats.stallMicroseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart)
                .count());
        // 注意：等待**不**改变 ring 记账；调用方必须已 Reclaim(completed) 后再分配，
        // 因此这里先 Reclaim 再重试（等待返回即代表该 fence 已完成）。
        m_ring->Reclaim(oldestFence);
        allocation = m_ring->TryAllocate(size, alignment);
        if (allocation)
        {
            ++m_stats.ringAllocations;
            return allocation;
        }
    }

    // 路径 4：仍无空间。若请求本可走 dedicated（只是没超过 1/4 阈值），
    // 不静默改道——显式失败让容量决策暴露出来（06 篇：不允许用增大 ring 掩盖问题）。
    ++m_stats.rejections;
    throw std::runtime_error{"upload ring has no safe span for " + std::to_string(size) + " bytes (" +
                             std::string{tag} + "); consider a larger --upload-ring or smaller frame payload"};
}

void D3D12UploadManager::CommitFrame(const std::uint64_t fenceValue)
{
    if (m_ring == nullptr)
    {
        throw std::logic_error{"D3D12UploadManager::CommitFrame before Initialize"};
    }

    // ring：本帧 current span（回绕帧可能是两个）统一挂到该 fence。
    m_ring->CommitFrame(fenceValue);

    // dedicated staging：只有**提交该帧的 fence** 完成后才释放——不是上传完成时就释放，
    // 因为"上传完成"与"引用它的绘制完成"是两条时间线（06 篇：旧对象的 retire fence
    // 是最后引用它的 frame fence）。
    for (Microsoft::WRL::ComPtr<ID3D12Resource>& resource : m_frameDedicated)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> owned = std::move(resource);
        m_deferred.Retire(fenceValue,
                          [owned]() mutable
                          {
                              if (owned != nullptr)
                              {
                                  owned->Unmap(0, nullptr);
                                  owned.Reset();
                              }
                          });
    }
    m_frameDedicated.clear();
}

void D3D12UploadManager::Reclaim(const std::uint64_t completedFenceValue)
{
    if (m_ring == nullptr)
    {
        throw std::logic_error{"D3D12UploadManager::Reclaim before Initialize"};
    }
    m_ring->Reclaim(completedFenceValue);
    static_cast<void>(m_deferred.Reclaim(completedFenceValue));
}

const UploadManagerStats& D3D12UploadManager::Stats() const noexcept
{
    return m_stats;
}

std::size_t D3D12UploadManager::PendingDedicatedCount() const noexcept
{
    return m_frameDedicated.size();
}

D3D12DeferredRelease& D3D12UploadManager::DeferredRelease() noexcept
{
    return m_deferred;
}
} // namespace MiniEngine::Rhi::D3D12
