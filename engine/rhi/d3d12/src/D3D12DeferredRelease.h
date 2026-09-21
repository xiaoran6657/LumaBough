// ============================================================================
// D3D12DeferredRelease.h — 按 fence 延迟释放的对象队列（纯 CPU，无 D3D 类型）
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 3 条）
// 职责：把"旧对象什么时候才能释放"收敛成一个判据：`completedFence >= retireFence`。
//       热重载（材质/环境）、resize、descriptor 表替换都走这里——旧对象只在
//       **最后引用它的 frame fence** 完成后释放，绝不按 CPU 帧号/交换链索引/时间猜测。
// 与 05 篇 descriptor allocator 的关系：descriptor 的 Retire/Reclaim 是同一判据在
//       区间记账上的实现；本类是通用版本（payload 由调用方给出，通常是 ComPtr 捕获）。
// 顺序契约：fence 必须非零且单调（乱序会让"按 completed 一次性回收"漏项）。
// 内部性说明：后端内部类型（src/）——它是机制而非公共契约。
// 关联：docs/architecture/README.md（Hot reload 与 deferred release）
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <stdexcept>
#include <utility>

namespace MiniEngine::Rhi::D3D12
{
class D3D12DeferredRelease final
{
  public:
    // 入队一个待释放对象：fenceValue 是"最后引用它的提交"的 fence。
    // 失败：fenceValue == 0、回调为空 → std::invalid_argument；fence 回退 → std::logic_error。
    void Retire(std::uint64_t fenceValue, std::function<void()> release)
    {
        if (fenceValue == 0U || !release)
        {
            throw std::invalid_argument{"invalid deferred release entry"};
        }
        if (fenceValue < m_lastFenceValue)
        {
            throw std::logic_error{"deferred release fences must be monotonic"};
        }
        m_entries.push_back(Entry{fenceValue, std::move(release)});
        m_lastFenceValue = fenceValue;
        ++m_retiredCount;
    }

    // 回收所有 retireFence <= completedFenceValue 的条目（按入队顺序执行回调）。
    std::size_t Reclaim(const std::uint64_t completedFenceValue)
    {
        std::size_t reclaimed = 0U;
        while (!m_entries.empty() && m_entries.front().fenceValue <= completedFenceValue)
        {
            // 先取出并出队再执行：回调里若再次 Retire（嵌套释放）不会破坏队列状态。
            std::function<void()> release = std::move(m_entries.front().release);
            m_entries.pop_front();
            if (release)
            {
                release();
            }
            ++reclaimed;
        }
        m_reclaimedCount += reclaimed;
        return reclaimed;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return m_entries.empty();
    }

    [[nodiscard]] std::size_t PendingCount() const noexcept
    {
        return m_entries.size();
    }

    // 统计（进 metadata/报告）：累计入队与累计已回收。
    [[nodiscard]] std::uint64_t RetiredCount() const noexcept
    {
        return m_retiredCount;
    }

    [[nodiscard]] std::uint64_t ReclaimedCount() const noexcept
    {
        return m_reclaimedCount;
    }

    [[nodiscard]] std::uint64_t OldestFence() const noexcept
    {
        return m_entries.empty() ? 0U : m_entries.front().fenceValue;
    }

  private:
    struct Entry final
    {
        std::uint64_t fenceValue = 0;
        std::function<void()> release;
    };

    std::deque<Entry> m_entries;
    std::uint64_t m_lastFenceValue = 0;
    std::uint64_t m_retiredCount = 0;
    std::uint64_t m_reclaimedCount = 0;
};
} // namespace MiniEngine::Rhi::D3D12
