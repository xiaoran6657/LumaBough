// ============================================================================
// GlobalTaskQueue.h — M7 v0 全局有锁任务队列（正确性基线）
// 里程碑：M7-03（任务系统契约、所有权与生命周期）
// 形态：mutex + deque + accepting 标志；容量有界；满/关闭时 TryPush 明确失败。
// 约束：
//   * 调用者（TaskSystem）负责在成功 publish 后唤醒等待者，且唤醒必须与等待者的
//     check→wait 窗口互斥（见 TaskSystem.cpp 的 WakeWaiters）；
//   * 锁内不执行用户 task、不等待；
//   * 本文件是 M7 v0 的性能基线：M7-04 才评估 per-worker deque / work stealing，
//     lock-free 不在 M7 必需项内（ADR-0008 备选方案）。
// 关联：engine/tasks/src/TaskSystem.cpp、docs/architecture/README.md
// ============================================================================

#pragma once

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Tasks/Task.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace MiniEngine::Tasks
{
class GlobalTaskQueue final
{
  public:
    explicit GlobalTaskQueue(const std::size_t capacity) : m_capacity(capacity)
    {
        // 容量 0 会让调度器永远无法接收任务：这是配置契约错误，Release 也必须暴露。
        ME_VERIFY(capacity > 0, "global task queue capacity must be positive");
    }

    [[nodiscard]] bool TryPush(Task task)
    {
        std::lock_guard lock(m_mutex);
        if (!m_accepting || m_tasks.size() >= m_capacity)
            return false;
        m_tasks.push_back(task);
        return true;
    }

    // 任意任务（worker / 显式 Worker 角色的等待者）。
    [[nodiscard]] std::optional<Task> TryPop()
    {
        std::lock_guard lock(m_mutex);
        return PopFrontLocked();
    }

    // 只取 MainHelpAllowed 任务（main thread 帮助路径）。队首不满足时**不弹出**：
    // 弹出再放回会破坏 FIFO 并可能造成活锁；non-helpable 任务由 worker 取走。
    [[nodiscard]] std::optional<Task> TryPopForMainHelp()
    {
        std::lock_guard lock(m_mutex);
        if (m_tasks.empty() || !HasFlag(m_tasks.front().flags, TaskFlags::MainHelpAllowed))
            return std::nullopt;
        return PopFrontLocked();
    }

    // 关闭取消路径：一次性取空队列（调用方负责 group.Done 记账与 payload 回收）。
    [[nodiscard]] std::deque<Task> DrainAll()
    {
        std::lock_guard lock(m_mutex);
        std::deque<Task> drained;
        drained.swap(m_tasks);
        return drained;
    }

    // 只窥探不弹出：给等待者的谓词用（必须与 TryPopForMainHelp 的取法一致，否则会空转）。
    [[nodiscard]] bool FrontAllowsMainHelp() const
    {
        std::lock_guard lock(m_mutex);
        return !m_tasks.empty() && HasFlag(m_tasks.front().flags, TaskFlags::MainHelpAllowed);
    }

    void StopAccepting()
    {
        std::lock_guard lock(m_mutex);
        m_accepting = false;
    }

    [[nodiscard]] bool Accepting() const
    {
        std::lock_guard lock(m_mutex);
        return m_accepting;
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

  private:
    [[nodiscard]] std::optional<Task> PopFrontLocked()
    {
        if (m_tasks.empty())
            return std::nullopt;
        Task task = m_tasks.front();
        m_tasks.pop_front();
        return task;
    }

    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<Task> m_tasks;
    bool m_accepting = true;
};
} // namespace MiniEngine::Tasks
