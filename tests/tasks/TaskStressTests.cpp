// ============================================================================
// TaskStressTests.cpp — M7-03 任务系统压力与竞争测试
// 里程碑：M7-03（M7-A07 全局有锁队列的功能与压力、M7-A09 不死锁）
// 覆盖规格的必测场景：突发发布不丢唤醒、submit 与 shutdown 竞争、取消后的 payload 计数。
// 说明：这些用例轮数较多（1000/500），Release 与 Tracy-on 也要在超时内完成；挂起视为
//   失败（死锁回归不允许人工重试）。
// 关联：docs/architecture/README.md、TaskSystemTests.cpp
// ============================================================================

#include "TaskTestSupport.h"

#include <MiniEngine/Tasks/TaskSystem.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace MiniEngine::Tasks;
using namespace MiniEngine::Tasks::TestSupport;
using namespace std::chrono_literals;

namespace
{
// 突发发布：随机让出 CPU 制造"提交与消费交错"的窗口；丢唤醒会让 Wait 挂到超时。
class BurstFixture final
{
  public:
    BurstFixture(TaskSystem& tasks, const std::uint64_t seed) : m_tasks(tasks), m_random(seed)
    {
    }

    void PublishWithRandomYields(const std::uint32_t count)
    {
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if ((m_random() & 3U) == 0U)
                std::this_thread::yield();

            const SubmitResult result = m_tasks.Submit(
                Task{&BurstFixture::Entry, this, nullptr, TaskFlags::MainHelpAllowed, {0, "Burst", 0}}, m_group);
            if (result == SubmitResult::Accepted)
                ++m_accepted;
            else if (result != SubmitResult::QueueFull)
                ++m_unexpected; // Stopping/InvalidTask 在正常发布阶段都不允许出现。
        }
    }

    void Wait()
    {
        m_tasks.Wait(m_group, TaskWaitRole::MainThread);
    }

    [[nodiscard]] std::uint64_t Accepted() const noexcept
    {
        return m_accepted;
    }

    [[nodiscard]] std::uint64_t Executed() const noexcept
    {
        return m_executed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t Unexpected() const noexcept
    {
        return m_unexpected;
    }

    [[nodiscard]] bool GroupComplete() const noexcept
    {
        return m_group.IsComplete();
    }

  private:
    static void Entry(void* raw) noexcept
    {
        static_cast<BurstFixture*>(raw)->m_executed.fetch_add(1, std::memory_order_relaxed);
    }

    TaskSystem& m_tasks;
    TaskGroup m_group;
    std::mt19937_64 m_random;
    std::uint64_t m_accepted = 0;
    std::uint64_t m_unexpected = 0;
    std::atomic<std::uint64_t> m_executed{0};
};

// submit 与 shutdown 竞争：每条 accepted 任务都必须有明确归宿（执行或取消），
// payload 由任务自己回收，取消的那部分由 owner 在 group 完成后回收。
class SubmitShutdownRace final
{
  public:
    SubmitShutdownRace(const std::uint64_t seed, const TaskSystemConfig config) : m_random(seed), m_config(config)
    {
    }

    void Run()
    {
        TaskSystem tasks(m_config);
        std::atomic<bool> stop{false};

        std::thread submitter(
            [this, &tasks, &stop]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    if ((m_random() & 7U) == 0U)
                        std::this_thread::yield();

                    PayloadPool::Job* payload = m_pool.Create();
                    const SubmitResult result = tasks.Submit(
                        Task{&RunPayloadTask, payload, nullptr, TaskFlags::MainHelpAllowed, {0, "Race", 0}}, m_group);

                    switch (result)
                    {
                    case SubmitResult::Accepted:
                        ++m_accepted;
                        break;
                    case SubmitResult::QueueFull:
                        m_pool.Retire(payload); // 发布失败：owner 立刻回收。
                        ++m_queueFull;
                        break;
                    case SubmitResult::Stopping:
                        m_pool.Retire(payload);
                        ++m_stopping;
                        stop.store(true, std::memory_order_relaxed);
                        break;
                    case SubmitResult::InvalidTask:
                    default:
                        m_pool.Retire(payload);
                        ++m_invalid;
                        break;
                    }
                }
            });

        std::this_thread::sleep_for(2ms);
        tasks.Shutdown(false); // 取消路径：未开始任务按 group.Done 记账后 join。
        stop.store(true, std::memory_order_relaxed);
        submitter.join();

        m_statistics = tasks.Statistics();
        m_groupComplete = m_group.IsComplete();
        m_pool.ReclaimOutstanding(); // group 完成后 owner 回收被取消的 payload。
    }

    [[nodiscard]] std::uint64_t Accepted() const noexcept
    {
        return m_accepted;
    }

    [[nodiscard]] std::uint64_t Executed() const noexcept
    {
        return m_statistics.executed;
    }

    [[nodiscard]] std::uint64_t CancelledBeforeExecution() const noexcept
    {
        return m_statistics.cancelled;
    }

    [[nodiscard]] std::uint64_t Submitted() const noexcept
    {
        return m_statistics.submitted;
    }

    [[nodiscard]] std::uint64_t Pending() const noexcept
    {
        return m_statistics.pending;
    }

    [[nodiscard]] std::uint64_t PayloadsCreated() const noexcept
    {
        return m_pool.Created();
    }

    [[nodiscard]] std::uint64_t PayloadsDestroyed() const noexcept
    {
        return m_pool.Destroyed();
    }

    [[nodiscard]] std::uint64_t QueueFull() const noexcept
    {
        return m_queueFull;
    }

    [[nodiscard]] std::uint64_t Invalid() const noexcept
    {
        return m_invalid;
    }

    [[nodiscard]] bool GroupComplete() const noexcept
    {
        return m_groupComplete;
    }

  private:
    TaskGroup m_group;
    PayloadPool m_pool;
    TaskSystemStatistics m_statistics;
    std::mt19937_64 m_random;
    TaskSystemConfig m_config;
    std::uint64_t m_accepted = 0;
    std::uint64_t m_queueFull = 0;
    std::uint64_t m_stopping = 0;
    std::uint64_t m_invalid = 0;
    bool m_groupComplete = false;
};
} // namespace

TEST(TaskStressTests, BurstyPublishDoesNotLoseWakeups)
{
    for (std::uint32_t round = 0; round < 1000; ++round)
    {
        TaskSystem tasks({.workerCount = 4});
        BurstFixture fixture(tasks, /*seed=*/6657 + round);
        fixture.PublishWithRandomYields(200);
        fixture.Wait();

        EXPECT_EQ(fixture.Unexpected(), 0U) << "round=" << round;
        EXPECT_EQ(fixture.Accepted(), fixture.Executed()) << "round=" << round;
        EXPECT_TRUE(fixture.GroupComplete()) << "round=" << round;
        tasks.Shutdown(true);
        EXPECT_EQ(tasks.Statistics().pending, 0U) << "round=" << round;
    }
}

TEST(TaskStressTests, BurstyPublishWithOneWorkerDoesNotLoseWakeups)
{
    for (std::uint32_t round = 0; round < 200; ++round)
    {
        TaskSystem tasks({.workerCount = 1});
        BurstFixture fixture(tasks, /*seed=*/991 + round);
        fixture.PublishWithRandomYields(64);
        fixture.Wait();

        EXPECT_EQ(fixture.Accepted(), fixture.Executed()) << "round=" << round;
        EXPECT_TRUE(fixture.GroupComplete()) << "round=" << round;
        tasks.Shutdown(true);
    }
}

TEST(TaskStressTests, SubmitRacingShutdownHasAccountedOutcome)
{
    for (std::uint32_t round = 0; round < 500; ++round)
    {
        SubmitShutdownRace fixture(/*seed=*/6657 + round, {.workerCount = 4, .injectQueueCapacity = 64});
        fixture.Run();

        EXPECT_EQ(fixture.Invalid(), 0U) << "round=" << round;
        EXPECT_EQ(fixture.Accepted(), fixture.Submitted()) << "round=" << round;
        EXPECT_EQ(fixture.Accepted(), fixture.Executed() + fixture.CancelledBeforeExecution()) << "round=" << round;
        EXPECT_EQ(fixture.PayloadsCreated(), fixture.PayloadsDestroyed()) << "round=" << round;
        EXPECT_EQ(fixture.Pending(), 0U) << "round=" << round;
        EXPECT_TRUE(fixture.GroupComplete()) << "round=" << round;
        EXPECT_GT(fixture.Accepted(), 0U) << "round=" << round; // 竞争必须真的发生过提交
    }
}

// 取消路径的定向覆盖：shutdown 与"仍在发布的提交者"竞争时，被取消的任务也必须
// 让 group 完成（否则 owner 永远不知道何时能回收 payload）。
TEST(TaskStressTests, CancelledTasksAlwaysCompleteTheirGroup)
{
    for (std::uint32_t round = 0; round < 100; ++round)
    {
        TaskSystem tasks({.workerCount = 2, .injectQueueCapacity = 8});
        TaskGroup group;
        PayloadPool pool;
        std::atomic<bool> stop{false};

        std::thread submitter(
            [&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    PayloadPool::Job* payload = pool.Create();
                    const SubmitResult result = tasks.Submit(
                        Task{&RunPayloadTask, payload, nullptr, TaskFlags::MainHelpAllowed, {0, "Cancel", 0}}, group);
                    if (result != SubmitResult::Accepted)
                    {
                        pool.Retire(payload); // 发布失败：owner 立刻回收。
                        if (result == SubmitResult::Stopping)
                            stop.store(true, std::memory_order_relaxed);
                    }
                }
            });

        std::this_thread::sleep_for(1ms);
        tasks.Shutdown(false);
        stop.store(true, std::memory_order_relaxed);
        submitter.join();

        EXPECT_TRUE(group.IsComplete()) << "round=" << round;
        pool.ReclaimOutstanding();
        EXPECT_EQ(pool.Created(), pool.Destroyed()) << "round=" << round;
        EXPECT_EQ(tasks.Statistics().pending, 0U) << "round=" << round;
    }
}

// M7-TASK-RACE-FLAKE 回归：用测试缝（`injectPublishStallMicroseconds`）把 publish 的
// "pending 已记账、归宿未定"窗口从纳秒级拉长到毫秒级，再把 shutdown 压在窗口中间。
// 修复前：shutdown 在清点 pending 时会撞上这条未落定的提交，`shutdown must leave no
// pending tasks` 触发 FailAssertion（进程终止 → ctest 失败，且看不到任何 EXPECT 行，
// 与 M7-10 记录里"失败但无断言输出"的现象一致）。
// 修复后：shutdown 先与 publish 的记账窗口互斥，在途提交要么已入队（随后被取消）、
// 要么已回滚，断言不可能再看到"半记账"状态。
TEST(TaskStressTests, ShutdownWaitsForInFlightPublishAccounting)
{
    for (std::uint32_t round = 0; round < 20; ++round)
    {
        SubmitShutdownRace fixture(
            /*seed=*/20260918 + round,
            {.workerCount = 2, .injectQueueCapacity = 64, .injectPublishStallMicroseconds = 2000});
        fixture.Run();

        // fixture 在启动提交线程后固定 sleep 2ms 再 Shutdown：配合 2ms 的 publish 停顿，
        // 每一轮都必然把 shutdown 压在记账窗口里。
        EXPECT_EQ(fixture.Accepted(), fixture.Submitted()) << "round=" << round;
        EXPECT_EQ(fixture.Accepted(), fixture.Executed() + fixture.CancelledBeforeExecution()) << "round=" << round;
        EXPECT_EQ(fixture.PayloadsCreated(), fixture.PayloadsDestroyed()) << "round=" << round;
        EXPECT_EQ(fixture.Pending(), 0U) << "round=" << round;
        EXPECT_TRUE(fixture.GroupComplete()) << "round=" << round;
    }
}

// Run this target with a timeout. A hang is a test failure, not a manual retry.
