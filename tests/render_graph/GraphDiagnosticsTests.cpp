#include "GraphState.h"
#include "RenderGraphTestFixtures.h"
#include "TraceRhi.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>

namespace MiniEngine::RenderGraph::Tests
{
namespace
{
using MiniEngine::Tests::TraceRhiDevice;

void DeclareDiagnosticGraph(RenderGraph& graph, std::uint32_t physicalIdentity = 1, std::uint32_t width = 32,
                            Rhi::ShaderStage stages = Rhi::ShaderStage::Pixel)
{
    auto descriptor = MakeColorDesc();
    descriptor.extent.width = width;
    const auto dead = graph.CreateTexture("dead", descriptor);
    (void)AddAttachmentPass(graph, "dead writer", dead);
    auto source = graph.CreateTexture("source \"quoted\"\\\n\t\x01", descriptor);
    source = AddAttachmentPass(graph, "producer", source);
    auto back = graph.ImportTexture(
        "back",
        {{physicalIdentity, 1, physicalIdentity}, descriptor, Rhi::ResourceAccess::None, Rhi::ResourceAccess::Present});
    graph.AddPass<int>(
        "present writer",
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(source, Rhi::ResourceAccess::SampledRead, stages);
            back = builder.Write(back, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(back, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    graph.Present(back);
    graph.AddPass<int>(
        "side effect",
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(source, Rhi::ResourceAccess::SampledRead);
            builder.SideEffect("observable \"read\"\\\n");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    const auto exported = graph.ImportBuffer("exported buffer", {{physicalIdentity, 1, physicalIdentity},
                                                                 MakeBufferDesc(),
                                                                 Rhi::ResourceAccess::VertexRead,
                                                                 Rhi::ResourceAccess::CopySource,
                                                                 ContentState::Defined,
                                                                 "buffer owner",
                                                                 "full external upload"});
    graph.Export(exported);
}

void ExpectSameDumps(const GraphDumpBundle& actual, const GraphDumpBundle& expected)
{
    EXPECT_EQ(actual.frameGraphJson, expected.frameGraphJson);
    EXPECT_EQ(actual.dot, expected.dot);
    EXPECT_EQ(actual.accessPlanJson, expected.accessPlanJson);
    EXPECT_EQ(actual.transientPlanJson, expected.transientPlanJson);
}

struct DiagnosticFrame
{
    TraceRhiDevice device;
    Rhi::SwapChainHandle chain;
    Rhi::FrameToken frame;
    RenderGraph graph;
    RgTexture back;
    DiagnosticFrame()
    {
        chain = device.CreateSwapChain({{32, 24}});
        frame = device.BeginFrame(chain);
        const auto actual = device.QueryTextureState(frame, frame.backBuffer);
        back = graph.ImportTexture("back", {frame.backBuffer,
                                            actual.descriptor,
                                            actual.access,
                                            Rhi::ResourceAccess::Present,
                                            ContentState::Undefined,
                                            "chain",
                                            {}});
    }
    void ClearAndPresent()
    {
        back = AddAttachmentPass(graph, "clear back", back);
        graph.Present(back);
    }
    void FinishSuccessfulFrame()
    {
        device.EndFrame(frame, chain);
        device.CompleteThrough(frame.serial);
        device.Shutdown();
        EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
        EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
    }
};
} // namespace

TEST(GraphDiagnostics, CallbackConflictReportsSetupPassResourceAndVersion)
{
    RenderGraph graph;
    const auto result = graph.TryCompile(
        [](RenderGraph& target)
        {
            const auto texture = target.CreateTexture("conflicted", MakeColorDesc());
            target.AddPass<int>(
                "conflict",
                [&](RgBuilder& builder, int&)
                {
                    (void)builder.Read(texture, Rhi::ResourceAccess::SampledRead);
                    (void)builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
                },
                [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
        });
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::InvalidDeclaration);
    EXPECT_EQ(result.error->phase, GraphPhase::Setup);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_EQ(result.error->pass, 0U);
    EXPECT_EQ(result.error->resource, 0U);
    EXPECT_EQ(result.error->version, 0U);
    EXPECT_EQ(result.error->passName, "conflict");
    EXPECT_EQ(result.error->resourceName, "conflicted");
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
}
TEST(GraphDiagnostics, StaleReadInDeclarationCallbackReturnsStaleHandle)
{
    RenderGraph graph;
    const auto result = graph.TryCompile(
        [](RenderGraph& target)
        {
            const auto old = target.CreateTexture("versions", MakeColorDesc());
            (void)AddAttachmentPass(target, "writer", old);
            AddSamplePass(target, "stale reader", old);
        });
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::StaleHandle);
    EXPECT_EQ(result.error->phase, GraphPhase::Setup);
    EXPECT_EQ(result.error->pass, 1U);
    EXPECT_EQ(result.error->resource, 0U);
    EXPECT_EQ(result.error->version, 0U);
    EXPECT_EQ(result.error->passName, "stale reader");
}
TEST(GraphDiagnostics, DescriptorFailureCannotPublishPartiallyDeclaredPlan)
{
    RenderGraph graph;
    const auto result = graph.TryCompile(
        [](RenderGraph& target)
        {
            (void)AddAttachmentPass(target, "valid declaration", target.CreateTexture("valid", MakeColorDesc()));
            auto invalid = MakeColorDesc();
            invalid.extent.height = 0;
            (void)target.CreateTexture("invalid", invalid);
        });
    EXPECT_FALSE(result.plan);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::InvalidDeclaration);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_EQ(result.error->resourceName, "invalid");
    EXPECT_FALSE(result.error->message.empty());
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
}
TEST(GraphDiagnostics, MissingAttachmentFailsAtCompileBoundaryWithContext)
{
    RenderGraph graph;
    const auto color = graph.CreateTexture("unattached", MakeColorDesc());
    graph.AddPass<int>(
        "missing attachment",
        [&](RgBuilder& builder, int&) { (void)builder.Write(color, Rhi::ResourceAccess::ColorWrite); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    const auto result = graph.TryCompile();
    EXPECT_FALSE(result.plan);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::InvalidDeclaration);
    EXPECT_EQ(result.error->phase, GraphPhase::Compiling);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_EQ(result.error->passName, "missing attachment");
    EXPECT_EQ(result.error->resourceName, "unattached");
    EXPECT_EQ(result.error->version, 1U);
}
TEST(GraphDiagnostics, DeadReadAfterDiscardStillFailsBeforeCull)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("discarded", MakeColorDesc());
    color = AddAttachmentPass(graph, "discard", color, Rhi::LoadOp::Clear, Rhi::StoreOp::DontCare);
    AddSamplePass(graph, "dead reader", color);
    const auto result = graph.TryCompile();
    EXPECT_FALSE(result.plan);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::UndefinedContent);
    EXPECT_EQ(result.error->phase, GraphPhase::Compiling);
    EXPECT_EQ(result.error->stage, CompileStage::ValidateDeclarations);
    EXPECT_EQ(result.error->pass, 1U);
    EXPECT_EQ(result.error->version, 1U);
}
TEST(GraphDiagnostics, StandardSetupExceptionBecomesStructuredFailure)
{
    RenderGraph graph;
    const auto result = graph.TryCompile(
        [](RenderGraph& target)
        {
            target.AddPass<int>(
                "throwing setup", [](RgBuilder&, int&) { throw std::runtime_error("setup \"failure\"\n"); },
                [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
        });
    EXPECT_FALSE(result.plan);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::InvalidDeclaration);
    EXPECT_EQ(result.error->phase, GraphPhase::Setup);
    EXPECT_EQ(result.error->passName, "throwing setup");
    EXPECT_EQ(result.error->message, "setup \"failure\"\n");
}
TEST(GraphDiagnostics, DiagnosticJsonEscapesControlsAndUsesNullForAbsentContext)
{
    GraphDiagnostic diagnostic;
    diagnostic.code = GraphErrorCode::UndefinedContent;
    diagnostic.stage = CompileStage::ValidateDeclarations;
    diagnostic.passName = "pass \"quoted\"\\\n";
    diagnostic.message = "tab\treturn\rcontrol\x01";
    const auto json = ToDiagnosticJson(diagnostic);
    EXPECT_NE(json.find("\"code\":\"undefined_content\""), std::string::npos);
    EXPECT_NE(json.find("\"stage\":\"validate_declarations\""), std::string::npos);
    EXPECT_NE(json.find("\"pass\":null"), std::string::npos);
    EXPECT_NE(json.find("\"resource\":null"), std::string::npos);
    EXPECT_NE(json.find("\"version\":null"), std::string::npos);
    EXPECT_NE(json.find("pass \\\"quoted\\\"\\\\\\n"), std::string::npos);
    EXPECT_NE(json.find("tab\\treturn\\rcontrol\\u0001"), std::string::npos);
    EXPECT_EQ(json.find('\x01'), std::string::npos);
    EXPECT_EQ(json, ToDiagnosticJson(diagnostic));
}
TEST(GraphDiagnostics, NewGraphIdentitiesProduceIdenticalDumpsOneHundredTimes)
{
    GraphDumpBundle baseline;
    std::uint64_t baselineHash = 0;
    for (std::uint32_t iteration = 0; iteration < 100; ++iteration)
    {
        SCOPED_TRACE(iteration);
        RenderGraph graph;
        graph.Reserve({iteration, iteration / 2});
        DeclareDiagnosticGraph(graph, iteration + 1);
        const auto plan = graph.Compile();
        const auto dumps = plan.Dumps({42, "Debug", "fixed-commit"});
        if (iteration == 0)
        {
            baselineHash = plan.Statistics().planHash;
            baseline = dumps;
        }
        EXPECT_EQ(plan.Statistics().planHash, baselineHash);
        ExpectSameDumps(dumps, baseline);
    }
}
TEST(GraphDiagnostics, MetadataChangesJsonHeadersWithoutChangingPlanHashOrDot)
{
    RenderGraph graph;
    DeclareDiagnosticGraph(graph);
    const auto plan = graph.Compile();
    const auto hash = plan.Statistics().planHash;
    const auto first = plan.Dumps({1, "D3D11 Debug", "commit-A"});
    const auto second = plan.Dumps({2, "D3D12 Release", "commit-B"});
    EXPECT_NE(first.frameGraphJson, second.frameGraphJson);
    EXPECT_NE(first.accessPlanJson, second.accessPlanJson);
    EXPECT_NE(first.transientPlanJson, second.transientPlanJson);
    EXPECT_EQ(first.dot, second.dot);
    EXPECT_EQ(plan.Statistics().planHash, hash);
    EXPECT_NE(first.frameGraphJson.find("\"frame\":1"), std::string::npos);
    EXPECT_NE(second.frameGraphJson.find("\"build\":\"D3D12 Release\""), std::string::npos);
    EXPECT_NE(second.frameGraphJson.find("\"commit\":\"commit-B\""), std::string::npos);
}
TEST(GraphDiagnostics, DescriptorAndReadStageChangesAffectSemanticHash)
{
    std::array<std::uint64_t, 3> hashes{};
    for (std::size_t variant = 0; variant < hashes.size(); ++variant)
    {
        RenderGraph graph;
        DeclareDiagnosticGraph(graph, 1, variant == 1 ? 64U : 32U,
                               variant == 2 ? Rhi::ShaderStage::Vertex : Rhi::ShaderStage::Pixel);
        hashes[variant] = graph.Compile().Statistics().planHash;
    }
    EXPECT_NE(hashes[0], hashes[1]);
    EXPECT_NE(hashes[0], hashes[2]);
    EXPECT_NE(hashes[1], hashes[2]);
}
TEST(GraphDiagnostics, DumpsExposeDependenciesVersionsRootsLifetimesAndCullReasons)
{
    RenderGraph graph;
    DeclareDiagnosticGraph(graph);
    const auto plan = graph.Compile();
    const auto dumps = plan.Dumps({73, "Debug", "diagnostic-fixture"});
    for (const auto field : {"schemaVersion", "planHash", "statistics", "stages", "executionOrder", "passes",
                             "resources", "versions", "producer", "readers", "edges", "roots", "lifetimes",
                             "physicalAllocations", "firstUse", "lastUse", "physicalSlot", "transitions"})
        EXPECT_NE(dumps.frameGraphJson.find(std::string("\"") + field + "\":"), std::string::npos) << field;
    EXPECT_NE(dumps.frameGraphJson.find("\"culledPasses\":1"), std::string::npos);
    EXPECT_NE(dumps.frameGraphJson.find("\"culledReason\":\"not_reachable_from_output_roots\""), std::string::npos);
    EXPECT_NE(dumps.frameGraphJson.find("\"physicalSlot\":null"), std::string::npos);
    EXPECT_NE(dumps.frameGraphJson.find("\"importedProducer\":true"), std::string::npos);
    EXPECT_NE(dumps.frameGraphJson.find("\"hazard\":\"RAW\""), std::string::npos);
    for (const auto kind : {"Present", "Export", "SideEffect"})
        EXPECT_NE(dumps.frameGraphJson.find(std::string("\"kind\":\"") + kind + "\""), std::string::npos);
    EXPECT_NE(dumps.accessPlanJson.find("\"kind\":\"access-plan\""), std::string::npos);
    EXPECT_NE(dumps.accessPlanJson.find("\"final\":true"), std::string::npos);
    EXPECT_NE(dumps.transientPlanJson.find("stable_first_fit_exact_descriptor_whole_resource_lane_pool"),
              std::string::npos);
    for (const auto token : {"digraph FrameGraph", "shape=box", "lightblue", "lightgray", "lightgreen", "orange",
                             "shape=diamond", " -> ", "RAW"})
        EXPECT_NE(dumps.dot.find(token), std::string::npos) << token;
}
TEST(GraphDiagnostics, DumpNamesEscapeQuotesBackslashAndControlBytes)
{
    RenderGraph graph;
    DeclareDiagnosticGraph(graph);
    const auto plan = graph.Compile();
    const auto dumps = plan.Dumps({8, "build\"\\\n", "commit\t\r"});
    const std::string escapedName = R"(source \"quoted\"\\\n\t\u0001)";
    EXPECT_NE(dumps.frameGraphJson.find(escapedName), std::string::npos);
    EXPECT_NE(dumps.dot.find(escapedName), std::string::npos);
    EXPECT_NE(dumps.frameGraphJson.find(R"(build\"\\\n)"), std::string::npos);
    EXPECT_NE(dumps.frameGraphJson.find(R"(commit\t\r)"), std::string::npos);
    EXPECT_EQ(dumps.frameGraphJson.find('\x01'), std::string::npos);
    EXPECT_EQ(dumps.dot.find('\x01'), std::string::npos);
    EXPECT_EQ(dumps.frameGraphJson.find('\t'), std::string::npos);
    EXPECT_EQ(dumps.frameGraphJson.find('\r'), std::string::npos);
}
TEST(GraphDiagnostics, SavesFourEvidenceFilesWithExactEmittedBytes)
{
    RenderGraph graph;
    DeclareDiagnosticGraph(graph);
    const auto plan = graph.Compile();
#ifdef NDEBUG
    constexpr const char* build = "Release";
#else
    constexpr const char* build = "Debug";
#endif
    const auto dumps = plan.Dumps({608, build, M608_SOURCE_COMMIT});
    const std::filesystem::path directory(M608_DUMP_DIR);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error) << error.message();
    const std::array<std::pair<const char*, const std::string*>, 4> files{
        {{"m6-framegraph.json", &dumps.frameGraphJson},
         {"m6-framegraph.dot", &dumps.dot},
         {"m6-access-plan.json", &dumps.accessPlanJson},
         {"m6-transient-plan.json", &dumps.transientPlanJson}}};
    for (const auto& [name, contents] : files)
    {
        SCOPED_TRACE(name);
        const auto path = directory / name;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << *contents;
        output.close();
        ASSERT_TRUE(output.good());
        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.is_open());
        const std::string actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        EXPECT_EQ(actual, *contents);
    }
}
TEST(GraphDiagnosticsExecution, DeadResourcesCallbacksAndMarkersNeverReachDevice)
{
    DiagnosticFrame fixture;
    const auto texture = fixture.graph.CreateTexture("dead texture", MakeColorDesc());
    const auto buffer = fixture.graph.CreateBuffer("dead buffer", MakeBufferDesc());
    std::uint32_t callbacks = 0;
    fixture.graph.AddPass<int>(
        "dead texture pass",
        [&](RgBuilder& builder, int&)
        {
            const auto written = builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(written, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
        },
        [&](const int&, const RgResources&, Rhi::IRhiCommandList& commands)
        {
            ++callbacks;
            commands.BeginLabel("dead callback marker");
            commands.EndLabel();
        });
    (void)AddBufferCopyWrite(fixture.graph, "dead buffer pass", buffer);
    fixture.ClearAndPresent();
    auto compiled = fixture.graph.TryCompile();
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled.plan->Statistics().culledPasses, 2U);
    EXPECT_EQ(compiled.plan->Statistics().physicalTransients, 0U);
    const auto eventCount = fixture.device.Events().size();
    const auto alive = fixture.device.Diagnostics().aliveObjects;
    const auto retiring = fixture.device.Diagnostics().retiringObjects;
    const auto result = compiled.plan->TryExecute(fixture.device, fixture.frame);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.workRecorded);
    EXPECT_TRUE(result.presentReady);
    EXPECT_EQ(callbacks, 0U);
    EXPECT_EQ(fixture.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(fixture.device.Diagnostics().retiringObjects, retiring);
    for (std::size_t index = eventCount; index < fixture.device.Events().size(); ++index)
    {
        const auto& event = fixture.device.Events()[index];
        EXPECT_EQ(event.find("CreateTexture"), std::string::npos);
        EXPECT_EQ(event.find("CreateBuffer"), std::string::npos);
        EXPECT_EQ(event.find("dead texture pass"), std::string::npos);
        EXPECT_EQ(event.find("dead buffer pass"), std::string::npos);
        EXPECT_EQ(event.find("dead callback marker"), std::string::npos);
    }
    fixture.FinishSuccessfulFrame();
}
TEST(GraphDiagnosticsExecution, DeadOrdinaryImportsAreNotQueriedOrImportedAtExecution)
{
    DiagnosticFrame fixture;
    const Rhi::TextureHandle poisonTexture{17, 1, 0xFEED};
    const Rhi::BufferHandle poisonBuffer{19, 1, 0xBEEF};
    // poison 不属于 device；若错误地 Query 它，后面的成功断言必然失败。
    EXPECT_THROW((void)fixture.device.QueryTextureState(fixture.frame, poisonTexture), Rhi::RhiException);
    EXPECT_THROW((void)fixture.device.QueryBufferState(fixture.frame, poisonBuffer), Rhi::RhiException);
    const auto texture =
        fixture.graph.ImportTexture("poison texture", {poisonTexture, MakeColorDesc(), Rhi::ResourceAccess::SampledRead,
                                                       Rhi::ResourceAccess::SampledRead, ContentState::Defined,
                                                       "external", "declared external contents"});
    const auto buffer =
        fixture.graph.ImportBuffer("poison buffer", {poisonBuffer, MakeBufferDesc(), Rhi::ResourceAccess::VertexRead,
                                                     Rhi::ResourceAccess::VertexRead, ContentState::Defined, "external",
                                                     "declared external contents"});
    AddSamplePass(fixture.graph, "dead texture read", texture);
    fixture.graph.AddPass<int>(
        "dead buffer read",
        [&](RgBuilder& builder, int&) { (void)builder.Read(buffer, Rhi::ResourceAccess::VertexRead); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    fixture.ClearAndPresent();
    auto plan = fixture.graph.Compile();
    EXPECT_EQ(plan.Statistics().liveResources, 1U);
    const auto result = plan.TryExecute(fixture.device, fixture.frame);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.presentReady);
    EXPECT_EQ(fixture.device.CanonicalTrace().find("poison"), std::string::npos);
    fixture.FinishSuccessfulFrame();
}
TEST(GraphDiagnosticsExecution, EmptyGraphSucceedsWithoutFrameQueriesOrGraphicsRecording)
{
    TraceRhiDevice device;
    RenderGraph graph;
    auto plan = graph.Compile();
    const auto events = device.Events();
    // 不提供 BeginFrame token；空计划必须在任何 RHI frame 查询之前完成。
    const auto result = plan.TryExecute(device, {});
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.error);
    EXPECT_FALSE(result.workRecorded);
    EXPECT_FALSE(result.presentReady);
    EXPECT_EQ(graph.Phase(), GraphPhase::Executed);
    EXPECT_EQ(device.Events(), events);
    EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, 0U);
    device.Shutdown();
}
TEST(GraphDiagnosticsExecution, EntirelyCulledGraphUsesTheSameNoWorkPath)
{
    TraceRhiDevice device;
    RenderGraph graph;
    (void)AddAttachmentPass(graph, "dead clear", graph.CreateTexture("dead", MakeColorDesc()));
    auto plan = graph.Compile();
    ASSERT_EQ(plan.Statistics().culledPasses, 1U);
    const auto events = device.Events();
    const auto result = plan.TryExecute(device, {});
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.workRecorded);
    EXPECT_FALSE(result.presentReady);
    EXPECT_EQ(device.Events(), events);
    EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
    device.Shutdown();
}
TEST(GraphDiagnosticsExecution, NonemptyGraphWithoutPresentFailsBeforeGraphicsOrSubmission)
{
    TraceRhiDevice device;
    RenderGraph graph;
    bool called = false;
    graph.AddPass<int>(
        "side effect", [](RgBuilder& builder, int&) { builder.SideEffect("external action"); },
        [&](const int&, const RgResources&, Rhi::IRhiCommandList&) { called = true; });
    auto plan = graph.Compile();
    const auto events = device.Events();
    const auto result = plan.TryExecute(device, {});
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::MissingPresent);
    EXPECT_EQ(result.error->phase, GraphPhase::Executing);
    EXPECT_FALSE(result.workRecorded);
    EXPECT_FALSE(result.presentReady);
    EXPECT_FALSE(called);
    EXPECT_EQ(device.Events(), events);
    EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, 0U);
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
    device.Shutdown();
}
TEST(GraphDiagnosticsExecution, ExportOnlyResourceIsNotMistakenForAnEmptyGraph)
{
    TraceRhiDevice device;
    RenderGraph graph;
    const auto imported = graph.ImportBuffer("exported", {{3, 1, 999},
                                                          MakeBufferDesc(),
                                                          Rhi::ResourceAccess::VertexRead,
                                                          Rhi::ResourceAccess::CopySource,
                                                          ContentState::Defined,
                                                          "external",
                                                          "full upload"});
    graph.Export(imported);
    auto plan = graph.Compile();
    EXPECT_TRUE(plan.ExecutionOrder().empty());
    ASSERT_EQ(plan.PhysicalAllocations().size(), 1U);
    const auto events = device.Events();
    const auto result = plan.TryExecute(device, {});
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, GraphErrorCode::MissingPresent);
    EXPECT_FALSE(result.workRecorded);
    EXPECT_FALSE(result.presentReady);
    EXPECT_EQ(device.Events(), events);
    device.Shutdown();
}
TEST(GraphDiagnosticsExecution, UndeclaredResolveFailurePreservesContextAndSkipsPresent)
{
    DiagnosticFrame fixture;
    const auto hidden = fixture.graph.CreateTexture("hidden", MakeColorDesc());
    fixture.ClearAndPresent();
    bool reached = false;
    fixture.graph.AddPass<int>(
        "undeclared resolver", [](RgBuilder& builder, int&) { builder.SideEffect("negative resolve fixture"); },
        [&](const int&, const RgResources& resources, Rhi::IRhiCommandList&)
        {
            reached = true;
            (void)resources.Get(hidden);
        });
    auto plan = fixture.graph.Compile();
    const auto alive = fixture.device.Diagnostics().aliveObjects;
    const auto result = plan.TryExecute(fixture.device, fixture.frame);
    EXPECT_FALSE(result);
    ASSERT_TRUE(result.error);
    EXPECT_TRUE(reached);
    EXPECT_TRUE(result.workRecorded);
    EXPECT_FALSE(result.presentReady);
    EXPECT_EQ(result.error->phase, GraphPhase::Executing);
    EXPECT_EQ(result.error->pass, 1U);
    EXPECT_EQ(result.error->passName, "undeclared resolver");
    EXPECT_EQ(result.error->code, GraphErrorCode::ExecutionFailure);
    EXPECT_EQ(result.error->resourceName, "hidden");
    EXPECT_EQ(result.error->resource, hidden.Resource());
    EXPECT_EQ(result.error->version, hidden.Version());
    EXPECT_NE(result.error->message.find("exact declared version"), std::string::npos);
    EXPECT_EQ(fixture.graph.Phase(), GraphPhase::Failed);
    EXPECT_EQ(fixture.device.Diagnostics().aliveObjects, alive);
    EXPECT_EQ(fixture.device.Diagnostics().lastSubmittedSerial, 0U);
    EXPECT_EQ(fixture.device.QueryTextureState(fixture.frame, fixture.frame.backBuffer).access,
              Rhi::ResourceAccess::ColorWrite);
    const auto trace = fixture.device.CanonicalTrace();
    EXPECT_NE(trace.find("BeginGraphics"), std::string::npos);
    EXPECT_EQ(trace.find("EndGraphics"), std::string::npos);
    EXPECT_EQ(trace.find("EndFrame"), std::string::npos);
    EXPECT_NE(trace.rfind("EndLabel"), std::string::npos);
    // 失败帧交给 owner 放弃；不调用 EndFrame/Present 来伪造成功恢复。
}

TEST(GraphDiagnosticsExecution, StructuredEntryMisusePreservesActiveAndCompletedExecution)
{
    DiagnosticFrame fixture;
    fixture.ClearAndPresent();
    CompiledRenderGraph* active = nullptr;
    fixture.graph.AddPass<int>(
        "entry misuse", [](RgBuilder& builder, int&) { builder.SideEffect("verify structured entry preconditions"); },
        [&](const int&, const RgResources&, Rhi::IRhiCommandList&)
        {
            const auto compile = fixture.graph.TryCompile();
            ASSERT_TRUE(compile.error);
            EXPECT_EQ(compile.error->code, GraphErrorCode::PhaseViolation);
            EXPECT_EQ(fixture.graph.Phase(), GraphPhase::Executing);
            const auto execute = active->TryExecute(fixture.device, fixture.frame);
            ASSERT_TRUE(execute.error);
            EXPECT_EQ(execute.error->code, GraphErrorCode::PhaseViolation);
            EXPECT_FALSE(execute.workRecorded);
            EXPECT_FALSE(execute.presentReady);
            EXPECT_EQ(fixture.graph.Phase(), GraphPhase::Executing);
        });
    auto plan = fixture.graph.Compile();
    active = &plan;
    const auto result = plan.TryExecute(fixture.device, fixture.frame);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.presentReady);
    const auto events = fixture.device.Events();
    const auto repeated = plan.TryExecute(fixture.device, fixture.frame);
    ASSERT_TRUE(repeated.error);
    EXPECT_EQ(repeated.error->code, GraphErrorCode::PhaseViolation);
    EXPECT_FALSE(repeated.workRecorded);
    EXPECT_FALSE(repeated.presentReady);
    EXPECT_EQ(fixture.device.Events(), events);
    EXPECT_EQ(fixture.graph.Phase(), GraphPhase::Executed);
    fixture.FinishSuccessfulFrame();
}

TEST(GraphDiagnosticsExecution, ImportFailuresAreClassifiedBeforeGraphicsRecording)
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        DiagnosticFrame fixture;
        auto descriptor = MakeBufferDesc();
        const auto physical = fixture.device.CreateBuffer(descriptor, {});
        if (scenario == 0)
            descriptor.size += 16;
        const auto imported = fixture.graph.ImportBuffer(
            "invalid import", {scenario == 2 ? Rhi::BufferHandle{999, 1, 999} : physical, descriptor,
                               scenario == 1 ? Rhi::ResourceAccess::CopyDestination : Rhi::ResourceAccess::None,
                               Rhi::ResourceAccess::CopyDestination});
        fixture.graph.Export(imported);
        fixture.ClearAndPresent();
        auto plan = fixture.graph.Compile();
        const auto events = fixture.device.Events();
        const auto result = plan.TryExecute(fixture.device, fixture.frame);
        ASSERT_TRUE(result.error);
        EXPECT_EQ(result.error->code, GraphErrorCode::InvalidImport);
        EXPECT_EQ(result.error->phase, GraphPhase::Executing);
        EXPECT_EQ(result.error->resourceName, "invalid import");
        EXPECT_FALSE(result.workRecorded);
        EXPECT_FALSE(result.presentReady);
        EXPECT_EQ(fixture.device.Events(), events);
    }
}
TEST(GraphDiagnostics, ErrorsOutsidePassAreNotAutomaticallyImportFailures)
{
    // final restore/EndGraphics 没有 pass ID，仍属于录制；无 pass 不等于正在验证 import。
    Detail::GraphState state(123);
    state.phase = GraphPhase::Executing;
    state.currentStage = CompileStage::EmitPlan;
    Rhi::RhiError error;
    error.operation = "EndGraphics";
    error.message = "injected close failure";
    const auto native = std::make_exception_ptr(Rhi::RhiException(error));
    const auto graph = std::make_exception_ptr(GraphCompileError("injected graph failure"));
    for (const auto& exception : {native, graph})
    {
        state.validatingImports = false;
        EXPECT_EQ(state.CaptureDiagnostic(exception).code, GraphErrorCode::ExecutionFailure);
        state.validatingImports = true;
        EXPECT_EQ(state.CaptureDiagnostic(exception).code, GraphErrorCode::InvalidImport);
    }
    EXPECT_EQ(state.CaptureDiagnostic(std::make_exception_ptr(UndefinedContentError("undefined"))).code,
              GraphErrorCode::UndefinedContent);
    EXPECT_EQ(state.CaptureDiagnostic(std::make_exception_ptr(StaleGraphHandleError("stale"))).code,
              GraphErrorCode::StaleHandle);
    GraphDiagnostic missing;
    missing.code = GraphErrorCode::MissingPresent;
    missing.message = "missing";
    EXPECT_EQ(state.CaptureDiagnostic(std::make_exception_ptr(GraphCompileError(missing))).code,
              GraphErrorCode::MissingPresent);
}
} // namespace MiniEngine::RenderGraph::Tests
