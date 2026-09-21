#include "TraceFixture.h"
#include <filesystem>
#include <fstream>
#include <limits>
using namespace MiniEngine::Rhi;
using namespace MiniEngine::Tests;
namespace
{
void ClearFrame(TraceRhiDevice& device, SwapChainHandle chain)
{
    auto frame = device.BeginFrame(chain);
    auto& commands = device.BeginGraphics(frame);
    (void)device.Import(frame, frame.backBuffer);
    auto found = device.ResourceStates().find(frame.backBuffer);
    const AccessTransition begin{frame.backBuffer,
                                 {},
                                 found == device.ResourceStates().end() ? ResourceAccess::None : found->second.access,
                                 ResourceAccess::ColorWrite};
    device.GraphCommandSink(frame).ApplyTransitions(std::span(&begin, 1));
    const auto extent = device.Lifetime().Describe(frame.backBuffer).extent;
    const ColorAttachment color{frame.backBuffer, LoadOp::Clear, StoreOp::Store, {0.1F, 0.2F, 0.3F, 1}};
    commands.BeginRendering({std::span(&color, 1), nullptr, extent});
    commands.EndRendering();
    const AccessTransition present{frame.backBuffer, {}, ResourceAccess::ColorWrite, ResourceAccess::Present};
    device.GraphCommandSink(frame).ApplyTransitions(std::span(&present, 1));
    device.EndGraphics(frame, commands);
    device.EndFrame(frame, chain);
}
TEST(TraceFrame, RejectsNestedFramesForgedTokensAndWrongChain)
{
    TriangleFixture fixture;
    auto otherChain = fixture.device.CreateSwapChain({{16, 16}});
    fixture.Start();
    EXPECT_THROW(fixture.device.BeginFrame(fixture.chain), RhiValidationError);
    for (int field = 0; field < 4; ++field)
    {
        auto forged = fixture.frame;
        if (field == 0)
            ++forged.serial;
        if (field == 1)
            ++forged.owner;
        if (field == 2)
            ++forged.recycleLane;
        if (field == 3)
            forged.backBuffer = {};
        EXPECT_THROW(fixture.device.AcquireBackBuffer(forged), RhiValidationError);
        EXPECT_THROW(fixture.device.EndGraphics(forged, *fixture.commands), RhiValidationError);
    }
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Transition(fixture.frame.backBuffer, ResourceAccess::Present);
    fixture.device.EndGraphics(fixture.frame, *fixture.commands);
    EXPECT_THROW(fixture.device.EndFrame(fixture.frame, otherChain), RhiValidationError);
    EXPECT_NO_THROW(fixture.device.EndFrame(fixture.frame, fixture.chain));
}
TEST(TraceFrame, GraphicsOpensClosesOnceAndSubmissionDoesNotCloseAgain)
{
    TriangleFixture fixture;
    auto frame = fixture.device.BeginFrame(fixture.chain);
    EXPECT_THROW(fixture.device.EndFrame(frame, fixture.chain), RhiValidationError);
    EXPECT_THROW(fixture.device.GraphCommandSink(frame), RhiValidationError);
    fixture.frame = frame;
    fixture.commands = &fixture.device.BeginGraphics(frame);
    EXPECT_THROW(fixture.device.BeginGraphics(frame), RhiValidationError);
    EXPECT_THROW(fixture.device.EndFrame(frame, fixture.chain), RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Transition(frame.backBuffer, ResourceAccess::Present);
    fixture.device.EndGraphics(frame, *fixture.commands);
    const auto events = fixture.device.Events().size();
    EXPECT_THROW(fixture.device.EndGraphics(frame, *fixture.commands), RhiValidationError);
    EXPECT_THROW(fixture.commands->BeginLabel("after close"), RhiValidationError);
    EXPECT_THROW(fixture.device.BeginGraphics(frame), RhiValidationError);
    EXPECT_NO_THROW(fixture.device.EndFrame(frame, fixture.chain));
    EXPECT_EQ(fixture.device.Events().size(), events + 1);
    EXPECT_THROW(fixture.device.EndFrame(frame, fixture.chain), RhiValidationError);
}
TEST(TraceFrame, OldCommandAndSinkReferencesCannotRecordIntoNextFrame)
{
    TriangleFixture fixture;
    fixture.Start();
    auto* old = fixture.commands;
    auto& sink = fixture.device.GraphCommandSink(fixture.frame);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.Start();
    const auto count = fixture.device.Events().size();
    EXPECT_THROW(old->SetPipeline(fixture.pipeline), RhiValidationError);
    EXPECT_THROW(sink.ApplyTransitions({}), RhiValidationError);
    EXPECT_THROW(fixture.device.EndGraphics(fixture.frame, *old), RhiValidationError);
    EXPECT_EQ(fixture.device.Events().size(), count);
}
TEST(TraceFrame, ForeignDeviceCommandAndTokenAreRejected)
{
    TriangleFixture left, right;
    left.Start();
    right.Start();
    EXPECT_THROW(left.device.BeginGraphics(right.frame), RhiValidationError);
    EXPECT_THROW(left.device.EndGraphics(left.frame, *right.commands), RhiValidationError);
    EXPECT_THROW(left.commands->SetPipeline(right.pipeline), RhiValidationError);
}
TEST(TraceFrame, CompletionIndependentOfEndFrameWaitsOnlyRequiredLane)
{
    TraceRhiDevice device;
    DriveCompletion(device);
    auto chain = device.CreateSwapChain({{16, 16}});
    const auto idleBefore = device.WaitIdleCount();
    for (int i = 0; i < 3; ++i)
        ClearFrame(device, chain);
    EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, 3U);
    EXPECT_EQ(device.Diagnostics().completedSerial, 0U);
    EXPECT_TRUE(device.Completion().waits.empty());
    device.Completion().onWait = [](std::uint64_t required) { return required - 1; };
    EXPECT_THROW(device.BeginFrame(chain), RhiValidationError);
    EXPECT_EQ(device.Diagnostics().activeFrameSerial, 0U);
    EXPECT_EQ(device.Diagnostics().completedSerial, 0U);
    ASSERT_EQ(device.Completion().waits.size(), 1U);
    EXPECT_EQ(device.Completion().waits.back(), 1U);
    device.Completion().onWait = [](std::uint64_t required) { return required; };
    ClearFrame(device, chain);
    EXPECT_EQ(device.Diagnostics().completedSerial, 1U);
    EXPECT_EQ(device.WaitIdleCount(), idleBefore);
    EXPECT_THROW(device.CompleteThrough(0), RhiValidationError);
    EXPECT_THROW(device.CompleteThrough(5), RhiValidationError);
    device.Shutdown();
}
TEST(TraceFrame, TwoBufferChainWaitsBeforeReusingBackBuffer)
{
    TraceRhiDevice device;
    DriveCompletion(device);
    auto chain = device.CreateSwapChain({{16, 16}, Format::Rgba8Unorm, 2});
    ClearFrame(device, chain);
    ClearFrame(device, chain);
    EXPECT_TRUE(device.Completion().waits.empty());
    ClearFrame(device, chain);
    ASSERT_EQ(device.Completion().waits.size(), 1U);
    EXPECT_EQ(device.Completion().waits[0], 1U);
    EXPECT_EQ(device.Diagnostics().completedSerial, 1U);
    device.Shutdown();
}
TEST(TraceFrame, ImmediateAndRecordedProfilesConsumeAtDifferentTimesWithSameTrace)
{
    TriangleFixture immediate(RhiBackend::D3D11), recorded(RhiBackend::D3D12);
    immediate.Start();
    recorded.Start();
    immediate.Render();
    recorded.Render();
    EXPECT_GT(immediate.device.ConsumedCommands(), 0U);
    EXPECT_EQ(recorded.device.ConsumedCommands(), 0U);
    immediate.commands->EndRendering();
    recorded.commands->EndRendering();
    immediate.Finish();
    recorded.Finish();
    EXPECT_EQ(immediate.device.ConsumedCommands(), recorded.device.ConsumedCommands());
    EXPECT_EQ(immediate.device.CanonicalTrace(), recorded.device.CanonicalTrace());
    immediate.device.Shutdown();
    recorded.device.Shutdown();
}
TEST(TraceFrame, ResizeRejectsOldFrameBackBufferAndGraphImport)
{
    TriangleFixture fixture;
    fixture.Start();
    const auto oldFrame = fixture.frame;
    auto import = fixture.device.Import(oldFrame, oldFrame.backBuffer);
    EXPECT_EQ(std::get<TextureHandle>(fixture.device.Resolve(import)), oldFrame.backBuffer);
    EXPECT_THROW(fixture.device.Destroy(oldFrame.backBuffer), RhiValidationError);
    EXPECT_THROW(fixture.device.ResizeSwapChain(fixture.chain, {32, 32}), RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.ResizeSwapChain(fixture.chain, {32, 32});
    EXPECT_THROW(fixture.device.Lifetime().ValidateAlive(oldFrame.backBuffer), RhiValidationError);
    auto current = fixture.device.BeginFrame(fixture.chain);
    EXPECT_THROW(fixture.device.AcquireBackBuffer(oldFrame), RhiValidationError);
    EXPECT_THROW(fixture.device.Resolve(import), RhiValidationError);
    EXPECT_NE(fixture.device.AcquireBackBuffer(current), oldFrame.backBuffer);
}
TEST(TraceFrame, ImportIsFrameLocalAndCannotBeForgedOrShared)
{
    TriangleFixture fixture, foreign;
    fixture.Start();
    foreign.Start();
    auto import = fixture.device.Import(fixture.frame, fixture.readback);
    EXPECT_EQ(std::get<BufferHandle>(fixture.device.Resolve(import)), fixture.readback);
    EXPECT_THROW(foreign.device.Resolve(import), RhiValidationError);
    EXPECT_THROW(fixture.device.Resolve({import.owner, std::numeric_limits<std::size_t>::max()}), RhiValidationError);
    EXPECT_THROW(fixture.device.Import(fixture.frame, fixture.pipeline), RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.Start();
    EXPECT_THROW(fixture.device.Resolve(import), RhiValidationError);
}
TEST(TraceFrame, ZeroExtentSuspendsWithoutFrameOrZeroSizedResources)
{
    TraceRhiDevice device;
    DriveCompletion(device);
    auto chain = device.CreateSwapChain({{16, 16}});
    ClearFrame(device, chain);
    const auto old = device.Diagnostics();
    device.ResizeSwapChain(chain, {0, 0});
    auto suspended = device.BeginFrame(chain);
    EXPECT_EQ(suspended.serial, 0U);
    EXPECT_EQ(device.Diagnostics().aliveObjects, old.aliveObjects);
    EXPECT_THROW(device.BeginGraphics(suspended), RhiValidationError);
    EXPECT_THROW(device.AcquireBackBuffer(suspended), RhiValidationError);
    EXPECT_THROW(device.EndFrame(suspended, chain), RhiValidationError);
    device.ResizeSwapChain(chain, {32, 16});
    EXPECT_NO_THROW(ClearFrame(device, chain));
    device.Shutdown();
}
TEST(TraceFrame, ResizeMinimizeRestoreAndBackgroundPauseStress)
{
    TraceRhiDevice device;
    DriveCompletion(device);
    auto chain = device.CreateSwapChain({{16, 16}});
    for (std::uint32_t i = 0; i < 100; ++i)
    {
        ClearFrame(device, chain);
        // 后台暂停不伪造 BeginFrame；恢复再继续，同样不改变提交号。
        const auto submitted = device.Diagnostics().lastSubmittedSerial;
        device.ResizeSwapChain(chain, {0, 0});
        EXPECT_EQ(device.BeginFrame(chain).serial, 0U);
        EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, submitted);
        device.ResizeSwapChain(chain, {16 + i % 7, 16 + i % 5});
        EXPECT_LE(device.Diagnostics().registrySlots, 4U);
    }
    device.Shutdown();
    EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
}
TEST(TraceFrame, WaitIdleAndShutdownCannotSubmitActiveRecording)
{
    TriangleFixture fixture;
    fixture.Start();
    EXPECT_THROW(fixture.device.WaitIdle(), RhiValidationError);
    EXPECT_THROW(fixture.device.Shutdown(), RhiValidationError);
    EXPECT_THROW(fixture.device.Destroy(fixture.chain), RhiValidationError);
}
TEST(TraceFrame, ContentsPersistAcrossReuseAndLoadCanUseStoredClear)
{
    TraceRhiDevice device;
    DriveCompletion(device);
    auto chain = device.CreateSwapChain({{16, 16}});
    for (int i = 0; i < 3; ++i)
        ClearFrame(device, chain);
    auto frame = device.BeginFrame(chain);
    auto& commands = device.BeginGraphics(frame);
    (void)device.Import(frame, frame.backBuffer);
    const AccessTransition transition{frame.backBuffer, {}, ResourceAccess::Present, ResourceAccess::ColorWrite};
    device.GraphCommandSink(frame).ApplyTransitions(std::span(&transition, 1));
    const ColorAttachment color{frame.backBuffer, LoadOp::Load, StoreOp::Store};
    EXPECT_NO_THROW(commands.BeginRendering({std::span(&color, 1), nullptr, {16, 16}}));
}
TEST(TraceResults, PendingThenUnavailableWithoutInventingGpuPixelsOrTicks)
{
    TriangleFixture fixture;
    fixture.FixedTriangle();
    EXPECT_FALSE(fixture.device.TryReadTimestamp(fixture.beginQuery));
    EXPECT_FALSE(fixture.device.TryReadTextureReadback(fixture.readback));
    fixture.device.CompleteThrough(fixture.frame.serial);
    const auto timestamp = fixture.device.TryReadTimestamp(fixture.beginQuery);
    const auto readback = fixture.device.TryReadTextureReadback(fixture.readback);
    ASSERT_TRUE(timestamp);
    ASSERT_TRUE(readback);
    EXPECT_TRUE(timestamp->unavailable);
    EXPECT_EQ(timestamp->validBits, 0);
    EXPECT_EQ(timestamp->frequency, 0U);
    EXPECT_TRUE(readback->unavailable);
    EXPECT_TRUE(readback->bytes.empty());
    EXPECT_FALSE(TimestampDeltaSeconds(*timestamp, *fixture.device.TryReadTimestamp(fixture.endQuery)));
    EXPECT_NE(TextureReadbackResultJson(readback).find("\"byteCount\":0"), std::string::npos);
    fixture.device.Shutdown();
}
TEST(TraceResults, NormalizesPaddedRowsAndRejectsInvalidReadbacks)
{
    // 人工字节 fixture，仅证明 row pitch 转换，不代表 GPU 截图。
    std::array<std::byte, 24> input{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        input[i] = static_cast<std::byte>(i + 1);
        input[i + 12] = static_cast<std::byte>(i + 9);
    }
    const auto result = NormalizeRgba8Readback(input, 12, {2, 2}, Format::Rgba8UnormSrgb);
    ASSERT_EQ(result.bytes.size(), 16U);
    EXPECT_EQ(result.rowPitch, 8U);
    for (std::size_t i = 0; i < result.bytes.size(); ++i)
        EXPECT_EQ(std::to_integer<unsigned>(result.bytes[i]), i + 1);
    EXPECT_NO_THROW(NormalizeRgba8Readback(std::span(input).first(20), 12, {2, 2}, Format::Rgba8Unorm));
    EXPECT_THROW(NormalizeRgba8Readback(std::span(input).first(19), 12, {2, 2}, Format::Rgba8Unorm),
                 RhiValidationError);
    EXPECT_THROW(NormalizeRgba8Readback(input, 7, {2, 2}, Format::Rgba8Unorm), RhiValidationError);
    EXPECT_THROW(NormalizeRgba8Readback(input, 12, {2, 2}, Format::Rgba16Float), RhiValidationError);
    EXPECT_THROW(NormalizeRgba8Readback(input, std::numeric_limits<std::uint64_t>::max(), {2, 2}, Format::Rgba8Unorm),
                 RhiValidationError);
}
TEST(TraceResults, TimestampDeltaRequiresMatchingFrameClockAndValidMetadata)
{
    TimestampResult begin{100, 1000, 7, false, 64, false}, end{350, 1000, 7, false, 64, false};
    ASSERT_TRUE(TimestampDeltaSeconds(begin, end));
    EXPECT_DOUBLE_EQ(*TimestampDeltaSeconds(begin, end), 0.25);
    for (int defect = 0; defect < 7; ++defect)
    {
        auto bad = end;
        if (defect == 0)
            bad.frameSerial = 8;
        if (defect == 1)
            bad.frequency = 2000;
        if (defect == 2)
            bad.frequency = 0;
        if (defect == 3)
            bad.disjoint = true;
        if (defect == 4)
            bad.unavailable = true;
        if (defect == 5)
            bad.validBits = 0;
        if (defect == 6)
            bad.validBits = 65;
        EXPECT_FALSE(TimestampDeltaSeconds(begin, bad)) << defect;
    }
    begin = {250, 1000, 7, false, 8, false};
    end = {10, 1000, 7, false, 8, false};
    EXPECT_DOUBLE_EQ(*TimestampDeltaSeconds(begin, end), 0.016);
    begin.ticks = 256;
    EXPECT_FALSE(TimestampDeltaSeconds(begin, end));
    EXPECT_NE(TimestampResultJson(std::nullopt).find("\"ready\":false"), std::string::npos);
}
TEST(TraceCanonical, HundredIndependentRunsHaveIdenticalHashesAndBothProfilesMatch)
{
    std::string reference;
    std::uint64_t referenceHash = 0;
    for (int i = 0; i < 100; ++i)
    {
        TriangleFixture d11(RhiBackend::D3D11), d12(RhiBackend::D3D12);
        d11.FixedTriangle();
        d12.FixedTriangle();
        EXPECT_EQ(d11.device.CanonicalTrace(), d12.device.CanonicalTrace()) << i;
        EXPECT_EQ(d11.device.StableHash(), d12.device.StableHash()) << i;
        if (i == 0)
        {
            reference = d11.device.CanonicalTrace();
            referenceHash = d11.device.StableHash();
            const std::filesystem::path output(M605_ARTIFACT_DIR);
            std::filesystem::create_directories(output);
            std::ofstream(output / "d3d11.trace", std::ios::binary) << reference;
            std::ofstream(output / "d3d12.trace", std::ios::binary) << d12.device.CanonicalTrace();
            d11.device.CompleteThrough(d11.frame.serial);
            d12.device.CompleteThrough(d12.frame.serial);
            std::ofstream(output / "timestamp.json")
                << TimestampResultJson(d11.device.TryReadTimestamp(d11.beginQuery));
            std::ofstream(output / "readback.json")
                << TextureReadbackResultJson(d11.device.TryReadTextureReadback(d11.readback));
        }
        EXPECT_EQ(d11.device.CanonicalTrace(), reference) << i;
        EXPECT_EQ(d11.device.StableHash(), referenceHash) << i;
        d11.device.Shutdown();
        d12.device.Shutdown();
    }
    const std::filesystem::path output(M605_ARTIFACT_DIR);
    std::ofstream(output / "determinism.json")
        << "{\"schema\":\"miniengine.trace-check.v1\",\"iterationsPerProfile\":100,"
           "\"profiles\":2,\"gpuExecuted\":false,\"stableHash\":"
        << referenceHash << '}';
}
TEST(TraceCanonical, DrawParametersAndLabelsAffectTrace)
{
    TriangleFixture first, second;
    first.Start();
    second.Start();
    first.Render();
    second.Render();
    first.Bind();
    second.Bind();
    first.commands->Draw(3, 1, 0, 0);
    second.commands->Draw(2, 1, 0, 0);
    EXPECT_NE(first.device.StableHash(), second.device.StableHash());
    first.commands->EndRendering();
    second.commands->EndRendering();
    first.commands->BeginLabel("x|y");
    second.commands->BeginLabel("x");
    EXPECT_NE(first.device.CanonicalTrace(), second.device.CanonicalTrace());
}
TEST(TraceFrame, DefaultCompletionDoesNotAdvanceWithoutProof)
{
    TraceRhiDevice device;
    auto chain = device.CreateSwapChain({{16, 16}});
    for (int i = 0; i < 3; ++i)
        ClearFrame(device, chain);
    EXPECT_THROW(device.BeginFrame(chain), RhiValidationError);
    EXPECT_EQ(device.Diagnostics().completedSerial, 0U);
    device.CompleteThrough(1);
    EXPECT_NO_THROW(ClearFrame(device, chain));
    device.CompleteThrough(4);
    device.Shutdown();
}
TEST(TraceFrame, MissingPresentCanBeRepairedBeforeClosing)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render();
    fixture.commands->EndRendering();
    EXPECT_THROW(fixture.device.EndGraphics(fixture.frame, *fixture.commands), RhiValidationError);
    EXPECT_NO_THROW(fixture.commands->BeginLabel("repair"));
    fixture.commands->EndLabel();
    EXPECT_NO_THROW(fixture.Finish());
    fixture.device.Shutdown();
}
TEST(TraceFrame, FailedInitialSwapChainCreationRollsBackBeforePublication)
{
    TraceRhiDevice device;
    const auto before = device.Diagnostics();
    device.FailNextBackBufferCreation();
    EXPECT_THROW(device.CreateSwapChain({{16, 16}}), RhiValidationError);
    EXPECT_EQ(device.Diagnostics().aliveObjects, before.aliveObjects);
    EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
    EXPECT_TRUE(device.Events().empty());
    EXPECT_THROW(device.CreateSwapChain({{65536, 16}}), RhiValidationError);
    EXPECT_TRUE(device.Events().empty());
    auto chain = device.CreateSwapChain({{16, 16}});
    ClearFrame(device, chain);
    device.CompleteThrough(1);
    device.Shutdown();
}
TEST(TraceFrame, FailedResizeInvalidatesOldHandlesAndCanBeRetried)
{
    TraceRhiDevice device;
    DriveCompletion(device);
    auto chain = device.CreateSwapChain({{16, 16}});
    auto frame = device.BeginFrame(chain);
    auto old = frame.backBuffer;
    auto& commands = device.BeginGraphics(frame);
    (void)device.Import(frame, old);
    const AccessTransition write{old, {}, ResourceAccess::None, ResourceAccess::ColorWrite};
    device.GraphCommandSink(frame).ApplyTransitions(std::span(&write, 1));
    const ColorAttachment color{old, LoadOp::Clear};
    commands.BeginRendering({std::span(&color, 1), nullptr, {16, 16}});
    commands.EndRendering();
    const AccessTransition present{old, {}, ResourceAccess::ColorWrite, ResourceAccess::Present};
    device.GraphCommandSink(frame).ApplyTransitions(std::span(&present, 1));
    device.EndGraphics(frame, commands);
    device.EndFrame(frame, chain);
    device.FailNextBackBufferCreation();
    EXPECT_THROW(device.ResizeSwapChain(chain, {32, 32}), RhiValidationError);
    EXPECT_THROW(device.Lifetime().ValidateAlive(old), RhiValidationError);
    EXPECT_EQ(device.BeginFrame(chain).serial, 0U);
    device.ResizeSwapChain(chain, {32, 32});
    ClearFrame(device, chain);
    device.Shutdown();
}
TEST(TraceFrame, BareHandleCannotDeclareGraphAccessWithoutFrameImport)
{
    TriangleFixture fixture;
    fixture.Start(false);
    const AccessTransition destination{{}, fixture.readback, ResourceAccess::None, ResourceAccess::CopyDestination};
    const auto before = fixture.device.Events().size();
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ApplyTransitions(std::span(&destination, 1)),
                 RhiValidationError);
    EXPECT_EQ(fixture.device.Events().size(), before);
    (void)fixture.device.Import(fixture.frame, fixture.readback);
    EXPECT_NO_THROW(fixture.device.GraphCommandSink(fixture.frame).ApplyTransitions(std::span(&destination, 1)));
}
TEST(TraceFrame, PreviousFrameUniformAccessDoesNotSubstituteCurrentDeclaration)
{
    TriangleFixture fixture;
    fixture.FixedTriangle();
    fixture.Start(false);
    fixture.Transition(fixture.vertex, ResourceAccess::VertexRead);
    fixture.Transition(fixture.index, ResourceAccess::IndexRead);
    fixture.Render();
    fixture.Bind();
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->EndRendering();
    fixture.Transition(fixture.uniform, ResourceAccess::UniformRead);
    fixture.Render(LoadOp::Load);
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
}
TEST(TraceResults, ReadbackIdentifiesFrameAndSourceAndBufferOverwriteInvalidatesSnapshot)
{
    TriangleFixture fixture;
    fixture.FixedTriangle();
    fixture.device.CompleteThrough(fixture.frame.serial);
    auto result = fixture.device.TryReadTextureReadback(fixture.readback);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->frameSerial, fixture.frame.serial);
    EXPECT_EQ(result->source, fixture.frame.backBuffer);
    fixture.Start();
    fixture.Transition(fixture.vertex, ResourceAccess::CopySource);
    fixture.Transition(fixture.readback, ResourceAccess::CopyDestination);
    fixture.commands->CopyBuffer({fixture.vertex, 0, 36}, {fixture.readback, 0, 36}, 36);
    EXPECT_FALSE(fixture.device.TryReadTextureReadback(fixture.readback));
}
} // namespace

TEST(TraceGraphImport, PublicImportBatchIsAtomicAndDoesNotSurviveFrame)
{
    TriangleFixture fixture;
    fixture.Start(false);
    auto& sink = fixture.device.GraphCommandSink(fixture.frame);
    const std::array<GraphResourceImport, 2> invalid{{{{}, fixture.vertex}, {fixture.frame.backBuffer, fixture.index}}};
    EXPECT_THROW(sink.ImportResources(invalid), RhiValidationError);
    const AccessTransition vertex{{}, fixture.vertex, ResourceAccess::None, ResourceAccess::VertexRead};
    EXPECT_THROW(sink.ApplyTransitions(std::span(&vertex, 1)), RhiValidationError);
    const GraphResourceImport imported{{}, fixture.vertex};
    EXPECT_NO_THROW(sink.ImportResources(std::span(&imported, 1)));
    EXPECT_NO_THROW(sink.ApplyTransitions(std::span(&vertex, 1)));
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.Start(false);
    const AccessTransition next{{}, fixture.vertex, ResourceAccess::VertexRead, ResourceAccess::VertexRead};
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ApplyTransitions(std::span(&next, 1)),
                 RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.Shutdown();
}

TEST(TraceFrame, DynamicUniformIsConsumedThroughFrameSetAndRetiresWithItsFrame)
{
    TriangleFixture fixture;
    fixture.Start();
    const std::array<std::byte, 16> constants{};
    const auto slice = fixture.device.WriteDynamicBuffer(fixture.frame, constants, 256);
    ResourceSetDesc desc;
    desc.layout = fixture.setLayout;
    desc.bindings.push_back({0, 0, BindingType::UniformBuffer, slice.view});
    EXPECT_THROW(fixture.device.CreateResourceSet(desc), RhiValidationError);
    const auto frameSet = fixture.device.CreateFrameResourceSet(fixture.frame, desc);
    EXPECT_THROW(fixture.device.Destroy(frameSet), RhiValidationError);
    fixture.Transition(slice.view.buffer, ResourceAccess::UniformRead);
    fixture.Render();
    fixture.Bind();
    const std::array<std::uint32_t, 1> offsets{0};
    fixture.commands->BindResourceSet(0, frameSet, offsets);
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
    fixture.commands->EndRendering();
    fixture.Finish();
    EXPECT_THROW(fixture.device.Lifetime().ValidateAlive(frameSet), RhiValidationError);
    EXPECT_THROW(fixture.device.Lifetime().ValidateAlive(slice.view.buffer), RhiValidationError);
    EXPECT_GE(fixture.device.Diagnostics().retiringObjects, 2);
    fixture.device.CompleteThrough(fixture.frame.serial);
    EXPECT_EQ(fixture.device.Diagnostics().retiringObjects, 0);
    fixture.Start();
    EXPECT_THROW(fixture.device.CreateFrameResourceSet(fixture.frame, desc), RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.Shutdown();
}
