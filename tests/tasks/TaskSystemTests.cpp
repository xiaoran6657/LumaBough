// ============================================================================
// TaskSystemTests.cpp — M7-03 任务契约/所有权/生命周期测试（功能与契约）
// 里程碑：M7-03（M7-A07 全局有锁队列、M7-A08 线程与关闭、M7-A09 nested wait-help）
// 覆盖规格的必测场景：0/1/大量 task、1/N worker、main thread wait-help、task 恰在
//   wait 前/谓词检查与睡眠之间完成、submit 与 shutdown 竞争（见 TaskStressTests）、
//   payload 析构计数、取消与 publish 失败、task 内等待子 group、10,000 次 init/shutdown。
// 关联：docs/architecture/README.md
// ============================================================================

#include "TaskTestSupport.h"

#include <MiniEngine/Tasks/TaskSystem.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <tlhelp32.h>
#endif

using namespace MiniEngine::Tasks;
using namespace MiniEngine::Tasks::TestSupport;
using namespace std::chrono_literals;

namespace
{
#if defined(_WIN32)
// 进程内活动线程数：这是"worker 线程是否被 join"的直接不变量（句柄计数会被三方库
// 自身的记账污染，例如 Tracy 客户端在线程登记时会额外持有句柄）。
[[nodiscard]] DWORD CountProcessThreads()
{
    const DWORD processId = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    DWORD count = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == processId)
                ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}
#endif
// ---------------------------------------------------------------------------
// 单 worker 内嵌套 Wait：父任务在 worker 上执行，子 group 必须靠等待者自己 help 才能推进。
// ---------------------------------------------------------------------------
class NestedWaitFixture final
{
  public:
    static constexpr std::uint32_t kChildren = 8;

    explicit NestedWaitFixture(TaskSystem& tasks) : m_tasks(tasks)
    {
    }

    [[nodiscard]] Task MakeParentTask() noexcept
    {
        // 父任务做嵌套等待，不是"纯 CPU 叶子"，因此不带 MainHelpAllowed：
        // 否则 main thread 可能执行它，而它内部的 Wait(Worker) 会因身份不符被断言拦下。
        return Task{&NestedWaitFixture::ParentEntry, this, nullptr, TaskFlags::None, {0, "NestedParent", 0}};
    }

    [[nodiscard]] std::uint32_t ChildExecutions() const noexcept
    {
        return m_childExecutions.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t SubmitFailures() const noexcept
    {
        return m_submitFailures.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool NestedWaitCompleted() const noexcept
    {
        return m_nestedWaitCompleted.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::uint32_t ExpectedChildren() noexcept
    {
        return kChildren;
    }

  private:
    static void ChildEntry(void* raw) noexcept
    {
        static_cast<NestedWaitFixture*>(raw)->m_childExecutions.fetch_add(1, std::memory_order_relaxed);
    }

    static void ParentEntry(void* raw) noexcept
    {
        auto* self = static_cast<NestedWaitFixture*>(raw);
        TaskGroup children;
        for (std::uint32_t index = 0; index < kChildren; ++index)
        {
            const SubmitResult result =
                self->m_tasks.Submit(Task{&ChildEntry, self, nullptr, TaskFlags::MainHelpAllowed, {0, "NestedChild", 0}},
                                     children);
            if (result != SubmitResult::Accepted)
            {
                self->m_submitFailures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        // 单 worker：等待者不 help 就是死锁。
        self->m_tasks.Wait(children, TaskWaitRole::Worker);
        self->m_nestedWaitCompleted.store(true, std::memory_order_release);
    }

    TaskSystem& m_tasks;
    std::atomic<std::uint32_t> m_childExecutions{0};
    std::atomic<std::uint32_t> m_submitFailures{0};
    std::atomic<bool> m_nestedWaitCompleted{false};
};

// ---------------------------------------------------------------------------
// 取消路径夹具：先占住 worker，再把有界队列填满，使取消时一定有未开始任务。
// ---------------------------------------------------------------------------
class CancelFixture final
{
  public:
    void FillQueue(TaskSystem& tasks, TaskGroup& group, const std::uint32_t capacity)
    {
        m_blocker.hold = 500ms;
        EXPECT_EQ(tasks.Submit(Task{&HoldWorker, &m_blocker, nullptr, TaskFlags::None, {0, "Blocker", 0}}, group),
                  SubmitResult::Accepted);
        WaitUntilEntered(m_blocker.entered);
        ASSERT_TRUE(m_blocker.entered.load(std::memory_order_acquire));

        for (std::uint32_t index = 0; index < capacity + 4; ++index)
        {
            PayloadPool::Job* payload = m_pool.Create();
            const SubmitResult result =
                tasks.Submit(Task{&RunPayloadTask, payload, nullptr, TaskFlags::MainHelpAllowed, {0, "Payload", 0}}, group);
            if (result == SubmitResult::QueueFull || result == SubmitResult::Stopping)
            {
                m_pool.Retire(payload); // 发布失败：owner 立刻回收（回滚路径）。
                break;
            }
            EXPECT_EQ(result, SubmitResult::Accepted);
            ++m_queued;
        }
    }

    [[nodiscard]] PayloadPool& Pool() noexcept
    {
        return m_pool;
    }

    [[nodiscard]] std::uint32_t Queued() const noexcept
    {
        return m_queued;
    }

  private:
    BoundedBlocker m_blocker;
    PayloadPool m_pool;
    std::uint32_t m_queued = 0;
};

// ---------------------------------------------------------------------------
// NonHelping 夹具：worker 被阻塞时，render 线程的等待不得执行队列里的任务。
// ---------------------------------------------------------------------------
class NonHelpingWaitFixture final
{
  public:
    explicit NonHelpingWaitFixture(TaskSystem& tasks) : m_tasks(tasks)
    {
    }

    void BlockWorkerAndQueueMainHelpTask()
    {
        m_blocker.hold = 50ms;
        EXPECT_EQ(m_tasks.Submit(Task{&HoldWorker, &m_blocker, nullptr, TaskFlags::None, {0, "Blocker", 0}}, m_group),
                  SubmitResult::Accepted);
        WaitUntilEntered(m_blocker.entered);
        ASSERT_TRUE(m_blocker.entered.load(std::memory_order_acquire));

        EXPECT_EQ(m_tasks.Submit(Task{&NonHelpingWaitFixture::MainHelpEntry, this, nullptr, TaskFlags::MainHelpAllowed,
                                      {0, "MainHelp", 0}},
                                 m_group),
                  SubmitResult::Accepted);
    }

    [[nodiscard]] std::uint32_t Executions() const noexcept
    {
        return m_executions.load(std::memory_order_relaxed);
    }

    void JoinWaiter()
    {
        if (m_waiter.joinable())
            m_waiter.join();
    }

    ~NonHelpingWaitFixture()
    {
        JoinWaiter();
    }

    // 在独立线程上以 NonHelping 角色等待（模拟 render thread）。
    void WaitFromRenderThread(const TaskWaitRole role)
    {
        m_waiter = std::thread(
            [this, role]
            {
                m_renderThreadId.store(std::this_thread::get_id(), std::memory_order_release);
                m_tasks.Wait(m_group, role);
                m_waiterReturned.store(true, std::memory_order_release);
            });
    }

    [[nodiscard]] bool RenderThreadExecutedTask() const noexcept
    {
        const std::thread::id executing = m_executingThread.load(std::memory_order_acquire);
        const std::thread::id render = m_renderThreadId.load(std::memory_order_acquire);
        return executing != std::thread::id{} && executing == render;
    }

    [[nodiscard]] bool WaiterReturned() const noexcept
    {
        return m_waiterReturned.load(std::memory_order_acquire);
    }

  private:
    static void MainHelpEntry(void* raw) noexcept
    {
        auto* self = static_cast<NonHelpingWaitFixture*>(raw);
        self->m_executions.fetch_add(1, std::memory_order_relaxed);
        self->m_executingThread.store(std::this_thread::get_id(), std::memory_order_release);
    }

    TaskSystem& m_tasks;
    TaskGroup m_group;
    BoundedBlocker m_blocker;
    std::atomic<std::uint32_t> m_executions{0};
    std::atomic<std::thread::id> m_executingThread{};
    std::atomic<std::thread::id> m_renderThreadId{};
    std::atomic<bool> m_waiterReturned{false};
    std::thread m_waiter;
};
} // namespace

TEST(TaskSystemTests, ExecutesEveryTaskExactlyOnce)
{
    TaskSystem tasks({.workerCount = 4});
    TaskGroup group;
    std::atomic<std::uint32_t> executions{0};

    for (std::uint32_t index = 0; index < 10000; ++index)
    {
        // inject 容量（4096）小于提交量：提交快于消费是正常背压（QueueFull），
        // 必须重试而不是断言 Accepted（见 TaskTestSupport.h 的说明）。
        ASSERT_TRUE(SubmitWithBackpressure(
            tasks, Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {index, "Test", 0}}, group));
    }

    tasks.Wait(group, TaskWaitRole::MainThread);
    EXPECT_EQ(executions.load(), 10000U);
    EXPECT_TRUE(group.IsComplete());

    const TaskSystemStatistics statistics = tasks.Statistics();
    EXPECT_EQ(statistics.submitted, 10000U);
    EXPECT_EQ(statistics.executed, 10000U);
    EXPECT_EQ(statistics.cancelled, 0U);
    EXPECT_EQ(statistics.pending, 0U);
}

TEST(TaskSystemTests, ZeroTasksCompletesImmediately)
{
    TaskSystem tasks({.workerCount = 1});
    TaskGroup group;
    tasks.Wait(group, TaskWaitRole::MainThread);
    EXPECT_TRUE(group.IsComplete());

    tasks.Shutdown(true);
    EXPECT_FALSE(tasks.IsAccepting());
    EXPECT_EQ(tasks.Statistics().pending, 0U);
}

TEST(TaskSystemTests, InvalidTaskIsRejected)
{
    TaskSystem tasks({.workerCount = 1});
    TaskGroup group;
    EXPECT_EQ(tasks.Submit(Task{}), SubmitResult::InvalidTask);
    EXPECT_EQ(tasks.Submit(Task{}, group), SubmitResult::InvalidTask);
    EXPECT_EQ(group.Remaining(), 0U); // 无效提交不得留下未配对的完成位。
}

TEST(TaskSystemTests, WorkerSeesWorkerLocalContextAndMainDoesNot)
{
    TaskSystem tasks({.workerCount = 2});
    TaskGroup group;
    std::atomic<std::uint32_t> observed{kNonWorkerIndex};

    EXPECT_EQ(TaskSystem::CurrentWorkerIndex(), kNonWorkerIndex);
    // 不带 MainHelpAllowed：只有 worker 能取，从而确定性地观察到 worker-local 索引
    // （带帮助标志时 main 可能先执行它，观测到的就是 kNonWorkerIndex）。
    ASSERT_EQ(tasks.Submit(Task{&RecordWorkerIndex, &observed, nullptr, TaskFlags::None, {0, "WorkerIndex", 0}}, group),
              SubmitResult::Accepted);
    tasks.Wait(group, TaskWaitRole::MainThread);

    EXPECT_LT(observed.load(), tasks.WorkerCount());
    EXPECT_EQ(TaskSystem::CurrentWorkerIndex(), kNonWorkerIndex);
}

TEST(TaskSystemTests, OneWorkerNestedWaitMakesProgress)
{
    for (const std::uint32_t workers : {1U, 4U})
    {
        TaskSystem tasks({.workerCount = workers});
        NestedWaitFixture fixture{tasks};

        TaskGroup outer;
        ASSERT_EQ(tasks.Submit(fixture.MakeParentTask(), outer), SubmitResult::Accepted);
        tasks.Wait(outer, TaskWaitRole::MainThread);

        EXPECT_EQ(fixture.SubmitFailures(), 0U) << "workers=" << workers;
        EXPECT_TRUE(fixture.NestedWaitCompleted()) << "workers=" << workers;
        EXPECT_EQ(fixture.ChildExecutions(), NestedWaitFixture::ExpectedChildren()) << "workers=" << workers;
        EXPECT_TRUE(outer.IsComplete()) << "workers=" << workers;
    }
}

TEST(TaskSystemTests, MainThreadHelpExecutesMainHelpAllowedTasksWhileWorkerBusy)
{
    TaskSystem tasks({.workerCount = 1});
    TaskGroup group;
    BoundedBlocker blocker{500ms};
    std::atomic<std::uint32_t> executions{0};

    ASSERT_EQ(tasks.Submit(Task{&HoldWorker, &blocker, nullptr, TaskFlags::None, {0, "Blocker", 0}}, group),
              SubmitResult::Accepted);
    WaitUntilEntered(blocker.entered);
    ASSERT_TRUE(blocker.entered.load(std::memory_order_acquire));

    ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Helpable", 0}}, group),
              SubmitResult::Accepted);

    // worker 被占住，只有 main 能推进；MainThread 角色取 MainHelpAllowed 任务。
    EXPECT_TRUE(tasks.TryExecuteOne(TaskWaitRole::MainThread));
    EXPECT_EQ(executions.load(), 1U);

    tasks.Wait(group, TaskWaitRole::MainThread);
    EXPECT_TRUE(group.IsComplete());
}

TEST(TaskSystemTests, MainThreadNeverTakesNonHelpableTasks)
{
    TaskSystem tasks({.workerCount = 1, .injectQueueCapacity = 8});
    TaskGroup group;
    BoundedBlocker blocker{300ms};
    std::atomic<std::uint32_t> executions{0};

    ASSERT_EQ(tasks.Submit(Task{&HoldWorker, &blocker, nullptr, TaskFlags::None, {0, "Blocker", 0}}, group),
              SubmitResult::Accepted);
    WaitUntilEntered(blocker.entered);
    ASSERT_TRUE(blocker.entered.load(std::memory_order_acquire));

    ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::None, {0, "NonHelpable", 0}}, group),
              SubmitResult::Accepted);

    // 队首是 non-helpable：main 必须空手返回（弹出再放回会破坏 FIFO 并可能活锁）。
    EXPECT_FALSE(tasks.TryExecuteOne(TaskWaitRole::MainThread));
    EXPECT_EQ(executions.load(), 0U);

    // NonHelping 永不取任务。
    EXPECT_FALSE(tasks.TryExecuteOne(TaskWaitRole::NonHelping));

    tasks.Wait(group, TaskWaitRole::MainThread); // worker 结束后由 worker 执行该任务。
    EXPECT_EQ(executions.load(), 1U);
    EXPECT_TRUE(group.IsComplete());
}

TEST(TaskSystemTests, QueueFullRollsBackGroupCounterAndPayload)
{
    TaskSystem tasks({.workerCount = 1, .injectQueueCapacity = 2});
    TaskGroup group;
    CancelFixture fixture;

    fixture.FillQueue(tasks, group, /*capacity=*/2);

    // 每一次成功 publish 都对应一个完成位；失败提交不得留下完成位。
    const std::uint64_t accepted = 1U + fixture.Queued(); // blocker + 队列里的 payload 任务
    EXPECT_EQ(group.Remaining(), accepted);
    // FillQueue 里有一次 QueueFull：它的 payload 已被 owner 立刻回收（回滚），且不入队。
    EXPECT_EQ(fixture.Pool().Created(), fixture.Queued() + 1U);
    EXPECT_EQ(fixture.Pool().Destroyed(), 1U);
    EXPECT_EQ(fixture.Pool().Outstanding(), fixture.Queued());

    tasks.Shutdown(false);
    EXPECT_TRUE(group.IsComplete());
    fixture.Pool().ReclaimOutstanding();
    EXPECT_EQ(fixture.Pool().Created(), fixture.Pool().Destroyed());

    const TaskSystemStatistics statistics = tasks.Statistics();
    EXPECT_EQ(statistics.submitted, accepted); // blocker + 队列里的 payload 任务
    EXPECT_EQ(statistics.executed + statistics.cancelled, statistics.submitted);
    EXPECT_EQ(statistics.pending, 0U);
}

TEST(TaskSystemTests, CancelShutdownCompletesGroupsAndDestroysPayloads)
{
    TaskSystem tasks({.workerCount = 1, .injectQueueCapacity = 4});
    CancelFixture fixture;
    TaskGroup group;

    fixture.FillQueue(tasks, group, /*capacity=*/4);
    ASSERT_GT(fixture.Queued(), 1U);

    const TaskSystemStatistics before = tasks.Statistics();
    tasks.Shutdown(false);

    const TaskSystemStatistics after = tasks.Statistics();
    EXPECT_TRUE(group.IsComplete());
    EXPECT_GE(after.cancelled, 1U);
    EXPECT_EQ(after.executed + after.cancelled, after.submitted);
    EXPECT_EQ(after.pending, 0U);
    EXPECT_EQ(after.submitted, before.submitted); // shutdown 不再接收新任务

    fixture.Pool().ReclaimOutstanding(); // group 完成后 owner 才回收 payload
    EXPECT_EQ(fixture.Pool().Created(), fixture.Pool().Destroyed());
    EXPECT_EQ(fixture.Pool().Outstanding(), 0U);
}

TEST(TaskSystemTests, RejectsSubmissionAfterShutdown)
{
    TaskSystem tasks({.workerCount = 2});
    tasks.Shutdown(true);
    EXPECT_EQ(tasks.Submit(Task{&Increment, nullptr}), SubmitResult::Stopping);

    TaskGroup group;
    EXPECT_EQ(tasks.Submit(Task{&Increment, nullptr}, group), SubmitResult::Stopping);
    EXPECT_EQ(group.Remaining(), 0U); // 失败提交必须回滚完成位。
    EXPECT_TRUE(group.IsComplete());
}

TEST(TaskSystemTests, ShutdownIsIdempotentAndDrainsPendingTasks)
{
    TaskSystem tasks({.workerCount = 2});
    std::atomic<std::uint32_t> executions{0};
    TaskGroup group;
    for (std::uint32_t index = 0; index < 64; ++index)
    {
        ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Drain", 0}}, group),
                  SubmitResult::Accepted);
    }

    tasks.Shutdown(true); // drain：全部执行完再停
    EXPECT_EQ(executions.load(), 64U);
    EXPECT_TRUE(group.IsComplete());

    tasks.Shutdown(true); // 幂等
    tasks.Shutdown(false);
    EXPECT_EQ(tasks.Statistics().pending, 0U);
}

TEST(TaskSystemTests, NonHelpingThreadNeverExecutesQueuedTask)
{
    TaskSystem tasks({.workerCount = 1});
    NonHelpingWaitFixture fixture{tasks};
    fixture.BlockWorkerAndQueueMainHelpTask();
    fixture.WaitFromRenderThread(TaskWaitRole::NonHelping);

    // worker 仍被占住（blocker 有界阻塞）：等待者不能替它执行队列里的任务。
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(fixture.RenderThreadExecutedTask());
    EXPECT_EQ(fixture.Executions(), 0U);

    fixture.JoinWaiter(); // blocker 到时后 worker 取走任务 → 组完成 → 等待者返回。
    EXPECT_TRUE(fixture.WaiterReturned());
    EXPECT_EQ(fixture.Executions(), 1U);
    EXPECT_FALSE(fixture.RenderThreadExecutedTask()); // 任务由 worker 执行，不是 render 线程。
    EXPECT_EQ(tasks.Statistics().pending, 0U);
}

TEST(TaskSystemTests, TaskCompletingDuringWaitIsAlwaysObserved)
{
    // 200 轮：等待者已经在等待时发布/完成；丢唤醒会表现为挂起（由 CTest 超时兜底）。
    for (std::uint32_t round = 0; round < 200; ++round)
    {
        TaskSystem tasks({.workerCount = 1});
        TaskGroup group;
        BoundedBlocker blocker{20ms};
        std::atomic<std::uint32_t> executions{0};

        ASSERT_EQ(tasks.Submit(Task{&HoldWorker, &blocker, nullptr, TaskFlags::None, {0, "Blocker", 0}}, group),
                  SubmitResult::Accepted);
        WaitUntilEntered(blocker.entered);
        ASSERT_TRUE(group.Remaining() >= 1U);

        std::thread submitter(
            [&tasks, &group, &executions]
            {
                std::this_thread::yield();
                (void)tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Race", 0}}, group);
            });

        tasks.Wait(group, TaskWaitRole::MainThread);
        submitter.join();

        EXPECT_TRUE(group.IsComplete()) << "round=" << round;
        EXPECT_EQ(executions.load(), 1U) << "round=" << round;
        tasks.Shutdown(true);
    }
}

TEST(TaskSystemTests, RepeatedInitShutdownDoesNotGrowHandles)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "handle/thread counting is Windows-only";
#else
    const DWORD threadsBefore = CountProcessThreads();
    DWORD before = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));

    for (std::uint32_t round = 0; round < 10000; ++round)
    {
        TaskSystem tasks({.workerCount = 1});
        tasks.Shutdown(true);
    }

    DWORD after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    const DWORD threadsAfter = CountProcessThreads();

    // 主要不变量：worker 线程必须被 join。线程泄漏会是 10,000 量级。
    EXPECT_LE(threadsAfter, threadsBefore + 4U) << "threads before=" << threadsBefore << " after=" << threadsAfter;
    // 句柄阈值 64 与"每轮泄漏一个句柄"（10,000）相差两个数量级；Tracy-on 构建实测 +56，
    // 来自 Tracy 客户端自身的线程登记记账，不是 TaskSystem 的泄漏。
    EXPECT_LE(after, before + 64U) << "handles before=" << before << " after=" << after;
#endif
}

// 契约违反必须在所有构建配置 fail-fast（不是 Debug-only 断言）。
TEST(TaskContractDeathTests, DuplicateDoneTerminates)
{
    EXPECT_DEATH(
        {
            TaskGroup group;
            group.Add();
            group.Done();
            group.Done();
        },
        "contract violation");
}

TEST(TaskContractDeathTests, OverflowingGroupCounterTerminates)
{
    EXPECT_DEATH(
        {
            TaskGroup group;
            group.Add(std::numeric_limits<std::uint32_t>::max());
            group.Add(2);
        },
        "contract violation");
}
