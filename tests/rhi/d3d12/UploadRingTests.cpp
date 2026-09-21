// ============================================================================
// UploadRingTests.cpp — 环形上传分配的纯 CPU 契约测试
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 2 条）
// 职责：穷举 06 篇「测试」清单里与 GPU 无关的部分：空 ring、精确填满、对齐 padding、
//       回绕（含同一帧回绕成两个 span 且同 fence）、head/tail 跨零后不覆盖本帧 span、
//       批量回收、溢出与不可表示、high-water 统计、CBV 256 对齐不变式。
// 为什么直接测生产实现：分配/回绕/回收是纯记账（UploadRingAllocator 无 D3D 类型），
//       与 05 篇一样用同一份实现满足"模型与实现等价"。
// 设备侧（持久映射、GPUVA 换算、dedicated 释放、纹理足迹）见 D3D12UploadDeviceTests。
// 关联：docs/architecture/README.md（Ring 分配 / 测试）
// ============================================================================
#include "D3D12TextureUpload.h" // CalcSubresourceIndex（06 篇的等价已测试函数）
#include "D3D12UploadRingAllocator.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

using MiniEngine::Rhi::D3D12::UploadRingAllocator;
using MiniEngine::Rhi::D3D12::UploadRingSpan;

namespace
{
constexpr std::uint64_t kCbvAlignment = UploadRingAllocator::kConstantBufferAlignment; // 256
} // namespace

// 空 ring：从 0 开始分配，且 head 随分配前进。
TEST(UploadRingTests, EmptyRingAllocatesFromZero)
{
    UploadRingAllocator ring{1024U};
    EXPECT_EQ(ring.Head(), 0U);
    EXPECT_TRUE(ring.IsIdle());
    EXPECT_EQ(ring.OldestPendingFence(), 0U);

    const auto span = ring.TryAllocate(128U, kCbvAlignment);
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(span->offset, 0U);
    EXPECT_EQ(span->size, 128U);
    EXPECT_EQ(ring.Head(), 128U);
    EXPECT_EQ(ring.CurrentSpanCount(), 1U);
}

// 对齐：CBV 请求必须向上取整到 256；head 推进到对齐后的末尾。
TEST(UploadRingTests, AlignsConstantBufferRequestsTo256)
{
    UploadRingAllocator ring{4096U};
    const auto first = ring.TryAllocate(64U, kCbvAlignment); // 占 [0,64)
    ASSERT_TRUE(first.has_value());

    const auto second = ring.TryAllocate(64U, kCbvAlignment);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->offset, kCbvAlignment) << "第二次 CBV 必须落在 256 对齐处（64 之后补 padding）";

    const auto third = ring.TryAllocate(kCbvAlignment, kCbvAlignment);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->offset % kCbvAlignment, 0U) << "CBV 地址必须始终 256 对齐";
}

// 精确填满：连续分配直到整段用完，head 归零。
TEST(UploadRingTests, ExactFillWrapsHeadToZero)
{
    UploadRingAllocator ring{512U};
    const auto first = ring.TryAllocate(256U, kCbvAlignment);
    const auto second = ring.TryAllocate(256U, kCbvAlignment);
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(first->offset, 0U);
    EXPECT_EQ(second->offset, 256U);
    EXPECT_EQ(ring.Head(), 0U) << "恰好用完时 head 必须回到 0（等价于回绕）";

    // 仍在同一帧内：再次分配会与第一个 span 重叠，必须失败而不是覆盖。
    EXPECT_FALSE(ring.TryAllocate(256U, kCbvAlignment).has_value()) << "同一帧内不得覆盖已分配的 span（回绕也不行）";
}

// 溢出与不可表示：请求 0 或大于整个 ring 时返回 nullopt（由策略层判定失败）。
TEST(UploadRingTests, RejectsZeroAndOversizedRequests)
{
    UploadRingAllocator ring{1024U};
    EXPECT_FALSE(ring.TryAllocate(0U, kCbvAlignment).has_value());
    EXPECT_FALSE(ring.TryAllocate(1024U + 1U, kCbvAlignment).has_value());
    EXPECT_FALSE(ring.TryAllocate(2048U, kCbvAlignment).has_value());
}

// 非 2 的幂对齐必须显式失败（不能静默按位运算得到怪值）。
TEST(UploadRingTests, RejectsNonPowerOfTwoAlignment)
{
    UploadRingAllocator ring{1024U};
    EXPECT_THROW(static_cast<void>(ring.TryAllocate(64U, 0U)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ring.TryAllocate(64U, 96U)), std::invalid_argument);
}

// 回绕（06 篇核心用例）：同一帧内尾部放不下时回绕到头部，形成**两个非回绕 span**，
// 二者互不覆盖；CommitFrame 必须给它们标记**同一个 fence**。
TEST(UploadRingTests, WrapsToZeroAndFlagsBothSpansWithOneFence)
{
    UploadRingAllocator ring{1024U};

    // 前一帧占头部 [0,256) 并已完成回收 → 头部空出来了。
    const auto previous = ring.TryAllocate(256U, kCbvAlignment);
    ASSERT_TRUE(previous.has_value());
    ring.CommitFrame(1U);
    ring.Reclaim(1U);

    // 本帧从 head=256 开始，一次占满尾部 [256,1024)。
    const auto tail = ring.TryAllocate(768U, kCbvAlignment);
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->offset, 256U);

    // 尾部已无空间 → 回绕到 [0,256)：不覆盖 [256,1024)。
    const auto wrapped = ring.TryAllocate(256U, kCbvAlignment);
    ASSERT_TRUE(wrapped.has_value());
    EXPECT_EQ(wrapped->offset, 0U) << "尾部放不下时必须回绕到头部，而不是覆盖";
    EXPECT_EQ(ring.CurrentSpanCount(), 2U) << "回绕帧必须记录两个非回绕 span（禁止 begin > end 的单一区间）";

    ring.CommitFrame(2U);
    EXPECT_EQ(ring.PendingSpanCount(), 2U);
    ring.Reclaim(1U);
    EXPECT_EQ(ring.PendingSpanCount(), 2U) << "两个 span 属于 fence 2：只回收 fence 1 不该释放任何一段";
    EXPECT_EQ(ring.OldestPendingFence(), 2U);

    // 整段仍被占用：任何新分配都必须失败。
    EXPECT_FALSE(ring.TryAllocate(1U, 16U).has_value());
    EXPECT_EQ(ring.OccupiedBytes(), 1024U);
    EXPECT_EQ(ring.HighWaterBytes(), 1024U);

    ring.Reclaim(2U);
    EXPECT_TRUE(ring.IsIdle());
}

// head 跨零后的复用：只有被真正回收的区间可再分配，未被回收的 pending 区间仍受保护。
TEST(UploadRingTests, ReclaimGatesReuseAcrossZero)
{
    UploadRingAllocator ring{1024U};
    const auto head = ring.TryAllocate(768U, kCbvAlignment); // [0,768)
    ASSERT_TRUE(head.has_value());
    ring.CommitFrame(10U);

    const auto tail = ring.TryAllocate(256U, kCbvAlignment); // [768,1024)，head 归零
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->offset, 768U);
    ring.CommitFrame(11U);

    EXPECT_EQ(ring.PendingSpanCount(), 2U);
    EXPECT_EQ(ring.OldestPendingFence(), 10U);
    EXPECT_FALSE(ring.TryAllocate(256U, kCbvAlignment).has_value()) << "两段都 pending：无安全 span";

    ring.Reclaim(10U); // 只回收 [0,768)
    EXPECT_EQ(ring.PendingSpanCount(), 1U);
    EXPECT_EQ(ring.OldestPendingFence(), 11U);

    const auto reused = ring.TryAllocate(768U, kCbvAlignment);
    ASSERT_TRUE(reused.has_value()) << "已回收的头部区间必须可复用";
    EXPECT_EQ(reused->offset, 0U);
    ring.CommitFrame(12U);

    // 现在 [0,768) 属 fence 12、[768,1024) 属 fence 11：都还在飞行中。
    EXPECT_FALSE(ring.TryAllocate(1U, 16U).has_value());
    ring.Reclaim(11U);
    EXPECT_EQ(ring.PendingSpanCount(), 1U);
    ring.Reclaim(12U);
    EXPECT_TRUE(ring.IsIdle()) << "两个 fence 都完成后必须回到完全空闲";
}

// 批量回收：一次 Reclaim 清掉多个已完成的 fence 批次。
TEST(UploadRingTests, ReclaimProcessesMultipleFencesAtOnce)
{
    UploadRingAllocator ring{1024U};
    for (std::uint64_t frame = 1U; frame <= 4U; ++frame)
    {
        const auto span = ring.TryAllocate(128U, kCbvAlignment);
        ASSERT_TRUE(span.has_value()) << "frame " << frame;
        ring.CommitFrame(frame);
        ring.Reclaim(frame); // 立刻回收，保证下一帧仍有空间
    }
    EXPECT_TRUE(ring.IsIdle());
    EXPECT_EQ(ring.HighWaterBytes(), 128U) << "每帧只占 128 字节，高水位不应累积";
}

// 提交契约：fence 非零且单调；空帧提交不推进基线。
TEST(UploadRingTests, CommitFenceMustBeNonZeroAndMonotonic)
{
    UploadRingAllocator ring{1024U};
    ring.CommitFrame(5U); // 无 current span：允许（不推进基线）

    const auto span = ring.TryAllocate(128U, kCbvAlignment);
    ASSERT_TRUE(span.has_value());
    EXPECT_THROW(ring.CommitFrame(0U), std::invalid_argument) << "fence 0 是保留值";

    ring.CommitFrame(7U);
    const auto next = ring.TryAllocate(128U, kCbvAlignment);
    ASSERT_TRUE(next.has_value());
    EXPECT_THROW(ring.CommitFrame(6U), std::logic_error) << "fence 回退必须失败";
    EXPECT_NO_THROW(ring.CommitFrame(7U)) << "同一 fence 提交多段（回绕帧）合法";
}

// 可诊断量：最大空闲空间随 pending 变化，供"是否需要 dedicated"判断。
TEST(UploadRingTests, LargestFreeSpanReflectsPendingOccupancy)
{
    UploadRingAllocator ring{1024U};
    EXPECT_EQ(ring.LargestFreeSpan(), 1024U);

    const auto first = ring.TryAllocate(256U, kCbvAlignment);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(ring.LargestFreeSpan(), 768U);

    ring.CommitFrame(3U);
    EXPECT_EQ(ring.LargestFreeSpan(), 768U) << "pending 与 current 一样占用空间";
}

// 容量 0 非法（ring 必须能放至少一个 span）。
TEST(UploadRingTests, RejectsZeroCapacityConstruction)
{
    EXPECT_THROW(UploadRingAllocator{0U}, std::invalid_argument);
}

// subresource 线性下标：mip-major、然后 slice、最后 plane（与 D3D12 约定一致）。
TEST(UploadRingTests, SubresourceIndexFollowsMipThenSliceOrder)
{
    using MiniEngine::Rhi::D3D12::CalcSubresourceIndex;
    constexpr std::uint32_t kMips = 4U;
    constexpr std::uint32_t kSlices = 6U;

    EXPECT_EQ(CalcSubresourceIndex(0U, 0U, 0U, kMips, kSlices), 0U);
    EXPECT_EQ(CalcSubresourceIndex(3U, 0U, 0U, kMips, kSlices), 3U) << "同一 slice 内 mip 连续";
    EXPECT_EQ(CalcSubresourceIndex(0U, 1U, 0U, kMips, kSlices), kMips) << "下一 slice 从 mipLevels 开始";
    EXPECT_EQ(CalcSubresourceIndex(0U, 0U, 1U, kMips, kSlices), kMips * kSlices) << "plane 在最后";
    EXPECT_EQ(CalcSubresourceIndex(3U, 5U, 0U, kMips, kSlices), kMips * kSlices - 1U)
        << "最后一个 slice 的最后一个 mip";
}
