#include <MiniEngine/Core/FixedStep.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace
{
using namespace std::chrono_literals;

TEST(FixedStepTests, AccumulatesRemainderAndUsesConstantDelta)
{
    MiniEngine::FixedStepScheduler scheduler{10ms, 100ms, 8};
    std::vector<MiniEngine::Duration> deltas;

    const auto first = scheduler.Tick(6ms, [&](const MiniEngine::Duration delta) { deltas.push_back(delta); });

    EXPECT_EQ(first.updateCount, 0U);
    EXPECT_NEAR(first.alpha, 0.6, 0.000001);

    const auto second = scheduler.Tick(6ms, [&](const MiniEngine::Duration delta) { deltas.push_back(delta); });

    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_EQ(deltas.front(), 10ms);
    EXPECT_EQ(second.updateCount, 1U);
    EXPECT_EQ(scheduler.Accumulator(), 2ms);
    EXPECT_NEAR(second.alpha, 0.2, 0.000001);
}

TEST(FixedStepTests, ClampsLongFrameBeforeUpdating)
{
    MiniEngine::FixedStepScheduler scheduler{10ms, 25ms, 8};
    std::uint32_t callbackCount = 0;

    const auto result = scheduler.Tick(100ms,
                                       [&](const MiniEngine::Duration delta)
                                       {
                                           EXPECT_EQ(delta, 10ms);
                                           ++callbackCount;
                                       });

    EXPECT_TRUE(result.wasClamped);
    EXPECT_FALSE(result.backlogDropped);
    EXPECT_EQ(result.acceptedElapsed, 25ms);
    EXPECT_EQ(result.updateCount, 2U);
    EXPECT_EQ(callbackCount, 2U);
    EXPECT_EQ(scheduler.Accumulator(), 5ms);
}

TEST(FixedStepTests, CapsUpdatesAndDropsWholeStepBacklog)
{
    MiniEngine::FixedStepScheduler scheduler{10ms, 100ms, 3};
    std::uint32_t callbackCount = 0;

    const auto result = scheduler.Tick(50ms, [&](const MiniEngine::Duration) { ++callbackCount; });

    EXPECT_EQ(result.updateCount, 3U);
    EXPECT_EQ(callbackCount, 3U);
    EXPECT_TRUE(result.backlogDropped);
    EXPECT_LT(scheduler.Accumulator(), scheduler.Step());
    EXPECT_GE(result.alpha, 0.0);
    EXPECT_LT(result.alpha, 1.0);
}

TEST(FixedStepTests, ResetDiscardsAccumulatedTime)
{
    MiniEngine::FixedStepScheduler scheduler{10ms, 100ms, 8};
    std::uint32_t callbackCount = 0;

    scheduler.Tick(7ms, [&](const MiniEngine::Duration) { ++callbackCount; });
    scheduler.Reset();

    const auto result = scheduler.Tick(3ms, [&](const MiniEngine::Duration) { ++callbackCount; });

    EXPECT_EQ(callbackCount, 0U);
    EXPECT_EQ(result.updateCount, 0U);
    EXPECT_EQ(scheduler.Accumulator(), 3ms);
}

TEST(FixedStepTests, TreatsNegativeElapsedAsZero)
{
    MiniEngine::FixedStepScheduler scheduler{10ms, 100ms, 8};

    const auto result = scheduler.Tick(-5ms, [](const MiniEngine::Duration) {});

    EXPECT_EQ(result.acceptedElapsed, MiniEngine::Duration::zero());
    EXPECT_EQ(result.updateCount, 0U);
    EXPECT_DOUBLE_EQ(result.alpha, 0.0);
}
} // namespace