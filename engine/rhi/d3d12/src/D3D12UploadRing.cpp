// ============================================================================
// D3D12UploadRing.cpp — 持久映射的 UPLOAD 缓冲实现
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 1 条）
// 职责：实现 D3D12UploadRing.h。关键点：
//   1) Map 只调用一次（readRange {0,0}）——持久映射，shutdown 才 Unmap；
//   2) 分配只做记账 + 指针换算（cpu = base + offset，gpu = gpuBase + offset）；
//   3) Shutdown 前必须 idle：pending span 未回收却 Unmap 会让 GPU 读到无效内存。
// 关联：docs/architecture/README.md（Upload Ring baseline）
// ============================================================================
#include "D3D12UploadRing.h"

#include "D3D12Diagnostics.h"

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
D3D12UploadRing::~D3D12UploadRing()
{
    // 析构兜底：若调用方忘了 Shutdown 而资源仍映射着，先 Unmap 再释放，
    // 避免调试层的 "resource still mapped at final release" 类告警。
    if (m_resource != nullptr && m_cpuBase != nullptr)
    {
        m_resource->Unmap(0, nullptr);
        m_cpuBase = nullptr;
    }
}

void D3D12UploadRing::Initialize(ID3D12Device& device, const std::uint64_t capacity)
{
    if (capacity == 0U)
    {
        throw std::invalid_argument{"upload ring capacity must be non-zero"};
    }
    if (m_allocator.has_value())
    {
        throw std::logic_error{"D3D12UploadRing::Initialize called twice"};
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = capacity;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource)),
                  "ID3D12Device::CreateCommittedResource(upload ring)");
    Internal::SetDebugName(m_resource.Get(), L"M5.D3D12.UploadRing");

    // 持久映射：readRange {0,0} = "CPU 只写、不读"，避免驱动为读回做额外处理。
    const D3D12_RANGE readRange{0U, 0U};
    ThrowIfFailed(m_resource->Map(0, &readRange, reinterpret_cast<void**>(&m_cpuBase)),
                  "ID3D12Resource::Map(upload ring)");
    m_gpuBase = m_resource->GetGPUVirtualAddress();
    m_allocator.emplace(capacity);

    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 upload ring created: capacity=" + std::to_string(capacity) +
                                                         " gpuBase=0x" + std::to_string(m_gpuBase));
}

UploadAllocation D3D12UploadRing::TryAllocate(const std::uint64_t size, const std::uint64_t alignment)
{
    if (!m_allocator.has_value())
    {
        throw std::logic_error{"D3D12UploadRing::TryAllocate before Initialize"};
    }

    const std::optional<UploadRingSpan> span = m_allocator->TryAllocate(size, alignment);
    if (!span.has_value())
    {
        return UploadAllocation{}; // 空 allocation：策略层决定等 fence 还是 dedicated
    }

    UploadAllocation allocation;
    allocation.cpu = m_cpuBase + span->offset;
    allocation.gpu = m_gpuBase + span->offset;
    allocation.offset = span->offset;
    allocation.size = span->size;
    allocation.source = m_resource.Get(); // CopyTextureRegion 需要资源本身（见 UploadAllocation）
    return allocation;
}

UploadAllocation D3D12UploadRing::TryAllocateConstant(const std::uint64_t size)
{
    // CBV 必须 256 字节对齐（06 篇「对齐」表）。
    return TryAllocate(size, UploadRingAllocator::kConstantBufferAlignment);
}

void D3D12UploadRing::CommitFrame(const std::uint64_t fenceValue)
{
    if (!m_allocator.has_value())
    {
        throw std::logic_error{"D3D12UploadRing::CommitFrame before Initialize"};
    }
    m_allocator->CommitFrame(fenceValue);
}

void D3D12UploadRing::Reclaim(const std::uint64_t completedFenceValue)
{
    if (!m_allocator.has_value())
    {
        throw std::logic_error{"D3D12UploadRing::Reclaim before Initialize"};
    }
    m_allocator->Reclaim(completedFenceValue);
}

void D3D12UploadRing::Shutdown(const std::string_view reason)
{
    if (!m_allocator.has_value())
    {
        return; // 从未初始化：无需清理
    }
    if (!IsIdle())
    {
        // 仍有 pending/current span：此时 Unmap 会让 GPU 读到无效内存。
        // 调用方必须先 FlushGpu + Reclaim（06 篇：不允许用增大 ring 掩盖问题）。
        throw std::logic_error{"upload ring shutdown requires an idle ring (flush and reclaim first): " +
                               std::string{reason}};
    }

    if (m_resource != nullptr && m_cpuBase != nullptr)
    {
        m_resource->Unmap(0, nullptr);
        m_cpuBase = nullptr;
    }
    m_resource.Reset();
    m_gpuBase = 0;
    m_allocator.reset();
}

ID3D12Resource& D3D12UploadRing::Native() const noexcept
{
    return *m_resource.Get();
}

const UploadRingAllocator& D3D12UploadRing::Allocator() const noexcept
{
    return *m_allocator;
}

std::uint64_t D3D12UploadRing::Capacity() const noexcept
{
    return m_allocator.has_value() ? m_allocator->Capacity() : 0U;
}

std::uint64_t D3D12UploadRing::HighWaterBytes() const noexcept
{
    return m_allocator.has_value() ? m_allocator->HighWaterBytes() : 0U;
}

bool D3D12UploadRing::IsIdle() const noexcept
{
    return m_allocator.has_value() && m_allocator->IsIdle();
}

bool D3D12UploadRing::IsMapped() const noexcept
{
    return m_cpuBase != nullptr;
}
} // namespace MiniEngine::Rhi::D3D12
