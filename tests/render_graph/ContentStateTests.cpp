#include "RenderGraphTestFixtures.h"
#include <gtest/gtest.h>

namespace MiniEngine::RenderGraph::Tests
{
TEST(ContentStateTests, RejectsLoadFromUndefinedTransient)
{
    RenderGraph graph;
    const auto color = graph.CreateTexture("color", MakeColorDesc());
    (void)AddAttachmentPass(graph, "invalid load", color, Rhi::LoadOp::Load);
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, RejectsReadFromUninitializedTexture)
{
    RenderGraph graph;
    AddSamplePass(graph, "read", graph.CreateTexture("undefined", MakeColorDesc()));
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, RejectsReadFromUninitializedBuffer)
{
    RenderGraph graph;
    const auto buffer = graph.CreateBuffer("undefined", MakeBufferDesc());
    graph.AddPass<int>(
        "read", [&](RgBuilder& builder, int&) { (void)builder.Read(buffer, Rhi::ResourceAccess::VertexRead); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, RejectsReadAfterDontCareStore)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "discard", color, Rhi::LoadOp::Clear, Rhi::StoreOp::DontCare);
    AddSamplePass(graph, "read discarded", color);
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, RejectsLoadAfterDontCareStore)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "discard", color, Rhi::LoadOp::Clear, Rhi::StoreOp::DontCare);
    (void)AddAttachmentPass(graph, "load discarded", color, Rhi::LoadOp::Load);
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, ClearAfterDontCareRestoresDefinition)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "discard", color, Rhi::LoadOp::Clear, Rhi::StoreOp::DontCare);
    color = AddAttachmentPass(graph, "rewrite", color, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
    AddSamplePass(graph, "valid read", color);
    EXPECT_NO_THROW((void)graph.Compile());
}
TEST(ContentStateTests, DontCareLoadDoesNotProveFullTriangleCoverage)
{
    RenderGraph graph;
    auto color = graph.CreateTexture("color", MakeColorDesc());
    color = AddAttachmentPass(graph, "initial clear", color);
    color = AddAttachmentPass(graph, "partial draw", color, Rhi::LoadOp::DontCare);
    AddSamplePass(graph, "undefined region", color);
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, FirstDontCareAttachmentWriteIsRejected)
{
    RenderGraph graph;
    const auto color = graph.CreateTexture("color", MakeColorDesc());
    (void)AddAttachmentPass(graph, "not a full write", color, Rhi::LoadOp::DontCare);
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, FullCoverageAnnotationCannotInitializeArbitraryDraw)
{
    RenderGraph graph;
    const auto color = graph.CreateTexture("color", MakeColorDesc());
    EXPECT_THROW((graph.AddPass<int>(
                     "false full write", [&](RgBuilder& builder, int&)
                     { (void)builder.Write(color, Rhi::ResourceAccess::ColorWrite, WriteCoverage::Full); },
                     [](const int&, const RgResources&, Rhi::IRhiCommandList&) {})),
                 GraphCompileError);
}
TEST(ContentStateTests, FirstBufferCopyRequiresExplicitCompleteCoverage)
{
    RenderGraph graph;
    const auto buffer = graph.CreateBuffer("buffer", MakeBufferDesc());
    (void)AddBufferCopyWrite(graph, "partial copy", buffer, WriteCoverage::Preserve);
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, CompleteBufferCopyDefinesNextVersionForReads)
{
    RenderGraph graph;
    auto buffer = graph.CreateBuffer("buffer", MakeBufferDesc());
    buffer = AddBufferCopyWrite(graph, "whole copy", buffer, WriteCoverage::Full);
    graph.AddPass<int>(
        "vertex reader", [&](RgBuilder& builder, int&) { (void)builder.Read(buffer, Rhi::ResourceAccess::VertexRead); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_NO_THROW((void)graph.Compile());
}
TEST(ContentStateTests, ReadOnlyDepthRequiresExistingDefinedContents)
{
    RenderGraph graph;
    const auto depth = graph.CreateTexture("depth", MakeDepthDesc());
    graph.AddPass<TexturePassData>(
        "read depth",
        [&](RgBuilder& builder, TexturePassData& data)
        {
            data.texture = builder.Read(depth, Rhi::ResourceAccess::DepthRead);
            builder.SetDepthAttachment(data.texture, Rhi::LoadOp::Load, Rhi::StoreOp::Store);
        },
        [](const TexturePassData&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}
TEST(ContentStateTests, ClearedDepthCanBeReadOnlyAttachmentThenSampled)
{
    RenderGraph graph;
    auto depth = graph.CreateTexture("depth", MakeDepthDesc());
    depth = AddAttachmentPass(graph, "clear depth", depth, Rhi::LoadOp::Clear, Rhi::StoreOp::Store, true);
    graph.AddPass<TexturePassData>(
        "read-only depth",
        [&](RgBuilder& builder, TexturePassData& data)
        {
            data.texture = builder.Read(depth, Rhi::ResourceAccess::DepthRead);
            EXPECT_THROW(builder.SetDepthAttachment(data.texture, Rhi::LoadOp::Clear, Rhi::StoreOp::Store),
                         GraphCompileError);
            builder.SetDepthAttachment(data.texture, Rhi::LoadOp::Load, Rhi::StoreOp::Store);
        },
        [](const TexturePassData&, const RgResources&, Rhi::IRhiCommandList&) {});
    AddSamplePass(graph, "sample depth", depth);
    EXPECT_NO_THROW((void)graph.Compile());
}
TEST(ContentStateTests, MissingAttachmentDeclarationIsNotAWriteProof)
{
    RenderGraph graph;
    const auto color = graph.CreateTexture("color", MakeColorDesc());
    graph.AddPass<int>(
        "missing attachment",
        [&](RgBuilder& builder, int&) { (void)builder.Write(color, Rhi::ResourceAccess::ColorWrite); },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    EXPECT_THROW((void)graph.Compile(), GraphCompileError);
}
} // namespace MiniEngine::RenderGraph::Tests
