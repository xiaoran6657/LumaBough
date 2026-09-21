// ============================================================================
// D3D12DescriptorHeap.cpp — heap 创建、句柄换算与分配器委托
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 1 条）
// 职责：实现 D3D12DescriptorHeap.h。分配/回收规则全在 D3D12DescriptorAllocator
//       （可被 CPU 测试穷举）；本文件只负责 GPU 侧：创建 heap、记录 increment、
//       以及 CPU/GPU 句柄的偏移换算与边界校验。
// 关联：docs/architecture/README.md
// ============================================================================
#include "D3D12DescriptorHeap.h"
#include <algorithm>

#include "D3D12Diagnostics.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <stdexcept>

namespace MiniEngine::Rhi::D3D12
{
void D3D12DescriptorHeap::Initialize(ID3D12Device& device, const D3D12_DESCRIPTOR_HEAP_TYPE type,
                                     const std::uint32_t capacity, const bool shaderVisible, const wchar_t* debugName)
{
    if (capacity == 0U)
    {
        // 05 篇：SAMPLER 容量 0 表示"不创建 sampler heap"（M5 全部 static sampler），
        // 而不是"创建一个空 heap"——空 heap 没有意义且容易被误绑。
        throw std::invalid_argument{"descriptor heap capacity must be non-zero"};
    }
    if (m_allocator != nullptr)
    {
        throw std::logic_error{"D3D12DescriptorHeap::Initialize called twice"};
    }

    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = type;
    description.NumDescriptors = capacity;
    description.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device.CreateDescriptorHeap(&description, IID_PPV_ARGS(&m_heap)),
                  "ID3D12Device::CreateDescriptorHeap");
    Internal::SetDebugName(m_heap.Get(), debugName);

    // 创建时记录 increment（05 篇要求；typed handle 换算全靠它，业务代码不手算字节）。
    m_increment = device.GetDescriptorHandleIncrementSize(type);
    m_capacity = capacity;
    m_shaderVisible = shaderVisible;
    m_allocator = std::make_unique<DescriptorRangeAllocator>(capacity);
}

DescriptorRange D3D12DescriptorHeap::Allocate(const std::uint32_t count)
{
    if (m_allocator == nullptr)
    {
        throw std::logic_error{"D3D12DescriptorHeap::Allocate before Initialize"};
    }
    const auto range = m_allocator->Allocate(count);
    ++m_allocationCount;
    m_highWaterCount = (std::max)(m_highWaterCount, m_allocator->UsedCount());
    return range;
}

void D3D12DescriptorHeap::Free(const DescriptorRange range)
{
    if (m_allocator == nullptr)
    {
        throw std::logic_error{"D3D12DescriptorHeap::Free before Initialize"};
    }
    m_allocator->Free(range);
}

void D3D12DescriptorHeap::Retire(const DescriptorRange range, const std::uint64_t fenceValue)
{
    if (m_allocator == nullptr)
    {
        throw std::logic_error{"D3D12DescriptorHeap::Retire before Initialize"};
    }
    m_allocator->Retire(range, fenceValue);
}

void D3D12DescriptorHeap::Reclaim(const std::uint64_t completedFenceValue)
{
    if (m_allocator == nullptr)
    {
        throw std::logic_error{"D3D12DescriptorHeap::Reclaim before Initialize"};
    }
    m_allocator->Reclaim(completedFenceValue);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeap::Cpu(const std::uint32_t index) const
{
    if (m_allocator == nullptr)
    {
        throw std::logic_error{"descriptor heap handle requested before Initialize"};
    }
    if (index >= m_capacity)
    {
        throw std::out_of_range{"CPU descriptor index exceeds heap capacity"};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorHeap::Gpu(const std::uint32_t index) const
{
    if (m_allocator == nullptr)
    {
        throw std::logic_error{"descriptor heap handle requested before Initialize"};
    }
    if (!m_shaderVisible)
    {
        // 非 shader-visible heap 没有 GPU 句柄：显式失败而不是返回无意义的值。
        throw std::out_of_range{"GPU descriptor handle requested on a non-shader-visible heap"};
    }
    if (index >= m_capacity)
    {
        throw std::out_of_range{"GPU descriptor index exceeds heap capacity"};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_increment;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeap::Cpu(const DescriptorRange& range) const
{
    return Cpu(range.base);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorHeap::Gpu(const DescriptorRange& range) const
{
    return Gpu(range.base);
}

ID3D12DescriptorHeap& D3D12DescriptorHeap::Native() const noexcept
{
    return *m_heap.Get();
}

std::uint32_t D3D12DescriptorHeap::UsedCount() const noexcept
{
    return m_allocator ? m_allocator->UsedCount() : 0;
}

std::uint32_t D3D12DescriptorHeap::Capacity() const noexcept
{
    return m_capacity;
}

std::uint32_t D3D12DescriptorHeap::Increment() const noexcept
{
    return m_increment;
}

bool D3D12DescriptorHeap::ShaderVisible() const noexcept
{
    return m_shaderVisible;
}

std::size_t D3D12DescriptorHeap::ActiveRangeCount() const noexcept
{
    return m_allocator != nullptr ? m_allocator->ActiveRangeCount() : 0U;
}

std::size_t D3D12DescriptorHeap::FreeBlockCount() const noexcept
{
    return m_allocator != nullptr ? m_allocator->FreeBlockCount() : 0U;
}

std::size_t D3D12DescriptorHeap::RetiredRangeCount() const noexcept
{
    return m_allocator != nullptr ? m_allocator->RetiredRangeCount() : 0U;
}

bool D3D12DescriptorHeap::IsInitialized() const noexcept
{
    return m_allocator != nullptr;
}
} // namespace MiniEngine::Rhi::D3D12
