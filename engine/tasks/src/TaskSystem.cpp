// ============================================================================
// TaskSystem.cpp — M7 任务系统实现（v0 全局队列 / v1 per-worker deque + 窃取）
// 里程碑：M7-03（契约、所有权与生命周期）、M7-04（调度器、工作窃取与等待）
// 职责：Task/TaskGroup 生命周期的唯一执行者：提交记账、worker 循环、wait-help、
//   shutdown 状态机（Running→Draining|Cancelling→Stopped）与统计。
// 依赖纪律：只用 Core（断言）与 Profiling（线程名/zone）；不 include RHI/asset/render，
//   不执行文件 I/O、GPU 等待或 RHI 调用（ADR-0008 第 5—7 条）。
// 关键顺序（不可交换，测试按此断言）：
//   Submit: 校验 → group.Add → publish → 失败回滚 group.Done；
//   Execute: 执行 task → 记 executed/pending → CompleteGuard 里 group.Done 恰好一次；
//   Shutdown: CAS 状态 → StopAccepting → drain 或 cancel → request_stop → 唤醒 → join。
// 版本差异（同一二进制内可选，便于 A/B 与 sweep）：
//   v0 GlobalQueue   ：单队列 + 条件变量（M7-03 基线）；
//   v1 PerWorkerDeque：worker 提交进自己的 local deque（LIFO），外部提交走 inject queue，
//                      worker 取任务顺序为 local → inject → 窃取若干 victim（FIFO 最旧）。
// 锁序（唯一的嵌套方向）：m_workMutex → { inject queue mutex, local deque mutex }。
//   反向不存在：发布路径先释放队列锁再进 WakeAllWaiters；deque 方法是叶子。
// 关联：docs/architecture/README.md、docs/architecture/README.md
// ============================================================================

#include <MiniEngine/Tasks/TaskSystem.h>

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Profiling/Profile.h>

#include "GlobalTaskQueue.h"
#include "WorkDeque.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace MiniEngine::Tasks
{
namespace
{
// worker-local context：worker 入口设置，退出清理。非 worker 线程保持 kNonWorkerIndex。
thread_local std::uint32_t t_workerIndex = kNonWorkerIndex;

[[nodiscard]] std::uint32_t HashCurrentThread() noexcept
{
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

[[nodiscard]] std::uint64_t NowTicks() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

[[nodiscard]] std::uint64_t TicksToNanoseconds(const std::uint64_t ticks) noexcept
{
    using Duration = std::chrono::steady_clock::duration;
    const Duration span{static_cast<Duration::rep>(ticks)};
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(span).count());
}
} // namespace

std::uint32_t DefaultWorkerCount() noexcept
{
    const unsigned int hardware = std::thread::hardware_concurrency();
    // 0 表示不可用；1/2 核时保留 main/render 与 I/O 线程后退化为 1 个 worker（显式 fallback）。
    if (hardware <= 2U)
        return 1U;
    return static_cast<std::uint32_t>(hardware - 2U);
}

class TaskSystem::Impl final
{
  public:
    enum class State : std::uint8_t
    {
        Running,
        Draining,
        Cancelling,
        Stopped
    };

    explicit Impl(const TaskSystemConfig& config) : m_config(config), m_queue(config.injectQueueCapacity)
    {
        // workerCount==0 会让 drain 无法完成 non-helpable 任务（main 不允许取它们），
        // 因此这不是"仅主线程模式"，而是必然死锁的配置错误。
        ME_VERIFY(config.workerCount >= 1, "task system requires at least one worker");

        // WorkerStatistics 含 std::atomic（不可复制/移动），不能放进 std::vector 的
        // resize 路径；用对齐的数组分配（aligned new 会满足 alignas 要求）。
        m_statistics = std::make_unique<WorkerStatistics[]>(m_config.workerCount);
        m_workerNames.reserve(m_config.workerCount);
        for (std::uint32_t index = 0; index < m_config.workerCount; ++index)
            m_workerNames.push_back("ME Worker " + std::to_string(index));

        // v1：per-worker local deque + worker-local PRNG（禁用全局 rand，保证可复现）。
        if (m_config.mode == SchedulerMode::PerWorkerDeque)
        {
            m_local.reserve(m_config.workerCount);
            m_prngState.reserve(m_config.workerCount);
            for (std::uint32_t index = 0; index < m_config.workerCount; ++index)
            {
                m_local.push_back(std::make_unique<WorkDeque>(m_config.localQueueCapacity));
                // XorShift64 的种子不能为 0：seed + index 再取非零兜底。
                const std::uint64_t seed = m_config.randomSeed + index;
                m_prngState.push_back(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed);
            }
        }

        // 先建好全部名字再起线程：worker 入口读取 m_workerNames[index]，不能与 push_back 竞争。
        m_workers.reserve(m_config.workerCount);
        for (std::uint32_t index = 0; index < m_config.workerCount; ++index)
        {
            m_workers.emplace_back([this, index](const std::stop_token stopToken) { WorkerMain(stopToken, index); });
        }
    }

    ~Impl() = default;

    [[nodiscard]] SubmitResult Publish(Task& task) noexcept
    {
        // 整个"记账窗口"（state 检查 → pending++ → 入队/回滚）与 Shutdown 的清点互斥：
        // 否则提交线程在此窗口内被抢占时，Shutdown 的 `pending == 0` 断言会看到半记账状态
        // 并终止进程（M7-TASK-RACE-FLAKE 的根因，回归用例
        // TaskStressTests.ShutdownWaitsForInFlightPublishAccounting）。
        const std::lock_guard<std::mutex> publishGate(m_publishGate);
        if (m_state.load(std::memory_order_acquire) != State::Running)
            return SubmitResult::Stopping;

        // 先记 pending 再入队：worker 可能在 push 返回前就弹出并执行（pending 必须先可见）。
        m_pending.fetch_add(1, std::memory_order_release);
        // 测试缝（默认 0＝关闭）：拉长"已记 pending、归宿未定"的窗口，供 M7-TASK-RACE-FLAKE
        // 的回归用例确定复现；生产配置恒为 0，只多一次比较。
        if (m_config.injectPublishStallMicroseconds != 0)
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::microseconds(m_config.injectPublishStallMicroseconds);
            while (std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
        }
        // v1：worker 内部的递归提交先进自己的 local deque（LIFO 局部性）；本地满或外部
        // 提交走有界 inject queue。两条路径的成功都表示"任务已发布"。
        const bool localRouted = TryPublishToLocal(task);
        if (!localRouted && !m_queue.TryPush(task))
        {
            m_pending.fetch_sub(1, std::memory_order_relaxed);
            // 入队失败可能是容量满，也可能是 shutdown 已经把 accepting 置 false。
            return m_state.load(std::memory_order_acquire) == State::Running ? SubmitResult::QueueFull
                                                                             : SubmitResult::Stopping;
        }
        m_submitted.fetch_add(1, std::memory_order_relaxed);
        // 只在"确实有线程在等"时唤醒（文档「victim 与退避」：publish 从"无工作"变为"有工作"
        // 时才发通知）。每次 publish 都 notify_all 会制造惊群与上下文切换风暴。
        // 安全性：等待者在进入等待前自增 m_waitingThreads，且谓词读队列时取队列锁；
        // 因此"发布后读到 0"意味着该线程要么尚未决定等待、要么会看到这份工作 —— 不丢唤醒。
        WakeIfWaitersPresent();
        return SubmitResult::Accepted;
    }

    void WaitImpl(TaskGroup& group, const TaskWaitRole role) noexcept
    {
        ME_ASSERT(CallerMatchesDeclaredRole(role), "waiter thread does not match the declared wait role");
        const TaskWaitRole effective = ResolveEffectiveRole(role);

        if (effective == TaskWaitRole::NonHelping)
        {
            // 不取任务：只等完成通知（render/I/O 线程保持 RHI ownership 与调度可预测）。
            group.WaitUntilComplete();
            group.SynchronizeWithCompleters();
            return;
        }

        while (!group.IsComplete())
        {
            if (TryExecuteOneImpl(effective))
                continue;
            WaitForWorkOrGroup(group, effective);
        }

        // 完成屏障：helper 路径靠轮询原子计数观察到完成，必须再取一次组内锁，
        // 确认最后一个 Done 已经离开 TaskGroup，之后调用者才可以安全回收/析构本组。
        group.SynchronizeWithCompleters();
    }

    // 公开入口：与 Wait 一样先校验角色身份再解析安全角色。否则"非 worker 声称 Worker"
    // 会取走 non-helpable 任务并在错误的线程上下文里执行它。
    [[nodiscard]] bool TryExecuteOnePublic(const TaskWaitRole role) noexcept
    {
        ME_ASSERT(CallerMatchesDeclaredRole(role), "waiter thread does not match the declared wait role");
        return TryExecuteOneImpl(ResolveEffectiveRole(role));
    }

    // v1 发布路径：只有 worker 线程的提交才进 local deque；本地满回退 inject（由调用方做）。
    [[nodiscard]] bool TryPublishToLocal(Task& task) noexcept
    {
        if (m_config.mode != SchedulerMode::PerWorkerDeque)
            return false;
        const std::uint32_t index = t_workerIndex;
        if (index == kNonWorkerIndex || index >= m_local.size())
            return false; // 外部提交（main/render/I/O/测试）统一走 inject queue。

        if (m_local[index]->TryPushLocal(task))
        {
            m_statistics[index].localPushes.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // 本地满：任务必须落到有界 inject queue，递归提交不得造成本地无界增长。
        m_statistics[index].injectedPushes.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] bool TryExecuteOneImpl(const TaskWaitRole role) noexcept
    {
        std::optional<Task> task = TryTake(role);
        if (!task.has_value())
            return false;
        // helper 执行：worker 线程帮助时计入自己的统计，main/render 线程不计入任何 worker。
        Execute(*task, t_workerIndex);
        return true;
    }

    void Shutdown(const bool drain) noexcept
    {
        State expected = State::Running;
        if (!m_state.compare_exchange_strong(expected, drain ? State::Draining : State::Cancelling,
                                             std::memory_order_acq_rel))
        {
            // 幂等：另一个调用者（或先前的调用）已经接管；这里只做 join 收尾。
            JoinIfNeeded();
            return;
        }

        // accepting 必须在清空队列之前关闭：否则 shutdown 之后仍可能被塞入永不完成的任务。
        m_queue.StopAccepting();
        for (const std::unique_ptr<WorkDeque>& deque : m_local)
            deque->StopAccepting();
        WakeAllWaiters();

        // 等所有"已在途"的 publish 落定（M7-TASK-RACE-FLAKE）：它们要么已入队（会被下面的
        // 排空/取消看见），要么因 accepting 已关闭而回滚；此后到达的提交会在 state 检查处
        // 被拒（CAS 已在本函数开头完成），不再触碰 pending。空作用域＝只借用锁做一次屏障。
        {
            const std::lock_guard<std::mutex> publishBarrier(m_publishGate);
        }

        if (drain)
        {
            // 排空：worker 仍在运行（尚未 request_stop），main 只帮助 MainHelpAllowed 任务；
            // 落在 local deque 里的任务由各 worker 自己消费（它们仍在循环）。
            while (m_pending.load(std::memory_order_acquire) != 0)
            {
                if (!TryExecuteOneImpl(TaskWaitRole::MainThread))
                    WaitForAnyProgress();
            }
        }
        else
        {
            // 取消：未开始任务按 group.Done 记账；payload 由 group 完成后的 owner 回收。
            (void)CancelQueuedTasks();
        }

        for (std::jthread& worker : m_workers)
            worker.request_stop();
        WakeAllWaiters();
        JoinIfNeeded();

        ME_VERIFY(m_pending.load(std::memory_order_acquire) == 0, "shutdown must leave no pending tasks");
        m_state.store(State::Stopped, std::memory_order_release);
    }

    [[nodiscard]] std::uint32_t WorkerCount() const noexcept
    {
        return m_config.workerCount;
    }

    [[nodiscard]] bool IsAccepting() const noexcept
    {
        return m_state.load(std::memory_order_acquire) == State::Running && m_queue.Accepting();
    }

    [[nodiscard]] TaskSystemStatistics Statistics() const
    {
        TaskSystemStatistics snapshot;
        snapshot.submitted = m_submitted.load(std::memory_order_relaxed);
        snapshot.executed = m_executed.load(std::memory_order_relaxed);
        snapshot.cancelled = m_cancelled.load(std::memory_order_relaxed);
        snapshot.pending = m_pending.load(std::memory_order_relaxed);
        snapshot.queueDepth = m_queue.Size();
        for (const std::unique_ptr<WorkDeque>& deque : m_local)
            snapshot.localQueueDepth += deque->Size();
        snapshot.sleepingWorkers = m_sleepingWorkers.load(std::memory_order_relaxed);
        snapshot.workerCount = m_config.workerCount;
        return snapshot;
    }

    [[nodiscard]] WorkerCounters WorkerCountersFor(const std::uint32_t workerIndex) const noexcept
    {
        ME_ASSERT(workerIndex < m_config.workerCount, "worker index out of range");
        if (workerIndex >= m_config.workerCount)
            return {};
        const WorkerStatistics& statistics = m_statistics[workerIndex];
        WorkerCounters counters;
        counters.executed = statistics.executed.load(std::memory_order_relaxed);
        counters.stealAttempts = statistics.stealAttempts.load(std::memory_order_relaxed);
        counters.stealSuccesses = statistics.stealSuccesses.load(std::memory_order_relaxed);
        counters.sleeps = statistics.sleeps.load(std::memory_order_relaxed);
        counters.queueLatencyNanoseconds = statistics.queueLatencyNanoseconds.load(std::memory_order_relaxed);
        counters.localPushes = statistics.localPushes.load(std::memory_order_relaxed);
        counters.injectedPushes = statistics.injectedPushes.load(std::memory_order_relaxed);
        // local deque 深度走高水位原子读，绝不加锁（诊断接口不得干扰被观测系统）。
        counters.localQueueHighWater = workerIndex < m_local.size() ? m_local[workerIndex]->HighWater() : 0U;
        counters.wakeups = statistics.wakeups.load(std::memory_order_relaxed);
        counters.lastVictim = statistics.lastVictim.load(std::memory_order_relaxed);
        return counters;
    }

    [[nodiscard]] std::uint64_t NextTaskId() noexcept
    {
        return m_nextTaskId.fetch_add(1, std::memory_order_relaxed);
    }

  private:
    void WorkerMain(const std::stop_token stopToken, const std::uint32_t index) noexcept
    {
        t_workerIndex = index;
        // 用函数而不是 ME_PROFILE_THREAD 宏：宏在 Tracy off 时是 ((void)0)，系统线程名会一起
        // 丢掉；SetThreadName 在两种构建下都设置系统线程名，只在 Tracy 打开时额外发给 profiler。
        Profiling::SetThreadName(m_workerNames[index].c_str());

        while (!stopToken.stop_requested())
        {
            if (std::optional<Task> task = TryTakeForWorker(index))
            {
                Execute(*task, index);
                continue;
            }

            std::unique_lock lock(m_workMutex);
            m_sleepingWorkers.fetch_add(1, std::memory_order_relaxed);
            m_waitingThreads.fetch_add(1, std::memory_order_relaxed);
            m_statistics[index].sleeps.fetch_add(1, std::memory_order_relaxed);
            m_workAvailable.wait(lock,
                                 [this, &stopToken]
                                 {
                                     return stopToken.stop_requested() ||
                                            m_state.load(std::memory_order_acquire) == State::Stopped ||
                                            HasPotentialWorkFor(TaskWaitRole::Worker);
                                 });
            m_waitingThreads.fetch_sub(1, std::memory_order_relaxed);
            m_sleepingWorkers.fetch_sub(1, std::memory_order_relaxed);
            // 唤醒后若确实看到工作，记一次 wakeup（spin storm 的反面证据）。
            if (HasPotentialWorkFor(TaskWaitRole::Worker))
                m_statistics[index].wakeups.fetch_add(1, std::memory_order_relaxed);
        }

        t_workerIndex = kNonWorkerIndex;
    }

    void Execute(Task task, const std::uint32_t workerIndex) noexcept
    {
        ME_PROFILE_ZONE_NAMED("ExecuteTask");

        {
            struct CompleteGuard
            {
                TaskGroup* group;
                ~CompleteGuard()
                {
                    if (group != nullptr)
                        group->Done();
                }
            } complete{task.group};

            // task 函数是 noexcept：可恢复错误必须写进 payload/result slot。异常一旦越过这里会
            // 直接 std::terminate，worker 外层的 catch 无法修复被破坏的 group 计数。
            task.Execute();

            // 记账先于 group.Done：被唤醒的等待者可能立刻销毁 TaskSystem 并断言 pending==0。
            m_executed.fetch_add(1, std::memory_order_relaxed);
            m_pending.fetch_sub(1, std::memory_order_relaxed);
            if (workerIndex != kNonWorkerIndex && workerIndex < m_config.workerCount)
            {
                WorkerStatistics& statistics = m_statistics[workerIndex];
                statistics.executed.fetch_add(1, std::memory_order_relaxed);
                if (task.debug.enqueueTicks != 0)
                {
                    const std::uint64_t now = NowTicks();
                    if (now > task.debug.enqueueTicks)
                    {
                        statistics.queueLatencyNanoseconds.fetch_add(TicksToNanoseconds(now - task.debug.enqueueTicks),
                                                                     std::memory_order_relaxed);
                    }
                }
            }
        }

        // 任务完成会改变等待者的谓词（group.IsComplete() / pending 记账）：必须通知等待者，
        // 否则 helper 只能等满 wait_for 的超时（Windows 上 ≈15.6ms 定时器节拍）。
        WakeIfWaitersPresent();
    }

    [[nodiscard]] std::optional<Task> TryTake(const TaskWaitRole role) noexcept
    {
        switch (role)
        {
        case TaskWaitRole::Worker:
            return TryTakeForWorker(t_workerIndex);
        case TaskWaitRole::MainThread:
            // main helper 只从 inject queue 取 MainHelpAllowed 任务：不窃取 worker local，
            // 以免破坏 owner 的 LIFO 局部性、也避免"部分过滤"造成的活锁。递归提交在本地
            // 满时会回退到 inject，因此 main-help 不会被饿死。
            return m_queue.TryPopForMainHelp();
        case TaskWaitRole::NonHelping:
        default:
            return std::nullopt;
        }
    }

    // worker 取任务顺序：自己的 local deque（LIFO）→ inject queue → 窃取若干 victim（FIFO 最旧）。
    [[nodiscard]] std::optional<Task> TryTakeForWorker(const std::uint32_t index) noexcept
    {
        if (index != kNonWorkerIndex && index < m_local.size())
        {
            if (std::optional<Task> task = m_local[index]->PopLocal())
                return task;
        }
        if (std::optional<Task> task = m_queue.TryPop())
            return task;
        if (m_config.mode != SchedulerMode::PerWorkerDeque || index == kNonWorkerIndex || m_local.empty())
            return std::nullopt;

        // 固定次数的 steal 尝试：victim 由 worker-local PRNG 选择（避免所有 thief 抢同一个
        // victim）；尝试完毕仍无工作就进入条件变量，不做永久 busy-spin。
        const std::uint32_t attempts = std::max<std::uint32_t>(1U, m_config.stealAttemptsBeforeSleep);
        for (std::uint32_t attempt = 0; attempt < attempts; ++attempt)
        {
            const std::uint32_t victim = NextVictim(index);
            if (victim == index || victim >= m_local.size())
                continue;
            m_statistics[index].stealAttempts.fetch_add(1, std::memory_order_relaxed);
            if (std::optional<Task> task = m_local[victim]->StealOldest())
            {
                m_statistics[index].stealSuccesses.fetch_add(1, std::memory_order_relaxed);
                m_statistics[index].stolenTasks.fetch_add(1, std::memory_order_relaxed);
                m_statistics[index].lastVictim.store(victim, std::memory_order_relaxed);
                return task;
            }
        }
        return std::nullopt;
    }

    // XorShift64：worker-local 状态、固定 seed 可复现；显式禁用全局 rand()。
    [[nodiscard]] std::uint32_t NextVictim(const std::uint32_t index) noexcept
    {
        std::uint64_t& state = m_prngState[index];
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint32_t>(state % m_config.workerCount);
    }

    [[nodiscard]] bool AnyLocalWork() const
    {
        for (const std::unique_ptr<WorkDeque>& deque : m_local)
        {
            if (!deque->Empty())
                return true;
        }
        return false;
    }

    [[nodiscard]] bool HasPotentialWorkFor(const TaskWaitRole role) const
    {
        switch (role)
        {
        case TaskWaitRole::Worker:
            return AnyLocalWork() || !m_queue.Empty();
        case TaskWaitRole::MainThread:
            return m_queue.FrontAllowsMainHelp();
        case TaskWaitRole::NonHelping:
        default:
            return false;
        }
    }

    // 谓词必须与 TryTake 一致：否则等待者会在"有活但自己取不到"时反复空转。
    // 注意 wait_for 的超时不是精确轮询：Windows 上未提升定时器精度时一个 wait_for(200µs)
    // 实际会睡满 ~15.6ms 的定时器节拍（sweep 实测：仅靠超时唤醒会让 200 任务的帧固定 15ms）。
    // 因此超时只是兜底，正常唤醒必须来自 publish 与**任务完成**两条通知路径。
    void WaitForWorkOrGroup(TaskGroup& group, const TaskWaitRole role) noexcept
    {
        std::unique_lock lock(m_workMutex);
        m_waitingThreads.fetch_add(1, std::memory_order_relaxed);
        m_workAvailable.wait_for(lock, std::chrono::microseconds(200),
                                 [this, &group, role]
                                 {
                                     return group.IsComplete() ||
                                            m_state.load(std::memory_order_acquire) != State::Running ||
                                            HasPotentialWorkFor(role);
                                 });
        m_waitingThreads.fetch_sub(1, std::memory_order_relaxed);
    }

    void WaitForAnyProgress() noexcept
    {
        std::unique_lock lock(m_workMutex);
        m_waitingThreads.fetch_add(1, std::memory_order_relaxed);
        m_workAvailable.wait_for(lock, std::chrono::microseconds(200),
                                 [this] { return m_pending.load(std::memory_order_acquire) == 0 || !m_queue.Empty(); });
        m_waitingThreads.fetch_sub(1, std::memory_order_relaxed);
    }

    // 有等待者才通知：publish 与"任务完成"是两条会改变等待者谓词的路径。
    void WakeIfWaitersPresent() noexcept
    {
        if (m_waitingThreads.load(std::memory_order_relaxed) != 0)
            WakeAllWaiters();
    }

    // 发布/停止都要经过这里：先取一次工作锁再通知，与等待者的"检查谓词→进入 wait"窗口互斥，
    // 否则会出现任务已入队但所有 worker 都在睡的丢唤醒。
    void WakeAllWaiters() noexcept
    {
        {
            std::lock_guard lock(m_workMutex);
        }
        m_workAvailable.notify_all();
    }

    [[nodiscard]] std::uint64_t CancelQueuedTasks() noexcept
    {
        std::uint64_t cancelled = 0;
        // 取消不等于遗忘：inject queue 与所有 local deque 里的未开始任务都要 group.Done，
        // owner 才能在本组完成后回收 payload。
        const auto cancel = [this, &cancelled](Task& task)
        {
            if (task.group != nullptr)
                task.group->Done();
            m_cancelled.fetch_add(1, std::memory_order_relaxed);
            m_pending.fetch_sub(1, std::memory_order_relaxed);
            ++cancelled;
        };

        for (Task& task : m_queue.DrainAll())
            cancel(task);
        for (const std::unique_ptr<WorkDeque>& deque : m_local)
        {
            for (Task& task : deque->DrainAll())
                cancel(task);
        }
        return cancelled;
    }

    void JoinIfNeeded() noexcept
    {
        std::lock_guard lock(m_joinMutex);
        if (m_joined)
            return;
        for (std::jthread& worker : m_workers)
        {
            if (worker.joinable())
                worker.request_stop();
        }
        WakeAllWaiters();
        for (std::jthread& worker : m_workers)
        {
            if (worker.joinable())
                worker.join();
        }
        m_workers.clear();
        m_joined = true;
    }

    [[nodiscard]] static bool CallerMatchesDeclaredRole(const TaskWaitRole role) noexcept
    {
        const bool onWorker = t_workerIndex != kNonWorkerIndex;
        switch (role)
        {
        case TaskWaitRole::Worker:
            return onWorker;
        case TaskWaitRole::MainThread:
            return !onWorker;
        case TaskWaitRole::NonHelping:
        default:
            return true; // 任何线程都可以声明"不帮助"；断言只拦把 worker 当 main / 把 main 当 worker。
        }
    }

    // Debug 断言之外的安全兜底：声明与身份不符时退化成安全角色，而不是在 Release 里死锁。
    [[nodiscard]] static TaskWaitRole ResolveEffectiveRole(const TaskWaitRole role) noexcept
    {
        const bool onWorker = t_workerIndex != kNonWorkerIndex;
        if (role == TaskWaitRole::Worker && !onWorker)
            return TaskWaitRole::NonHelping; // 非 worker 声称 worker：不取任务（安全）
        if (role == TaskWaitRole::MainThread && onWorker)
            return TaskWaitRole::Worker; // worker 声称 main：允许帮助（worker 取任意任务是合法超集）
        return role;
    }

    TaskSystemConfig m_config;
    GlobalTaskQueue m_queue;
    mutable std::mutex m_workMutex;
    std::condition_variable m_workAvailable;
    std::atomic<State> m_state{State::Running};
    // publish 的"记账窗口"与 Shutdown 的清点互斥（M7-TASK-RACE-FLAKE 的修复）：
    // 没有它时，提交线程在 state 检查之后、入队之前被抢占，Shutdown 末尾的
    // `pending == 0` 断言会撞上这条未落定的提交并终止进程（FailAssertion）。
    // 锁序：m_publishGate → 队列内锁；Shutdown 侧的 StopAccepting 是完整调用（不跨锁持有），
    // 因此不存在反向持有，不会死锁。
    std::mutex m_publishGate;
    std::atomic<std::uint64_t> m_submitted{0};
    std::atomic<std::uint64_t> m_executed{0};
    std::atomic<std::uint64_t> m_cancelled{0};
    std::atomic<std::uint64_t> m_pending{0};
    std::atomic<std::uint64_t> m_sleepingWorkers{0};
    // 正在 m_workAvailable 上等待的线程数（worker 睡眠 + helper 等待）。发布/完成据此决定
    // 是否需要通知：无人等待时通知只会制造惊群与上下文切换风暴。
    std::atomic<std::uint64_t> m_waitingThreads{0};
    std::atomic<std::uint64_t> m_nextTaskId{1};
    std::unique_ptr<WorkerStatistics[]> m_statistics;
    std::vector<std::unique_ptr<WorkDeque>> m_local; // v1：per-worker deque（按 worker 索引）
    std::vector<std::uint64_t> m_prngState;          // v1：worker-local PRNG（仅该 worker 访问）
    std::vector<std::string> m_workerNames;
    std::vector<std::jthread> m_workers;
    std::mutex m_joinMutex;
    bool m_joined = false;
};

TaskSystem::TaskSystem(const TaskSystemConfig& config) : m_impl(std::make_unique<Impl>(config))
{
}

TaskSystem::~TaskSystem()
{
    if (m_impl != nullptr)
    {
        // 兜底 drain：析构不丢已接收任务。显式 Shutdown 仍是契约要求（初始化/关闭顺序），
        // 但漏调时也不能静默丢任务或悬空 payload。
        m_impl->Shutdown(true);
    }
}

SubmitResult TaskSystem::Submit(Task task) noexcept
{
    if (!task.IsValid())
        return SubmitResult::InvalidTask;
    task.debug.id = m_impl->NextTaskId();
    task.debug.enqueueTicks = NowTicks();
    if (task.debug.submitterThread == 0)
        task.debug.submitterThread = HashCurrentThread();
    return m_impl->Publish(task);
}

SubmitResult TaskSystem::Submit(Task task, TaskGroup& group) noexcept
{
    if (!task.IsValid())
        return SubmitResult::InvalidTask;

    task.debug.id = m_impl->NextTaskId();
    task.debug.enqueueTicks = NowTicks();
    if (task.debug.submitterThread == 0)
        task.debug.submitterThread = HashCurrentThread();

    group.Add(); // 预留完成位必须在 publish 之前。
    task.group = &group;
    const SubmitResult result = m_impl->Publish(task);
    if (result != SubmitResult::Accepted)
    {
        group.Done(); // 回滚恰好一次，与上面的 Add 配对。
        return result;
    }
    return SubmitResult::Accepted;
}

void TaskSystem::Wait(TaskGroup& group, const TaskWaitRole role) noexcept
{
    ME_PROFILE_ZONE_NAMED("TaskGroup::WaitHelp");
    if (m_impl != nullptr)
        m_impl->WaitImpl(group, role);
}

bool TaskSystem::TryExecuteOne(const TaskWaitRole role) noexcept
{
    return m_impl != nullptr && m_impl->TryExecuteOnePublic(role);
}

void TaskSystem::Shutdown(const bool drain) noexcept
{
    ME_PROFILE_ZONE_NAMED("TaskSystem::Shutdown");
    if (m_impl != nullptr)
        m_impl->Shutdown(drain);
}

std::uint32_t TaskSystem::WorkerCount() const noexcept
{
    return m_impl != nullptr ? m_impl->WorkerCount() : 0U;
}

bool TaskSystem::IsAccepting() const noexcept
{
    return m_impl != nullptr && m_impl->IsAccepting();
}

TaskSystemStatistics TaskSystem::Statistics() const noexcept
{
    return m_impl != nullptr ? m_impl->Statistics() : TaskSystemStatistics{};
}

WorkerCounters TaskSystem::WorkerCountersFor(const std::uint32_t workerIndex) const noexcept
{
    return m_impl != nullptr ? m_impl->WorkerCountersFor(workerIndex) : WorkerCounters{};
}

std::uint32_t TaskSystem::CurrentWorkerIndex() noexcept
{
    return t_workerIndex;
}
} // namespace MiniEngine::Tasks
