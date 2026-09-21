// ============================================================================
// WorkDeque.h — M7-04 per-worker 工作队列（owner bottom / thief top）
// 里程碑：M7-04（调度器、工作窃取与等待）
// 形态：每个 worker 一个 deque + 短临界区 mutex（v1 用锁证明正确性；lock-free 只在
//   有 profile 证据表明 deque 锁是新瓶颈时才另开实验，见 ADR-0008 备选方案）。
// 语义：
//   * owner 从 bottom push/pop（LIFO，最近任务优先，局部性好、深度优先展开递归）；
//   * thief 从 top 窃取最旧任务（FIFO 暴露较大并行分支）；
//   * 容量有界：本地满时必须回滚 group 并把任务路由到有界 inject queue，
//     递归提交不得造成本地无界增长。
// 关联：engine/tasks/src/TaskSystem.cpp（TryTakeOrSteal）、docs/architecture/README.md
// ============================================================================

#pragma once

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Tasks/Task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace MiniEngine::Tasks
{
class WorkDeque final
{
  public:
    explicit WorkDeque(const std::size_t capacity) : m_capacity(capacity)
    {
        // 容量 0 的本地队列会让 worker 永远只能依赖 inject queue：配置契约错误，Release 也暴露。
        ME_VERIFY(capacity > 0, "work deque capacity must be positive");
    }

    [[nodiscard]] bool TryPushLocal(Task task)
    {
        std::lock_guard lock(m_mutex);
        if (!m_accepting || m_tasks.size() >= m_capacity)
            return false;
        m_tasks.push_back(task);
        // 深度高水位在锁内维护：避免调用方为了统计再取一次锁（这是递归提交的热路径）。
        const auto depth = m_tasks.size();
        auto observed = m_highWater.load(std::memory_order_relaxed);
        while (depth > observed &&
               !m_highWater.compare_exchange_weak(observed, depth, std::memory_order_relaxed,
                                                 std::memory_order_relaxed))
        {
        }
        return true;
    }

    [[nodiscard]] std::optional<Task> PopLocal()
    {
        std::lock_guard lock(m_mutex);
        if (m_tasks.empty())
            return std::nullopt;

        Task task = m_tasks.back();
        m_tasks.pop_back();
        return task;
    }

    [[nodiscard]] std::optional<Task> StealOldest()
    {
        std::lock_guard lock(m_mutex);
        if (m_tasks.empty())
            return std::nullopt;

        Task task = m_tasks.front();
        m_tasks.pop_front();
        return task;
    }

    [[nodiscard]] std::size_t Size() const
    {
        std::lock_guard lock(m_mutex);
        return m_tasks.size();
    }

    [[nodiscard]] bool Empty() const
    {
        std::lock_guard lock(m_mutex);
        return m_tasks.empty();
    }

    // 取消路径：一次性取空（调用方负责 group.Done 记账与 payload 回收）。
    [[nodiscard]] std::deque<Task> DrainAll()
    {
        std::lock_guard lock(m_mutex);
        std::deque<Task> drained;
        drained.swap(m_tasks);
        return drained;
    }

    // 深度高水位（诊断递归深度）：relaxed 读，不参与同步。
    [[nodiscard]] std::uint64_t HighWater() const noexcept
    {
        return m_highWater.load(std::memory_order_relaxed);
    }

    void StopAccepting()
    {
        std::lock_guard lock(m_mutex);
        m_accepting = false;
    }

  private:
    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<Task> m_tasks;
    bool m_accepting = true;
    std::atomic<std::uint64_t> m_highWater{0};
};
} // namespace MiniEngine::Tasks

// Do not replace this with a lock-free deque until an experiment demonstrates
// that these short critical sections are an end-to-end bottleneck.
// QueueFull must roll back TaskGroup/payload ownership or route to the bounded
// inject queue; recursive submission must not create unbounded local growth.
