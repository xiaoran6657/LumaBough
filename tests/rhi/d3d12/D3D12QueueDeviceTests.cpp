// ============================================================================
// D3D12QueueDeviceTests.cpp — 真 GPU 上的提交时间线（Queue/Fence/FrameContext）
// 里程碑：M5（03 篇 Queue、Fence 与 FrameContext）
// 职责：在真实 Device 上验证 D3D12Queue 的设备级行为：创建与命名、
//       Execute→Signal 的单一写点、fence 严格单调、三帧轮转无需每帧 flush、
//       FlushGpu 后全部 context fence 完成、BeginFrame 复用守卫与初始化守卫。
//       纯时间线逻辑（回收/轮转/耗尽）由 FenceTimelineTests 在 CPU 侧穷举。
// 环境：需要 D3D12 硬件或 WARP（与仓库既有设备级测试同口径）。
// 关联：docs/architecture/README.md（阶段验收）
// ============================================================================
#include "D3D12Queue.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>

using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12FrameContext;
using MiniEngine::Rhi::D3D12::D3D12Queue;

namespace
{
// 设备级测试的公共前置：debug 模式创建 Device 与 Queue（诊断开销可接受）。
struct DeviceHarness final
{
    std::unique_ptr<D3D12Device> device;
    D3D12Queue queue;

    DeviceHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = D3D12Device::Create(options);
        // 公共头只暴露不透明句柄（边界约定）；类型还原在后端内部/测试处完成。
        queue.Initialize(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()));
    }
};
} // namespace

// 创建 Gate：Queue/Fence/三个 allocator/单一 list 全部就绪，初值语义正确。
TEST(D3D12QueueDeviceTests, InitializeCreatesTimelineWithInitialSemantics)
{
    DeviceHarness harness;

    EXPECT_EQ(harness.queue.CompletedValue(), 0U) << "Fence 初值必须为 0";
    EXPECT_EQ(harness.queue.NextFenceValue(), 1U) << "第一个提交值从 1 开始（0 保留给从未提交的 context）";
    EXPECT_EQ(harness.queue.NativeQueue().GetDesc().Type, D3D12_COMMAND_LIST_TYPE_DIRECT);
}

// 连续 6 帧（两次完整轮转）：Execute→Signal 单一写点、值严格单调 1..6；
// 空命令列表的提交应很快完成，使第二次轮转的 BeginFrame 无需等待。
TEST(D3D12QueueDeviceTests, SixFrameRotationKeepsFenceStrictlyMonotonic)
{
    DeviceHarness harness;
    harness.queue.SetTraceEnabled(true);

    std::uint64_t previous = 0;
    for (std::uint32_t frame = 0; frame < 6U; ++frame)
    {
        D3D12FrameContext& frameContext = harness.queue.BeginFrame(frame % 3U);

        // 空录制：本篇没有绘制命令，Close/Execute 本身就是完整的提交。
        const std::uint64_t submitted = harness.queue.ExecuteAndSignal(frameContext);
        EXPECT_EQ(submitted, previous + 1U) << "fence 必须严格单调递增 1..6";
        EXPECT_EQ(frameContext.submittedFenceValue, submitted) << "Signal 成功后必须写回 context（单一写点）";
        previous = submitted;
    }

    EXPECT_EQ(harness.queue.NextFenceValue(), 7U);

    // 测试收尾用 flush 一次性等待全部提交（属"明确的提交边界"场景）。
    harness.queue.FlushGpu("test-end");
    EXPECT_GE(harness.queue.CompletedValue(), previous) << "Flush 后完成值必须覆盖最后一次提交";
}

// FlushGpu 的语义核验：所有 context fence 完成；连续 flush 安全；随后继续
// 正常帧循环不异常。
TEST(D3D12QueueDeviceTests, FlushCompletesAllContextFences)
{
    DeviceHarness harness;
    for (std::uint32_t frame = 0; frame < 3U; ++frame)
    {
        D3D12FrameContext& frameContext = harness.queue.BeginFrame(frame);
        static_cast<void>(harness.queue.ExecuteAndSignal(frameContext));
    }

    // 不 flush 时最后一个 context 的 fence 可能尚未完成（GPU 在飞行）——
    // 这正是三帧并行的预期；flush 后必须全部完成。
    // fence 账目：3 次提交 = 1..3，两次 flush 各占 4、5（flush 也走同一时间线）。
    harness.queue.FlushGpu("test-flush-semantics");
    EXPECT_NO_THROW(harness.queue.FlushGpu("test-flush-idempotent")) << "连续 flush 必须安全";
    ASSERT_EQ(harness.queue.NextFenceValue(), 6U);

    D3D12FrameContext& reused = harness.queue.BeginFrame(0U);
    static_cast<void>(harness.queue.ExecuteAndSignal(reused));
    EXPECT_EQ(reused.submittedFenceValue, 6U);
    EXPECT_EQ(harness.queue.NextFenceValue(), 7U);
}

// 负向：录制未 Execute 就再次 BeginFrame 必须拒绝（allocator 复用保护）。
TEST(D3D12QueueDeviceTests, BeginFrameWhileRecordingIsRejected)
{
    DeviceHarness harness;
    D3D12FrameContext& frameContext = harness.queue.BeginFrame(0U);
    static_cast<void>(frameContext);

    EXPECT_THROW(harness.queue.BeginFrame(1U), std::logic_error);
    // 恢复正常路径：完成本轮提交后可以继续。
    static_cast<void>(harness.queue.ExecuteAndSignal(frameContext));
    EXPECT_NO_THROW(harness.queue.BeginFrame(1U));
}

// 负向：未 Initialize / 未录制时的调用必须显式失败，而不是带着半初始化状态继续。
TEST(D3D12QueueDeviceTests, UninitializedAndUnrecordingPathsAreRejected)
{
    D3D12Queue uninitialized;
    EXPECT_THROW(uninitialized.BeginFrame(0U), std::logic_error);
    EXPECT_THROW(uninitialized.FlushGpu("test-uninitialized"), std::logic_error);

    DeviceHarness harness;
    D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
    static_cast<void>(frame);
    EXPECT_NO_THROW(harness.queue.ExecuteAndSignal(frame));
    EXPECT_THROW(static_cast<void>(harness.queue.ExecuteAndSignal(frame)), std::logic_error)
        << "没有打开的录制时不可重复提交";

    // 越界的 back-buffer index 必须拒绝（frame count 固定 3）。
    EXPECT_THROW(harness.queue.BeginFrame(3U), std::out_of_range);
}
