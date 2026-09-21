#include "TraceFixture.h"
#include <cmath>
#include <limits>
#include <type_traits>

using namespace MiniEngine::Rhi;
using namespace MiniEngine::Tests;
namespace
{
// ordinary pass 只持有 IRhiCommandList；graph sink 和 device 同步不是其 API。
template <class T>
concept HasBarrier = requires(T& value) { value.Barrier(); };
template <class T>
concept HasWait = requires(T& value) { value.WaitIdle(); };
template <class T>
concept HasNative = requires(T& value) { value.GetNative(); };
static_assert(!HasBarrier<IRhiCommandList> && !HasWait<IRhiCommandList> && !HasNative<IRhiCommandList>);
static_assert(!std::is_base_of_v<IRhiGraphCommandSink, IRhiCommandList>);

TEST(CommandValidation, FixedTriangleRunsEveryMinimumCommand)
{
    TriangleFixture fixture;
    fixture.FixedTriangle();
    const auto trace = fixture.device.CanonicalTrace();
    for (const char* operation : {"BeginLabel", "EndLabel", "BeginRendering", "EndRendering", "SetPipeline",
                                  "SetViewport", "SetScissor", "BindVertexBuffer", "BindIndexBuffer", "BindResourceSet",
                                  "Draw", "DrawIndexed", "CopyBuffer", "CopyTextureForReadback", "WriteTimestamp"})
        EXPECT_NE(trace.find(std::string(":") + operation + ' '), std::string::npos) << operation;
    EXPECT_GT(fixture.device.ConsumedCommands(), 0U);
    fixture.device.Shutdown();
    EXPECT_EQ(fixture.device.Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(fixture.device.Diagnostics().retiringObjects, 0U);
}
TEST(CommandValidation, InvalidCommandReportsFramePassIndexAndHandleBeforeConsumption)
{
    TriangleFixture fixture(RhiBackend::D3D11);
    fixture.Start();
    fixture.commands->BeginLabel("shadow pass");
    const auto before = fixture.device.ConsumedCommands();
    const auto events = fixture.device.Events().size();
    try
    {
        fixture.commands->BindVertexBuffer(0, {fixture.vertex, 0, 36}, 0);
        FAIL() << "invalid stride was consumed";
    }
    catch (const RhiException& exception)
    {
        const auto& error = exception.Error();
        EXPECT_EQ(error.code, RhiErrorCode::InvalidState);
        EXPECT_EQ(error.operation, "BindVertexBuffer");
        EXPECT_EQ(error.objectName, "triangle vertices");
        EXPECT_NE(error.message.find("frame=" + std::to_string(fixture.frame.serial)), std::string::npos);
        EXPECT_NE(error.message.find("pass=shadow pass command=5"), std::string::npos);
    }
    EXPECT_EQ(fixture.device.ConsumedCommands(), before);
    EXPECT_EQ(fixture.device.Events().size(), events);
}
TEST(CommandValidation, StaleHandleKeepsInvalidHandleCodeAndName)
{
    TriangleFixture fixture;
    fixture.device.Destroy(fixture.vertex);
    fixture.Start(false);
    try
    {
        fixture.commands->BindVertexBuffer(0, {fixture.vertex, 0, 36}, 12);
        FAIL();
    }
    catch (const RhiException& exception)
    {
        EXPECT_EQ(exception.Error().code, RhiErrorCode::InvalidHandle);
        EXPECT_EQ(exception.Error().objectName, "triangle vertices");
        EXPECT_NE(exception.Error().message.find("command=1"), std::string::npos);
    }
}
TEST(CommandValidation, DrawNeedsRenderingPipelineAndDynamicState)
{
    TriangleFixture fixture;
    fixture.Start();
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.Render();
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->SetPipeline(fixture.pipeline);
    fixture.commands->BindVertexBuffer(0, {fixture.vertex, 0, 36}, 12);
    const std::array<std::uint32_t, 1> offsets{256};
    fixture.commands->BindResourceSet(0, fixture.set, offsets);
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->SetViewport({0, 0, 16, 16, 0, 1});
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->SetScissor({0, 0, 16, 16});
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
}
TEST(CommandValidation, MissingResourceSetAndVertexBufferDoNotReachConsumer)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render();
    fixture.commands->SetPipeline(fixture.pipeline);
    fixture.commands->SetViewport({0, 0, 16, 16, 0, 1});
    fixture.commands->SetScissor({0, 0, 16, 16});
    const auto before = fixture.device.Events().size();
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    const std::array<std::uint32_t, 1> offsets{256};
    fixture.commands->BindResourceSet(0, fixture.set, offsets);
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    EXPECT_EQ(fixture.device.Events().size(), before + 1);
    fixture.commands->BindVertexBuffer(0, {fixture.vertex, 0, 36}, 12);
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
}
TEST(CommandValidation, IndexedDrawRequiresIndexAndValidRange)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render();
    fixture.commands->SetPipeline(fixture.pipeline);
    fixture.commands->SetViewport({0, 0, 16, 16, 0, 1});
    fixture.commands->SetScissor({0, 0, 16, 16});
    fixture.commands->BindVertexBuffer(0, {fixture.vertex, 0, 36}, 12);
    const std::array<std::uint32_t, 1> offsets{256};
    fixture.commands->BindResourceSet(0, fixture.set, offsets);
    EXPECT_THROW(fixture.commands->DrawIndexed(3, 1, 0, 0, 0), RhiValidationError);
    fixture.commands->BindIndexBuffer({fixture.index, 0, 6}, IndexType::UInt16);
    EXPECT_THROW(fixture.commands->DrawIndexed(4, 1, 0, 0, 0), RhiValidationError);
    EXPECT_THROW(fixture.commands->DrawIndexed(3, 1, 0, 1, 0), RhiValidationError);
    EXPECT_NO_THROW(fixture.commands->DrawIndexed(3, 1, 0, 0, 0));
}
TEST(CommandValidation, NestedAndUnclosedRenderingAndLabelsAreRejected)
{
    TriangleFixture fixture;
    fixture.Start();
    EXPECT_THROW(fixture.commands->EndLabel(), RhiValidationError);
    EXPECT_THROW(fixture.commands->EndRendering(), RhiValidationError);
    fixture.Render();
    const ColorAttachment color{fixture.frame.backBuffer, LoadOp::Clear};
    EXPECT_THROW(fixture.commands->BeginRendering({std::span(&color, 1), nullptr, {16, 16}}), RhiValidationError);
    EXPECT_THROW(fixture.device.EndGraphics(fixture.frame, *fixture.commands), RhiValidationError);
    fixture.commands->EndRendering();
    fixture.commands->BeginLabel("unclosed");
    EXPECT_THROW(fixture.device.EndGraphics(fixture.frame, *fixture.commands), RhiValidationError);
    fixture.commands->EndLabel();
    fixture.Finish();
}
TEST(CommandValidation, InitialLoadAndDontCareReadRequireExplicitInitialization)
{
    TriangleFixture fixture;
    fixture.Start();
    EXPECT_THROW(fixture.Render(LoadOp::Load), RhiValidationError);
    fixture.Render(LoadOp::DontCare);
    fixture.Bind();
    fixture.commands->Draw(3, 1, 0, 0);
    fixture.commands->EndRendering();
    EXPECT_THROW(fixture.Transition(fixture.frame.backBuffer, ResourceAccess::CopySource), RhiValidationError);
    EXPECT_THROW(fixture.Render(LoadOp::Load), RhiValidationError);
    fixture.Render(LoadOp::Clear);
    fixture.commands->EndRendering();
    EXPECT_NO_THROW(fixture.Render(LoadOp::Load));
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, StoreDontCareInvalidatesAfterScopeAndClearRepairsIt)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render(LoadOp::Clear, StoreOp::DontCare);
    fixture.commands->EndRendering();
    EXPECT_THROW(fixture.Render(LoadOp::Load), RhiValidationError);
    EXPECT_THROW(fixture.Transition(fixture.frame.backBuffer, ResourceAccess::Present), RhiValidationError);
    fixture.Render(LoadOp::Clear);
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, AttachmentValidationIsAtomicAndChecksUsageExtentAndAliasing)
{
    TriangleFixture fixture;
    auto wrong = fixture.device.CreateTexture({TextureDimension::Texture2D,
                                               {8, 8},
                                               1,
                                               1,
                                               1,
                                               Format::Rgba8Unorm,
                                               TextureUsage::ColorAttachment,
                                               "wrong extent"});
    fixture.Start();
    fixture.Transition(fixture.frame.backBuffer, ResourceAccess::ColorWrite);
    fixture.Transition(wrong, ResourceAccess::ColorWrite);
    std::array attachments{ColorAttachment{fixture.frame.backBuffer, LoadOp::Clear},
                           ColorAttachment{wrong, LoadOp::Clear}};
    const auto before = fixture.device.Events().size();
    EXPECT_THROW(fixture.commands->BeginRendering({attachments, nullptr, {16, 16}}), RhiValidationError);
    EXPECT_EQ(fixture.device.Events().size(), before);
    EXPECT_FALSE(fixture.device.ResourceStates().at(fixture.frame.backBuffer).defined);
    attachments[1] = attachments[0];
    EXPECT_THROW(fixture.commands->BeginRendering({attachments, nullptr, {16, 16}}), RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, InvalidLoadStoreAndNonFiniteClearAreRejected)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Transition(fixture.frame.backBuffer, ResourceAccess::ColorWrite);
    ColorAttachment color{fixture.frame.backBuffer, static_cast<LoadOp>(255)};
    auto render = [&] { fixture.commands->BeginRendering({std::span(&color, 1), nullptr, {16, 16}}); };
    EXPECT_THROW(render(), RhiValidationError);
    color.load = LoadOp::Clear;
    color.store = static_cast<StoreOp>(255);
    EXPECT_THROW(render(), RhiValidationError);
    color.store = StoreOp::Store;
    color.clearColor[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(render(), RhiValidationError);
}
TEST(CommandValidation, ViewportScissorValidateFiniteRangeAndRenderingExtent)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render();
    fixture.Bind();
    EXPECT_THROW(fixture.commands->SetViewport({0, 0, std::numeric_limits<float>::infinity(), 16, 0, 1}),
                 RhiValidationError);
    EXPECT_THROW(fixture.commands->SetViewport({0, 0, 16, 16, 1, 0}), RhiValidationError);
    EXPECT_THROW(fixture.commands->SetScissor({-1, 0, 16, 16}), RhiValidationError);
    fixture.commands->SetScissor({1, 0, 16, 16});
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->SetScissor({0, 0, 16, 16});
    fixture.commands->SetViewport({1, 0, 16, 16, 0, 1});
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
}
TEST(CommandValidation, DynamicOffsetValidationPrecedesConsumption)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render();
    fixture.Bind();
    const auto before = fixture.device.Events().size();
    const std::array<std::uint32_t, 1> misaligned{1}, pastEnd{512};
    EXPECT_THROW(fixture.commands->BindResourceSet(0, fixture.set, {}), RhiValidationError);
    EXPECT_THROW(fixture.commands->BindResourceSet(0, fixture.set, misaligned), RhiValidationError);
    EXPECT_THROW(fixture.commands->BindResourceSet(0, fixture.set, pastEnd), RhiValidationError);
    EXPECT_EQ(fixture.device.Events().size(), before);
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
}
TEST(CommandValidation, CopyRequiresDeclaredBothSidesAndChecksRanges)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Transition(fixture.vertex, ResourceAccess::CopySource);
    EXPECT_THROW(fixture.commands->CopyBuffer({fixture.vertex, 0, 36}, {fixture.copy, 0, 36}, 36), RhiValidationError);
    fixture.Transition(fixture.copy, ResourceAccess::CopyDestination);
    EXPECT_THROW(fixture.commands->CopyBuffer({fixture.vertex, 0, 36}, {fixture.copy, 0, 36}, 37), RhiValidationError);
    EXPECT_THROW(fixture.commands->CopyBuffer({fixture.vertex, std::numeric_limits<std::uint64_t>::max(), 36},
                                              {fixture.copy, 0, 36}, 1),
                 RhiValidationError);
    EXPECT_NO_THROW(fixture.commands->CopyBuffer({fixture.vertex, 0, 36}, {fixture.copy, 0, 36}, 36));
    fixture.Render();
    EXPECT_THROW(fixture.commands->CopyBuffer({fixture.vertex, 0, 36}, {fixture.copy, 0, 36}, 36), RhiValidationError);
}
TEST(CommandValidation, PartialInitializationTracksByteRangesWithoutPretendingWholeBufferDefined)
{
    TriangleFixture fixture;
    std::array<std::byte, 4> data{};
    auto source =
        fixture.device.CreateBuffer({16, BufferUsage::CopySource, MemoryDomain::GpuOnly, "partial source"}, data);
    fixture.Start();
    fixture.Transition(source, ResourceAccess::CopySource);
    fixture.Transition(fixture.copy, ResourceAccess::CopyDestination);
    EXPECT_THROW(fixture.commands->CopyBuffer({source, 0, 16}, {fixture.copy, 0, 16}, 16), RhiValidationError);
    EXPECT_NO_THROW(fixture.commands->CopyBuffer({source, 0, 4}, {fixture.copy, 0, 4}, 4));
    EXPECT_TRUE(fixture.device.ResourceStates().at(fixture.copy).Contains(0, 4));
    EXPECT_FALSE(fixture.device.ResourceStates().at(fixture.copy).Contains(0, 5));
}
TEST(CommandValidation, ScreenshotRequiresReadbackMemoryFullExtentAndDestinationAccess)
{
    TriangleFixture fixture;
    fixture.Start();
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Transition(fixture.frame.backBuffer, ResourceAccess::CopySource);
    EXPECT_THROW(fixture.commands->CopyTextureForReadback(fixture.frame.backBuffer, fixture.readback, {16, 16}),
                 RhiValidationError);
    fixture.Transition(fixture.readback, ResourceAccess::CopyDestination);
    EXPECT_THROW(fixture.commands->CopyTextureForReadback(fixture.frame.backBuffer, fixture.readback, {8, 8}),
                 RhiValidationError);
    EXPECT_THROW(fixture.commands->CopyTextureForReadback(fixture.frame.backBuffer, fixture.copy, {16, 16}),
                 RhiValidationError);
    EXPECT_NO_THROW(fixture.commands->CopyTextureForReadback(fixture.frame.backBuffer, fixture.readback, {16, 16}));
}
TEST(CommandValidation, TransitionBatchIsAtomicAndCannotRunInsideRendering)
{
    TriangleFixture fixture;
    fixture.Start();
    std::array transitions{
        AccessTransition{fixture.frame.backBuffer, {}, ResourceAccess::None, ResourceAccess::ColorWrite},
        AccessTransition{{}, fixture.copy, ResourceAccess::CopySource, ResourceAccess::CopyDestination}};
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ApplyTransitions(transitions), RhiValidationError);
    EXPECT_FALSE(fixture.device.ResourceStates().contains(fixture.frame.backBuffer));
    fixture.Render();
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ApplyTransitions({}), RhiValidationError);
}
TEST(CommandValidation, TimestampCannotBeRewrittenWhileInFlight)
{
    TriangleFixture fixture;
    fixture.FixedTriangle();
    EXPECT_FALSE(fixture.device.TryReadTimestamp(fixture.beginQuery));
    fixture.Start();
    EXPECT_THROW(fixture.commands->WriteTimestamp(fixture.beginQuery), RhiValidationError);
    fixture.device.CompleteThrough(fixture.device.Diagnostics().lastSubmittedSerial);
    EXPECT_NO_THROW(fixture.commands->WriteTimestamp(fixture.beginQuery));
    EXPECT_THROW(fixture.commands->WriteTimestamp(fixture.beginQuery), RhiValidationError);
}
TEST(CommandValidation, SampledAttachmentHazardIsRejectedUntilDeclaredReadable)
{
    TriangleFixture fixture;
    auto texture = fixture.device.CreateTexture({TextureDimension::Texture2D,
                                                 {16, 16},
                                                 1,
                                                 1,
                                                 1,
                                                 Format::Rgba8Unorm,
                                                 TextureUsage::ColorAttachment | TextureUsage::Sampled,
                                                 "sampled target"});
    auto layout = fixture.device.CreateResourceSetLayout(
        {0, {{1, BindingType::SampledTexture, 1, ShaderStage::Pixel}}, "sampled layout"});
    PipelineLayoutDesc pl;
    pl.sets[0] = layout;
    pl.setCount = 1;
    auto pipelineDesc = fixture.device.Lifetime().Describe(fixture.pipeline);
    pipelineDesc.layout = fixture.device.CreatePipelineLayout(pl);
    auto ps = TriangleFixture::Shader(ShaderStage::Pixel);
    ps.manifest.bindings = {{0, 1, BindingType::SampledTexture, 1, 0}};
    pipelineDesc.pixelShader = fixture.device.CreateShader(ps);
    auto pipeline = fixture.device.CreateGraphicsPipeline(pipelineDesc);
    ResourceSetDesc setDesc;
    setDesc.layout = layout;
    ResourceBinding binding;
    binding.binding = 1;
    binding.type = BindingType::SampledTexture;
    binding.texture = texture;
    setDesc.bindings = {binding};
    auto resources = fixture.device.CreateResourceSet(setDesc);
    fixture.Start();
    fixture.Transition(texture, ResourceAccess::ColorWrite);
    const ColorAttachment color{texture, LoadOp::Clear, StoreOp::Store};
    fixture.commands->BeginRendering({std::span(&color, 1), nullptr, {16, 16}});
    fixture.Bind();
    fixture.commands->SetPipeline(pipeline);
    fixture.commands->BindResourceSet(0, resources, {});
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->EndRendering();
    fixture.Transition(texture, ResourceAccess::SampledRead);
    fixture.Render();
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.Shutdown();
}
TEST(CommandValidation, ReadOnlyDepthRequiresDefinedLoadAndNonWritingPipeline)
{
    TriangleFixture fixture;
    auto depth = fixture.device.CreateTexture(
        {TextureDimension::Texture2D, {16, 16}, 1, 1, 1, Format::D32Float, TextureUsage::DepthStencil, "main depth"});
    auto desc = fixture.device.Lifetime().Describe(fixture.pipeline);
    desc.depthFormat = Format::D32Float;
    desc.depthTest = true;
    desc.depthWrite = false;
    auto readonlyPipeline = fixture.device.CreateGraphicsPipeline(desc);
    desc.depthWrite = true;
    auto writingPipeline = fixture.device.CreateGraphicsPipeline(desc);
    fixture.Start();
    EXPECT_THROW(fixture.Transition(depth, ResourceAccess::DepthRead), RhiValidationError);
    fixture.Transition(depth, ResourceAccess::DepthWrite);
    DepthAttachment attachment{depth, LoadOp::Clear, StoreOp::Store, 1.0F, 0};
    fixture.commands->BeginRendering({{}, &attachment, {16, 16}});
    fixture.commands->EndRendering();
    fixture.Transition(depth, ResourceAccess::DepthRead);
    EXPECT_THROW(fixture.commands->BeginRendering({{}, &attachment, {16, 16}}), RhiValidationError);
    attachment.load = LoadOp::Load;
    fixture.Transition(fixture.frame.backBuffer, ResourceAccess::ColorWrite);
    const ColorAttachment color{fixture.frame.backBuffer, LoadOp::Clear, StoreOp::Store};
    fixture.commands->BeginRendering({std::span(&color, 1), &attachment, {16, 16}});
    fixture.Bind();
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->SetPipeline(writingPipeline);
    EXPECT_THROW(fixture.commands->Draw(3, 1, 0, 0), RhiValidationError);
    fixture.commands->SetPipeline(readonlyPipeline);
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.Shutdown();
}
TEST(CommandValidation, MissingReadAccessNamesActualUniformResource)
{
    TriangleFixture fixture;
    fixture.Start(false);
    fixture.Transition(fixture.vertex, ResourceAccess::VertexRead);
    fixture.Transition(fixture.index, ResourceAccess::IndexRead);
    fixture.Render();
    fixture.Bind();
    try
    {
        fixture.commands->Draw(3, 1, 0, 0);
        FAIL();
    }
    catch (const RhiException& exception)
    {
        EXPECT_EQ(exception.Error().objectName, "scene constants");
        EXPECT_EQ(exception.Error().backend, "d3d12");
        EXPECT_NE(exception.Error().message.find("handle=scene constants"), std::string::npos);
    }
}
TEST(CommandValidation, RejectsAttachmentsWhoseClearWouldNotDefineWholeTexture)
{
    TriangleFixture fixture;
    auto mipmapped = fixture.device.CreateTexture({TextureDimension::Texture2D,
                                                   {16, 16},
                                                   2,
                                                   1,
                                                   1,
                                                   Format::Rgba8Unorm,
                                                   TextureUsage::ColorAttachment,
                                                   "mip target"});
    auto cube = fixture.device.CreateTexture({TextureDimension::TextureCube,
                                              {16, 16},
                                              1,
                                              6,
                                              1,
                                              Format::Rgba8Unorm,
                                              TextureUsage::ColorAttachment,
                                              "cube target"});
    fixture.Start();
    for (auto target : {mipmapped, cube})
    {
        fixture.Transition(target, ResourceAccess::ColorWrite);
        const ColorAttachment attachment{target, LoadOp::Clear};
        EXPECT_THROW(fixture.commands->BeginRendering({std::span(&attachment, 1), nullptr, {16, 16}}),
                     RhiValidationError);
        EXPECT_FALSE(fixture.device.ResourceStates().at(target).defined);
    }
}
TEST(CommandValidation, BindingAndDynamicStateCanBePreparedBeforeRendering)
{
    TriangleFixture fixture;
    fixture.Start();
    EXPECT_NO_THROW(fixture.Bind());
    fixture.Render();
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.Shutdown();
}
TEST(CommandValidation, ActiveAttachmentsCannotBeDestroyedUntilEndRendering)
{
    TriangleFixture fixture;
    auto color = fixture.device.CreateTexture({TextureDimension::Texture2D,
                                               {16, 16},
                                               1,
                                               1,
                                               1,
                                               Format::Rgba8Unorm,
                                               TextureUsage::ColorAttachment,
                                               "owned color"});
    auto depth = fixture.device.CreateTexture(
        {TextureDimension::Texture2D, {16, 16}, 1, 1, 1, Format::D32Float, TextureUsage::DepthStencil, "owned depth"});
    auto desc = fixture.device.Lifetime().Describe(fixture.pipeline);
    desc.depthFormat = Format::D32Float;
    auto pipeline = fixture.device.CreateGraphicsPipeline(desc);
    fixture.Start();
    fixture.Transition(color, ResourceAccess::ColorWrite);
    fixture.Transition(depth, ResourceAccess::DepthWrite);
    const ColorAttachment attachment{color, LoadOp::Clear, StoreOp::DontCare};
    const DepthAttachment depthAttachment{depth, LoadOp::Clear, StoreOp::DontCare};
    fixture.commands->BeginRendering({std::span(&attachment, 1), &depthAttachment, {16, 16}});
    fixture.Bind();
    fixture.commands->SetPipeline(pipeline);
    EXPECT_THROW(fixture.device.Destroy(color), RhiValidationError);
    EXPECT_THROW(fixture.device.Destroy(depth), RhiValidationError);
    EXPECT_NO_THROW(fixture.commands->Draw(3, 1, 0, 0));
    fixture.commands->EndRendering();
    EXPECT_NO_THROW(fixture.device.Destroy(color));
    EXPECT_NO_THROW(fixture.device.Destroy(depth));
    EXPECT_FALSE(fixture.device.ResourceStates().contains(color));
    EXPECT_FALSE(fixture.device.ResourceStates().contains(depth));
    EXPECT_THROW(fixture.commands->EndRendering(), RhiValidationError);
    EXPECT_FALSE(fixture.device.ResourceStates().contains(depth));
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.device.Shutdown();
}
TEST(CommandValidation, BypassedOwnerDestroyStillReportsStructuredErrorWithoutGhostState)
{
    TriangleFixture fixture;
    auto depth = fixture.device.CreateTexture({TextureDimension::Texture2D,
                                               {16, 16},
                                               1,
                                               1,
                                               1,
                                               Format::D32Float,
                                               TextureUsage::DepthStencil,
                                               "destroyed depth"});
    fixture.Start();
    fixture.Transition(depth, ResourceAccess::DepthWrite);
    fixture.Transition(fixture.frame.backBuffer, ResourceAccess::ColorWrite);
    const ColorAttachment color{fixture.frame.backBuffer, LoadOp::Clear};
    const DepthAttachment attachment{depth, LoadOp::Clear, StoreOp::DontCare};
    fixture.commands->BeginRendering({std::span(&color, 1), &attachment, {16, 16}});
    fixture.Bind();
    // 故意绕过 adapter 的公开 Destroy 保护，验证公共 recording 仍不会抛出 STL 异常或重建旧项。
    fixture.device.Lifetime().Destroy(depth);
    fixture.device.CompleteThrough(fixture.device.Diagnostics().lastSubmittedSerial);
    EXPECT_FALSE(fixture.device.ResourceStates().contains(depth));
    try
    {
        fixture.commands->Draw(3, 1, 0, 0);
        FAIL();
    }
    catch (const RhiException& exception)
    {
        EXPECT_EQ(exception.Error().code, RhiErrorCode::InvalidHandle);
        EXPECT_EQ(exception.Error().objectName, "destroyed depth");
    }
    EXPECT_THROW(fixture.commands->EndRendering(), RhiValidationError);
    EXPECT_FALSE(fixture.device.ResourceStates().contains(depth));
}
TEST(CommandValidation, ResetTransientContentsClearsContentAndPreservesAccess)
{
    TriangleFixture fixture;
    fixture.Start(false);
    fixture.Transition(fixture.vertex, ResourceAccess::CopySource);
    const auto beforeEvents = fixture.device.Events().size();
    const GraphResourceImport reset{{}, fixture.vertex};
    EXPECT_NO_THROW(fixture.device.GraphCommandSink(fixture.frame).ResetTransientContents(std::span(&reset, 1)));
    const auto& state = fixture.device.ResourceStates().at(fixture.vertex);
    EXPECT_EQ(state.access, ResourceAccess::CopySource);
    EXPECT_EQ(state.declaredFrame, fixture.frame.serial);
    EXPECT_FALSE(state.defined);
    EXPECT_TRUE(state.ranges.empty());
    ASSERT_EQ(fixture.device.Events().size(), beforeEvents + 1);
    EXPECT_NE(fixture.device.Events().back().find("ResetTransientContents"), std::string::npos);
    fixture.Transition(fixture.vertex, ResourceAccess::VertexRead);
    EXPECT_EQ(fixture.device.ResourceStates().at(fixture.vertex).access, ResourceAccess::VertexRead);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, ResetTransientContentsValidatesWholeBatchAtomically)
{
    TriangleFixture fixture;
    fixture.Start(false);
    fixture.Transition(fixture.vertex, ResourceAccess::CopySource);
    const auto before = fixture.device.ResourceStates().at(fixture.vertex);
    const std::array<GraphResourceImport, 2> reset{{{{}, fixture.vertex}, {{}, fixture.copy}}};
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ResetTransientContents(reset), RhiValidationError);
    const auto& after = fixture.device.ResourceStates().at(fixture.vertex);
    EXPECT_EQ(after.access, before.access);
    EXPECT_EQ(after.declaredFrame, before.declaredFrame);
    EXPECT_EQ(after.defined, before.defined);
    EXPECT_EQ(after.ranges, before.ranges);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, ResetTransientContentsRejectsEmptyBackbufferAndAmbiguousBatches)
{
    TriangleFixture fixture;
    fixture.Start(false);
    const auto sink = [&]() -> IRhiGraphCommandSink& { return fixture.device.GraphCommandSink(fixture.frame); };
    EXPECT_THROW(sink().ResetTransientContents({}), RhiValidationError);
    const GraphResourceImport backbuffer{fixture.frame.backBuffer, {}};
    (void)fixture.device.Import(fixture.frame, fixture.frame.backBuffer);
    EXPECT_THROW(sink().ResetTransientContents(std::span(&backbuffer, 1)), RhiValidationError);
    const GraphResourceImport ambiguous{fixture.frame.backBuffer, fixture.vertex};
    EXPECT_THROW(sink().ResetTransientContents(std::span(&ambiguous, 1)), RhiValidationError);
    const GraphResourceImport neither{};
    EXPECT_THROW(sink().ResetTransientContents(std::span(&neither, 1)), RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, ResetTransientContentsRejectsRenderingScope)
{
    TriangleFixture fixture;
    fixture.Start(false);
    fixture.Transition(fixture.vertex, ResourceAccess::VertexRead);
    fixture.Render();
    const GraphResourceImport reset{{}, fixture.vertex};
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ResetTransientContents(std::span(&reset, 1)),
                 RhiValidationError);
    fixture.commands->EndRendering();
    fixture.Finish();
}
TEST(CommandValidation, ResetTransientContentsRejectsPreviousBackbufferAndDeadHandle)
{
    TriangleFixture fixture;
    fixture.Start(false);
    const auto previousBackbuffer = fixture.frame.backBuffer;
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
    fixture.Start(false);
    ASSERT_NE(previousBackbuffer, fixture.frame.backBuffer);
    const GraphResourceImport previous{previousBackbuffer, {}};
    fixture.device.GraphCommandSink(fixture.frame).ImportResources(std::span(&previous, 1));
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ResetTransientContents(std::span(&previous, 1)),
                 RhiValidationError);
    fixture.Transition(fixture.copy, ResourceAccess::CopyDestination);
    fixture.device.Destroy(fixture.copy);
    const GraphResourceImport dead{{}, fixture.copy};
    EXPECT_THROW(fixture.device.GraphCommandSink(fixture.frame).ResetTransientContents(std::span(&dead, 1)),
                 RhiValidationError);
    fixture.Render();
    fixture.commands->EndRendering();
    fixture.Finish();
}
} // namespace
