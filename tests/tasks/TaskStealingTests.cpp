// ============================================================================
// TaskStealingTests.cpp — M7-04 调度器 v1（per-worker deque + work stealing）测试
// 里程碑：M7-04（M7-A10 无 missed wakeup/spin storm/underflow；M7-A11 stealing 版本功能、
//   停止与可见性正确）
// 覆盖规格的必测故障：单 task、任务数少于/等于/远大于 worker 数、空队列 burst、递归 10 层、
//   1 worker nested wait、victim 在 steal 检查后变空、shutdown 与 notify/steal 同时发生、
//   10,000 帧后 pending/active/sleeping/handle 回到基线。
// 关联：docs/architecture/README.md
// ============================================================================

#include "TaskTestSupport.h"

#include <MiniEngine/Tasks/TaskSystem.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

using namespace MiniEngine::Tasks;
using namespace MiniEngine::Tasks::TestSupport;
using namespace std::chrono_literals;

namespace
{
[[nodiscard]] TaskSystemConfig StealingConfig(const std::uint32_t workers = 4,
                                             const std::uint32_t injectCapacity = 256,
                                             const std::uint32_t localCapacity = 64)
{
    TaskSystemConfig config;
    config.mode = SchedulerMode::PerWorkerDeque;
    config.workerCount = workers;
    config.injectQueueCapacity = injectCapacity;
    config.localQueueCapacity = localCapacity;
    config.stealAttemptsBeforeSleep = 4;
    config.randomSeed = 6657;
    return config;
}

// 递归扇出：每层任务再提交 2 个子任务，直到 depth。总任务数 = 2^(depth+1)-1。
// 子任务挂在同一个 group 上：父任务先 Add 完子任务再完成自己，因此 group 计数不会提前归零。
struct FanOut
{
    TaskSystem* tasks = nullptr;
    TaskGroup* group = nullptr;
    std::atomic<std::uint32_t>* executions = nullptr;
    std::atomic<std::uint32_t>* submitFailures = nullptr;
    std::uint32_t depth = 0;
};

void FanOutEntry(void* raw) noexcept
{
    auto* node = static_cast<FanOut*>(raw);
    node->executions->fetch_add(1, std::memory_order_relaxed);
    if (node->depth > 0)
    {
        for (std::uint32_t branch = 0; branch < 2; ++branch)
        {
            // 子节点由任务自己拥有（任务执行路径释放）："payload 允许 owned block"的形态。
            auto* child = new FanOut{node->tasks, node->group, node->executions, node->submitFailures, node->depth - 1};
            const SubmitResult result = node->tasks->Submit(
                Task{&FanOutEntry, child, nullptr, TaskFlags::None, {0, "FanOutChild", 0}}, *node->group);
            if (result != SubmitResult::Accepted)
            {
                node->submitFailures->fetch_add(1, std::memory_order_relaxed);
                delete child; // 发布失败：owner 立刻回收，不留悬空 payload。
            }
        }
    }
    delete node;
}

// 单 worker 嵌套等待：父任务提交的子任务先进该 worker 的 local deque，再由父任务自己的
// Wait(Worker) 取回；本地容量只有 8，因此另外 8 个会走 inject queue 回退路径。
struct NestedParent
{
    TaskSystem* tasks = nullptr;
    std::atomic<std::uint32_t>* children = nullptr;
    std::atomic<std::uint32_t>* submitFailures = nullptr;
};

void NestedParentEntry(void* raw) noexcept
{
    auto* parent = static_cast<NestedParent*>(raw);
    TaskGroup childrenGroup;
    for (std::uint32_t index = 0; index < 16; ++index)
    {
        const SubmitResult result =
            parent->tasks->Submit(Task{&Increment, parent->children, nullptr, TaskFlags::None, {0, "Child", 0}},
                                  childrenGroup);
        if (result != SubmitResult::Accepted)
            parent->submitFailures->fetch_add(1, std::memory_order_relaxed);
    }
    parent->tasks->Wait(childrenGroup, TaskWaitRole::Worker);
    delete parent;
}
} // namespace

TEST(TaskStealingTests, ExecutesEveryTaskExactlyOnceWithLocalDequesAndStealing)
{
    for (const std::uint32_t workers : {1U, 2U, 8U})
    {
        TaskSystem tasks(StealingConfig(workers));
        TaskGroup group;
        std::atomic<std::uint32_t> executions{0};

        for (std::uint32_t index = 0; index < 10000; ++index)
        {
            // inject 容量有限：提交快于消费是正常背压，必须重试而不是断言 Accepted。
            ASSERT_TRUE(SubmitWithBackpressure(
                tasks, Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Steal", 0}}, group));
        }

        tasks.Wait(group, TaskWaitRole::MainThread);
        EXPECT_EQ(executions.load(), 10000U) << "workers=" << workers;
        EXPECT_TRUE(group.IsComplete()) << "workers=" << workers;

        const TaskSystemStatistics statistics = tasks.Statistics();
        EXPECT_EQ(statistics.executed, 10000U) << "workers=" << workers;
        EXPECT_EQ(statistics.pending, 0U) << "workers=" << workers;
        tasks.Shutdown(true);
    }
}

TEST(TaskStealingTests, HandlesSingleAndFewerTasksThanWorkers)
{
    TaskSystem tasks(StealingConfig(/*workers=*/8));
    std::atomic<std::uint32_t> executions{0};

    {
        TaskGroup single;
        ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::None, {0, "Single", 0}}, single),
                  SubmitResult::Accepted);
        tasks.Wait(single, TaskWaitRole::MainThread);
        EXPECT_EQ(executions.load(), 1U);
    }

    {
        TaskGroup few;
        for (std::uint32_t index = 0; index < 3; ++index)
        {
            ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::None, {0, "Few", 0}}, few),
                      SubmitResult::Accepted);
        }
        tasks.Wait(few, TaskWaitRole::MainThread);
        EXPECT_EQ(executions.load(), 4U);
    }

    tasks.Shutdown(true);
    EXPECT_EQ(tasks.Statistics().pending, 0U);
}

TEST(TaskStealingTests, RecursiveTenLevelFanOutCompletesWithoutUnderflow)
{
    TaskSystem tasks(StealingConfig(/*workers=*/4));
    std::atomic<std::uint32_t> executions{0};
    std::atomic<std::uint32_t> submitFailures{0};
    TaskGroup group;
    auto* root = new FanOut{&tasks, &group, &executions, &submitFailures, /*depth=*/10};

    ASSERT_EQ(tasks.Submit(Task{&FanOutEntry, root, nullptr, TaskFlags::MainHelpAllowed, {0, "FanOutRoot", 0}}, group),
              SubmitResult::Accepted);
    tasks.Wait(group, TaskWaitRole::MainThread);

    // 2^0 + ... + 2^10 = 2047 个任务，每个恰好一次。
    EXPECT_EQ(submitFailures.load(), 0U);
    EXPECT_EQ(executions.load(), 2047U);
    EXPECT_TRUE(group.IsComplete());
    EXPECT_EQ(tasks.Statistics().pending, 0U);
    tasks.Shutdown(true);
}

TEST(TaskStealingTests, OneWorkerNestedWaitUsesLocalDeque)
{
    TaskSystem tasks(StealingConfig(/*workers=*/1, /*injectCapacity=*/16, /*localCapacity=*/8));
    std::atomic<std::uint32_t> children{0};
    std::atomic<std::uint32_t> submitFailures{0};
    TaskGroup outer;

    auto* parent = new NestedParent{&tasks, &children, &submitFailures};
    ASSERT_EQ(tasks.Submit(Task{&NestedParentEntry, parent, nullptr, TaskFlags::None, {0, "Parent", 0}}, outer),
              SubmitResult::Accepted);
    tasks.Wait(outer, TaskWaitRole::MainThread);

    EXPECT_EQ(submitFailures.load(), 0U);
    EXPECT_EQ(children.load(), 16U);
    EXPECT_TRUE(outer.IsComplete());
    tasks.Shutdown(true);
}

TEST(TaskStealingTests, BurstyBurstOnEmptyQueuesDoesNotLoseWakeups)
{
    // 空队列频繁 burst：每轮只提交 1 个任务，等它完成再提交下一个（最坏的空/满转换）。
    StallDiag diag;
    StallWatchdog watchdog(diag);

    for (std::uint32_t round = 0; round < 500; ++round)
    {
        PublishStallPhase(diag, /*phase=*/5, round); // 5 = 构造 TaskSystem 之前的窗口
        TaskSystem tasks(StealingConfig(/*workers=*/4));
        std::atomic<std::uint32_t> executions{0};
        PublishStallDiag(diag, tasks, /*phase=*/0, round, 0);

        for (std::uint32_t index = 0; index < 4; ++index)
        {
            TaskGroup group;
            ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Burst", 0}},
                                   group),
                      SubmitResult::Accepted);
            PublishStallDiag(diag, tasks, /*phase=*/1, round, group.Remaining());
            tasks.Wait(group, TaskWaitRole::MainThread);
            PublishStallDiag(diag, tasks, /*phase=*/2, round, group.Remaining());
        }

        EXPECT_EQ(executions.load(), 4U) << "round=" << round;
        PublishStallDiag(diag, tasks, /*phase=*/3, round, 0);
        tasks.Shutdown(true);
        PublishStallDiag(diag, tasks, /*phase=*/4, round, 0);
        EXPECT_EQ(tasks.Statistics().pending, 0U) << "round=" << round;
    }
}

TEST(TaskStealingTests, StealRaceWithEmptyVictimsStaysAccounted)
{
    // victim 在 steal 检查后变空：只有 worker 自己提交的任务才进 local deque，因此这里用
    // 递归扇出制造真实可偷的工作（外部提交全部走 inject queue，永远不会产生窃取）。
    // local 容量只有 4、depth=6（127 个任务），因此"本地满回退 inject"与窃取都会触发。
    std::uint64_t totalSteals = 0;
    for (std::uint32_t round = 0; round < 50; ++round)
    {
        TaskSystem tasks(StealingConfig(/*workers=*/8, /*injectCapacity=*/64, /*localCapacity=*/4));
        std::atomic<std::uint32_t> executions{0};
        std::atomic<std::uint32_t> submitFailures{0};
        TaskGroup group;
        auto* root = new FanOut{&tasks, &group, &executions, &submitFailures, /*depth=*/6};

        ASSERT_EQ(tasks.Submit(Task{&FanOutEntry, root, nullptr, TaskFlags::MainHelpAllowed, {0, "FanOutRoot", 0}}, group),
                  SubmitResult::Accepted);
        tasks.Wait(group, TaskWaitRole::MainThread);

        EXPECT_EQ(submitFailures.load(), 0U) << "round=" << round;
        EXPECT_EQ(executions.load(), 127U) << "round=" << round; // 2^0+…+2^6
        EXPECT_EQ(tasks.Statistics().pending, 0U) << "round=" << round;

        for (std::uint32_t worker = 0; worker < tasks.WorkerCount(); ++worker)
        {
            const WorkerCounters counters = tasks.WorkerCountersFor(worker);
            EXPECT_LE(counters.stealSuccesses, counters.stealAttempts) << "round=" << round;
            totalSteals += counters.stealSuccesses;
        }
        tasks.Shutdown(true);
    }
    // 为 0 说明窃取路径根本没被执行（测试失效）；8 worker + 递归扇出必然产生窃取。
    EXPECT_GT(totalSteals, 0U);
}

TEST(TaskStealingTests, IdleWorkersDoNotSpinStorm)
{
    TaskSystem tasks(StealingConfig(/*workers=*/4));
    TaskGroup group;
    std::atomic<std::uint32_t> executions{0};
    for (std::uint32_t index = 0; index < 256; ++index)
    {
        ASSERT_EQ(tasks.Submit(Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Quiet", 0}}, group),
                  SubmitResult::Accepted);
    }
    tasks.Wait(group, TaskWaitRole::MainThread);
    std::this_thread::sleep_for(20ms); // 让最后一次唤醒结算完成，避免快照落在竞态里

    std::uint64_t wakeupsBefore = 0;
    for (std::uint32_t worker = 0; worker < tasks.WorkerCount(); ++worker)
        wakeupsBefore += tasks.WorkerCountersFor(worker).wakeups;

    std::this_thread::sleep_for(50ms);

    std::uint64_t wakeupsAfter = 0;
    for (std::uint32_t worker = 0; worker < tasks.WorkerCount(); ++worker)
        wakeupsAfter += tasks.WorkerCountersFor(worker).wakeups;

    // 无工作时的等待必须是条件变量阻塞：50ms 空闲不允许产生任何新增唤醒。
    EXPECT_EQ(wakeupsAfter, wakeupsBefore);
    EXPECT_LE(tasks.Statistics().sleepingWorkers, tasks.WorkerCount());

    tasks.Shutdown(true);
}

TEST(TaskStealingTests, ShutdownRacingStealAndNotifyHasAccountedOutcome)
{
    for (std::uint32_t round = 0; round < 200; ++round)
    {
        TaskSystem tasks(StealingConfig(/*workers=*/4, /*injectCapacity=*/32, /*localCapacity=*/8));
        TaskGroup group;
        PayloadPool pool;
        std::atomic<bool> stop{false};

        std::thread submitter(
            [&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    PayloadPool::Job* payload = pool.Create();
                    const SubmitResult result =
                        tasks.Submit(Task{&RunPayloadTask, payload, nullptr, TaskFlags::MainHelpAllowed, {0, "Stop", 0}},
                                     group);
                    if (result != SubmitResult::Accepted)
                    {
                        pool.Retire(payload); // 发布失败：owner 立刻回收
                        if (result == SubmitResult::Stopping)
                            stop.store(true, std::memory_order_relaxed);
                    }
                }
            });

        std::this_thread::sleep_for(1ms);
        tasks.Shutdown(false); // 取消 + join 与 steal/notify 并发
        stop.store(true, std::memory_order_relaxed);
        submitter.join();

        const TaskSystemStatistics statistics = tasks.Statistics();
        EXPECT_EQ(statistics.executed + statistics.cancelled, statistics.submitted) << "round=" << round;
        EXPECT_EQ(statistics.pending, 0U) << "round=" << round;
        EXPECT_TRUE(group.IsComplete()) << "round=" << round;
        pool.ReclaimOutstanding();
        EXPECT_EQ(pool.Created(), pool.Destroyed()) << "round=" << round;
    }
}

// 生命周期回归（M7-03 遗留缺陷）：Wait 返回后 group 必须可以被立刻析构，
// 因为"计数归零"只有在最后一个 Done 离开 TaskGroup 之后才对等待者可见。
// 修复前：Debug CRT 报 "unlock of unowned mutex"（Release 是踩到下一轮复用的同地址对象）。
TEST(TaskStealingTests, WaitReturnMeansGroupMayBeDestroyedImmediately)
{
    TaskSystem tasks(StealingConfig(/*workers=*/4));
    std::atomic<std::uint32_t> executions{0};

    for (std::uint32_t round = 0; round < 5000; ++round)
    {
        // 堆上分配：析构时机完全由等待者控制，最容易暴露"完成线程还在组内"的竞态。
        auto* group = new TaskGroup();
        if (!SubmitWithBackpressure(tasks, Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Life", 0}},
                                    *group, std::chrono::seconds(5)))
        {
            delete group;
            FAIL() << "round=" << round;
        }
        tasks.Wait(*group, TaskWaitRole::MainThread);
        delete group; // 必须安全：Wait 返回即意味着没有人在组内了
    }

    EXPECT_EQ(executions.load(), 5000U);
    tasks.Shutdown(true);
}

TEST(TaskStealingTests, TenThousandFramesReturnToBaseline)
{
#if defined(_WIN32)
    DWORD handlesBefore = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore));
#endif

    TaskSystem tasks(StealingConfig(/*workers=*/4));
    std::atomic<std::uint32_t> executions{0};

    // 10,000 "帧"：每帧提交 1 个任务并等待，模拟帧循环的空/满转换与休眠/唤醒。
    for (std::uint32_t frame = 0; frame < 10000; ++frame)
    {
        TaskGroup group;
        ASSERT_TRUE(SubmitWithBackpressure(
            tasks, Task{&Increment, &executions, nullptr, TaskFlags::MainHelpAllowed, {0, "Frame", 0}}, group));
        tasks.Wait(group, TaskWaitRole::MainThread);
    }

    const TaskSystemStatistics statistics = tasks.Statistics();
    EXPECT_EQ(executions.load(), 10000U);
    EXPECT_EQ(statistics.pending, 0U);
    EXPECT_EQ(statistics.queueDepth, 0U);
    EXPECT_EQ(statistics.localQueueDepth, 0U);
    EXPECT_LE(statistics.sleepingWorkers, statistics.workerCount);

#if defined(_WIN32)
    DWORD handlesAfter = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter));
    // 句柄阈值 64 与"每轮泄漏一个句柄"相差两个数量级；Tracy-on 构建实测 +56 来自 Tracy 客户端
    // 自身的线程登记记账（见 M7-03 记录的 D8）。
    EXPECT_LE(handlesAfter, handlesBefore + 64U) << "handles before=" << handlesBefore << " after=" << handlesAfter;
#endif

    tasks.Shutdown(true);
    EXPECT_EQ(tasks.Statistics().pending, 0U);
}
