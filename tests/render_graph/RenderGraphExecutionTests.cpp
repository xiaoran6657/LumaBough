#include "RenderGraphTestFixtures.h"
#include "TraceRhi.h"
#include <gtest/gtest.h>
#include <optional>

using namespace MiniEngine::Rhi;
using namespace MiniEngine::RenderGraph;
using namespace MiniEngine::RenderGraph::Tests;
using MiniEngine::Tests::TraceRhiDevice;
namespace
{
struct ExecutionFixture
{
    TraceRhiDevice device;
    SwapChainHandle chain;
    FrameToken frame;
    RenderGraph graph;
    RgTexture back;
    ExecutionFixture()
    {
        chain = device.CreateSwapChain({{32, 24}});
        frame = device.BeginFrame(chain);
        const auto snapshot = device.QueryTextureState(frame, frame.backBuffer);
        back = graph.ImportTexture("BackBuffer", {frame.backBuffer,
                                                  snapshot.descriptor,
                                                  snapshot.access,
                                                  ResourceAccess::Present,
                                                  ContentState::Undefined,
                                                  "chain",
                                                  {}});
    }
    void Clear()
    {
        back = AddAttachmentPass(graph, "ClearBack", back);
        graph.Present(back);
    }
    void Finish()
    {
        device.EndFrame(frame, chain);
        device.CompleteThrough(frame.serial);
        device.Shutdown();
        EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
        EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
    }
};
TEST(GraphExecution, SetupCompileDoNotCreatePhysicalResourcesOrCommands)
{
    ExecutionFixture f;
    const auto alive = f.device.Diagnostics().aliveObjects;
    const auto events = f.device.Events().size();
    const auto texture = f.graph.CreateTexture("Transient", MakeColorDesc());
    const auto written = AddAttachmentPass(f.graph, "TransientClear", texture);
    f.graph.AddPass<int>(
        "ConsumeTransient",
        [&](RgBuilder& b, int&)
        {
            (void)b.Read(written, ResourceAccess::SampledRead);
            b.SideEffect("exercise transient retirement");
        },
        [](const int&, const RgResources&, IRhiCommandList&) {});
    f.Clear();
    auto plan = f.graph.Compile();
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(f.device.Events().size(), events);
    plan.Execute(f.device, f.frame);
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_GT(f.device.Diagnostics().retiringObjects, 0U);
    f.Finish();
}
TEST(GraphExecution, FinalTransitionOccursAfterLastCallbackAndImportsRemainOwned)
{
    ExecutionFixture f;
    f.Clear();
    bool tail = false;
    f.graph.AddPass<int>(
        "Tail", [](RgBuilder& b, int&) { b.SideEffect("trace marker"); },
        [&](const int&, const RgResources&, IRhiCommandList& commands)
        {
            EXPECT_EQ(f.device.QueryTextureState(f.frame, f.frame.backBuffer).access, ResourceAccess::ColorWrite);
            commands.BeginLabel("last callback marker");
            commands.EndLabel();
            tail = true;
        });
    auto plan = f.graph.Compile();
    plan.Execute(f.device, f.frame);
    ASSERT_TRUE(tail);
    EXPECT_EQ(f.device.QueryTextureState(f.frame, f.frame.backBuffer).access, ResourceAccess::Present);
    const auto trace = f.device.CanonicalTrace();
    const auto marker = trace.find("last callback marker");
    ASSERT_NE(marker, std::string::npos);
    EXPECT_NE(trace.find("ApplyTransitions", marker), std::string::npos);
    f.Finish();
}
TEST(GraphExecution, ResolverRejectsUndeclaredOldVersionAndEscapedUse)
{
    ExecutionFixture f;
    const auto original = f.back;
    const auto hidden = f.graph.CreateTexture("Hidden", MakeColorDesc());
    std::optional<RgResources> saved;
    f.graph.AddPass<TexturePassData>(
        "Clear",
        [&](RgBuilder& b, TexturePassData& d)
        {
            f.back = d.texture = b.Write(f.back, ResourceAccess::ColorWrite);
            b.SetColorAttachment(d.texture, LoadOp::Clear, StoreOp::Store);
        },
        [&](const TexturePassData& d, const RgResources& r, IRhiCommandList&)
        {
            EXPECT_EQ(r.Get(d.texture), f.frame.backBuffer);
            EXPECT_THROW((void)r.Get(original), GraphCompileError);
            EXPECT_THROW((void)r.Get(hidden), GraphCompileError);
            saved = r;
            EXPECT_THROW(f.graph.Reset(), GraphPhaseError);
            EXPECT_THROW((void)f.graph.Compile(), GraphPhaseError);
        });
    f.graph.Present(f.back);
    f.graph.AddPass<int>(
        "After", [](RgBuilder& b, int&) { b.SideEffect("resolver lifetime negative"); },
        [&](const int&, const RgResources&, IRhiCommandList&)
        {
            ASSERT_TRUE(saved);
            EXPECT_THROW((void)saved->Get(f.back), GraphPhaseError);
        });
    auto plan = f.graph.Compile();
    plan.Execute(f.device, f.frame);
    EXPECT_THROW((void)saved->Get(f.back), GraphPhaseError);
    EXPECT_THROW(plan.Execute(f.device, f.frame), GraphPhaseError);
    f.Finish();
    f.graph.Reset();
    EXPECT_THROW((void)saved->Get(f.back), GraphPhaseError);
}
TEST(GraphExecution, EarlierPassResolvesItsDeclaredVersionBeforeLaterOverwrite)
{
    ExecutionFixture f;
    auto resource = f.graph.CreateTexture("Versions", MakeColorDesc());
    int callbacks = 0;
    for (int i = 0; i < 2; ++i)
        f.graph.AddPass<TexturePassData>(
            "Write" + std::to_string(i),
            [&](RgBuilder& b, TexturePassData& d)
            {
                resource = d.texture = b.Write(resource, ResourceAccess::ColorWrite);
                b.SetColorAttachment(resource, LoadOp::Clear, StoreOp::Store);
                b.SideEffect("observe both resource versions");
            },
            [&](const TexturePassData& d, const RgResources& r, IRhiCommandList&)
            {
                EXPECT_TRUE(r.Get(d.texture));
                ++callbacks;
            });
    f.Clear();
    auto plan = f.graph.Compile();
    plan.Execute(f.device, f.frame);
    EXPECT_EQ(callbacks, 2);
    f.Finish();
}
TEST(GraphExecution, DescriptorMismatchFailsBeforeAnyNativeOrCommands)
{
    ExecutionFixture f;
    auto desc = MakeColorDesc();
    const auto physical = f.device.CreateTexture(desc);
    desc.extent.width++;
    f.graph.Export(
        f.graph.ImportTexture("Mismatch", {physical, desc, ResourceAccess::None, ResourceAccess::ColorWrite}));
    f.Clear();
    auto plan = f.graph.Compile();
    const auto events = f.device.Events().size(), alive = f.device.Diagnostics().aliveObjects;
    EXPECT_THROW(plan.Execute(f.device, f.frame), GraphCompileError);
    EXPECT_EQ(f.device.Events().size(), events);
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(f.graph.Phase(), GraphPhase::Failed);
}
TEST(GraphExecution, InitialAccessMismatchFailsBeforeCommands)
{
    ExecutionFixture f;
    auto desc = MakeBufferDesc();
    const auto physical = f.device.CreateBuffer(desc, {});
    f.graph.Export(f.graph.ImportBuffer(
        "WrongAccess", {physical, desc, ResourceAccess::CopyDestination, ResourceAccess::CopyDestination}));
    f.Clear();
    auto plan = f.graph.Compile();
    const auto events = f.device.Events().size();
    EXPECT_THROW(plan.Execute(f.device, f.frame), GraphCompileError);
    EXPECT_EQ(f.device.Events().size(), events);
}
TEST(GraphExecution, ClaimedDefinedImportRequiresActualContentProof)
{
    ExecutionFixture f;
    auto desc = MakeColorDesc();
    const auto physical = f.device.CreateTexture(desc);
    f.graph.Export(f.graph.ImportTexture("Unproven", {physical, desc, ResourceAccess::None, ResourceAccess::SampledRead,
                                                      ContentState::Defined, "owner", "claimed upload"}));
    f.Clear();
    auto plan = f.graph.Compile();
    const auto events = f.device.Events().size();
    EXPECT_THROW(plan.Execute(f.device, f.frame), UndefinedContentError);
    EXPECT_EQ(f.device.Events().size(), events);
}
TEST(GraphExecution, ForeignDeviceImportFailsWithoutPartialImports)
{
    ExecutionFixture f;
    TraceRhiDevice other;
    auto desc = MakeBufferDesc();
    auto physical = other.CreateBuffer(desc, {});
    f.graph.Export(
        f.graph.ImportBuffer("Foreign", {physical, desc, ResourceAccess::None, ResourceAccess::CopyDestination}));
    f.Clear();
    auto plan = f.graph.Compile();
    const auto events = f.device.Events().size();
    EXPECT_THROW(plan.Execute(f.device, f.frame), RhiException);
    EXPECT_EQ(f.device.Events().size(), events);
}
TEST(GraphExecution, EmptyFullWriteIsRejectedAndTransientReleased)
{
    ExecutionFixture f;
    f.Clear();
    auto buffer = f.graph.CreateBuffer("EmptyWrite", MakeBufferDesc());
    const auto written = AddBufferCopyWrite(f.graph, "FakeCopy", buffer);
    f.graph.AddPass<int>(
        "ObserveFakeCopy",
        [&](RgBuilder& b, int&)
        {
            (void)b.Read(written, ResourceAccess::CopySource);
            b.SideEffect("verify false full-write claim");
        },
        [](const int&, const RgResources&, IRhiCommandList&) {});
    auto plan = f.graph.Compile();
    const auto alive = f.device.Diagnostics().aliveObjects;
    EXPECT_THROW(plan.Execute(f.device, f.frame), UndefinedContentError);
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(f.device.Diagnostics().lastSubmittedSerial, 0U);
    EXPECT_EQ(f.graph.Phase(), GraphPhase::Failed);
}
TEST(GraphExecution, CompleteCopyActuallyDefinesBufferAndResolvesBothKinds)
{
    ExecutionFixture f;
    const auto desc = MakeBufferDesc();
    const std::array<std::byte, 32> bytes{};
    const auto physical = f.device.CreateBuffer(desc, bytes);
    const auto snapshot = f.device.QueryBufferState(f.frame, physical);
    const auto src =
        f.graph.ImportBuffer("Source", {physical, desc, snapshot.access, ResourceAccess::CopySource,
                                        ContentState::Defined, "test owner", "CreateBuffer full initial data"});
    auto dst = f.graph.CreateBuffer("Destination", desc);
    struct CopyData
    {
        RgBuffer source;
        RgBuffer destination;
    };
    f.graph.AddPass<CopyData>(
        "Copy",
        [&](RgBuilder& b, CopyData& d)
        {
            b.SideEffect("exercise copy content validation");
            d.source = b.Read(src, ResourceAccess::CopySource);
            dst = d.destination = b.Write(dst, ResourceAccess::CopyDestination, WriteCoverage::Full);
        },
        [](const CopyData& d, const RgResources& r, IRhiCommandList& commands)
        { commands.CopyBuffer({r.Get(d.source), 0, 32}, {r.Get(d.destination), 0, 32}, 32); });
    f.Clear();
    auto plan = f.graph.Compile();
    plan.Execute(f.device, f.frame);
    EXPECT_TRUE(f.device.QueryBufferState(f.frame, physical).fullyDefined);
    f.Finish();
}
TEST(GraphExecution, PartialCopyDoesNotSatisfyFullCoverage)
{
    ExecutionFixture f;
    const auto desc = MakeBufferDesc();
    const std::array<std::byte, 32> bytes{};
    const auto physical = f.device.CreateBuffer(desc, bytes);
    auto snapshot = f.device.QueryBufferState(f.frame, physical);
    const auto src = f.graph.ImportBuffer("Source", {physical, desc, snapshot.access, ResourceAccess::CopySource,
                                                     ContentState::Defined, "owner", "full initial data"});
    const auto dst = f.graph.CreateBuffer("Destination", desc);
    struct CopyData
    {
        RgBuffer source;
        RgBuffer destination;
    };
    f.graph.AddPass<CopyData>(
        "Partial",
        [&](RgBuilder& b, CopyData& d)
        {
            b.SideEffect("exercise copy content validation");
            d.source = b.Read(src, ResourceAccess::CopySource);
            d.destination = b.Write(dst, ResourceAccess::CopyDestination, WriteCoverage::Full);
        },
        [](const CopyData& d, const RgResources& r, IRhiCommandList& c)
        { c.CopyBuffer({r.Get(d.source), 0, 16}, {r.Get(d.destination), 0, 16}, 16); });
    f.Clear();
    auto plan = f.graph.Compile();
    EXPECT_THROW(plan.Execute(f.device, f.frame), UndefinedContentError);
}
TEST(GraphExecution, CallbackFailureClosesScopesAndDoesNotSubmitOrDestroyImports)
{
    ExecutionFixture f;
    f.Clear();
    const auto resource = f.graph.CreateTexture("FailTexture", MakeColorDesc());
    std::optional<RgResources> saved;
    f.graph.AddPass<TexturePassData>(
        "Failure",
        [&](RgBuilder& b, TexturePassData& d)
        {
            b.SideEffect("execute injected callback failure");
            d.texture = b.Write(resource, ResourceAccess::ColorWrite);
            b.SetColorAttachment(d.texture, LoadOp::Clear, StoreOp::Store);
        },
        [&](const TexturePassData&, const RgResources& r, IRhiCommandList&)
        {
            saved = r;
            throw std::runtime_error("callback failure");
        });
    auto plan = f.graph.Compile();
    const auto alive = f.device.Diagnostics().aliveObjects;
    EXPECT_THROW(plan.Execute(f.device, f.frame), std::runtime_error);
    EXPECT_EQ(f.graph.Phase(), GraphPhase::Failed);
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(f.device.Diagnostics().lastSubmittedSerial, 0U);
    EXPECT_EQ(f.device.QueryTextureState(f.frame, f.frame.backBuffer).access, ResourceAccess::ColorWrite);
    EXPECT_THROW((void)saved->Get(resource), GraphPhaseError);
    const auto trace = f.device.CanonicalTrace();
    EXPECT_NE(trace.rfind("EndRendering"), std::string::npos);
    EXPECT_NE(trace.rfind("EndLabel"), std::string::npos);
}
TEST(GraphExecution, OldPlanAfterResetAndOwnerDestructionCannotExecute)
{
    ExecutionFixture f;
    f.Clear();
    auto plan = f.graph.Compile();
    f.graph.Reset();
    EXPECT_THROW(plan.Execute(f.device, f.frame), StaleGraphHandleError);
    std::optional<CompiledRenderGraph> orphan;
    {
        RenderGraph owner;
        orphan = owner.Compile();
    }
    EXPECT_THROW(orphan->Execute(f.device, f.frame), StaleGraphHandleError);
}
TEST(GraphExecution, ActualPresentIdentityRejectsOrdinaryTextureBeforeCommands)
{
    ExecutionFixture f;
    auto desc = MakeColorDesc();
    const auto physical = f.device.CreateTexture(desc);
    const auto output =
        f.graph.ImportTexture("FakeBack", {physical, desc, ResourceAccess::None, ResourceAccess::Present});
    const auto written = AddAttachmentPass(f.graph, "FakeClear", output);
    f.graph.Present(written);
    f.Clear();
    auto plan = f.graph.Compile();
    const auto events = f.device.Events().size();
    EXPECT_THROW(plan.Execute(f.device, f.frame), GraphCompileError);
    EXPECT_EQ(f.device.Events().size(), events);
}
struct ResourceSetExecutionFixture
{
    TraceRhiDevice device;
    SwapChainHandle chain;
    FrameToken frame;
    ResourceSetLayoutHandle layout;
    RenderGraph graph;
    RgTexture back;

    ResourceSetExecutionFixture()
    {
        ResourceSetLayoutDesc layoutDesc;
        layoutDesc.set = 0;
        layoutDesc.entries.push_back({0, BindingType::SampledTexture, 1, ShaderStage::Pixel});
        layout = device.CreateResourceSetLayout(layoutDesc);
        chain = device.CreateSwapChain({{32, 24}});
        frame = device.BeginFrame(chain);
        const auto snapshot = device.QueryTextureState(frame, frame.backBuffer);
        back = graph.ImportTexture("BackBuffer", {frame.backBuffer,
                                                  snapshot.descriptor,
                                                  snapshot.access,
                                                  ResourceAccess::Present,
                                                  ContentState::Undefined,
                                                  "chain",
                                                  {}});
    }

    ResourceSetDesc SetDesc(TextureHandle texture) const
    {
        ResourceSetDesc desc;
        desc.layout = layout;
        desc.bindings.push_back({0, 0, BindingType::SampledTexture, {}, texture, {}});
        desc.debugName = "graph.frame-set";
        return desc;
    }

    void Clear()
    {
        back = AddAttachmentPass(graph, "ClearBack", back);
        graph.Present(back);
    }

    void Finish()
    {
        device.EndFrame(frame, chain);
        device.CompleteThrough(frame.serial);
        device.Shutdown();
        EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
        EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
    }
};

TEST(GraphExecution, ResolverCreatesFrameResourceSetForDeclaredTransientRead)
{
    ResourceSetExecutionFixture f;
    auto transient = f.graph.CreateTexture("TransientSample", MakeColorDesc());
    RgTexture written;
    f.graph.AddPass<TexturePassData>(
        "WriteTransient",
        [&](RgBuilder& b, TexturePassData& data)
        {
            written = data.texture = b.Write(transient, ResourceAccess::ColorWrite);
            b.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store);
            b.SideEffect("produce transient sampled input");
        },
        [](const TexturePassData&, const RgResources&, IRhiCommandList&) {});
    bool created = false;
    f.graph.AddPass<TexturePassData>(
        "BindTransient",
        [&](RgBuilder& b, TexturePassData& data)
        {
            data.texture = b.Read(written, ResourceAccess::SampledRead);
            b.SideEffect("bind transient sampled input");
        },
        [&](const TexturePassData& data, const RgResources& resources, IRhiCommandList&)
        {
            const auto physical = resources.Get(data.texture);
            created = resources.CreateResourceSet(f.SetDesc(physical)).IsValid();
        });
    f.Clear();
    auto plan = f.graph.Compile();
    plan.Execute(f.device, f.frame);
    EXPECT_TRUE(created);
    EXPECT_NE(f.device.CanonicalTrace().find("CreateFrameResourceSet"), std::string::npos);
    f.Finish();
}

TEST(GraphExecution, ResourceSetResolverRejectsExpiredResolver)
{
    ResourceSetExecutionFixture f;
    std::optional<RgResources> saved;
    f.graph.AddPass<int>(
        "SaveResolver", [](RgBuilder& b, int&) { b.SideEffect("save resolver for phase check"); },
        [&](const int&, const RgResources& resources, IRhiCommandList&) { saved = resources; });
    f.Clear();
    auto plan = f.graph.Compile();
    plan.Execute(f.device, f.frame);
    ASSERT_TRUE(saved.has_value());
    EXPECT_THROW((void)saved->CreateResourceSet(f.SetDesc(TextureHandle{})), GraphPhaseError);
    f.Finish();
}

TEST(GraphExecution, ResourceSetResolverRejectsUndeclaredFrameGraphResourceAndReleasesTransient)
{
    ResourceSetExecutionFixture f;
    const auto first = f.graph.CreateTexture("First", MakeColorDesc());
    const auto second = f.graph.CreateTexture("Second", MakeColorDesc());
    std::optional<TextureHandle> secondPhysical;
    f.graph.AddPass<TexturePassData>(
        "WriteSecond",
        [&](RgBuilder& b, TexturePassData& data)
        {
            data.texture = b.Write(second, ResourceAccess::ColorWrite);
            b.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store);
            b.SideEffect("capture another pass physical resource");
        },
        [&](const TexturePassData& data, const RgResources& resources, IRhiCommandList&)
        { secondPhysical = resources.Get(data.texture); });
    RgTexture firstWritten;
    f.graph.AddPass<TexturePassData>(
        "WriteFirst",
        [&](RgBuilder& b, TexturePassData& data)
        {
            firstWritten = data.texture = b.Write(first, ResourceAccess::ColorWrite);
            b.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store);
            b.SideEffect("produce declared input");
        },
        [](const TexturePassData&, const RgResources&, IRhiCommandList&) {});
    f.graph.AddPass<TexturePassData>(
        "BindUndeclared",
        [&](RgBuilder& b, TexturePassData& data)
        {
            data.texture = b.Read(firstWritten, ResourceAccess::SampledRead);
            b.SideEffect("reject another pass resource binding");
        },
        [&](const TexturePassData&, const RgResources& resources, IRhiCommandList&)
        {
            ASSERT_TRUE(secondPhysical.has_value());
            (void)resources.CreateResourceSet(f.SetDesc(*secondPhysical));
        });
    f.Clear();
    auto plan = f.graph.Compile();
    const auto alive = f.device.Diagnostics().aliveObjects;
    EXPECT_THROW(plan.Execute(f.device, f.frame), GraphCompileError);
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(f.device.CanonicalTrace().find("CreateFrameResourceSet"), std::string::npos);
}

TEST(GraphExecution, ResourceSetResolverRejectsWriteAccessAndReleasesTransient)
{
    ResourceSetExecutionFixture f;
    const auto transient = f.graph.CreateTexture("WriteOnly", MakeColorDesc());
    f.graph.AddPass<TexturePassData>(
        "WriteOnlyBinding",
        [&](RgBuilder& b, TexturePassData& data)
        {
            data.texture = b.Write(transient, ResourceAccess::ColorWrite);
            b.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store);
            b.SideEffect("reject write access as sampled binding");
        },
        [&](const TexturePassData& data, const RgResources& resources, IRhiCommandList&)
        {
            const auto physical = resources.Get(data.texture);
            (void)resources.CreateResourceSet(f.SetDesc(physical));
        });
    f.Clear();
    auto plan = f.graph.Compile();
    const auto alive = f.device.Diagnostics().aliveObjects;
    EXPECT_THROW(plan.Execute(f.device, f.frame), GraphCompileError);
    EXPECT_EQ(f.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(f.device.CanonicalTrace().find("CreateFrameResourceSet"), std::string::npos);
}

} // namespace
