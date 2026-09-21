// ============================================================================
// D3D12ResourceStateTrackerDeviceTests.cpp — 真 GPU 上的 barrier 生成与状态安全
// 里程碑：M5（07 篇 Resource Barrier 与状态跟踪）
// 职责：在真实设备上验证设备侧 tracker：注册资源（实际初始状态）→ 同一 recording
//       内 transition + 批量 flush → Execute → commit；子资源粒度只影响目标 mip；
//       rollback 之后仍能正确重放（证明没有状态污染）；注销后旧 generation 失效；
//       全程 Debug Layer 零 state mismatch。
// 纯状态机规则（before==after、pending/commit、trace hash）由 ResourceStateTrackerTests
// 在同一份 registry 实现上穷举。
// 环境：需要 D3D12 硬件或 WARP。
// 关联：docs/architecture/README.md（验证与负向 fixture）
// ============================================================================
#include "D3D12Queue.h"
#include "D3D12ResourceStateTracker.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

using MiniEngine::Rhi::D3D12::D3D12Queue;
using MiniEngine::Rhi::D3D12::D3D12ResourceStateTracker;
using MiniEngine::Rhi::D3D12::ResourceKey;

namespace
{
struct TrackerHarness final
{
    std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device;
    D3D12Queue queue;
    D3D12ResourceStateTracker tracker;

    TrackerHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = MiniEngine::Rhi::D3D12::D3D12Device::Create(options);
        queue.Initialize(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()));
    }

    [[nodiscard]] ID3D12Device* NativeDevice() const
    {
        return static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    }

    // 创建一张 RGBA 纹理（DEFAULT，初始 COPY_DEST）。
    // 尺寸与 mip 数必须自相一致：1×1 只允许 1 个 mip，否则调试层会报非法 mip 描述
    // （子资源用例因此用 2×2 + 2 mip）。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateTexture(const std::uint32_t width,
                                                                       const std::uint32_t height,
                                                                       const std::uint32_t mipLevels) const
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1U;
        description.MipLevels = static_cast<UINT16>(mipLevels);
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1U;

        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        EXPECT_TRUE(SUCCEEDED(NativeDevice()->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
                                                                      &description, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                      nullptr, IID_PPV_ARGS(&texture))));
        return texture;
    }

    // 单 mip 便捷入口（绝大多数用例只需要一张能转换状态的纹理）。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateTexture() const
    {
        return CreateTexture(1U, 1U, 1U);
    }
};
} // namespace

// 正向：注册 → 同一 recording 内 transition → flush → Execute → commit，
// 全程零调试层消息；commit 后状态即为新状态。
TEST(D3D12ResourceStateTrackerDeviceTests, TransitionsWithinOneRecordingCommitCleanly)
{
    TrackerHarness harness;
    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = harness.CreateTexture();
    ASSERT_NE(texture.Get(), nullptr);

    const ResourceKey key =
        harness.tracker.Register(*texture.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.Texture");
    EXPECT_EQ(harness.tracker.CurrentState(key), D3D12_RESOURCE_STATE_COPY_DEST);
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), 1U);

    harness.tracker.BeginRecording();
    harness.tracker.Transition(key, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EXPECT_EQ(harness.tracker.PendingBarrierCount(), 1U);

    MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
    const std::uint32_t flushed = harness.tracker.FlushBarriersTo(harness.queue.CommandList());
    EXPECT_EQ(flushed, 1U) << "一条 transition 应生成恰好一条 barrier";
    EXPECT_EQ(harness.tracker.PendingBarrierCount(), 0U);

    static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    harness.tracker.CommitExecuted();

    EXPECT_EQ(harness.tracker.CurrentState(key), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EXPECT_EQ(harness.tracker.BarrierCount(), 1U);
    harness.queue.FlushGpu("tracker-test");
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "合法状态转换不得产生消息";
}

// 子资源粒度：只转 mip 1 时，实际发出的 barrier 只覆盖 mip 1。
TEST(D3D12ResourceStateTrackerDeviceTests, SubresourceTransitionOnlyTouchesThatMip)
{
    TrackerHarness harness;
    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = harness.CreateTexture(2U, 2U, 2U);
    ASSERT_NE(texture.Get(), nullptr);

    const ResourceKey key =
        harness.tracker.Register(*texture.Get(), 2U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.TwoMip");

    harness.tracker.BeginRecording();
    harness.tracker.Transition(key, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1U);
    MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
    EXPECT_EQ(harness.tracker.FlushBarriersTo(harness.queue.CommandList()), 1U);
    static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    harness.tracker.CommitExecuted();

    EXPECT_EQ(harness.tracker.CurrentState(key, 1U), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EXPECT_EQ(harness.tracker.CurrentState(key, 0U), D3D12_RESOURCE_STATE_COPY_DEST) << "mip 0 不受影响";

    harness.queue.FlushGpu("tracker-test");
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 失败回滚后的重放：Rollback 丢弃 pending，下一次 recording 仍从 committed 出发。
TEST(D3D12ResourceStateTrackerDeviceTests, RollbackKeepsCommittedStateAndAllowsReplay)
{
    TrackerHarness harness;
    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = harness.CreateTexture();
    const ResourceKey key =
        harness.tracker.Register(*texture.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.Rollback");

    // 第一段 recording：登记转换但**不**提交（模拟 Close/Execute 失败）。
    harness.tracker.BeginRecording();
    harness.tracker.Transition(key, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    harness.tracker.Rollback();
    EXPECT_EQ(harness.tracker.CurrentState(key), D3D12_RESOURCE_STATE_COPY_DEST);

    // 第二段 recording：重放同一条转换，必须仍然从 COPY_DEST 出发。
    harness.tracker.BeginRecording();
    harness.tracker.Transition(key, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
    EXPECT_EQ(harness.tracker.FlushBarriersTo(harness.queue.CommandList()), 1U);
    static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    harness.tracker.CommitExecuted();

    harness.queue.FlushGpu("tracker-test");
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "回滚后重放不得产生 state mismatch";
}

// 注销与 generation：注销后旧键失效；同 id 新 generation 从实际初始状态开始。
TEST(D3D12ResourceStateTrackerDeviceTests, UnregisterInvalidatesKeyAndAllowsNewGeneration)
{
    TrackerHarness harness;
    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = harness.CreateTexture();

    const ResourceKey generationZero =
        harness.tracker.Register(*texture.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.Gen0", 0U);
    harness.tracker.Unregister(generationZero);
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), 0U);
    EXPECT_THROW(static_cast<void>(harness.tracker.CurrentState(generationZero)), std::out_of_range);

    const ResourceKey generationOne =
        harness.tracker.Register(*texture.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.Gen1", 1U);
    EXPECT_EQ(harness.tracker.CurrentState(generationOne), D3D12_RESOURCE_STATE_COPY_DEST);

    // 同一 id 的旧 generation 与新 generation 不允许并存（避免两个真值）。
    EXPECT_THROW(harness.tracker.Register(*texture.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.Gen2", 2U),
                 std::logic_error);

    harness.tracker.Unregister(generationOne);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 断言辅助：状态不符时必须抛出带资源名的 logic_error（Present 前断言即用它）。
TEST(D3D12ResourceStateTrackerDeviceTests, VerifyStateReportsMismatchWithResourceName)
{
    TrackerHarness harness;
    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = harness.CreateTexture();
    const ResourceKey key =
        harness.tracker.Register(*texture.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST, L"M5.Test.Verify");

    EXPECT_NO_THROW(harness.tracker.VerifyState(key, D3D12_RESOURCE_STATE_COPY_DEST));
    try
    {
        harness.tracker.VerifyState(key, D3D12_RESOURCE_STATE_PRESENT);
        FAIL() << "状态不符必须抛出";
    }
    catch (const std::logic_error& error)
    {
        const std::string message{error.what()};
        EXPECT_NE(message.find("M5.Test.Verify"), std::string::npos) << "错误必须带资源名：" << message;
    }
}
