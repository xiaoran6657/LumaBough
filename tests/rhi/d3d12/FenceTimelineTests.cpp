// ============================================================================
// FenceTimelineTests.cpp — fence 时间线的纯 CPU 契约测试
// 里程碑：M5（03 篇 Queue、Fence 与 FrameContext；手抄清单第 3 条）
// 职责：在无 GPU 的前提下穷举时间线规则：回收只看 completed、三 context 轮转
//       10,000 帧不出现 Reset-before-complete、fence 严格单调且耗尽即 fatal、
//       retire fence 乱序即拒绝。期望值全部手算可推导，不依赖任何运行时输出。
// 对应关系：模型（FenceTimelineModel.h）的每条规则与生产 D3D12Queue 一一对应；
//       设备级行为另由 D3D12QueueDeviceTests 在真 GPU 上验证。
// 关联：docs/architecture/README.md（测试清单）
// ============================================================================
#include "FenceTimelineModel.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

using MiniEngine::Rhi::D3D12::Tests::FenceTimelineModel;
using MiniEngine::Rhi::D3D12::Tests::kModelFenceSentinel;

// 回收唯一判据 completed >= retireFence：0、相等、未完成、多项一次性回收。
TEST(FenceTimelineTests, ReclaimsOnlyCompletedValues)
{
    FenceTimelineModel model{3U};
    model.Retire(3U, 10);
    model.Retire(5U, 20);
    model.Retire(8U, 30);

    // completed=0：什么都不可回收（retire fence 最小为 1）。
    EXPECT_EQ(model.Reclaim(0U).size(), 0U);
    EXPECT_EQ(model.RetiredCount(), 3U);

    // completed=3：恰好完成第一项（<= 语义，不是 <）。
    const auto first = model.Reclaim(3U);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first.front(), 10);

    // completed=4：5 仍未完成（严格按值比较，不允许"接近就算完成"）。
    EXPECT_EQ(model.Reclaim(4U).size(), 0U);
    EXPECT_EQ(model.RetiredCount(), 2U);

    // completed=8：剩余两项一次性回收，且顺序保持入队顺序。
    const auto rest = model.Reclaim(8U);
    ASSERT_EQ(rest.size(), 2U);
    EXPECT_EQ(rest.front(), 20);
    EXPECT_EQ(rest.back(), 30);
    EXPECT_EQ(model.RetiredCount(), 0U);
}

// 三 context 轮转 10,000 帧：GPU 恒定落后两帧完成时，任何一帧的 BeginFrame
// 都不得命中 Reset-before-complete（这正是三帧在飞行的意义）。
TEST(FenceTimelineTests, ThreeContextRotationOver10000FramesNeverResetsBeforeComplete)
{
    FenceTimelineModel model{3U};
    std::uint64_t lastSubmitted = 0;
    std::uint64_t completedTarget = 0;

    for (std::uint64_t frame = 0; frame < 10000U; ++frame)
    {
        const std::size_t frameIndex = static_cast<std::size_t>(frame % 3U);

        // 模拟 GPU 完成"最近两次提交之前"的全部工作（两帧滞后）。
        if (completedTarget + 2U <= lastSubmitted)
        {
            completedTarget += 2U;
            model.SetCompleted(completedTarget);
        }

        // 轮转复用下 allocator 的 fence 必然已完成：不抛即通过。
        EXPECT_NO_THROW(model.BeginFrame(frameIndex));
        lastSubmitted = model.Submit(frameIndex);
    }

    // 全程严格单调且从未触碰哨兵。
    EXPECT_EQ(lastSubmitted, 10000U);
    EXPECT_EQ(model.NextFenceValue(), 10001U);
    EXPECT_LT(model.Completed(), 10000U) << "落后两帧的完成进度不应追平最后一次提交";
}

// 复用尚未完成的 context：模型必须以 Reset-before-complete 拒绝（负向捕获）。
TEST(FenceTimelineTests, ResetBeforeCompleteIsRejected)
{
    FenceTimelineModel model{3U};
    static_cast<void>(model.Submit(0U)); // fence 1 → context 0
    model.SetCompleted(0U);              // GPU 尚未完成任何工作

    EXPECT_TRUE(model.NeedsWait(0U));
    EXPECT_THROW(model.BeginFrame(0U), std::logic_error);

    // 完成之后同一 context 可以安全复用。
    model.SetCompleted(1U);
    EXPECT_FALSE(model.NeedsWait(0U));
    EXPECT_NO_THROW(model.BeginFrame(0U));
}

// 从未提交的 context（fence 0）永远不需要等待——BeginFrame 的初值语义。
TEST(FenceTimelineTests, UnsubmittedContextNeverWaits)
{
    FenceTimelineModel model{3U};
    EXPECT_EQ(model.Completed(), 0U);
    EXPECT_FALSE(model.NeedsWait(1U));
    EXPECT_NO_THROW(model.BeginFrame(1U));
}

// fence 时间线在 UINT64_MAX 处 fatal：不允许回绕，也不允许复用旧值。
TEST(FenceTimelineTests, TimelineExhaustionIsFatalInsteadOfWrapping)
{
    FenceTimelineModel model{3U};
    // 时间线直接跳到哨兵（SetNextFenceValueForTest）：逐次 Submit 循环到这里
    // 是 1.8e19 次迭代，等效死循环——本用例曾因此挂死 ctest，教训已写进模型注释。
    model.SetNextFenceValueForTest(kModelFenceSentinel);
    EXPECT_EQ(model.NextFenceValue(), kModelFenceSentinel);
    EXPECT_THROW(model.Submit(0U), std::overflow_error);

    // 哨兵值也不可作为完成进度出现（device removed 语义）。
    EXPECT_THROW(model.SetCompleted(kModelFenceSentinel), std::runtime_error);
}

// retire fence 必须非零且单调：乱序 retire 是时间线逻辑错误的信号，必须拒绝。
TEST(FenceTimelineTests, RetireFenceMustBeNonZeroAndMonotonic)
{
    FenceTimelineModel model{3U};
    EXPECT_THROW(model.Retire(0U, 1), std::invalid_argument) << "fence 0 意味着从未提交，不可作为回收判据";

    model.Retire(5U, 1);
    EXPECT_THROW(model.Retire(4U, 2), std::logic_error) << "乱序 retire 会让 completed 一次性回收漏项";
    EXPECT_NO_THROW(model.Retire(5U, 2)) << "同值 retire（同一提交内多个对象）合法";
}
