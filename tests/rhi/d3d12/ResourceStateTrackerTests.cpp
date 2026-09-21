// ============================================================================
// ResourceStateTrackerTests.cpp — 状态机的纯 CPU 契约测试
// 里程碑：M5（07 篇 Resource Barrier 与状态跟踪；手抄清单第 2 条）
// 职责：穷举 07 篇的状态规则：只在不同状态时发 barrier、whole/subresource 两种粒度、
//       pending 与 committed 分离（Execute 才 commit、失败回滚不污染）、
//       generation 失效、未注册/越界/重复注册的失败语义、trace hash 的顺序敏感性。
// 为什么直接测生产实现：状态机是纯逻辑（ResourceStateRegistry 不接触 GPU），
//       按 05/06 篇的同一原则用同一份实现满足"模型与实现等价"。
// 设备级 barrier 真实生成见 D3D12ResourceStateTrackerDeviceTests。
// 关联：docs/architecture/README.md（状态表 / Tracker 边界）
// ============================================================================
#include "D3D12ResourceStateRegistry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

using MiniEngine::Rhi::D3D12::BarrierRequest;
using MiniEngine::Rhi::D3D12::ResourceKey;
using MiniEngine::Rhi::D3D12::ResourceStateRegistry;

namespace
{
constexpr ResourceKey kTextureA{100U, 0U};
constexpr ResourceKey kTextureB{200U, 0U};

constexpr D3D12_RESOURCE_STATES kCopyDest = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kRenderTarget = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr D3D12_RESOURCE_STATES kPresent = D3D12_RESOURCE_STATE_PRESENT;
} // namespace

// 注册后：未开始 recording 时读到的就是 committed（实际初始状态）。
TEST(ResourceStateTrackerTests, RegisteredStateIsVisibleBeforeRecording)
{
    ResourceStateRegistry registry;
    registry.Register(kTextureA, 1U, kCopyDest);

    EXPECT_EQ(registry.TrackedResourceCount(), 1U);
    EXPECT_EQ(registry.SubresourceCount(kTextureA), 1U);
    EXPECT_EQ(registry.CurrentState(kTextureA, 0U), kCopyDest);
    EXPECT_FALSE(registry.IsRecording());
}

// 只在不同状态时生成 barrier：同状态 transition 必须什么都不产生。
TEST(ResourceStateTrackerTests, EmitsNothingWhenStateIsUnchanged)
{
    ResourceStateRegistry registry;
    registry.Register(kTextureA, 1U, kCopyDest);
    registry.BeginRecording();

    EXPECT_TRUE(registry.Transition(kTextureA, kCopyDest).empty()) << "before==after 不得生成 barrier";
    EXPECT_EQ(registry.BarrierCount(), 0U);
    EXPECT_TRUE(registry.PendingBarriers().empty());

    const std::vector<BarrierRequest> created = registry.Transition(kTextureA, kShaderRead);
    ASSERT_EQ(created.size(), 1U);
    EXPECT_EQ(created.front().before, kCopyDest);
    EXPECT_EQ(created.front().after, kShaderRead);
    EXPECT_EQ(created.front().subresource, 0U);
    EXPECT_EQ(registry.BarrierCount(), 1U);
}

// 整资源与单 subresource 两种粒度：全资源只对"确实不一致"的子资源生成 barrier。
TEST(ResourceStateTrackerTests, WholeAndPerSubresourceTransitions)
{
    ResourceStateRegistry registry;
    registry.Register(kTextureA, 4U, kCopyDest); // 例如 4 个 mip
    registry.BeginRecording();

    // 先把 mip 1 单独转成 shader read。
    const std::vector<BarrierRequest> single = registry.Transition(kTextureA, kShaderRead, 1U);
    ASSERT_EQ(single.size(), 1U);
    EXPECT_EQ(single.front().subresource, 1U);

    // 再把整个资源转成 shader read：只应为剩余 3 个子资源生成 barrier。
    const std::vector<BarrierRequest> rest = registry.Transition(kTextureA, kShaderRead);
    ASSERT_EQ(rest.size(), 3U) << "已处于目标状态的 subresource 不得重复转";
    for (const BarrierRequest& request : rest)
    {
        EXPECT_NE(request.subresource, 1U);
        EXPECT_EQ(request.before, kCopyDest);
        EXPECT_EQ(request.after, kShaderRead);
    }
    EXPECT_TRUE(registry.AllSubresourcesMatch(kTextureA, kShaderRead));
    EXPECT_FALSE(registry.FirstMismatchingSubresource(kTextureA, kShaderRead).has_value());
}

// pending 与 committed 分离：Transition 只改 pending；CommitExecuted 之后才是全局真值。
TEST(ResourceStateTrackerTests, PendingIsNotCommittedUntilExecuted)
{
    ResourceStateRegistry registry;
    registry.Register(kTextureA, 1U, kCopyDest);

    registry.BeginRecording();
    static_cast<void>(registry.Transition(kTextureA, kShaderRead));
    EXPECT_EQ(registry.CurrentState(kTextureA, 0U), kShaderRead) << "recording 中读到的是 pending";

    // flush 之后才能 commit（07 篇：状态变化必须先真正进入 command list）。
    registry.ClearPendingBarriers();
    registry.CommitExecuted();
    EXPECT_FALSE(registry.IsRecording());
    EXPECT_EQ(registry.CurrentState(kTextureA, 0U), kShaderRead) << "commit 后 committed 已更新";
}

// 失败回滚：pending 被丢弃，committed 不变——下一次 recording 从干净状态开始。
TEST(ResourceStateTrackerTests, RollbackDiscardsPendingWithoutPollutingCommitted)
{
    ResourceStateRegistry registry;
    registry.Register(kTextureA, 1U, kCopyDest);

    registry.BeginRecording();
    static_cast<void>(registry.Transition(kTextureA, kShaderRead));
    EXPECT_EQ(registry.PendingBarriers().size(), 1U);

    registry.Rollback(); // 模拟 Close/Execute 失败
    EXPECT_FALSE(registry.IsRecording());
    EXPECT_EQ(registry.CurrentState(kTextureA, 0U), kCopyDest) << "rollback 后仍是 committed 的旧状态";

    // 下一段 recording 必须从 committed 出发，不能残留上次的 pending。
    registry.BeginRecording();
    EXPECT_EQ(registry.CurrentState(kTextureA, 0U), kCopyDest);
    const std::vector<BarrierRequest> created = registry.Transition(kTextureA, kShaderRead);
    ASSERT_EQ(created.size(), 1U);
    EXPECT_EQ(created.front().before, kCopyDest);
}

// 失败语义：未注册/越界/未 recording/重复 recording/重复注册都要显式失败。
TEST(ResourceStateTrackerTests, RejectsInvalidOperations)
{
    ResourceStateRegistry registry;
    registry.Register(kTextureA, 2U, kCopyDest);

    // 未 BeginRecording 就 Transition。
    EXPECT_THROW(static_cast<void>(registry.Transition(kTextureA, kShaderRead)), std::logic_error);
    // 未注册的 key。
    EXPECT_THROW(static_cast<void>(registry.CurrentState(kTextureB, 0U)), std::out_of_range);

    registry.BeginRecording();
    EXPECT_THROW(registry.BeginRecording(), std::logic_error) << "重复 BeginRecording 必须失败";
    EXPECT_THROW(static_cast<void>(registry.Transition(kTextureB, kShaderRead)), std::out_of_range);
    EXPECT_THROW(static_cast<void>(registry.Transition(kTextureA, kShaderRead, 2U)), std::out_of_range)
        << "subresource 越界必须失败";

    // 仍有未发 barrier 时 commit 必须失败（否则 committed 与 GPU 真实状态分叉）。
    static_cast<void>(registry.Transition(kTextureA, kShaderRead));
    EXPECT_THROW(registry.CommitExecuted(), std::logic_error);

    registry.ClearPendingBarriers();
    registry.CommitExecuted();
    EXPECT_THROW(registry.CommitExecuted(), std::logic_error) << "已结束的 recording 不能再次 commit";
}

// generation：注销后以新 generation 重注册，旧键立即失效（resize/重建的核心判据）。
TEST(ResourceStateTrackerTests, GenerationInvalidatesOldKeys)
{
    ResourceStateRegistry registry;
    const ResourceKey generationZero{500U, 0U};
    registry.Register(generationZero, 1U, kPresent);
    registry.BeginRecording();
    static_cast<void>(registry.Transition(generationZero, kRenderTarget));
    registry.ClearPendingBarriers();
    registry.CommitExecuted();

    registry.Unregister(generationZero);
    EXPECT_THROW(static_cast<void>(registry.CurrentState(generationZero, 0U)), std::out_of_range);

    const ResourceKey generationOne{500U, 1U};
    registry.Register(generationOne, 1U, kPresent);
    EXPECT_EQ(registry.CurrentState(generationOne, 0U), kPresent) << "新 generation 从实际初始状态开始";
    EXPECT_THROW(static_cast<void>(registry.CurrentState(generationZero, 0U)), std::out_of_range)
        << "旧 generation 仍然无效";

    // 同一 (id, generation) 重复注册必须失败（否则状态表会出现两个真值）。
    EXPECT_THROW(registry.Register(generationOne, 1U, kPresent), std::logic_error);
}

// 空 subresource 数非法（资源至少有一个子资源）。
TEST(ResourceStateTrackerTests, RejectsZeroSubresourceRegistration)
{
    ResourceStateRegistry registry;
    EXPECT_THROW(registry.Register(kTextureA, 0U, kCopyDest), std::invalid_argument);
}

// trace hash：同一序列稳定、顺序变化必须改变、BeginRecording 复位。
TEST(ResourceStateTrackerTests, TraceHashIsStableAndOrderSensitive)
{
    ResourceStateRegistry first;
    first.Register(kTextureA, 2U, kCopyDest);
    first.Register(kTextureB, 1U, kRenderTarget);
    first.BeginRecording();
    static_cast<void>(first.Transition(kTextureA, kShaderRead));
    static_cast<void>(first.Transition(kTextureB, kShaderRead));
    const std::uint64_t hashOfSequence = first.TraceHash();

    ResourceStateRegistry second;
    second.Register(kTextureA, 2U, kCopyDest);
    second.Register(kTextureB, 1U, kRenderTarget);
    second.BeginRecording();
    static_cast<void>(second.Transition(kTextureA, kShaderRead));
    static_cast<void>(second.Transition(kTextureB, kShaderRead));
    EXPECT_EQ(second.TraceHash(), hashOfSequence) << "同一 barrier 序列必须得到同一 hash";

    // 顺序交换：先动 B 再动 A → hash 必须不同（trace 是顺序敏感的）。
    ResourceStateRegistry reordered;
    reordered.Register(kTextureA, 2U, kCopyDest);
    reordered.Register(kTextureB, 1U, kRenderTarget);
    reordered.BeginRecording();
    static_cast<void>(reordered.Transition(kTextureB, kShaderRead));
    static_cast<void>(reordered.Transition(kTextureA, kShaderRead));
    EXPECT_NE(reordered.TraceHash(), hashOfSequence);

    // 新 recording 复位：空 recording 的 hash 与"另一段空 recording"一致，
    // 且不等于有条目的 recording（否则复位就没生效）。
    ResourceStateRegistry fresh;
    fresh.Register(kTextureA, 2U, kCopyDest);
    fresh.BeginRecording();
    EXPECT_EQ(fresh.BarrierCount(), 0U);

    ResourceStateRegistry freshPeer;
    freshPeer.Register(kTextureA, 2U, kCopyDest);
    freshPeer.BeginRecording();
    EXPECT_EQ(fresh.TraceHash(), freshPeer.TraceHash()) << "空 recording 的初始 hash 必须一致";
    EXPECT_NE(fresh.TraceHash(), hashOfSequence) << "空 recording 不得与有条目的 recording 同 hash";
}

// 跨"资源地址不同"的稳定性：trace/state hash 必须只反映**逻辑次序**，
// 不能含资源指针——否则两次相同运行会得到不同 hash（本篇设备级取证实测发现的缺陷）。
TEST(ResourceStateTrackerTests, HashesAreIndependentOfResourceAddresses)
{
    // 两个 registry 的布局与操作完全相同，但 id 取值不同（模拟两次运行的地址差异）。
    ResourceStateRegistry first;
    first.Register(ResourceKey{0x1111U, 0U}, 1U, kCopyDest);
    first.Register(ResourceKey{0x2222U, 0U}, 2U, kRenderTarget);
    first.BeginRecording();
    static_cast<void>(first.Transition(ResourceKey{0x1111U, 0U}, kShaderRead));
    static_cast<void>(first.Transition(ResourceKey{0x2222U, 0U}, kShaderRead));
    first.ClearPendingBarriers();
    first.CommitExecuted();

    ResourceStateRegistry second;
    second.Register(ResourceKey{0xAAAAU, 0U}, 1U, kCopyDest);
    second.Register(ResourceKey{0xBBBBU, 0U}, 2U, kRenderTarget);
    second.BeginRecording();
    static_cast<void>(second.Transition(ResourceKey{0xAAAAU, 0U}, kShaderRead));
    static_cast<void>(second.Transition(ResourceKey{0xBBBBU, 0U}, kShaderRead));

    EXPECT_EQ(second.TraceHash(), first.TraceHash()) << "trace hash 必须与资源地址无关";
    second.ClearPendingBarriers();
    second.CommitExecuted();
    EXPECT_EQ(second.CommittedStateHash(), first.CommittedStateHash()) << "state hash 同样必须与地址无关";
}

// committed 状态集合的 hash：跨实例稳定，随状态变化而变（用于跨 run 比较）。
TEST(ResourceStateTrackerTests, CommittedStateHashReflectsStates)
{
    ResourceStateRegistry first;
    first.Register(kTextureA, 1U, kCopyDest);
    first.Register(kTextureB, 1U, kPresent);
    const std::uint64_t initialHash = first.CommittedStateHash();

    ResourceStateRegistry second;
    second.Register(kTextureA, 1U, kCopyDest);
    second.Register(kTextureB, 1U, kPresent);
    EXPECT_EQ(second.CommittedStateHash(), initialHash) << "相同注册必须得到相同状态集合 hash";

    second.BeginRecording();
    static_cast<void>(second.Transition(kTextureA, kShaderRead));
    second.ClearPendingBarriers();
    EXPECT_EQ(second.CommittedStateHash(), initialHash) << "未 commit 前 committed hash 不变";
    second.CommitExecuted();
    EXPECT_NE(second.CommittedStateHash(), initialHash) << "commit 后必须变化";
}
