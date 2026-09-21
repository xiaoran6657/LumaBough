#include "RenderGraphTestFixtures.h"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace MiniEngine::RenderGraph::Tests
{
namespace
{
void AddEffect(RenderGraph& graph, std::string_view name)
{
    graph.AddPass<int>(
        name, [](RgBuilder& builder, int&) { builder.SideEffect("observable CPU fixture"); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}
void AddLiveRead(RenderGraph& graph, std::string_view name, RgTexture input,
                 Rhi::ShaderStage stages = Rhi::ShaderStage::Pixel)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(input, Rhi::ResourceAccess::SampledRead, stages);
            builder.SideEffect("consume texture");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}
void AddLiveRead(RenderGraph& graph, std::string_view name, RgBuffer input)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(input, Rhi::ResourceAccess::VertexRead);
            builder.SideEffect("consume buffer");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}
RgTexture ImportColor(RenderGraph& graph, std::string_view name = "external",
                      Rhi::ResourceAccess finalAccess = Rhi::ResourceAccess::SampledRead,
                      Rhi::TextureHandle physical = {0, 1, 1})
{
    // 只验证编译声明；该句柄不交给 device，也不代表真实 GPU 内容证据。
    return graph.ImportTexture(name, {physical, MakeColorDesc(), Rhi::ResourceAccess::CopyDestination, finalAccess,
                                      ContentState::Defined, "external owner", "CPU fixture"});
}
std::vector<std::uint32_t> Order(const CompiledRenderGraph& plan)
{
    return {plan.ExecutionOrder().begin(), plan.ExecutionOrder().end()};
}
void ExpectEdge(const CompiledRenderGraph& plan, std::uint32_t from, std::uint32_t to, std::uint32_t resource,
                std::uint32_t version, Hazard hazard, bool contributes)
{
    const DependencyEdge expected{from, to, resource, version, hazard, contributes};
    EXPECT_NE(std::find(plan.Dependencies().begin(), plan.Dependencies().end(), expected), plan.Dependencies().end());
}
} // namespace

TEST(GraphCompiler, EmptyGraphEmitsAllTenStagesWithoutResources)
{
    RenderGraph graph;
    const auto plan = graph.Compile();
    constexpr std::array expected{CompileStage::ValidateDeclarations,
                                  CompileStage::CreateNodes,
                                  CompileStage::CreateDependencies,
                                  CompileStage::FindRoots,
                                  CompileStage::Cull,
                                  CompileStage::TopologicalSort,
                                  CompileStage::Lifetimes,
                                  CompileStage::PhysicalSlots,
                                  CompileStage::AccessTransitions,
                                  CompileStage::EmitPlan};
    EXPECT_TRUE(
        std::equal(plan.CompilationStages().begin(), plan.CompilationStages().end(), expected.begin(), expected.end()));
    EXPECT_TRUE(plan.ExecutionOrder().empty());
    EXPECT_TRUE(plan.ResourceVersions().empty());
    EXPECT_TRUE(plan.PhysicalAllocations().empty());
    EXPECT_TRUE(plan.Transitions().empty());
    EXPECT_EQ(graph.Phase(), GraphPhase::Compiled);
}
TEST(GraphCompiler, ThreeDeclaredOneLiveTwoCulledHaveExplicitReasons)
{
    RenderGraph graph;
    (void)AddAttachmentPass(graph, "dead color", graph.CreateTexture("color", MakeColorDesc()));
    (void)AddBufferCopyWrite(graph, "dead buffer", graph.CreateBuffer("buffer", MakeBufferDesc()));
    AddEffect(graph, "root");
    const auto plan = graph.Compile();
    const auto stats = plan.Statistics();
    EXPECT_EQ(stats.declaredPasses, 3U);
    EXPECT_EQ(stats.livePasses, 1U);
    EXPECT_EQ(stats.culledPasses, 2U);
    EXPECT_EQ(stats.liveResources, 0U);
    EXPECT_EQ(stats.physicalResources, 0U);
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{2}));
    ASSERT_EQ(plan.Passes().size(), 3U);
    for (std::size_t index = 0; index < 2; ++index)
    {
        EXPECT_FALSE(plan.Passes()[index].live);
        EXPECT_EQ(plan.Passes()[index].executionOrder, kInvalidGraphIndex);
        EXPECT_EQ(plan.Passes()[index].culledReason, "not_reachable_from_output_roots");
    }
    EXPECT_TRUE(plan.Passes()[2].live);
}
TEST(GraphCompiler, DeadConsumerCullsItsEntireProducerChain)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "clear", color);
    color = AddAttachmentPass(graph, "load", color, Rhi::LoadOp::Load);
    AddSamplePass(graph, "unobserved read", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(plan.Statistics().culledPasses, 3U);
    EXPECT_EQ(plan.Statistics().livePasses, 0U);
    EXPECT_TRUE(plan.Lifetimes().empty());
    EXPECT_TRUE(plan.Transitions().empty());
    EXPECT_TRUE(std::none_of(plan.ResourceVersions().begin(), plan.ResourceVersions().end(),
                             [](const auto& version) { return version.live; }));
}
TEST(GraphCompiler, SideEffectReadKeepsRawProducer)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "producer", color);
    AddLiveRead(graph, "consumer", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{0, 1}));
    EXPECT_EQ(plan.Statistics().roots, 1U);
    ASSERT_EQ(plan.Dependencies().size(), 1U);
    ExpectEdge(plan, 0, 1, color.Resource(), 1, Hazard::ReadAfterWrite, true);
}
TEST(GraphCompiler, ImportedOrdinaryReadIsNotAnOutputRoot)
{
    RenderGraph graph;
    AddSamplePass(graph, "ordinary read", ImportColor(graph));
    const auto plan = graph.Compile();
    EXPECT_EQ(plan.Statistics().roots, 0U);
    EXPECT_EQ(plan.Statistics().culledPasses, 1U);
    EXPECT_TRUE(plan.Dependencies().empty());
    EXPECT_TRUE(plan.PhysicalAllocations().empty());
    EXPECT_TRUE(plan.Transitions().empty());
}
TEST(GraphCompiler, ImportedVersionZeroHasExternalProducerAndRecordedReader)
{
    RenderGraph graph;
    const auto color = ImportColor(graph);
    AddLiveRead(graph, "consumer", color);
    const auto plan = graph.Compile();
    ASSERT_EQ(plan.ResourceVersions().size(), 1U);
    const auto& version = plan.ResourceVersions()[0];
    EXPECT_EQ(version.version, 0U);
    EXPECT_EQ(version.producer, kInvalidGraphIndex);
    EXPECT_TRUE(version.importedProducer);
    EXPECT_TRUE(version.live);
    ASSERT_EQ(version.readersCount, 1U);
    ASSERT_LT(version.readersBegin, plan.VersionReaders().size());
    EXPECT_EQ(plan.VersionReaders()[version.readersBegin], 0U);
    EXPECT_TRUE(plan.Dependencies().empty());
}
TEST(GraphCompiler, ExportVersionZeroKeepsImportedResourceWithoutPass)
{
    RenderGraph graph;
    const auto color = ImportColor(graph);
    graph.Export(color);
    const auto plan = graph.Compile();
    EXPECT_EQ(plan.Statistics().livePasses, 0U);
    EXPECT_EQ(plan.Statistics().roots, 1U);
    EXPECT_EQ(plan.Statistics().liveResources, 1U);
    EXPECT_EQ(plan.Statistics().physicalResources, 1U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 0U);
    ASSERT_EQ(plan.Transitions().size(), 1U);
    EXPECT_TRUE(plan.Transitions()[0].final);
    EXPECT_EQ(plan.Transitions()[0].resource, color.Resource());
    EXPECT_EQ(plan.Transitions()[0].before, Rhi::ResourceAccess::CopyDestination);
    EXPECT_EQ(plan.Transitions()[0].after, Rhi::ResourceAccess::SampledRead);
    ASSERT_EQ(plan.ResourceVersions().size(), 1U);
    EXPECT_TRUE(plan.ResourceVersions()[0].live);
}
TEST(GraphCompiler, PresentAndExportKeepFinalWriters)
{
    RenderGraph graph;
    auto back = ImportColor(graph, "back", Rhi::ResourceAccess::Present, {0, 1, 1});
    auto exported = ImportColor(graph, "export", Rhi::ResourceAccess::SampledRead, {1, 1, 1});
    back = AddAttachmentPass(graph, "present writer", back);
    exported = AddAttachmentPass(graph, "export writer", exported);
    graph.Present(back);
    graph.Export(exported);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{0, 1}));
    EXPECT_EQ(plan.Statistics().roots, 2U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 0U);
    EXPECT_EQ(std::count_if(plan.Transitions().begin(), plan.Transitions().end(),
                            [](const auto& transition) { return transition.final; }),
              2);
}
TEST(GraphCompiler, ClearOverwriteDoesNotKeepOldWriter)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "obsolete writer", color);
    color = AddAttachmentPass(graph, "clear replacement", color);
    AddLiveRead(graph, "output", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{1, 2}));
    EXPECT_EQ(plan.Statistics().culledPasses, 1U);
    ExpectEdge(plan, 0, 1, color.Resource(), 1, Hazard::WriteAfterWrite, false);
    ExpectEdge(plan, 1, 2, color.Resource(), 2, Hazard::ReadAfterWrite, true);
}
TEST(GraphCompiler, AttachmentLoadKeepsOldWriterAsContentDependency)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "initialize", color);
    color = AddAttachmentPass(graph, "preserve", color, Rhi::LoadOp::Load);
    AddLiveRead(graph, "output", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{0, 1, 2}));
    ExpectEdge(plan, 0, 1, color.Resource(), 1, Hazard::WriteAfterWrite, true);
}
TEST(GraphCompiler, FullBufferCopyCullsOldWriterButPreserveCopyRetainsIt)
{
    for (const auto coverage : {WriteCoverage::Full, WriteCoverage::Preserve})
    {
        RenderGraph graph;
        auto buffer = graph.CreateBuffer("buffer", MakeBufferDesc());
        buffer = AddBufferCopyWrite(graph, "initialize", buffer);
        buffer = AddBufferCopyWrite(graph, "update", buffer, coverage);
        AddLiveRead(graph, "output", buffer);
        const auto plan = graph.Compile();
        const bool preserve = coverage == WriteCoverage::Preserve;
        EXPECT_EQ(plan.Statistics().livePasses, preserve ? 3U : 2U);
        ExpectEdge(plan, 0, 1, buffer.Resource(), 1, Hazard::WriteAfterWrite, preserve);
    }
}
TEST(GraphCompiler, WarDoesNotResurrectDeadReaderOrOverwrittenProducer)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "obsolete producer", color);
    AddSamplePass(graph, "dead reader", color);
    color = AddAttachmentPass(graph, "replacement", color);
    AddLiveRead(graph, "output", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{2, 3}));
    ExpectEdge(plan, 1, 2, color.Resource(), 1, Hazard::WriteAfterRead, false);
    EXPECT_EQ(plan.Statistics().culledPasses, 2U);
}
TEST(GraphCompiler, AllEarlierLiveReadersPrecedeOverwrite)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "initialize", color);
    AddLiveRead(graph, "reader one", color);
    AddLiveRead(graph, "reader two", color);
    color = AddAttachmentPass(graph, "replacement", color);
    AddLiveRead(graph, "new reader", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(plan.Dependencies().size(), 6U);
    ExpectEdge(plan, 1, 3, color.Resource(), 1, Hazard::WriteAfterRead, false);
    ExpectEdge(plan, 2, 3, color.Resource(), 1, Hazard::WriteAfterRead, false);
    ExpectEdge(plan, 0, 3, color.Resource(), 1, Hazard::WriteAfterWrite, false);
}
TEST(GraphCompiler, RepeatedReadMergesStagesAndDoesNotDuplicateEdgesOrReaders)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "producer", color);
    graph.AddPass<int>(
        "consumer",
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(color, Rhi::ResourceAccess::SampledRead, Rhi::ShaderStage::Vertex);
            (void)builder.Read(color, Rhi::ResourceAccess::SampledRead, Rhi::ShaderStage::Pixel);
            builder.SideEffect("two stages");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    const auto plan = graph.Compile();
    ASSERT_EQ(plan.Dependencies().size(), 1U);
    ASSERT_EQ(plan.VersionReaders().size(), 1U);
    ASSERT_EQ(plan.Transitions().size(), 2U);
    EXPECT_EQ(plan.Transitions()[1].stages, Rhi::ShaderStage::Vertex | Rhi::ShaderStage::Pixel);
}
TEST(GraphCompiler, SimultaneouslyReadyPassesUseDeclarationIndex)
{
    RenderGraph graph;
    const auto color = AddAttachmentPass(graph, "producer", graph.CreateTexture("color", MakeColorDesc()));
    AddLiveRead(graph, "newly ready reader", color);
    AddEffect(graph, "already ready independent root");
    const auto plan = graph.Compile();
    // producer 完成后 index 1 刚入队，仍必须先于已经 ready 的 index 2。
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{0, 1, 2}));
}
TEST(GraphCompiler, LifetimesUseDenseLiveOrderAndExcludeDeadResources)
{
    RenderGraph graph;
    const auto dead = graph.CreateTexture("dead", MakeColorDesc());
    (void)AddAttachmentPass(graph, "dead producer", dead);
    auto live = graph.CreateTexture("live", MakeColorDesc());
    live = AddAttachmentPass(graph, "live producer", live);
    AddSamplePass(graph, "dead read", live);
    AddLiveRead(graph, "live consumer", live);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{1, 3}));
    ASSERT_EQ(plan.Lifetimes().size(), 1U);
    EXPECT_EQ(plan.Lifetimes()[0].resource, live.Resource());
    EXPECT_EQ(plan.Lifetimes()[0].firstUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[0].lastUse, 1U);
    ASSERT_EQ(plan.PhysicalAllocations().size(), 1U);
    EXPECT_EQ(plan.PhysicalAllocations()[0].resource, live.Resource());
    EXPECT_EQ(plan.Lifetimes()[0].physicalSlot, plan.PhysicalAllocations()[0].slot);
}
TEST(GraphCompiler, LogicalVersionsShareOneSlotAndNonoverlappingResourcesReuseWholeObject)
{
    RenderGraph graph;
    auto first = graph.CreateBuffer("first", MakeBufferDesc());
    first = AddBufferCopyWrite(graph, "first initialize", first);
    first = AddBufferCopyWrite(graph, "first preserve", first, WriteCoverage::Preserve);
    AddLiveRead(graph, "first output", first);
    auto second = graph.CreateBuffer("second", MakeBufferDesc());
    second = AddBufferCopyWrite(graph, "second initialize", second);
    AddLiveRead(graph, "second output", second);
    const auto plan = graph.Compile();
    EXPECT_EQ(plan.Statistics().resourceVersions, 5U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 1U);
    ASSERT_EQ(plan.PhysicalAllocations().size(), 1U);
    ASSERT_EQ(plan.Lifetimes().size(), 2U);
    EXPECT_EQ(plan.Lifetimes()[0].physicalSlot, plan.Lifetimes()[1].physicalSlot);
    for (const auto& transition : plan.Transitions())
        EXPECT_EQ(transition.physicalSlot, plan.PhysicalAllocations()[0].slot);
}
TEST(GraphCompiler, AccessPlannerSkipsCulledReadsAndStartsAtLiveWriter)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "obsolete clear", color);
    AddSamplePass(graph, "dead sample", color);
    color = AddAttachmentPass(graph, "live clear", color);
    AddLiveRead(graph, "output", color);
    const auto plan = graph.Compile();
    ASSERT_EQ(plan.Transitions().size(), 2U);
    EXPECT_EQ(plan.Transitions()[0].pass, 2U);
    EXPECT_EQ(plan.Transitions()[0].executionOrder, 0U);
    EXPECT_EQ(plan.Transitions()[0].before, Rhi::ResourceAccess::None);
    EXPECT_EQ(plan.Transitions()[0].after, Rhi::ResourceAccess::ColorWrite);
    EXPECT_EQ(plan.Transitions()[1].pass, 3U);
    EXPECT_EQ(plan.Transitions()[1].before, Rhi::ResourceAccess::ColorWrite);
    EXPECT_EQ(plan.Transitions()[1].after, Rhi::ResourceAccess::SampledRead);
}
TEST(GraphCompiler, DeadUndefinedReadStillReturnsContextAndNoPlan)
{
    RenderGraph graph;
    const auto color = graph.CreateTexture("undefined", MakeColorDesc());
    AddSamplePass(graph, "dead invalid reader", color);
    const auto result = graph.TryCompile();
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.plan.has_value());
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::UndefinedContent);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_EQ(result.error->pass, 0U);
    EXPECT_EQ(result.error->resource, color.Resource());
    EXPECT_EQ(result.error->version, 0U);
    EXPECT_EQ(result.error->passName, "dead invalid reader");
    EXPECT_EQ(result.error->resourceName, "undefined");
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
}
TEST(GraphCompiler, DeadUndefinedLoadIsRejectedBeforeCulling)
{
    RenderGraph graph;
    (void)AddAttachmentPass(graph, "dead invalid load", graph.CreateTexture("undefined", MakeColorDesc()),
                            Rhi::LoadOp::Load);
    const auto result = graph.TryCompile();
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::UndefinedContent);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_FALSE(result.plan);
}
TEST(GraphCompiler, TryCompileCapturesDeclarationFailureInsideCallback)
{
    RenderGraph graph;
    const auto result = graph.TryCompile(
        [](RenderGraph& target)
        {
            auto descriptor = MakeColorDesc();
            descriptor.extent.width = 0;
            (void)target.CreateTexture("invalid descriptor", descriptor);
        });
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::InvalidDeclaration);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_FALSE(result.error->message.empty());
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
}
TEST(GraphCompiler, TryCompileClassifiesForeignHandleAndResetRecovers)
{
    RenderGraph foreign;
    const auto texture = foreign.CreateTexture("foreign", MakeColorDesc());
    RenderGraph graph;
    const auto failure =
        graph.TryCompile([&](RenderGraph& target) { (void)AddAttachmentPass(target, "wrong owner", texture); });
    ASSERT_TRUE(failure.error);
    EXPECT_EQ(failure.error->code, GraphErrorCode::StaleHandle);
    EXPECT_EQ(failure.error->passName, "wrong owner");
    EXPECT_FALSE(failure.plan);
    graph.Reset();
    auto success = graph.TryCompile([](RenderGraph& target) { AddEffect(target, "recovered"); });
    ASSERT_TRUE(success);
    EXPECT_FALSE(success.error);
    EXPECT_EQ(success.plan->Statistics().livePasses, 1U);
}
TEST(GraphCompiler, TryCompileRejectsTransientExportOwnership)
{
    RenderGraph graph;
    const auto result = graph.TryCompile([](RenderGraph& target)
                                         { target.Export(target.CreateBuffer("transient", MakeBufferDesc())); });
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::InvalidDeclaration);
    EXPECT_EQ(result.error->resourceName, "transient");
}
TEST(GraphCompiler, CompileDoesNotInvokeEvenLiveCallbacks)
{
    RenderGraph graph;
    std::uint32_t calls = 0;
    for (const bool live : {false, true})
        graph.AddPass<int>(
            live ? "live" : "dead",
            [&](RgBuilder& builder, int&)
            {
                if (live)
                    builder.SideEffect("visible");
            },
            [&](const int&, const RgResources&, Rhi::IRhiCommandList&) { ++calls; });
    const auto plan = graph.Compile();
    EXPECT_EQ(calls, 0U);
    EXPECT_EQ(plan.Statistics().livePasses, 1U);
    EXPECT_EQ(plan.Statistics().culledPasses, 1U);
}
TEST(GraphCompiler, HashAndAllFourDumpsMatchAcrossOneHundredCompiles)
{
    RenderGraph graph;
    GraphDumpBundle baseline;
    std::uint64_t hash = 0;
    const GraphDumpContext context{17, "Debug", "fixed-commit"};
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        SCOPED_TRACE(iteration);
        graph.Reset();
        graph.Reserve({16, 8});
        auto color = graph.CreateTexture("color \"quoted\"\n\\", MakeColorDesc());
        color = AddAttachmentPass(graph, "dead writer", color);
        color = AddAttachmentPass(graph, "live writer", color);
        AddLiveRead(graph, "consumer", color);
        const auto plan = graph.Compile();
        const auto dumps = plan.Dumps(context);
        if (iteration == 0)
        {
            hash = plan.Statistics().planHash;
            baseline = dumps;
            EXPECT_FALSE(dumps.frameGraphJson.empty());
            EXPECT_FALSE(dumps.dot.empty());
            EXPECT_FALSE(dumps.accessPlanJson.empty());
            EXPECT_FALSE(dumps.transientPlanJson.empty());
        }
        EXPECT_EQ(plan.Statistics().planHash, hash);
        EXPECT_EQ(dumps.frameGraphJson, baseline.frameGraphJson);
        EXPECT_EQ(dumps.dot, baseline.dot);
        EXPECT_EQ(dumps.accessPlanJson, baseline.accessPlanJson);
        EXPECT_EQ(dumps.transientPlanJson, baseline.transientPlanJson);
    }
}
TEST(GraphCompiler, PlanHashIgnoresGraphIdentityPhysicalHandlesAndDumpMetadata)
{
    std::array<std::uint64_t, 2> hashes{};
    for (std::uint32_t index = 0; index < hashes.size(); ++index)
    {
        RenderGraph graph;
        graph.Reserve({index * 40, index * 20});
        auto color =
            ImportColor(graph, "external", Rhi::ResourceAccess::SampledRead, {index + 2, index + 7, index + 101});
        color = AddAttachmentPass(graph, "clear", color);
        graph.Export(color);
        const auto plan = graph.Compile();
        hashes[index] = plan.Statistics().planHash;
        (void)plan.Dumps({index, index == 0 ? "D3D11 Debug" : "D3D12 Release", std::to_string(index)});
        EXPECT_EQ(plan.Statistics().planHash, hashes[index]);
    }
    EXPECT_EQ(hashes[0], hashes[1]);
}
TEST(GraphCompiler, PlanHashChangesWithDescriptorShaderStagesAndAttachmentClear)
{
    std::array<std::uint64_t, 4> hashes{};
    for (std::size_t variant = 0; variant < hashes.size(); ++variant)
    {
        RenderGraph graph;
        auto descriptor = MakeColorDesc();
        if (variant == 1)
            ++descriptor.extent.width;
        auto color = graph.CreateTexture("color", descriptor);
        graph.AddPass<int>(
            "clear",
            [&](RgBuilder& builder, int&)
            {
                color = builder.Write(color, Rhi::ResourceAccess::ColorWrite);
                builder.SetColorAttachment(color, Rhi::LoadOp::Clear, Rhi::StoreOp::Store,
                                           {variant == 3 ? 0.5F : 0.0F, 0, 0, 1});
            },
            [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
        AddLiveRead(graph, "consumer", color, variant == 2 ? Rhi::ShaderStage::Vertex : Rhi::ShaderStage::Pixel);
        const auto plan = graph.Compile();
        hashes[variant] = plan.Statistics().planHash;
    }
    for (std::size_t first = 0; first < hashes.size(); ++first)
        for (std::size_t second = first + 1; second < hashes.size(); ++second)
            EXPECT_NE(hashes[first], hashes[second]);
}
TEST(GraphCompiler, PlanHashChangesWithOutputRootAndOldContentContract)
{
    std::array<std::uint64_t, 3> hashes{};
    for (std::size_t variant = 0; variant < hashes.size(); ++variant)
    {
        RenderGraph graph;
        auto color = graph.CreateTexture("color", MakeColorDesc());
        color = AddAttachmentPass(graph, "initialize", color);
        color = AddAttachmentPass(graph, "update", color, variant == 2 ? Rhi::LoadOp::Load : Rhi::LoadOp::Clear);
        if (variant == 0)
            AddSamplePass(graph, "consumer", color);
        else
            AddLiveRead(graph, "consumer", color);
        hashes[variant] = graph.Compile().Statistics().planHash;
    }
    EXPECT_NE(hashes[0], hashes[1]);
    EXPECT_NE(hashes[1], hashes[2]);
}
TEST(GraphCompiler, CompleteTextureCopyDoesNotKeepObsoleteAttachmentWriter)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "obsolete attachment", color);
    graph.AddPass<int>(
        "complete copy", [&](RgBuilder& builder, int&)
        { color = builder.Write(color, Rhi::ResourceAccess::CopyDestination, WriteCoverage::Full); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    AddLiveRead(graph, "output", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{1, 2}));
    ExpectEdge(plan, 0, 1, color.Resource(), 1, Hazard::WriteAfterWrite, false);
    ASSERT_EQ(plan.Transitions().size(), 2U);
    EXPECT_EQ(plan.Transitions()[0].before, Rhi::ResourceAccess::None);
    EXPECT_EQ(plan.Transitions()[0].after, Rhi::ResourceAccess::CopyDestination);
}
TEST(GraphCompiler, IndependentSideEffectKeepsAnOtherwiseOverwrittenWriter)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    graph.AddPass<int>(
        "observable initial writer",
        [&](RgBuilder& builder, int&)
        {
            color = builder.Write(color, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(color, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
            builder.SideEffect("external observation of initial write");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    color = AddAttachmentPass(graph, "replacement", color);
    AddLiveRead(graph, "output", color);
    const auto plan = graph.Compile();
    EXPECT_EQ(Order(plan), (std::vector<std::uint32_t>{0, 1, 2}));
    EXPECT_EQ(plan.Statistics().roots, 2U);
    ExpectEdge(plan, 0, 1, color.Resource(), 1, Hazard::WriteAfterWrite, false);
}
} // namespace MiniEngine::RenderGraph::Tests
