#include "RenderGraphTestFixtures.h"

#include <gtest/gtest.h>

#include <MiniEngine/Rhi/RhiValidation.h>
#include <cstdint>
#include <string_view>

namespace MiniEngine::RenderGraph::Tests
{
namespace
{
void AddLiveColorWrite(RenderGraph& graph, std::string_view name, RgTexture& texture,
                       Rhi::LoadOp load = Rhi::LoadOp::Clear, Rhi::StoreOp store = Rhi::StoreOp::Store)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            texture = builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(texture, load, store);
            builder.SideEffect("transient lifetime test");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}

void AddLiveTextureCopyWrite(RenderGraph& graph, std::string_view name, RgTexture& texture,
                             WriteCoverage coverage = WriteCoverage::Full)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            texture = builder.Write(texture, Rhi::ResourceAccess::CopyDestination, coverage);
            builder.SideEffect("transient lifetime test");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}

void AddLiveBufferCopyWrite(RenderGraph& graph, std::string_view name, RgBuffer& buffer,
                            WriteCoverage coverage = WriteCoverage::Full)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            buffer = builder.Write(buffer, Rhi::ResourceAccess::CopyDestination, coverage);
            builder.SideEffect("transient lifetime test");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}

void AddLiveTextureRead(RenderGraph& graph, std::string_view name, RgTexture texture)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(texture, Rhi::ResourceAccess::SampledRead);
            builder.SideEffect("transient lifetime test");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}

void AddLiveBufferRead(RenderGraph& graph, std::string_view name, RgBuffer buffer)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            (void)builder.Read(buffer, Rhi::ResourceAccess::VertexRead);
            builder.SideEffect("transient lifetime test");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}

void AddDeadColorWrite(RenderGraph& graph, std::string_view name, RgTexture& texture)
{
    graph.AddPass<int>(
        name,
        [&](RgBuilder& builder, int&)
        {
            texture = builder.Write(texture, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(texture, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
}

std::uint32_t PhysicalCountForTexturePair(const Rhi::TextureDesc& firstDescriptor,
                                          const Rhi::TextureDesc& secondDescriptor)
{
    RenderGraph graph;
    auto first = graph.CreateTexture("first", firstDescriptor);
    auto second = graph.CreateTexture("second", secondDescriptor);
    AddLiveTextureCopyWrite(graph, "first write", first);
    AddLiveTextureCopyWrite(graph, "second write", second);
    const auto plan = graph.Compile();
    EXPECT_EQ(plan.Lifetimes().size(), 2U);
    EXPECT_EQ(plan.Statistics().physicalResources, plan.PhysicalAllocations().size());
    return plan.Statistics().physicalResources;
}

void ExpectSameSlot(const CompiledRenderGraph& plan)
{
    ASSERT_EQ(plan.Lifetimes().size(), 2U);
    ASSERT_EQ(plan.PhysicalAllocations().size(), 1U);
    EXPECT_EQ(plan.Lifetimes()[0].physicalSlot, plan.Lifetimes()[1].physicalSlot);
    for (const auto& transition : plan.Transitions())
        EXPECT_EQ(transition.physicalSlot, plan.Lifetimes()[0].physicalSlot);
}
} // namespace

TEST(TransientLifetimeTests, ExactDisjointDescriptorsReuseOnePhysicalSlot)
{
    RenderGraph graph;
    auto first = graph.CreateTexture("first", MakeColorDesc());
    auto second = graph.CreateTexture("second", MakeColorDesc());
    AddLiveTextureCopyWrite(graph, "first write", first);
    AddLiveTextureCopyWrite(graph, "second write", second);

    const auto plan = graph.Compile();

    EXPECT_EQ(plan.Statistics().virtualResources, 2U);
    EXPECT_EQ(plan.Statistics().liveResources, 2U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 1U);
    EXPECT_EQ(plan.Statistics().physicalResources, 1U);
    ExpectSameSlot(plan);
}

TEST(TransientLifetimeTests, DebugNameDoesNotChangeSemanticReuse)
{
    auto first = MakeColorDesc();
    auto second = first;
    first.debugName = "first native name";
    second.debugName = "second native name";

    EXPECT_EQ(PhysicalCountForTexturePair(first, second), 1U);
}

TEST(TransientLifetimeTests, EverySupportedSemanticTextureFieldDifferencePreventsReuse)
{
    const auto baseline = MakeColorDesc();
    const auto expectNoReuse = [&](std::string_view field, const Rhi::TextureDesc& variant)
    {
        SCOPED_TRACE(field);
        EXPECT_EQ(PhysicalCountForTexturePair(baseline, variant), 2U);
    };

    auto width = baseline;
    ++width.extent.width;
    expectNoReuse("extent.width", width);

    auto height = baseline;
    ++height.extent.height;
    expectNoReuse("extent.height", height);

    auto mipLevels = baseline;
    mipLevels.mipLevels = 2;
    expectNoReuse("mipLevels", mipLevels);

    auto format = baseline;
    format.format = Rhi::Format::Rgba8UnormSrgb;
    expectNoReuse("format", format);

    auto usage = baseline;
    usage.usage = Rhi::TextureUsage::ColorAttachment | Rhi::TextureUsage::Sampled | Rhi::TextureUsage::CopyDestination;
    expectNoReuse("usage", usage);

    auto colorHint = baseline;
    colorHint.clearColorHint[2] = 0.25F;
    expectNoReuse("clearColorHint", colorHint);

    auto depthHint = baseline;
    depthHint.clearDepthHint = 0.25F;
    expectNoReuse("clearDepthHint", depthHint);

    auto stencilHint = baseline;
    stencilHint.clearStencilHint = 1U;
    expectNoReuse("clearStencilHint", stencilHint);

    // 当前附件绑定只支持 2D/单层；此处用完整 copy 声明验证合法 cube descriptor。
    auto dimension = baseline;
    dimension.dimension = Rhi::TextureDimension::TextureCube;
    dimension.extent.height = dimension.extent.width;
    dimension.arrayLayers = 6;
    expectNoReuse("dimension", dimension);

    // sampleCount != 1、2D arrayLayers != 1、cube arrayLayers != 6 当前均由 validator 拒绝。
    // 这些形状在分配前失败，不能绕过验证进入复用计划。
}

TEST(TransientLifetimeTests, SamePassBoundaryDoesNotReuse)
{
    RenderGraph graph;
    auto first = graph.CreateTexture("first", MakeColorDesc());
    auto second = graph.CreateTexture("second", MakeColorDesc());
    graph.AddPass<int>(
        "same pass writes",
        [&](RgBuilder& builder, int&)
        {
            first = builder.Write(first, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(first, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
            second = builder.Write(second, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(second, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
            builder.SideEffect("same pass lifetime boundary");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.Lifetimes().size(), 2U);
    EXPECT_EQ(plan.Lifetimes()[0].firstUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[1].firstUse, 0U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 2U);
    EXPECT_NE(plan.Lifetimes()[0].physicalSlot, plan.Lifetimes()[1].physicalSlot);
}

TEST(TransientLifetimeTests, OverlappingIntervalsDoNotReuse)
{
    RenderGraph graph;
    auto first = graph.CreateTexture("first", MakeColorDesc());
    auto second = graph.CreateTexture("second", MakeColorDesc());
    AddLiveColorWrite(graph, "first write", first);
    AddLiveColorWrite(graph, "second write", second);
    AddLiveTextureRead(graph, "first read", first);

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.Lifetimes().size(), 2U);
    EXPECT_EQ(plan.Lifetimes()[0].firstUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[0].lastUse, 2U);
    EXPECT_EQ(plan.Lifetimes()[1].firstUse, 1U);
    EXPECT_EQ(plan.Lifetimes()[1].lastUse, 1U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 2U);
    EXPECT_NE(plan.Lifetimes()[0].physicalSlot, plan.Lifetimes()[1].physicalSlot);
}

TEST(TransientLifetimeTests, StableFirstFitUsesDeclarationOrderForFirstUseTies)
{
    RenderGraph graph;
    auto first = graph.CreateTexture("first", MakeColorDesc());
    auto second = graph.CreateTexture("second", MakeColorDesc());
    auto third = graph.CreateTexture("third", MakeColorDesc());
    graph.AddPass<int>(
        "tied first uses",
        [&](RgBuilder& builder, int&)
        {
            first = builder.Write(first, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(first, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
            second = builder.Write(second, Rhi::ResourceAccess::ColorWrite);
            builder.SetColorAttachment(second, Rhi::LoadOp::Clear, Rhi::StoreOp::Store);
            builder.SideEffect("stable first fit");
        },
        [](const int&, const RgResources&, Rhi::IRhiCommandList&) {});
    AddLiveTextureRead(graph, "first extends", first);
    AddLiveTextureCopyWrite(graph, "third write", third);

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.Lifetimes().size(), 3U);
    EXPECT_EQ(plan.Lifetimes()[0].firstUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[0].lastUse, 1U);
    EXPECT_EQ(plan.Lifetimes()[1].firstUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[1].lastUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[2].firstUse, 2U);
    EXPECT_EQ(plan.Lifetimes()[2].lastUse, 2U);
    EXPECT_EQ(plan.Lifetimes()[0].physicalSlot, 0U);
    EXPECT_EQ(plan.Lifetimes()[1].physicalSlot, 1U);
    EXPECT_EQ(plan.Lifetimes()[2].physicalSlot, 0U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 2U);
}

TEST(TransientLifetimeTests, LogicalVersionsUseOneSlotAndUnionLiveUses)
{
    RenderGraph graph;
    auto buffer = graph.CreateBuffer("versions", MakeBufferDesc());
    AddLiveBufferCopyWrite(graph, "initial copy", buffer, WriteCoverage::Full);
    AddLiveBufferRead(graph, "vertex read", buffer);
    AddLiveBufferCopyWrite(graph, "preserve copy", buffer, WriteCoverage::Preserve);
    AddLiveBufferRead(graph, "final vertex read", buffer);

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.Lifetimes().size(), 1U);
    const auto& lifetime = plan.Lifetimes()[0];
    EXPECT_EQ(lifetime.firstUse, 0U);
    EXPECT_EQ(lifetime.lastUse, 3U);
    EXPECT_EQ(lifetime.usageUnion,
              static_cast<std::uint32_t>(Rhi::BufferUsage::CopyDestination | Rhi::BufferUsage::Vertex));
    ASSERT_EQ(plan.ResourceVersions().size(), 3U);
    ASSERT_EQ(plan.PhysicalAllocations().size(), 1U);
    EXPECT_EQ(plan.PhysicalAllocations()[0].resource, lifetime.resource);
    for (const auto& transition : plan.Transitions())
        EXPECT_EQ(transition.physicalSlot, lifetime.physicalSlot);
}

TEST(TransientLifetimeTests, CullingUsesDenseLiveOrderOnly)
{
    RenderGraph graph;
    auto dead = graph.CreateTexture("dead", MakeColorDesc());
    AddDeadColorWrite(graph, "culled writer", dead);
    auto live = graph.CreateTexture("live", MakeColorDesc());
    AddLiveTextureCopyWrite(graph, "live writer", live);

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.ExecutionOrder().size(), 1U);
    EXPECT_EQ(plan.ExecutionOrder()[0], 1U);
    ASSERT_EQ(plan.Lifetimes().size(), 1U);
    EXPECT_EQ(plan.Lifetimes()[0].resource, live.Resource());
    EXPECT_EQ(plan.Lifetimes()[0].firstUse, 0U);
    EXPECT_EQ(plan.Lifetimes()[0].lastUse, 0U);
    EXPECT_EQ(plan.Statistics().culledPasses, 1U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 1U);
}

TEST(TransientLifetimeTests, ImportedResourceNeverSharesTransientSlot)
{
    RenderGraph graph;
    const auto external = graph.ImportTexture(
        "external", {Rhi::TextureHandle{10, 1, 1}, MakeColorDesc(), Rhi::ResourceAccess::CopyDestination,
                     Rhi::ResourceAccess::SampledRead, ContentState::Defined, "external owner", "CPU test evidence"});
    AddLiveTextureRead(graph, "external read", external);
    auto transient = graph.CreateTexture("transient", MakeColorDesc());
    AddLiveTextureCopyWrite(graph, "transient write", transient);

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.Lifetimes().size(), 2U);
    ASSERT_EQ(plan.PhysicalAllocations().size(), 2U);
    EXPECT_EQ(plan.Statistics().physicalTransients, 1U);
    EXPECT_TRUE(plan.PhysicalAllocations()[0].imported);
    EXPECT_FALSE(plan.PhysicalAllocations()[1].imported);
    EXPECT_NE(plan.Lifetimes()[0].physicalSlot, plan.Lifetimes()[1].physicalSlot);
}

TEST(TransientLifetimeTests, TransientExportIsRejected)
{
    RenderGraph graph;
    const auto transient = graph.CreateBuffer("transient", MakeBufferDesc());
    EXPECT_THROW(graph.Export(transient), GraphCompileError);
}

TEST(TransientLifetimeTests, UndefinedTransientReadIsRejected)
{
    RenderGraph graph;
    const auto transient = graph.CreateTexture("undefined", MakeColorDesc());
    AddLiveTextureRead(graph, "undefined read", transient);

    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}

TEST(TransientLifetimeTests, DontCareStoreMakesFollowingReadUndefined)
{
    RenderGraph graph;
    auto transient = graph.CreateTexture("discarded", MakeColorDesc());
    AddLiveColorWrite(graph, "discard", transient, Rhi::LoadOp::Clear, Rhi::StoreOp::DontCare);
    AddLiveTextureRead(graph, "read discarded", transient);

    EXPECT_THROW((void)graph.Compile(), UndefinedContentError);
}

TEST(TransientLifetimeTests, OwnerSwitchResetsContentButRetainsPhysicalAccess)
{
    RenderGraph graph;
    auto first = graph.CreateTexture("first", MakeColorDesc());
    auto second = graph.CreateTexture("second", MakeColorDesc());
    AddLiveColorWrite(graph, "first color write", first);
    AddLiveTextureCopyWrite(graph, "second copy write", second);

    const auto plan = graph.Compile();

    ASSERT_EQ(plan.Transitions().size(), 2U);
    EXPECT_EQ(plan.Transitions()[0].before, Rhi::ResourceAccess::None);
    EXPECT_EQ(plan.Transitions()[0].after, Rhi::ResourceAccess::ColorWrite);
    EXPECT_TRUE(plan.Transitions()[0].resetContent);
    EXPECT_EQ(plan.Transitions()[1].before, Rhi::ResourceAccess::ColorWrite);
    EXPECT_EQ(plan.Transitions()[1].after, Rhi::ResourceAccess::CopyDestination);
    EXPECT_TRUE(plan.Transitions()[1].resetContent);
}
TEST(TransientLifetimeTests, UnsupportedSampleAndLayerCountsFailBeforeAllocation)
{
    auto desc = MakeColorDesc();
    desc.sampleCount = 2;
    EXPECT_THROW(PhysicalCountForTexturePair(MakeColorDesc(), desc), Rhi::RhiValidationError);
    desc = MakeColorDesc();
    desc.arrayLayers = 2;
    EXPECT_THROW(PhysicalCountForTexturePair(MakeColorDesc(), desc), Rhi::RhiValidationError);
}
TEST(TransientLifetimeTests, EveryBufferDescriptorFieldAndKindIsolated)
{
    const Rhi::BufferDesc baseline{32, Rhi::BufferUsage::CopyDestination, Rhi::MemoryDomain::GpuOnly, {}};
    const auto count = [&](Rhi::BufferDesc second)
    {
        RenderGraph graph;
        auto a = graph.CreateBuffer("a", baseline);
        auto b = graph.CreateBuffer("b", second);
        AddLiveBufferCopyWrite(graph, "a", a);
        AddLiveBufferCopyWrite(graph, "b", b);
        return graph.Compile().Statistics().physicalTransients;
    };
    EXPECT_EQ(count(baseline), 1U);
    auto variant = baseline;
    variant.size = 64;
    EXPECT_EQ(count(variant), 2U);
    variant = baseline;
    variant.usage = variant.usage | Rhi::BufferUsage::CopySource;
    EXPECT_EQ(count(variant), 2U);
    variant = baseline;
    variant.memory = Rhi::MemoryDomain::GpuToCpu;
    EXPECT_EQ(count(variant), 2U);
    variant = baseline;
    variant.debugName = "different debug label";
    EXPECT_EQ(count(variant), 1U);
    RenderGraph graph;
    auto buffer = graph.CreateBuffer("buffer", baseline);
    auto texture = graph.CreateTexture("texture", MakeColorDesc());
    AddLiveBufferCopyWrite(graph, "buffer", buffer);
    AddLiveTextureCopyWrite(graph, "texture", texture);
    EXPECT_EQ(graph.Compile().Statistics().physicalTransients, 2U);
}
} // namespace MiniEngine::RenderGraph::Tests
