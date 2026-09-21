#include "RenderGraphTestFixtures.h"
#include "TraceRhi.h"
#include <MiniEngine/RenderGraph/TransientResourcePool.h>
#include <array>
#include <gtest/gtest.h>
#include <map>

using namespace MiniEngine::Rhi;
using namespace MiniEngine::RenderGraph;
using namespace MiniEngine::RenderGraph::Tests;
using MiniEngine::Tests::TraceRhiDevice;
namespace
{
RgTexture ImportBack(RenderGraph& graph, TraceRhiDevice& device, const FrameToken& frame)
{
    const auto snapshot = device.QueryTextureState(frame, frame.backBuffer);
    return graph.ImportTexture("BackBuffer", {frame.backBuffer,
                                              snapshot.descriptor,
                                              snapshot.access,
                                              ResourceAccess::Present,
                                              ContentState::Undefined,
                                              "swapchain",
                                              {}});
}
void AddKeptClear(RenderGraph& graph, RgTexture texture, TextureHandle& physical)
{
    graph.AddPass<TexturePassData>(
        "transient-clear-" + std::to_string(texture.Resource()),
        [&](RgBuilder& builder, TexturePassData& data)
        {
            data.texture = builder.Write(texture, ResourceAccess::ColorWrite);
            builder.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store);
            builder.SideEffect("pool lifetime observation");
        },
        [&physical](const TexturePassData& data, const RgResources& resources, IRhiCommandList&)
        { physical = resources.Get(data.texture); });
}
std::array<TextureHandle, 2> RunPoolFrame(TraceRhiDevice& device, SwapChainHandle chain, TransientResourcePool& pool,
                                          const TextureDesc& desc, FrameToken* token = nullptr)
{
    const auto frame = device.BeginFrame(chain);
    if (token)
        *token = frame;
    RenderGraph graph;
    auto back = ImportBack(graph, device, frame);
    std::array<TextureHandle, 2> handles{};
    AddKeptClear(graph, graph.CreateTexture("A", desc), handles[0]);
    AddKeptClear(graph, graph.CreateTexture("B", desc), handles[1]);
    back = AddAttachmentPass(graph, "back-clear", back);
    graph.Present(back);
    auto plan = graph.Compile();
    EXPECT_EQ(plan.Statistics().physicalTransients, 1U);
    EXPECT_EQ(plan.Statistics().physicalResources, 2U);
    plan.Execute(device, frame, pool);
    EXPECT_EQ(handles[0], handles[1]);
    device.EndFrame(frame, chain);
    return handles;
}
// M6-11：截图读回场景；readbackSize 由调用方给定，用于验证 Full coverage 的尺寸契约。
GraphExecutionResult RunReadbackFrame(TraceRhiDevice& device, SwapChainHandle chain, TransientResourcePool& pool,
                                      Extent2D extent, std::uint64_t readbackSize)
{
    TextureDesc sourceDesc;
    sourceDesc.extent = extent;
    sourceDesc.format = Format::Rgba8Unorm;
    sourceDesc.usage = TextureUsage::Sampled | TextureUsage::CopySource | TextureUsage::CopyDestination;
    sourceDesc.debugName = "readback source";
    const auto source = device.CreateTexture(sourceDesc);
    const std::vector<std::byte> pixels(static_cast<std::size_t>(extent.width) * extent.height * 4);
    const TextureSubresourceData upload{pixels, static_cast<std::uint64_t>(extent.width) * 4, pixels.size()};
    device.UploadTexture(source, std::span(&upload, 1));
    const auto readback = device.CreateBuffer(
        {readbackSize, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "ScreenshotReadback"}, {});
    const auto frame = device.BeginFrame(chain);
    RenderGraph graph;
    auto back = ImportBack(graph, device, frame);
    const auto sourceState = device.QueryTextureState(frame, source);
    const auto importedSource =
        graph.ImportTexture("source", {source, sourceState.descriptor, sourceState.access, ResourceAccess::CopySource,
                                       ContentState::Defined, "readback owner", "upload"});
    const auto readbackState = device.QueryBufferState(frame, readback);
    const auto importedReadback = graph.ImportBuffer(
        "readback", {readback, readbackState.descriptor, readbackState.access, ResourceAccess::CopyDestination,
                     ContentState::Undefined, "readback owner", "allocate"});
    struct CopyData
    {
        RgTexture source;
        RgBuffer readback;
    };
    RgBuffer exported;
    graph.AddPass<CopyData>(
        "screenshot",
        [&](RgBuilder& builder, CopyData& pass)
        {
            pass.source = builder.Read(importedSource, ResourceAccess::CopySource);
            pass.readback = builder.Write(importedReadback, ResourceAccess::CopyDestination, WriteCoverage::Full);
            exported = pass.readback;
            builder.SideEffect("requested screenshot");
        },
        [extent](const CopyData& pass, const RgResources& resources, IRhiCommandList& commands)
        { commands.CopyTextureForReadback(resources.Get(pass.source), resources.Get(pass.readback), extent); });
    graph.Export(exported);
    graph.Present(AddAttachmentPass(graph, "back", back));
    auto plan = graph.Compile();
    return plan.TryExecute(device, frame, pool);
}

TEST(TransientPool, ThreeLanesReuseOnlyAfterBeginFrameCompletionAndReachStableHighWater)
{
    TraceRhiDevice device;
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    const auto chain = device.CreateSwapChain({{32, 24}});
    const auto initialIdleCount = device.WaitIdleCount();
    TransientResourcePool pool(device);
    std::map<std::uint32_t, TextureHandle> laneHandles;
    std::uint64_t highWater = 0;
    for (unsigned i = 0; i < 180; ++i)
    {
        FrameToken frame;
        const auto handles = RunPoolFrame(device, chain, pool, MakeColorDesc(), &frame);
        if (i < 3)
        {
            for (const auto& [lane, handle] : laneHandles)
            {
                (void)lane;
                EXPECT_NE(handle, handles[0]);
            }
            laneHandles.emplace(frame.recycleLane, handles[0]);
            highWater = pool.Statistics().highWaterBytes;
        }
        else
        {
            EXPECT_EQ(handles[0], laneHandles.at(frame.recycleLane));
            EXPECT_GE(device.Diagnostics().completedSerial, frame.serial - 3);
            EXPECT_EQ(pool.Statistics().created, 3U);
            EXPECT_EQ(pool.Statistics().highWaterBytes, highWater);
        }
    }
    ASSERT_EQ(laneHandles.size(), 3U);
    const auto stats = pool.Statistics();
    EXPECT_EQ(stats.created, 3U);
    EXPECT_EQ(stats.reused, 177U);
    EXPECT_EQ(stats.retired, 0U);
    EXPECT_EQ(stats.resources, 3U);
    EXPECT_EQ(stats.highWaterResources, 3U);
    EXPECT_EQ(stats.bytes, 3U * 32 * 24 * 4);
    EXPECT_FALSE(device.Completion().waits.empty());
    EXPECT_EQ(device.WaitIdleCount(), initialIdleCount);
    pool.Clear();
    EXPECT_EQ(pool.Statistics().retired, 3U);
    EXPECT_EQ(pool.Statistics().bytes, 0U);
    EXPECT_GT(device.Diagnostics().retiringObjects, 0U);
    device.WaitIdle();
    device.Shutdown();
    EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
}
TEST(TransientPool, ResizeFormatAndFullUsageChangesRetireObsoleteLaneObjects)
{
    TraceRhiDevice device;
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    const auto chain = device.CreateSwapChain({{32, 24}});
    TransientResourcePool pool(device);
    auto desc = MakeColorDesc();
    for (unsigned phase = 0; phase < 4; ++phase)
    {
        if (phase == 1)
        {
            desc.extent = {64, 48};
            device.ResizeSwapChain(chain, {64, 48});
        }
        if (phase == 2)
            desc.format = Format::Rgba16Float;
        if (phase == 3)
            desc.usage = TextureUsage::ColorAttachment;
        for (unsigned i = 0; i < 3; ++i)
            RunPoolFrame(device, chain, pool, desc);
        EXPECT_EQ(pool.Statistics().created, 3U * (phase + 1));
        EXPECT_EQ(pool.Statistics().retired, 3U * phase);
        EXPECT_EQ(pool.Statistics().resources, 3U);
    }
    const auto highWater = pool.Statistics().highWaterBytes;
    for (unsigned i = 0; i < 30; ++i)
        RunPoolFrame(device, chain, pool, desc);
    EXPECT_EQ(pool.Statistics().created, 12U);
    EXPECT_EQ(pool.Statistics().highWaterBytes, highWater);
    EXPECT_EQ(pool.Statistics().bytes, 3U * 64 * 48 * 8);
    pool.Clear();
    device.Shutdown();
}
TEST(TransientPool, ForeignDeviceFailsBeforeRecording)
{
    TraceRhiDevice first, second;
    TransientResourcePool pool(first);
    const auto chain = second.CreateSwapChain({{32, 24}});
    const auto frame = second.BeginFrame(chain);
    RenderGraph graph;
    auto back = ImportBack(graph, second, frame);
    back = AddAttachmentPass(graph, "back", back);
    graph.Present(back);
    auto plan = graph.Compile();
    const auto result = plan.TryExecute(second, frame, pool);
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.workRecorded);
    EXPECT_EQ(pool.Statistics().created, 0U);
    EXPECT_THROW((void)second.GraphCommandSink(frame), RhiException);
    first.Shutdown();
    // 负例保留未提交 frame，由测试替身析构丢弃；不能提交残缺图。
}
TEST(TransientPool, OwnerSwitchCannotInheritDefinedBufferToFakeAFullWrite)
{
    TraceRhiDevice device;
    TransientResourcePool pool(device);
    const auto chain = device.CreateSwapChain({{32, 24}});
    const auto desc = MakeBufferDesc();
    const std::array<std::byte, 32> data{};
    const auto source = device.CreateBuffer(desc, data);
    const auto frame = device.BeginFrame(chain);
    RenderGraph graph;
    auto back = ImportBack(graph, device, frame);
    const auto snapshot = device.QueryBufferState(frame, source);
    const auto imported = graph.ImportBuffer("source", {source, desc, snapshot.access, ResourceAccess::CopySource,
                                                        ContentState::Defined, "test", "CreateBuffer initialData"});
    const auto a = graph.CreateBuffer("A", desc);
    const auto b = graph.CreateBuffer("B", desc);
    struct CopyData
    {
        RgBuffer source, destination;
    };
    graph.AddPass<CopyData>(
        "real-copy",
        [&](RgBuilder& builder, CopyData& pass)
        {
            pass.source = builder.Read(imported, ResourceAccess::CopySource);
            pass.destination = builder.Write(a, ResourceAccess::CopyDestination, WriteCoverage::Full);
            builder.SideEffect("first owner must have defined bytes");
        },
        [](const CopyData& pass, const RgResources& resources, IRhiCommandList& commands)
        { commands.CopyBuffer({resources.Get(pass.source), 0, 32}, {resources.Get(pass.destination), 0, 32}, 32); });
    graph.AddPass<BufferPassData>(
        "missing-copy",
        [&](RgBuilder& builder, BufferPassData& pass)
        {
            pass.buffer = builder.Write(b, ResourceAccess::CopyDestination, WriteCoverage::Full);
            builder.SideEffect("negative: deliberately omit actual write");
        },
        [](const BufferPassData&, const RgResources&, IRhiCommandList&) {});
    graph.Present(AddAttachmentPass(graph, "back", back));
    auto plan = graph.Compile();
    ASSERT_EQ(plan.Statistics().physicalTransients, 1U);
    const auto result = plan.TryExecute(device, frame, pool);
    ASSERT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::UndefinedContent);
    EXPECT_EQ(result.error->passName, "missing-copy");
    EXPECT_FALSE(result.presentReady);
    EXPECT_EQ(pool.Statistics().resources, 0U);
    EXPECT_EQ(pool.Statistics().retired, 1U);
    EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, 0U);
}
TEST(TransientPool, RepeatedLeaseInTheSameFrameIsRejectedBeforeAnyAllocation)
{
    // M6-11 负向通道：同一 frame 内第二次 lease = 提前复用 transient lane；
    // 必须在任何分配/录制前被拒绝，且不产生新的 transient 计数。
    TraceRhiDevice device;
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    const auto chain = device.CreateSwapChain({{32, 24}});
    TransientResourcePool pool(device);
    const auto desc = MakeColorDesc();
    for (int lane = 0; lane < 3; ++lane)
        RunPoolFrame(device, chain, pool, desc);
    ASSERT_EQ(pool.Statistics().created, 3U);
    const auto frame = device.BeginFrame(chain);
    std::array<TextureHandle, 4> handles{};
    const auto build = [&](RenderGraph& graph, std::size_t offset)
    {
        auto back = ImportBack(graph, device, frame);
        AddKeptClear(graph, graph.CreateTexture("A", desc), handles[offset]);
        AddKeptClear(graph, graph.CreateTexture("B", desc), handles[offset + 1]);
        graph.Present(AddAttachmentPass(graph, "back", back));
        return graph.Compile();
    };
    RenderGraph firstGraph;
    auto first = build(firstGraph, 0);
    first.Execute(device, frame, pool);
    EXPECT_EQ(pool.Statistics().created, 3U);
    EXPECT_EQ(pool.Statistics().reused, 1U);
    RenderGraph secondGraph;
    auto second = build(secondGraph, 2);
    const auto result = second.TryExecute(device, frame, pool);
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::PhaseViolation);
    EXPECT_FALSE(result.workRecorded);
    EXPECT_EQ(pool.Statistics().created, 3U);
    EXPECT_EQ(pool.Statistics().reused, 1U);
    // 失败帧不提交；测试替身析构时丢弃未结束 frame，与 ForeignDeviceFailsBeforeRecording 相同。
}
TEST(TransientPool, FullCoverageReadbackRejectsPaddedBuffer)
{
    // M6-11 回归：图把截图缓冲声明为 Full coverage，但 RHI 只标记紧凑像素字节
    // (width*height*4)。缓冲按 256B 行对齐补大时必须被执行期定义性校验拒绝，
    // 这正是外部 resize / 非 64 像素对齐宽度暴露的缺陷。
    TraceRhiDevice device;
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    const Extent2D extent{40, 2};
    const auto chain = device.CreateSwapChain({extent, Format::Rgba8Unorm, 3, true, "readback chain"});
    TransientResourcePool pool(device);
    const auto result = RunReadbackFrame(device, chain, pool, extent, 256 * extent.height);
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::UndefinedContent);
}
TEST(TransientPool, FullCoverageReadbackAcceptsTightBuffer)
{
    TraceRhiDevice device;
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    const Extent2D extent{40, 2};
    const auto chain = device.CreateSwapChain({extent, Format::Rgba8Unorm, 3, true, "readback chain"});
    TransientResourcePool pool(device);
    const auto result =
        RunReadbackFrame(device, chain, pool, extent, static_cast<std::uint64_t>(extent.width) * extent.height * 4);
    EXPECT_TRUE(result);
    EXPECT_TRUE(result.presentReady);
}
TEST(TransientPool, NoTransientGraphEvictsUnusedEntriesOnItsSafeLane)
{
    TraceRhiDevice device;
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    const auto chain = device.CreateSwapChain({{32, 24}});
    TransientResourcePool pool(device);
    for (unsigned i = 0; i < 3; ++i)
        RunPoolFrame(device, chain, pool, MakeColorDesc());
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto frame = device.BeginFrame(chain);
        RenderGraph graph;
        graph.Present(AddAttachmentPass(graph, "back", ImportBack(graph, device, frame)));
        auto plan = graph.Compile();
        plan.Execute(device, frame, pool);
        device.EndFrame(frame, chain);
    }
    EXPECT_EQ(pool.Statistics().created, 3U);
    EXPECT_EQ(pool.Statistics().retired, 3U);
    EXPECT_EQ(pool.Statistics().resources, 0U);
    device.Shutdown();
}
} // namespace
