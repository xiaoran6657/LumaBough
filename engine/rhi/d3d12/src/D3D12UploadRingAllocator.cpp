// ============================================================================
// D3D12UploadRingAllocator.cpp — 环形分配、回绕与 fence 延迟回收
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 1/2 条）
// 职责：实现 D3D12UploadRingAllocator.h。分配算法严格按 06 篇伪代码：
//       reclaim → alignedHead = AlignUp(head) → 尾部放不下则尝试回绕到 0 →
//       两者都失败返回 nullopt。**任何情况下都不覆盖 pending 或 current span**。
// 关联：docs/architecture/README.md（Ring 分配）
// ============================================================================
#include "D3D12UploadRingAllocator.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <algorithm>
#include <stdexcept>

namespace MiniEngine::Rhi::D3D12
{
UploadRingAllocator::UploadRingAllocator(const std::uint64_t capacity) : m_capacity(capacity)
{
    if (capacity == 0U)
    {
        throw std::invalid_argument{"upload ring capacity must be non-zero"};
    }
}

bool UploadRingAllocator::Overlaps(const std::uint64_t begin, const std::uint64_t end) const
{
    const auto intersects = [=](const std::uint64_t spanBegin, const std::uint64_t spanEnd)
    { return begin < spanEnd && spanBegin < end; };
    for (const PendingSpan& span : m_pending)
    {
        if (intersects(span.begin, span.end))
        {
            return true;
        }
    }
    for (const Span& span : m_current)
    {
        if (intersects(span.begin, span.end))
        {
            return true;
        }
    }
    return false;
}

void UploadRingAllocator::Record(const std::uint64_t begin, const std::uint64_t end)
{
    // 相邻的 current span 合并（同一帧连续分配不会产生碎片）。
    // 注意：回绕后 begin < 上一个 span 的 end，因此不会误合并成"跨零的单一区间"。
    if (!m_current.empty() && m_current.back().end == begin)
    {
        m_current.back().end = end;
    }
    else
    {
        m_current.push_back(Span{begin, end});
    }

    // high-water = 峰值占用（pending + current 字节数），供 benchmark/metadata 记录。
    m_highWater = std::max(m_highWater, OccupiedBytes());
}

std::optional<UploadRingSpan> UploadRingAllocator::TryAllocate(const std::uint64_t size, const std::uint64_t alignment)
{
    // 对齐必须是 2 的幂（AlignUp 内部还会做溢出检查，见 D3D12Common.h）。
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
    {
        throw std::invalid_argument{"upload ring alignment must be a power of two"};
    }
    if (size == 0U || size > m_capacity)
    {
        // 0 或超过整个 ring：不属于"暂时没空间"，直接返回 nullopt 让策略层判定失败。
        return std::nullopt;
    }

    // 1) 从 head 向上对齐后尝试尾部。
    const std::uint64_t begin = AlignUp(m_head, alignment);
    if (begin <= m_capacity && size <= m_capacity - begin && !Overlaps(begin, begin + size))
    {
        const std::uint64_t end = begin + size;
        Record(begin, end);
        // 恰好用完则回到 0，否则指向 end（下一次分配从对齐后的 head 开始）。
        m_head = end == m_capacity ? 0U : end;
        return UploadRingSpan{begin, size};
    }

    // 2) 尾部放不下：尝试回绕到 0（仍然不能覆盖 pending/current）。
    if (size <= m_capacity && !Overlaps(0U, size))
    {
        Record(0U, size);
        m_head = size == m_capacity ? 0U : size;
        return UploadRingSpan{0U, size};
    }

    // 3) 没有安全 span：交给策略层（等最老 fence 一次，或 dedicated staging）。
    return std::nullopt;
}

void UploadRingAllocator::CommitFrame(const std::uint64_t fenceValue)
{
    if (m_current.empty())
    {
        // 本帧没有分配：不推进 lastCommittedFence（避免"空帧"抬高单调基线）。
        return;
    }
    if (fenceValue == 0U)
    {
        throw std::invalid_argument{"upload ring retire fence must be non-zero"};
    }
    if (fenceValue < m_lastCommittedFence)
    {
        throw std::logic_error{"upload ring commit fences must be monotonic"};
    }

    // 回绕帧会在此产生两个（或更多）span，全部标记同一 fence。
    for (const Span& span : m_current)
    {
        m_pending.push_back(PendingSpan{span.begin, span.end, fenceValue});
    }
    m_lastCommittedFence = fenceValue;
    m_current.clear();
}

void UploadRingAllocator::Reclaim(const std::uint64_t completedFenceValue)
{
    // pending 按 fence 单调入队：只需看队首，可一次批量回收多批。
    while (!m_pending.empty() && m_pending.front().fenceValue <= completedFenceValue)
    {
        m_pending.pop_front();
    }
}

std::uint64_t UploadRingAllocator::Capacity() const noexcept
{
    return m_capacity;
}

std::uint64_t UploadRingAllocator::Head() const noexcept
{
    return m_head;
}

std::uint64_t UploadRingAllocator::HighWaterBytes() const noexcept
{
    return m_highWater;
}

std::uint64_t UploadRingAllocator::OccupiedBytes() const noexcept
{
    std::uint64_t occupied = 0U;
    for (const PendingSpan& span : m_pending)
    {
        occupied += span.end - span.begin;
    }
    for (const Span& span : m_current)
    {
        occupied += span.end - span.begin;
    }
    return occupied;
}

std::size_t UploadRingAllocator::PendingSpanCount() const noexcept
{
    return m_pending.size();
}

std::size_t UploadRingAllocator::CurrentSpanCount() const noexcept
{
    return m_current.size();
}

std::uint64_t UploadRingAllocator::OldestPendingFence() const noexcept
{
    return m_pending.empty() ? 0U : m_pending.front().fenceValue;
}

std::uint64_t UploadRingAllocator::LargestFreeSpan() const noexcept
{
    // 诊断用途：把已占用区间排序后求最大空隙（含回绕处的首尾合并）。
    std::vector<Span> occupied;
    occupied.reserve(m_pending.size() + m_current.size());
    for (const PendingSpan& span : m_pending)
    {
        occupied.push_back(Span{span.begin, span.end});
    }
    for (const Span& span : m_current)
    {
        occupied.push_back(span);
    }
    std::sort(occupied.begin(), occupied.end(),
              [](const Span& left, const Span& right) { return left.begin < right.begin; });

    std::uint64_t largest = 0U;
    std::uint64_t cursor = 0U;
    for (const Span& span : occupied)
    {
        if (span.begin > cursor)
        {
            largest = std::max(largest, span.begin - cursor);
        }
        cursor = std::max(cursor, span.end);
    }
    if (cursor < m_capacity)
    {
        largest = std::max(largest, m_capacity - cursor);
    }
    return largest;
}

bool UploadRingAllocator::IsIdle() const noexcept
{
    return m_current.empty() && m_pending.empty();
}
} // namespace MiniEngine::Rhi::D3D12
