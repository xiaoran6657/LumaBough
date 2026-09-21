// ============================================================================
// TaskGroup.h — M7 任务组（不可复制的完成计数器）
// 里程碑：M7-03（任务系统契约、所有权与生命周期）
// 契约（顺序不可交换）：
//   1. 先初始化 task/payload；
//   2. 再 Add(1) 预留完成位；
//   3. 最后 publish 到队列；publish 失败必须回滚 Add 与 payload；
//   4. task 无论成功/取消都恰好 Done() 一次（由 Execute 的 scope guard 保证）；
//   5. counter 到 0 唤醒等待者；Wait 返回后调用者可安全回收本组 context。
// fail-fast：counter overflow / underflow（重复 Done）是破坏生命周期契约的程序错误，
//   所有构建配置都必须终止进程。不能用会在 Release 消失的 Debug-only assert 代替：
//   Debug/Release 行为不一致会让契约在性能 run 里静默失效。
// 关联：docs/architecture/README.md（TaskGroup 契约 / 等待语义）
// ============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>

namespace MiniEngine::Tasks
{
namespace Detail
{
// 契约违反：写一行 stderr（便于在测试/CI 日志里定位）后终止进程。
// 这里不依赖 Core 的日志：完成计数是任务系统最底层的不变量，必须在任何服务可用之前
// 也能 fail-fast。
[[noreturn]] inline void FailTaskContract(const char* reason) noexcept
{
    std::fputs("[MiniEngine::Tasks] contract violation: ", stderr);
    std::fputs(reason == nullptr ? "unknown" : reason, stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
    std::terminate();
}
} // namespace Detail

class TaskGroup final
{
  public:
    TaskGroup() = default;
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    void Add(std::uint32_t count = 1) noexcept
    {
        if (count == 0)
            return;

        auto current = m_remaining.load(std::memory_order_relaxed);
        for (;;)
        {
            if (count > std::numeric_limits<std::uint32_t>::max() - current)
                Detail::FailTaskContract("TaskGroup::Add overflow");
            if (m_remaining.compare_exchange_weak(current, current + count, std::memory_order_relaxed,
                                                  std::memory_order_relaxed))
                return;
        }
    }

    // 完成一次。**必须在持锁状态下**把计数减到 0 并 notify：这样"计数归零"这一观察结果
    // 只在完成线程离开 TaskGroup 所有成员之后才对等待者可见。
    // 否则会出现真实的 use-after-free：等待者看到 IsComplete()==true → 返回 → 析构 group，
    // 而完成线程还在 lock/notify 组内的 mutex/CV（Debug CRT 报 "unlock of unowned mutex"，
    // Release 则是踩到下一帧复用的同地址对象 → 随机挂起）。
    void Done() noexcept
    {
        std::lock_guard lock(m_waitMutex);
        auto current = m_remaining.load(std::memory_order_relaxed);
        for (;;)
        {
            if (current == 0)
                Detail::FailTaskContract("TaskGroup::Done without a matching Add");
            if (m_remaining.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
                break;
        }

        if (current == 1)
            m_completed.notify_all(); // 持锁 notify：与等待者的 check→wait 窗口互斥，不丢唤醒。
    }

    [[nodiscard]] bool IsComplete() const noexcept
    {
        return m_remaining.load(std::memory_order_acquire) == 0;
    }

    [[nodiscard]] std::uint32_t Remaining() const noexcept
    {
        return m_remaining.load(std::memory_order_relaxed);
    }

    // 只允许 TaskSystem 的 NonHelping 路径调用：等待者不取任务，只等完成通知。
    // helper 角色（MainThread/Worker）等待调度器的工作条件变量，以便新发布的任务能唤醒它。
    void WaitUntilComplete()
    {
        std::unique_lock lock(m_waitMutex);
        m_completed.wait(lock, [this] { return IsComplete(); });
    }

    // 完成屏障：任何"观察到本组已完成"的等待者，返回前必须调用一次，确保最后一个 Done
    // 已经离开 TaskGroup 的全部成员（mutex/CV）。调用之后 owner 可以安全析构本组。
    // helper 路径靠轮询 atomic 计数判断完成，因此它必须显式取一次锁；NonHelping 路径
    // 本身就是在锁内观察到完成的，再取一次只是保持一致的收尾语义。
    void SynchronizeWithCompleters() noexcept
    {
        std::lock_guard lock(m_waitMutex);
    }

  private:
    std::atomic<std::uint32_t> m_remaining{0};
    mutable std::mutex m_waitMutex;
    std::condition_variable m_completed;
};
} // namespace MiniEngine::Tasks
