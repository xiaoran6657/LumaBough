// ============================================================================
// TaskTestSupport.h — M7-03 任务测试共享夹具（非产品代码）
// 里程碑：M7-03（任务系统契约、所有权与生命周期）
// 职责：两个测试文件共用的最小工具：计数任务、有界阻塞任务、payload 所有权/析构
//   记账池。模板里提到的 Fixture 属于测试私有实现，必须按真实接口自行实现。
// 关联：tests/tasks/TaskSystemTests.cpp、tests/tasks/TaskStressTests.cpp
// ============================================================================

#pragma once

#include <MiniEngine/Tasks/TaskSystem.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace MiniEngine::Tasks::TestSupport
{
inline void Increment(void* raw) noexcept
{
    static_cast<std::atomic<std::uint32_t>*>(raw)->fetch_add(1, std::memory_order_relaxed);
}

inline void RecordWorkerIndex(void* raw) noexcept
{
    static_cast<std::atomic<std::uint32_t>*>(raw)->store(TaskSystem::CurrentWorkerIndex(), std::memory_order_release);
}

// 有界阻塞任务：占住唯一 worker 一段确定时间，用来制造"队列里确实排着任务"的窗口。
// 无界阻塞会让 Shutdown 的 join 死等，所以窗口必须有界（这也是 compute task 不得
// 阻塞等待的契约在测试里的边界）。
struct BoundedBlocker
{
    std::chrono::milliseconds hold{200};
    std::atomic<bool> entered{false};
};

inline void HoldWorker(void* raw) noexcept
{
    auto* blocker = static_cast<BoundedBlocker*>(raw);
    blocker->entered.store(true, std::memory_order_release);
    std::this_thread::sleep_for(blocker->hold);
}

// 等待 worker 真正进入阻塞任务；超时交给调用方的 EXPECT 兜底（不在等待里断言）。
inline bool WaitUntilEntered(const std::atomic<bool>& flag, const std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// ---------------------------------------------------------------------------
// payload 记账池：owner 显式拥有 payload；任务执行路径与取消回收路径都必须让它
// 恰好析构一次。outstanding 列表防止"取消回收"与"任务自回收"重复 delete。
// ---------------------------------------------------------------------------
class PayloadPool final
{
  public:
    struct Job
    {
        PayloadPool* pool = nullptr;
        std::uint64_t tag = 0;

        ~Job()
        {
            if (pool != nullptr)
                pool->m_destroyed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    [[nodiscard]] Job* Create()
    {
        auto* job = new Job{this, m_created.fetch_add(1, std::memory_order_relaxed) + 1};
        std::lock_guard lock(m_mutex);
        m_outstanding.push_back(job);
        return job;
    }

    // 任务执行路径：先从 outstanding 摘除再释放。
    void Retire(Job* job) noexcept
    {
        {
            std::lock_guard lock(m_mutex);
            std::erase(m_outstanding, job);
        }
        delete job;
    }

    // 取消路径：group 完成后 owner 回收未执行的 payload。
    void ReclaimOutstanding()
    {
        std::vector<Job*> remaining;
        {
            std::lock_guard lock(m_mutex);
            remaining.swap(m_outstanding);
        }
        for (Job* job : remaining)
            delete job;
    }

    [[nodiscard]] std::uint64_t Created() const noexcept
    {
        return m_created.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t Destroyed() const noexcept
    {
        return m_destroyed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t Outstanding() const
    {
        std::lock_guard lock(m_mutex);
        return m_outstanding.size();
    }

  private:
    mutable std::mutex m_mutex;
    std::vector<Job*> m_outstanding;
    std::atomic<std::uint64_t> m_created{0};
    std::atomic<std::uint64_t> m_destroyed{0};
};

inline void RunPayloadTask(void* raw) noexcept
{
    auto* job = static_cast<PayloadPool::Job*>(raw);
    job->pool->Retire(job);
}

// 有界队列的正常使用方式：QueueFull 时让出 CPU 后重试（重试是安全的——失败的
// Submit 已经把 group 计数回滚）。测试里提交量大于 inject 容量时必须用它，
// 否则断言 Accepted 会因为"提交快于消费"这种正常背压而失败。
[[nodiscard]] inline bool SubmitWithBackpressure(TaskSystem& tasks, Task task, TaskGroup& group,
                                                const std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        const SubmitResult result = tasks.Submit(task, group);
        if (result == SubmitResult::Accepted)
            return true;
        if (result != SubmitResult::QueueFull)
            return false; // Stopping/InvalidTask 不是背压，直接失败。
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
}

// ---------------------------------------------------------------------------
// 挂起诊断：正常路径零输出（不改变时序），只在"心跳停止 3 秒"时打印最后一次
// 主线程快照并终止进程。竞态用例挂起时报错文本为空是最难查的一类失败，这个看门狗
// 让挂起自带现场（阶段、round、pending/queue/local/sleeping、per-worker 计数）。
// 快照字段全部由主线程在安全点写入（不读正在销毁的 TaskSystem）。
// ---------------------------------------------------------------------------
struct StallDiag
{
    std::atomic<std::uint64_t> heartbeat{0};
    std::atomic<std::uint32_t> round{0};
    std::atomic<std::uint32_t> phase{0}; // 0=ctor 1=submit 2=wait 3=shutdown 4=done
    std::atomic<std::uint32_t> groupRemaining{0};
    std::atomic<std::uint64_t> pending{0};
    std::atomic<std::uint64_t> queueDepth{0};
    std::atomic<std::uint64_t> localQueueDepth{0};
    std::atomic<std::uint64_t> sleepingWorkers{0};
    std::atomic<std::uint64_t> executed{0};
    std::atomic<std::uint64_t> sleeps{0};
    std::atomic<std::uint64_t> wakeups{0};
    std::atomic<std::uint64_t> stealAttempts{0};
    std::atomic<std::uint64_t> stealSuccesses{0};
    std::atomic<std::uint64_t> localPushes{0};
    std::atomic<std::uint64_t> injectedPushes{0};
};

// CRT-free 面包屑：只在环境变量 ME_STALL_BREADCRUMB 指向文件时启用（默认完全无 I/O，
// 不影响被观测系统的时序）。用 Win32 直接写文件，绕过 CRT 的文件锁与堆——挂起时
// fprintf(stderr) 可能因为锁竞争永远拿不到，这一步就是为了排除那个可能。
#if defined(_WIN32)
class StallBreadcrumb final
{
  public:
    StallBreadcrumb()
    {
        wchar_t path[512] = {};
        const DWORD length = GetEnvironmentVariableW(L"ME_STALL_BREADCRUMB", path, 512);
        if (length == 0 || length >= 512)
            return;
        m_handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    }

    ~StallBreadcrumb()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }

    StallBreadcrumb(const StallBreadcrumb&) = delete;
    StallBreadcrumb& operator=(const StallBreadcrumb&) = delete;

    void Write(const char* tag, const std::uint32_t phase, const std::uint32_t round) noexcept
    {
        if (m_handle == INVALID_HANDLE_VALUE)
            return;
        char buffer[160] = {};
        const int length = std::snprintf(buffer, sizeof(buffer), "%s phase=%u round=%u\n", tag, phase, round);
        if (length <= 0)
            return;
        DWORD written = 0;
        (void)WriteFile(m_handle, buffer, static_cast<DWORD>(length), &written, nullptr);
        (void)FlushFileBuffers(m_handle);
    }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
#endif

// 没有 TaskSystem 可用时（例如构造之前）只推进相位。
inline void PublishStallPhase(StallDiag& diag, const std::uint32_t phase, const std::uint32_t round)
{
    diag.phase.store(phase, std::memory_order_relaxed);
    diag.round.store(round, std::memory_order_relaxed);
    diag.heartbeat.fetch_add(1, std::memory_order_release);
}

// 主线程在每次进入/离开阶段时调用：**只写原子**（不加任何锁）。
// 诊断本身不能改变被观测系统的时序——带锁的快照会显著降低竞态复现率。
inline void PublishStallDiag(StallDiag& diag, const TaskSystem& tasks, const std::uint32_t phase,
                            const std::uint32_t round, const std::uint32_t groupRemaining)
{
    std::uint64_t sleeps = 0;
    std::uint64_t wakeups = 0;
    std::uint64_t attempts = 0;
    std::uint64_t successes = 0;
    std::uint64_t localPushes = 0;
    std::uint64_t injectedPushes = 0;
    std::uint64_t localHighWater = 0;
    for (std::uint32_t worker = 0; worker < tasks.WorkerCount(); ++worker)
    {
        // WorkerCountersFor 是纯原子读（local deque 深度走高水位原子），因此无锁。
        const WorkerCounters counters = tasks.WorkerCountersFor(worker);
        sleeps += counters.sleeps;
        wakeups += counters.wakeups;
        attempts += counters.stealAttempts;
        successes += counters.stealSuccesses;
        localPushes += counters.localPushes;
        injectedPushes += counters.injectedPushes;
        localHighWater += counters.localQueueHighWater;
    }
    diag.sleeps.store(sleeps, std::memory_order_relaxed);
    diag.wakeups.store(wakeups, std::memory_order_relaxed);
    diag.stealAttempts.store(attempts, std::memory_order_relaxed);
    diag.stealSuccesses.store(successes, std::memory_order_relaxed);
    diag.localPushes.store(localPushes, std::memory_order_relaxed);
    diag.injectedPushes.store(injectedPushes, std::memory_order_relaxed);
    diag.localQueueDepth.store(localHighWater, std::memory_order_relaxed); // 高水位（无锁近似）
    diag.phase.store(phase, std::memory_order_relaxed);
    diag.round.store(round, std::memory_order_relaxed);
    diag.groupRemaining.store(groupRemaining, std::memory_order_relaxed);
    diag.heartbeat.fetch_add(1, std::memory_order_release);
}

class StallWatchdog final
{
  public:
    explicit StallWatchdog(StallDiag& diag) : m_diag(diag)
    {
        m_thread = std::thread([this] { Loop(); });
    }

    ~StallWatchdog()
    {
        m_stop.store(true, std::memory_order_relaxed);
        if (m_thread.joinable())
            m_thread.join();
    }

    StallWatchdog(const StallWatchdog&) = delete;
    StallWatchdog& operator=(const StallWatchdog&) = delete;

  private:
    void Loop()
    {
        std::uint64_t last = m_diag.heartbeat.load(std::memory_order_acquire);
        std::uint32_t stuckMilliseconds = 0;
        while (!m_stop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uint64_t now = m_diag.heartbeat.load(std::memory_order_acquire);
            if (now != last)
            {
                last = now;
                stuckMilliseconds = 0;
                continue;
            }
            stuckMilliseconds += 100;
            if (stuckMilliseconds < 3000)
                continue;

            std::fprintf(stderr,
                         "[stall] stuck 3s: phase=%u round=%u groupRemaining=%u pending=%llu queue=%llu local=%llu "
                         "sleeping=%llu executed=%llu sleeps=%llu wakeups=%llu stealAttempts=%llu stealSuccesses=%llu "
                         "localPushes=%llu injectedPushes=%llu\n",
                         m_diag.phase.load(std::memory_order_relaxed), m_diag.round.load(std::memory_order_relaxed),
                         m_diag.groupRemaining.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.pending.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.queueDepth.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.localQueueDepth.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.sleepingWorkers.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.executed.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.sleeps.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.wakeups.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.stealAttempts.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.stealSuccesses.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.localPushes.load(std::memory_order_relaxed),
                         (unsigned long long)m_diag.injectedPushes.load(std::memory_order_relaxed));
            std::fflush(stderr);
            std::_Exit(3); // 挂起就是失败：让 ctest 立刻看到，而不是等 300s 超时。
        }
    }

    StallDiag& m_diag;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
} // namespace MiniEngine::Tasks::TestSupport
