// ============================================================================
// D3D12DescriptorAllocator.cpp — 首次适配 / 合并 / generation 校验 / fence 回收
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 1/2 条）
// 职责：实现 D3D12DescriptorAllocator.h。全部为纯 CPU 记账逻辑，可被 CPU 测试
//       逐分支穷举；D3D12DescriptorHeap 只在句柄换算与创建时接触 GPU。
// 关联：docs/architecture/README.md（分配与热重载）
// ============================================================================
#include "D3D12DescriptorAllocator.h"

#include <iterator>
#include <stdexcept>

namespace MiniEngine::Rhi::D3D12
{
DescriptorRangeAllocator::DescriptorRangeAllocator(const std::uint32_t capacity) : m_capacity(capacity)
{
    if (capacity == 0U)
    {
        throw std::invalid_argument{"descriptor allocator capacity must be non-zero"};
    }
    // 初始状态：一个覆盖全空间的空闲块。
    m_freeByBase.emplace(0U, capacity);
}

DescriptorRange DescriptorRangeAllocator::Allocate(const std::uint32_t count)
{
    if (count == 0U)
    {
        throw std::invalid_argument{"zero descriptor allocation"};
    }
    if (m_nextGeneration == UINT32_MAX)
    {
        // generation 耗尽是逻辑错误（不是容量问题）：回绕会让"过期 range"重新有效。
        throw std::overflow_error{"descriptor generation exhausted"};
    }

    // 首次适配：map 按 base 有序，取第一个放得下的空闲块（保证区间连续）。
    for (auto iterator = m_freeByBase.begin(); iterator != m_freeByBase.end(); ++iterator)
    {
        const auto [base, available] = *iterator;
        if (available < count)
        {
            continue;
        }

        m_freeByBase.erase(iterator);
        if (available > count)
        {
            m_freeByBase.emplace(base + count, available - count);
        }
        const DescriptorRange result{base, count, m_nextGeneration++};
        m_activeByBase.emplace(base, result);
        return result;
    }

    // 无足够连续空间：容量边界（05 篇「测试」的"容量边界与溢出"）。
    throw std::bad_alloc{};
}

std::map<std::uint32_t, DescriptorRange>::iterator DescriptorRangeAllocator::FindActive(const DescriptorRange& range)
{
    if (!range)
    {
        throw std::logic_error{"descriptor range is empty (default-constructed)"};
    }
    const auto active = m_activeByBase.find(range.base);
    // 三处校验缺一不可：base 存在、count 完全一致、generation 一致。
    if (active == m_activeByBase.end() || active->second.count != range.count ||
        active->second.generation != range.generation)
    {
        throw std::logic_error{"descriptor range is stale, partial or already released"};
    }
    return active;
}

void DescriptorRangeAllocator::Free(const DescriptorRange range)
{
    const auto active = FindActive(range);
    m_activeByBase.erase(active);
    FreeAndCoalesce(range);
}

void DescriptorRangeAllocator::Retire(DescriptorRange range, const std::uint64_t fenceValue)
{
    if (!range)
    {
        throw std::invalid_argument{"invalid retired descriptor range"};
    }
    if (fenceValue == 0U)
    {
        // fence 0 是"从未提交"的保留值，作为回收判据没有意义。
        throw std::invalid_argument{"retire fence must be non-zero"};
    }
    if (fenceValue < m_lastRetireFence)
    {
        // retire 顺序必须单调：乱序会让"按 completed 一次性回收"漏项。
        throw std::logic_error{"retire fences must be monotonic"};
    }
    if (range.base >= m_capacity || range.count > m_capacity - range.base)
    {
        throw std::out_of_range{"retired descriptor range exceeds capacity"};
    }

    const auto active = FindActive(range);
    m_activeByBase.erase(active);
    m_retired.push({fenceValue, range});
    m_lastRetireFence = fenceValue;
}

void DescriptorRangeAllocator::Reclaim(const std::uint64_t completedFenceValue)
{
    // 队列按 fence 单调入队，因此只看队首。
    while (!m_retired.empty() && m_retired.front().fenceValue <= completedFenceValue)
    {
        FreeAndCoalesce(m_retired.front().range);
        m_retired.pop();
    }
}

void DescriptorRangeAllocator::FreeAndCoalesce(DescriptorRange range)
{
    auto next = m_freeByBase.lower_bound(range.base);
    if (next != m_freeByBase.end() && range.count > next->first - range.base)
    {
        throw std::logic_error{"descriptor free overlap (next block)"};
    }

    // 与前一个空闲块相邻则合并。
    if (next != m_freeByBase.begin())
    {
        const auto previous = std::prev(next);
        const std::uint32_t previousEnd = previous->first + previous->second;
        if (previousEnd > range.base)
        {
            throw std::logic_error{"descriptor free overlap (previous block)"};
        }
        if (previousEnd == range.base)
        {
            range.base = previous->first;
            range.count += previous->second;
            m_freeByBase.erase(previous);
        }
    }

    // 与后一个空闲块相邻则合并。
    if (next != m_freeByBase.end() && range.base + range.count == next->first)
    {
        range.count += next->second;
        m_freeByBase.erase(next);
    }

    m_freeByBase.emplace(range.base, range.count);
}

std::uint32_t DescriptorRangeAllocator::UsedCount() const noexcept
{
    std::uint32_t free = 0;
    for (const auto& [base, count] : m_freeByBase)
    {
        static_cast<void>(base);
        free += count;
    }
    return m_capacity - free;
}

std::uint32_t DescriptorRangeAllocator::Capacity() const noexcept
{
    return m_capacity;
}

std::size_t DescriptorRangeAllocator::ActiveRangeCount() const noexcept
{
    return m_activeByBase.size();
}

std::size_t DescriptorRangeAllocator::FreeBlockCount() const noexcept
{
    return m_freeByBase.size();
}

std::size_t DescriptorRangeAllocator::RetiredRangeCount() const noexcept
{
    return m_retired.size();
}

std::uint32_t DescriptorRangeAllocator::LargestFreeBlock() const noexcept
{
    std::uint32_t largest = 0U;
    for (const auto& [base, count] : m_freeByBase)
    {
        static_cast<void>(base);
        largest = count > largest ? count : largest;
    }
    return largest;
}
} // namespace MiniEngine::Rhi::D3D12
