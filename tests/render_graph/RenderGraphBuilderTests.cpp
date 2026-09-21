#include "RenderGraphTestFixtures.h"
#include <MiniEngine/Rhi/RhiError.h>
#include <gtest/gtest.h>
#include <optional>
#include <type_traits>

namespace MiniEngine::RenderGraph::Tests
{
static_assert(!std::is_same_v<RgTexture, Rhi::TextureHandle>);
static_assert(!std::is_same_v<RgBuffer, Rhi::BufferHandle>);
static_assert(!std::is_convertible_v<RgTexture, Rhi::TextureHandle>);
static_assert(!std::is_convertible_v<Rhi::TextureHandle, RgTexture>);
static_assert(!std::is_constructible_v<RgTexture, std::uint32_t, std::uint32_t>);
static_assert(!std::is_convertible_v<RgTexture, RgBuffer>);

TEST(GraphBuilder, WritesIssueNewVersionsAndRejectLaterOldReads)
{
    RenderGraph graph;
    const auto v0 = graph.CreateTexture("color", MakeColorDesc());
    const auto v1 = AddAttachmentPass(graph, "clear", v0);
    ASSERT_EQ(v0.Version(), 0U);
    ASSERT_EQ(v1.Version(), 1U);
    ASSERT_EQ(v0.Resource(), v1.Resource());
    EXPECT_THROW(AddSamplePass(graph, "old read", v0), StaleGraphHandleError);
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
}
TEST(GraphBuilder, EarlierReadersMayPrecedeNextVersionWrite)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "producer", color);
    AddSamplePass(graph, "reader one", color);
    AddSamplePass(graph, "reader two", color);
    const auto next = AddAttachmentPass(graph, "next writer", color, Rhi::LoadOp::Load);
    AddSamplePass(graph, "new reader", next);
    EXPECT_EQ(graph.Compile().Statistics().declaredPasses, 5U);
}
TEST(GraphBuilder, CrossGraphHandleRejectedEvenWithMatchingIndexAndVersion)
{
    RenderGraph first;
    RenderGraph second;
    const auto one = first.CreateTexture("first", MakeColorDesc());
    const auto two = second.CreateTexture("second", MakeColorDesc());
    ASSERT_EQ(one.Resource(), two.Resource());
    ASSERT_EQ(one.Generation(), two.Generation());
    ASSERT_NE(one.Owner(), two.Owner());
    EXPECT_THROW(AddAttachmentPass(second, "foreign", one), StaleGraphHandleError);
}
TEST(GraphBuilder, ResetInvalidatesTextureAndBufferHandles)
{
    RenderGraph graph;
    const auto oldTexture = graph.CreateTexture("old texture", MakeColorDesc());
    const auto oldBuffer = graph.CreateBuffer("old buffer", MakeBufferDesc());
    graph.Reset();
    (void)graph.CreateTexture("new texture", MakeColorDesc());
    (void)graph.CreateBuffer("new buffer", MakeBufferDesc());
    EXPECT_THROW(AddAttachmentPass(graph, "stale texture", oldTexture), StaleGraphHandleError);
    graph.Reset();
    EXPECT_THROW(AddBufferCopyWrite(graph, "stale buffer", oldBuffer), StaleGraphHandleError);
}
TEST(GraphBuilder, ResetDoesNotReacceptSixteenBitGenerationWrap)
{
    RenderGraph graph;
    const auto old = graph.CreateTexture("old", MakeColorDesc());
    for (std::size_t index = 0; index < 65536; ++index)
        graph.Reset();
    const auto current = graph.CreateTexture("current", MakeColorDesc());
    EXPECT_GT(current.Generation(), 65535ULL);
    EXPECT_THROW(AddAttachmentPass(graph, "old generation", old), StaleGraphHandleError);
}
TEST(GraphBuilder, CompiledPlanInvalidatedByResetAndOwnerDestruction)
{
    RenderGraph graph;
    auto plan = graph.Compile();
    graph.Reset();
    EXPECT_THROW((void)plan.Statistics(), StaleGraphHandleError);
    std::optional<CompiledRenderGraph> destroyed;
    {
        RenderGraph owner;
        destroyed.emplace(owner.Compile());
    }
    EXPECT_THROW((void)destroyed->Transitions(), StaleGraphHandleError);
}
TEST(GraphBuilder, SealedGraphCannotMutateOrCompileTwice)
{
    RenderGraph graph;
    auto plan = graph.Compile();
    EXPECT_THROW((void)graph.CreateTexture("late", MakeColorDesc()), GraphPhaseError);
    EXPECT_THROW((void)graph.CreateBuffer("late", MakeBufferDesc()), GraphPhaseError);
    EXPECT_THROW((void)graph.Compile(), GraphPhaseError);
    EXPECT_THROW((graph.AddPass<int>(
                     "late", [](RgBuilder&, int&) {}, [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                 GraphPhaseError);
}
TEST(GraphBuilder, SetupCannotResetCompileOrNestGraphPass)
{
    RenderGraph graph;
    graph.AddPass<int>(
        "setup",
        [&](RgBuilder&, int&)
        {
            EXPECT_THROW(graph.Reset(), GraphPhaseError);
            EXPECT_THROW((void)graph.Compile(), GraphPhaseError);
            EXPECT_THROW((void)graph.CreateTexture("bypass", MakeColorDesc()), GraphPhaseError);
            EXPECT_THROW(
                (graph.AddPass<int>(
                    "nested", [](RgBuilder&, int&) {}, [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                GraphPhaseError);
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_EQ(graph.PassCount(), 1U);
    EXPECT_EQ(graph.Compile().Statistics().declaredPasses, 1U);
}
TEST(GraphBuilder, EscapedBuilderCannotOperateInLaterSetupOrAfterReset)
{
    RenderGraph graph;
    std::optional<RgBuilder> escaped;
    graph.AddPass<int>(
        "capture", [&](RgBuilder& builder, int&) { escaped = builder; },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_THROW((void)escaped->CreateTexture("outside", MakeColorDesc()), GraphPhaseError);
    graph.AddPass<int>(
        "later", [&](RgBuilder&, int&) { EXPECT_THROW(escaped->SideEffect("wrong pass"), GraphPhaseError); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    graph.Reset();
    graph.AddPass<int>(
        "same index", [&](RgBuilder&, int&) { EXPECT_THROW(escaped->SideEffect("wrong generation"), GraphPhaseError); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}
TEST(GraphBuilder, FailedSetupRequiresResetAndEscapedResourceStaysStale)
{
    RenderGraph graph;
    RgTexture escaped;
    EXPECT_THROW((graph.AddPass<int>(
                     "failure",
                     [&](RgBuilder& builder, int&)
                     {
                         escaped = builder.CreateTexture("temporary", MakeColorDesc());
                         throw std::runtime_error("setup failure");
                     },
                     [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                 std::runtime_error);
    EXPECT_EQ(graph.Phase(), GraphPhase::Failed);
    EXPECT_THROW((void)graph.Compile(), GraphPhaseError);
    graph.Reset();
    (void)graph.CreateTexture("replacement", MakeColorDesc());
    EXPECT_THROW(AddAttachmentPass(graph, "escaped", escaped), StaleGraphHandleError);
}
TEST(GraphBuilder, BuilderCanDeclareResourcesButDoesNotRunExecute)
{
    RenderGraph graph;
    bool executed = false;
    graph.AddPass<int>(
        "declare",
        [&](RgBuilder& builder, int&)
        {
            const auto color = builder.CreateTexture("created in setup", MakeColorDesc());
            const auto buffer = builder.CreateBuffer("buffer in setup", MakeBufferDesc());
            EXPECT_TRUE(color);
            EXPECT_TRUE(buffer);
            builder.SideEffect("test callback boundary");
        },
        [&](const int&, const RgResources&, Rhi::IRhiCommandList&) { executed = true; });
    EXPECT_EQ(graph.ResourceCount(), 2U);
    EXPECT_FALSE(executed);
    EXPECT_EQ(graph.Compile().Statistics().roots, 1U);
    EXPECT_FALSE(executed);
}
TEST(GraphBuilder, BlankNamesDuplicateNamesAndBlankSideEffectsFail)
{
    RenderGraph graph;
    EXPECT_THROW((void)graph.CreateTexture(" \t", MakeColorDesc()), GraphCompileError);
    (void)graph.CreateTexture("name", MakeColorDesc());
    EXPECT_THROW((void)graph.CreateBuffer("name", MakeBufferDesc()), GraphCompileError);
    EXPECT_THROW((graph.AddPass<int>(
                     "effect", [](RgBuilder& builder, int&) { builder.SideEffect("\t\n "); },
                     [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                 GraphCompileError);
}
TEST(GraphBuilder, TransientExportAndPresentNeverTransferOwnership)
{
    RenderGraph graph;
    const auto texture = graph.CreateTexture("texture", MakeColorDesc());
    const auto buffer = graph.CreateBuffer("buffer", MakeBufferDesc());
    EXPECT_THROW(graph.Export(texture), GraphCompileError);
    EXPECT_THROW(graph.Export(buffer), GraphCompileError);
    EXPECT_THROW(graph.Present(texture), GraphCompileError);
    graph.AddPass<int>(
        "builder roots",
        [&](RgBuilder& builder, int&)
        {
            EXPECT_THROW(builder.Export(texture), GraphCompileError);
            EXPECT_THROW(builder.Export(buffer), GraphCompileError);
            EXPECT_THROW(builder.Present(texture), GraphCompileError);
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}
TEST(GraphBuilder, BufferWritesAreVersionedAndOldInputsBecomeStale)
{
    RenderGraph graph;
    const auto v0 = graph.CreateBuffer("buffer", MakeBufferDesc());
    const auto v1 = AddBufferCopyWrite(graph, "fill", v0);
    EXPECT_EQ(v1.Version(), 1U);
    EXPECT_THROW(AddBufferCopyWrite(graph, "stale", v0), StaleGraphHandleError);
}
TEST(GraphBuilder, AccessDirectionAndUsageCannotBeMisdeclared)
{
    for (const auto access : {Rhi::ResourceAccess::ColorWrite, Rhi::ResourceAccess::None, Rhi::ResourceAccess::Present,
                              Rhi::ResourceAccess::VertexRead})
    {
        RenderGraph graph;
        const auto texture = graph.CreateTexture("texture", MakeColorDesc());
        EXPECT_THROW((graph.AddPass<int>(
                         "invalid read", [&](RgBuilder& builder, int&) { (void)builder.Read(texture, access); },
                         [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                     GraphCompileError);
    }
    RenderGraph graph;
    const auto texture = graph.CreateTexture("texture", MakeColorDesc());
    EXPECT_THROW((graph.AddPass<int>(
                     "invalid write",
                     [&](RgBuilder& builder, int&) { (void)builder.Write(texture, Rhi::ResourceAccess::SampledRead); },
                     [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                 GraphCompileError);
}
TEST(GraphBuilder, SamePassCannotReadThenWriteOrWriteThenRead)
{
    for (const bool readFirst : {false, true})
    {
        RenderGraph graph;
        const auto texture = graph.CreateTexture("texture", MakeColorDesc());
        EXPECT_THROW((graph.AddPass<int>(
                         "conflict",
                         [&](RgBuilder& builder, int&)
                         {
                             if (readFirst)
                             {
                                 (void)builder.Read(texture, Rhi::ResourceAccess::SampledRead);
                                 (void)builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
                             }
                             else
                             {
                                 const auto output = builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
                                 (void)builder.Read(output, Rhi::ResourceAccess::SampledRead);
                             }
                         },
                         [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                     GraphCompileError);
    }
}
TEST(GraphBuilder, RepeatedReadMergesShaderStagesWithoutNewVersion)
{
    RenderGraph graph;
    auto texture = graph.CreateTexture("texture", MakeColorDesc());
    texture = AddAttachmentPass(graph, "clear", texture);
    graph.AddPass<int>(
        "both stages",
        [&](RgBuilder& builder, int&)
        {
            builder.SideEffect("observe merged stage declaration");
            EXPECT_EQ(builder.Read(texture, Rhi::ResourceAccess::SampledRead, Rhi::ShaderStage::Vertex), texture);
            EXPECT_EQ(builder.Read(texture, Rhi::ResourceAccess::SampledRead, Rhi::ShaderStage::Pixel), texture);
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    auto plan = graph.Compile();
    ASSERT_EQ(plan.Transitions().size(), 2U);
    EXPECT_EQ(plan.Transitions()[1].stages, Rhi::ShaderStage::Vertex | Rhi::ShaderStage::Pixel);
}
TEST(GraphBuilder, AttachmentsRequireMatchingDeclarationsAndEqualExtents)
{
    RenderGraph graph;
    const auto texture = graph.CreateTexture("texture", MakeColorDesc());
    auto larger = MakeColorDesc();
    larger.extent.width += 1;
    const auto other = graph.CreateTexture("other", larger);
    graph.AddPass<int>(
        "attachments",
        [&](RgBuilder& builder, int&)
        {
            EXPECT_THROW(builder.SetColorAttachment(texture, Rhi::LoadOp::Clear, Rhi::StoreOp::Store),
                         GraphCompileError);
            const auto output = builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(output, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
            EXPECT_THROW(builder.SetColorAttachment(output, Rhi::LoadOp::Clear, Rhi::StoreOp::Store),
                         GraphCompileError);
            const auto second = builder.Write(other, Rhi::ResourceAccess::ColorWrite);
            EXPECT_THROW(builder.SetColorAttachment(second, Rhi::LoadOp::Clear, Rhi::StoreOp::Store),
                         GraphCompileError);
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_THROW((void)graph.Compile(), GraphCompileError);
}
TEST(GraphBuilder, TextureStructureValidationDoesNotNeedFakeCapabilities)
{
    RenderGraph graph;
    auto descriptor = MakeColorDesc();
    descriptor.extent.width = 0;
    EXPECT_THROW((void)graph.CreateTexture("zero", descriptor), Rhi::RhiException);
    descriptor = MakeColorDesc();
    descriptor.clearDepthHint = 2;
    EXPECT_THROW((void)graph.CreateTexture("clear", descriptor), Rhi::RhiException);
    EXPECT_EQ(graph.ResourceCount(), 0U);
}
} // namespace MiniEngine::RenderGraph::Tests
