// ============================================================================
// DeferredReleaseTests.cpp — fence 延迟释放队列的纯 CPU 契约测试
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 3 条）
// 职责：锁定"旧对象只在 completedFence >= retireFence 时释放"这一判据，以及
//       单调性、批量回收、回调语义（先出队再执行，回调内可安全再入队）。
//       这是热重载与 resize 的资源安全前提：**不能用 CPU 帧号/时间代替 fence**。
// 关联：docs/architecture/README.md（Hot reload 与 deferred release）
// ============================================================================
#include "D3D12DeferredRelease.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

using MiniEngine::Rhi::D3D12::D3D12DeferredRelease;

// 回收严格按 fence：未完成的绝不提前释放，且按入队顺序执行。
TEST(DeferredReleaseTests, ReleasesOnlyAfterFenceCompletesInOrder)
{
    D3D12DeferredRelease deferred;
    std::vector<int> released;

    deferred.Retire(5U, [&released]() { released.push_back(5); });
    deferred.Retire(7U, [&released]() { released.push_back(7); });
    deferred.Retire(7U, [&released]() { released.push_back(70); }); // 同一 fence 的多个对象

    EXPECT_EQ(deferred.Reclaim(4U), 0U);
    EXPECT_TRUE(released.empty()) << "completed < retireFence 时不得释放任何对象";
    EXPECT_EQ(deferred.PendingCount(), 3U);

    EXPECT_EQ(deferred.Reclaim(5U), 1U);
    ASSERT_EQ(released.size(), 1U);
    EXPECT_EQ(released.front(), 5);

    EXPECT_EQ(deferred.Reclaim(7U), 2U) << "同一 fence 的多个对象一次批量释放";
    EXPECT_EQ(released.size(), 3U);
    EXPECT_TRUE(deferred.Empty());
}

// 契约失败：fence 0 / 空回调 / fence 回退都必须显式失败。
TEST(DeferredReleaseTests, RejectsInvalidEntriesAndNonMonotonicFences)
{
    D3D12DeferredRelease deferred;
    EXPECT_THROW(deferred.Retire(0U, []() {}), std::invalid_argument) << "fence 0 是保留值";
    EXPECT_THROW(deferred.Retire(3U, std::function<void()>{}), std::invalid_argument) << "空回调没有意义";

    deferred.Retire(3U, []() {});
    EXPECT_THROW(deferred.Retire(2U, []() {}), std::logic_error) << "fence 回退会让批量回收漏项";
    EXPECT_NO_THROW(deferred.Retire(3U, []() {})) << "同 fence 多个对象合法";
}

// 回调内再次 Retire（嵌套释放）必须安全：回收实现先出队再执行。
TEST(DeferredReleaseTests, AllowsNestedRetireFromInsideCallback)
{
    D3D12DeferredRelease deferred;
    int released = 0;

    deferred.Retire(4U,
                    [&deferred, &released]()
                    {
                        ++released;
                        deferred.Retire(9U, [&released]() { ++released; });
                    });

    EXPECT_EQ(deferred.Reclaim(4U), 1U);
    EXPECT_EQ(released, 1);
    EXPECT_EQ(deferred.PendingCount(), 1U) << "回调内新入队的对象仍在队列里";

    EXPECT_EQ(deferred.Reclaim(8U), 0U);
    EXPECT_EQ(deferred.Reclaim(9U), 1U);
    EXPECT_EQ(released, 2);
}

// 统计量：入队与回收计数可用于 metadata 取证。
TEST(DeferredReleaseTests, TracksRetiredAndReclaimedCounts)
{
    D3D12DeferredRelease deferred;
    deferred.Retire(2U, []() {});
    deferred.Retire(3U, []() {});
    EXPECT_EQ(deferred.RetiredCount(), 2U);
    EXPECT_EQ(deferred.ReclaimedCount(), 0U);
    EXPECT_EQ(deferred.OldestFence(), 2U);

    static_cast<void>(deferred.Reclaim(2U));
    EXPECT_EQ(deferred.ReclaimedCount(), 1U);
    EXPECT_EQ(deferred.OldestFence(), 3U);
    static_cast<void>(deferred.Reclaim(100U));
    EXPECT_EQ(deferred.ReclaimedCount(), 2U);
    EXPECT_EQ(deferred.OldestFence(), 0U) << "空队列的最老 fence 为 0";
}
