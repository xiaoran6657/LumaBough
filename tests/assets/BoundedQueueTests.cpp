// ============================================================================
// BoundedQueueTests.cpp — 有界队列原语（M7-06，白盒）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：验证 M7-A19 依赖的两个性质：容量不允许静默溢出、关闭后不再接收；
//       以及 high-water 记账是"满载行为可复现"的证据来源。
//       白盒：直接 include engine/assets/async/src/BoundedQueue.h（内部头）。
// 关联：engine/assets/async/src/BoundedQueue.h
//       tests/assets/AsyncAssetLoaderTests.cpp（loader 级背压用例）
// ============================================================================

#include "BoundedQueue.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <utility>

namespace
{
using MiniEngine::Assets::BoundedQueue;
} // namespace

TEST(BoundedQueueTests, RejectsPushBeyondCapacityWithoutLosingItems)
{
    BoundedQueue<int> queue(2);
    EXPECT_TRUE(queue.TryPush(1));
    EXPECT_TRUE(queue.TryPush(2));
    EXPECT_FALSE(queue.TryPush(3)); // 满载：调用方必须自己处理背压
    EXPECT_EQ(queue.Size(), 2U);
    EXPECT_EQ(queue.Capacity(), 2U);

    ASSERT_TRUE(queue.TryPop().has_value());
    EXPECT_EQ(*queue.TryPop(), 2);
    EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(BoundedQueueTests, CloseStopsAcceptingAndDrainsRemainingItems)
{
    BoundedQueue<int> queue(4);
    EXPECT_TRUE(queue.TryPush(7));
    queue.Close();
    EXPECT_TRUE(queue.IsClosed());
    EXPECT_FALSE(queue.TryPush(8));
    // 关闭不等于丢弃：已入队项仍可被取走（关闭语义是"不再接收"）。
    ASSERT_TRUE(queue.TryPop().has_value());
    EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(BoundedQueueTests, HighWaterTracksTheDeepestFill)
{
    BoundedQueue<int> queue(4);
    EXPECT_EQ(queue.HighWater(), 0U);
    EXPECT_TRUE(queue.TryPush(1));
    EXPECT_TRUE(queue.TryPush(2));
    EXPECT_TRUE(queue.TryPush(3));
    EXPECT_EQ(queue.HighWater(), 3U);
    static_cast<void>(queue.TryPop());
    static_cast<void>(queue.TryPop());
    EXPECT_TRUE(queue.TryPush(4));
    // high-water 只记录历史最大深度：它是"背压是否真的发生"的机读证据。
    EXPECT_EQ(queue.HighWater(), 3U);
    EXPECT_EQ(queue.Size(), 2U);
}

// 队列存放 shared_ptr 的实际用法（请求载荷）：移动语义必须成立，且不复制载荷。
TEST(BoundedQueueTests, MovesSharedOwnershipWithoutCopying)
{
    BoundedQueue<std::shared_ptr<int>> queue(1);
    auto value = std::make_shared<int>(42);
    EXPECT_TRUE(queue.TryPush(value));
    EXPECT_EQ(value.use_count(), 2); // 队列持有一份
    auto popped = queue.TryPop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(**popped, 42);
    EXPECT_EQ(value.use_count(), 2); // 出队后由 popped 持有
}
