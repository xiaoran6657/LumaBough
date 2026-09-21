#include "CommandValidation.h"
#include <MiniEngine/Rhi/RhiPipeline.h>
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>

using namespace MiniEngine::Rhi;
namespace
{
RhiCapabilities Capabilities()
{
    RhiCapabilities c;
    c.maxTextureDimension2D = 16384;
    c.maxColorAttachments = 4;
    c.maxAnisotropy = 16;
    c.uniformBufferOffsetAlignment = 256;
    c.formatSupport.fill(31);
    c.cubeFormatSupport.fill(31);
    return c;
}
ShaderDesc Shader(ShaderStage stage)
{
    static const std::array<std::byte, 4> bytes{};
    ShaderDesc s;
    s.stage = stage;
    s.bytecode = bytes;
    s.sourceHash = std::string(64, 'a');
    s.semanticHash = std::string(64, 'b');
    s.entryPoint = stage == ShaderStage::Vertex ? "VSMain" : "PSMain";
    if (stage == ShaderStage::Vertex)
        s.manifest.vertexInputs.push_back({VertexSemantic::Position, VertexFormat::Float3, 0});
    else
        s.manifest.colorOutputMask = 1;
    return s;
}
GraphicsPipelineDesc Pipeline()
{
    GraphicsPipelineDesc p;
    p.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
    p.colorAttachmentCount = 1;
    p.colorFormats[0] = Format::Rgba8Unorm;
    p.depthTest = p.depthWrite = false;
    return p;
}
TEST(PipelineContract, ManifestRequiresIdentityAndRejectsStageBindingAndInputErrors)
{
    auto shader = Shader(ShaderStage::Vertex);
    EXPECT_NO_THROW(ValidateShaderDesc(shader));
    shader.semanticHash.clear();
    EXPECT_THROW(ValidateShaderDesc(shader), RhiValidationError);
    shader = Shader(ShaderStage::Vertex);
    shader.manifest.vertexInputs.push_back(shader.manifest.vertexInputs[0]);
    EXPECT_THROW(ValidateShaderDesc(shader), RhiValidationError);
    shader = Shader(ShaderStage::Pixel);
    shader.manifest.bindings.push_back({0, 0, BindingType::UniformBuffer, 1, 0});
    EXPECT_THROW(ValidateShaderDesc(shader), RhiValidationError);
    shader = Shader(ShaderStage::Vertex);
    shader.manifest.colorOutputMask = 1;
    EXPECT_THROW(ValidateShaderDesc(shader), RhiValidationError);
}
TEST(PipelineContract, ReflectionLayoutAndAttachmentMismatchFailBeforeCreation)
{
    auto vs = Shader(ShaderStage::Vertex), ps = Shader(ShaderStage::Pixel);
    auto p = Pipeline();
    auto caps = Capabilities();
    ResourceSetLayoutDesc set{0, {{0, BindingType::UniformBuffer, 1, ShaderStage::Pixel, false, 64}}, ""};
    ps.manifest.bindings.push_back({0, 0, BindingType::UniformBuffer, 1, 64});
    std::array layouts{set};
    EXPECT_NO_THROW(ValidateGraphicsPipeline(p, vs, &ps, layouts, caps));
    layouts[0].entries[0].visibility = ShaderStage::Vertex;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, &ps, layouts, caps), RhiValidationError);
    layouts[0] = set;
    layouts[0].entries[0].uniformBytes = 48;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, &ps, layouts, caps), RhiValidationError);
    layouts[0] = set;
    p.colorFormats[0] = Format::D32Float;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, &ps, layouts, caps), RhiValidationError);
    p = Pipeline();
    ps.manifest.colorOutputMask = 3;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, &ps, layouts, caps), RhiValidationError);
    ps.manifest.colorOutputMask = 1;
    p.vertexAttributes[0].format = VertexFormat::Float2;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, &ps, layouts, caps), RhiValidationError);
}
TEST(PipelineContract, DepthOnlyPipelineAcceptsAbsentPixelAndPreservesShadowRasterFields)
{
    auto vs = Shader(ShaderStage::Vertex);
    GraphicsPipelineDesc p;
    p.vertexAttributes = Pipeline().vertexAttributes;
    p.depthFormat = Format::D32Float;
    p.depthBias = 100;
    p.slopeScaledDepthBias = 1;
    EXPECT_NO_THROW(ValidateGraphicsPipeline(p, vs, nullptr, {}, Capabilities()));
    auto key = PipelineSemanticKey(p, vs, nullptr, {});
    p.frontFace = FrontFace::CounterClockwise;
    EXPECT_NE(key, PipelineSemanticKey(p, vs, nullptr, {}));
    p.depthFormat = Format::Unknown;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, nullptr, {}, Capabilities()), RhiValidationError);
}
TEST(PipelineContract, LegacyD24DepthIsExpressibleAndCannotBecomeColor)
{
    auto vs = Shader(ShaderStage::Vertex), ps = Shader(ShaderStage::Pixel);
    auto p = Pipeline();
    auto caps = Capabilities();
    p.depthFormat = Format::D24UnormS8Uint;
    p.depthTest = p.depthWrite = true;
    EXPECT_NO_THROW(ValidateGraphicsPipeline(p, vs, &ps, {}, caps));
    const auto d24 = PipelineSemanticKey(p, vs, &ps, {});
    p.depthFormat = Format::D32Float;
    EXPECT_NE(d24, PipelineSemanticKey(p, vs, &ps, {}));
    p.colorFormats[0] = Format::D24UnormS8Uint;
    EXPECT_THROW(ValidateGraphicsPipeline(p, vs, &ps, {}, caps), RhiValidationError);
    TextureDesc depth{TextureDimension::Texture2D,
                      {16, 16},
                      1,
                      1,
                      1,
                      Format::D24UnormS8Uint,
                      TextureUsage::DepthStencil | TextureUsage::Sampled,
                      "legacy main depth"};
    EXPECT_NO_THROW(ValidateTextureDesc(depth, caps));
    depth.usage = TextureUsage::ColorAttachment;
    EXPECT_THROW(ValidateTextureDesc(depth, caps), RhiValidationError);
    depth.usage = TextureUsage::DepthStencil;
    caps.formatSupport[static_cast<std::size_t>(Format::D24UnormS8Uint)] = 0;
    EXPECT_THROW(ValidateTextureDesc(depth, caps), RhiValidationError);
}
TEST(PipelineContract, CanonicalLayoutIgnoresNamesAndEntryOrderButChecksEveryBindingField)
{
    ResourceSetLayoutDesc a{0,
                            {{0, BindingType::UniformBuffer, 1, ShaderStage::Vertex, false, 64},
                             {3, BindingType::Sampler, 1, ShaderStage::Pixel}},
                            "a"};
    auto b = a;
    std::reverse(b.entries.begin(), b.entries.end());
    b.debugName = "b";
    EXPECT_TRUE(LayoutCompatible(a, b));
    EXPECT_EQ(SemanticHash(a), SemanticHash(b));
    for (int field = 0; field < 6; ++field)
    {
        b = a;
        switch (field)
        {
        case 0:
            b.set = 1;
            break;
        case 1:
            b.entries[0].binding = 1;
            break;
        case 2:
            b.entries[0].count = 2;
            break;
        case 3:
            b.entries[0].visibility = ShaderStage::Pixel;
            break;
        case 4:
            b.entries[0].dynamicOffset = true;
            break;
        case 5:
            b.entries[0].uniformBytes = 80;
            break;
        }
        EXPECT_FALSE(LayoutCompatible(a, b));
    }
    b = a;
    b.entries[1].comparisonSampler = true;
    EXPECT_FALSE(LayoutCompatible(a, b));
}
TEST(PipelineContract, PipelineKeyUsesSemanticRevisionsAndAllRasterDepthFieldsInsteadOfHandles)
{
    auto vs = Shader(ShaderStage::Vertex), ps = Shader(ShaderStage::Pixel);
    auto p = Pipeline();
    const auto key = PipelineSemanticKey(p, vs, &ps, {});
    p.debugName = "another";
    p.vertexShader = ShaderHandle{5, 6, 7};
    p.pixelShader = ShaderHandle{1, 2, 3};
    p.layout = PipelineLayoutHandle{3, 4, 5};
    vs.sourceHash = std::string(64, 'c');
    EXPECT_EQ(key, PipelineSemanticKey(p, vs, &ps, {}));
    for (int field = 0; field < 13; ++field)
    {
        auto changed = Pipeline();
        switch (field)
        {
        case 0:
            changed.frontFace = FrontFace::CounterClockwise;
            break;
        case 1:
            changed.cullMode = CullMode::None;
            break;
        case 2:
            changed.depthClip = false;
            break;
        case 3:
            changed.depthBias = 2;
            break;
        case 4:
            changed.depthBiasClamp = 0.25F;
            break;
        case 5:
            changed.slopeScaledDepthBias = 1.5F;
            break;
        case 6:
            changed.depthTest = true;
            break;
        case 7:
            changed.depthWrite = true;
            break;
        case 8:
            changed.depthCompare = CompareOp::Always;
            break;
        case 9:
            changed.alphaBlend = true;
            break;
        case 10:
            changed.colorFormats[0] = Format::Rgba16Float;
            break;
        case 11:
            changed.vertexAttributes[0].instanceStepRate = 1;
            break;
        case 12:
            changed.sampleCount = 2;
            break;
        }
        EXPECT_NE(key, PipelineSemanticKey(changed, vs, &ps, {})) << field;
    }
    p = Pipeline();
    p.depthBiasClamp = -0.0F;
    EXPECT_EQ(key, PipelineSemanticKey(p, vs, &ps, {}));
    vs.semanticHash[0] = 'd';
    EXPECT_NE(key, PipelineSemanticKey(p, vs, &ps, {}));
    p.slopeScaledDepthBias = std::numeric_limits<float>::infinity();
    EXPECT_THROW(PipelineSemanticKey(p, vs, &ps, {}), RhiValidationError);
}
struct Payload : ResourcePayload
{
    explicit Payload(int& count) : live(count)
    {
        ++live;
    }
    ~Payload() override
    {
        --live;
    }
    int& live;
};
class CommandValidationTest : public testing::Test
{
  protected:
    int live = 0;
    DeviceLifetime device{Capabilities()};
    std::vector<ResourceIdentity> owned;
    SwapChainHandle chain;
    FrameToken frame;
    PayloadFactory Make()
    {
        return [&] { return std::make_unique<Payload>(live); };
    }
    template <class H> H Keep(H h)
    {
        owned.emplace_back(h);
        return h;
    }
    ShaderHandle MakeShader(ShaderDesc desc)
    {
        return Keep(device.CreateShader(desc, Make()));
    }
    ResourceSetLayoutHandle Layout(bool dynamic = true, std::uint32_t uniformBytes = 64)
    {
        return Keep(device.CreateResourceSetLayout(
            {0, {{0, BindingType::UniformBuffer, 1, ShaderStage::Pixel, dynamic, uniformBytes}}, ""}, Make()));
    }
    GraphicsPipelineHandle MakePipeline(ResourceSetLayoutHandle layout = {})
    {
        auto vs = Shader(ShaderStage::Vertex), ps = Shader(ShaderStage::Pixel);
        if (layout)
            ps.manifest.bindings.push_back({0, 0, BindingType::UniformBuffer, 1, 64});
        auto vertex = MakeShader(vs), pixel = MakeShader(ps);
        PipelineLayoutDesc l;
        if (layout)
        {
            l.setCount = 1;
            l.sets[0] = layout;
        }
        auto pl = Keep(device.CreatePipelineLayout(l, Make()));
        auto p = Pipeline();
        p.vertexShader = vertex;
        p.pixelShader = pixel;
        p.layout = pl;
        return Keep(device.CreateGraphicsPipeline(p, Make()));
    }
    ResourceSetHandle Set(ResourceSetLayoutHandle layout)
    {
        auto buffer = Keep(device.CreateBuffer({512, BufferUsage::Uniform, MemoryDomain::CpuToGpu, ""}, Make()));
        ResourceBinding b;
        b.buffer = {buffer, 0, 64};
        return Keep(device.CreateResourceSet({layout, {b}, ""}, Make()));
    }
    BufferHandle Vertex()
    {
        return Keep(device.CreateBuffer({36, BufferUsage::Vertex, MemoryDomain::GpuOnly, ""}, Make()));
    }
    void Start()
    {
        chain = Keep(device.CreateSwapChain({{32, 32}}, Make()));
        device.ResizeBackBuffers(chain, {32, 32},
                                 [&]
                                 {
                                     std::vector<DeviceLifetime::BackBufferCandidate> result;
                                     for (int i = 0; i < 3; ++i)
                                         result.push_back({{TextureDimension::Texture2D,
                                                            {32, 32},
                                                            1,
                                                            1,
                                                            1,
                                                            Format::Rgba8Unorm,
                                                            TextureUsage::ColorAttachment,
                                                            ""},
                                                           Make()()});
                                     return result;
                                 });
        frame = device.BeginFrame(chain);
    }
    void Rendering(CommandValidation& command)
    {
        const ColorAttachment color{frame.backBuffer};
        command.BeginRendering({std::span(&color, 1), nullptr, {32, 32}});
    }
    void TearDown() override
    {
        if (device.Diagnostics().activeFrameSerial)
            device.EndFrame(frame, chain);
        device.Collect(device.LastSubmitted());
        for (auto it = owned.rbegin(); it != owned.rend(); ++it)
        {
            try
            {
                device.ValidateAlive(*it);
            }
            catch (const RhiValidationError& e)
            {
                EXPECT_EQ(e.Error().code, RhiErrorCode::InvalidHandle);
                continue;
            }
            EXPECT_NO_THROW(device.Destroy(*it));
        }
        device.Collect(device.LastSubmitted());
        EXPECT_NO_THROW(device.CheckShutdown());
        EXPECT_EQ(live, 0);
    }
};
TEST_F(CommandValidationTest, DrawRequiresPipelineSetAndVertexBufferAndRejectsInvalidRanges)
{
    auto layout = Layout(), clone = Layout();
    auto set = Set(clone);
    auto pipeline = MakePipeline(layout);
    auto vertex = Vertex();
    Start();
    CommandValidation commands(device, frame);
    Rendering(commands);
    EXPECT_THROW(commands.Draw(3), RhiValidationError);
    commands.SetPipeline(pipeline);
    EXPECT_THROW(commands.Draw(3), RhiValidationError);
    const std::array<std::uint32_t, 1> offset{256};
    commands.BindResourceSet(0, set, offset);
    EXPECT_THROW(commands.Draw(3), RhiValidationError);
    commands.BindVertexBuffer(0, {vertex, 0, 36}, 12);
    EXPECT_NO_THROW(commands.Draw(3));
    EXPECT_THROW(commands.Draw(3, 1, 1), RhiValidationError);
    EXPECT_THROW(commands.DrawIndexed(3), RhiValidationError);
    commands.EndRendering();
    EXPECT_THROW(commands.Draw(3), RhiValidationError);
}
TEST_F(CommandValidationTest, InstanceStartIsAddedAfterStepDivisionForBothDrawForms)
{
    auto p = Pipeline();
    p.vertexShader = MakeShader(Shader(ShaderStage::Vertex));
    p.pixelShader = MakeShader(Shader(ShaderStage::Pixel));
    p.layout = Keep(device.CreatePipelineLayout({}, Make()));
    p.vertexAttributes[0].instanceStepRate = 2;
    const auto pipeline = Keep(device.CreateGraphicsPipeline(p, Make()));
    const auto vertex = Vertex();
    const auto index = Keep(device.CreateBuffer({6, BufferUsage::Index, MemoryDomain::GpuOnly, ""}, Make()));
    Start();
    CommandValidation commands(device, frame);
    Rendering(commands);
    commands.SetPipeline(pipeline);
    commands.BindVertexBuffer(0, {vertex, 0, 36}, 12);
    commands.BindIndexBuffer({index, 0, 6}, IndexType::UInt16);
    EXPECT_NO_THROW(commands.Draw(3, 4, 0, 1));
    EXPECT_NO_THROW(commands.DrawIndexed(3, 4, 0, 0, 1));
    EXPECT_THROW(commands.Draw(3, 3, 0, 2), RhiValidationError);
    EXPECT_THROW(commands.DrawIndexed(3, 3, 0, 0, 2), RhiValidationError);
    EXPECT_THROW(commands.Draw(3, 1, 0, 3), RhiValidationError);
}
TEST_F(CommandValidationTest, IndexedBaseVertexRejectsUnsupportedOffsetsForBothIndexTypes)
{
    const auto pipeline = MakePipeline();
    const auto vertex = Vertex();
    const auto index = Keep(device.CreateBuffer({12, BufferUsage::Index, MemoryDomain::GpuOnly, ""}, Make()));
    Start();
    CommandValidation commands(device, frame);
    Rendering(commands);
    commands.SetPipeline(pipeline);
    commands.BindVertexBuffer(0, {vertex, 0, 12}, 12);
    for (auto type : {IndexType::UInt16, IndexType::UInt32})
    {
        const std::uint64_t bytes = type == IndexType::UInt16 ? 6 : 12;
        commands.BindIndexBuffer({index, 0, bytes}, type);
        // 可以用 index {0,0,0} 访问单个顶点；CPU validator 不读取或猜测 GPU index 内容。
        EXPECT_NO_THROW(commands.DrawIndexed(3, 1, 0, 0, 0));
        for (auto base : {1, -1, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()})
            EXPECT_THROW(commands.DrawIndexed(3, 1, 0, base, 0), RhiValidationError);
        EXPECT_THROW(commands.DrawIndexed(3, 1, 1, 0, 0), RhiValidationError);
    }
}
TEST_F(CommandValidationTest, DynamicOffsetsRequireExactCountAlignmentAndFullRange)
{
    auto layout = Layout();
    auto set = Set(layout);
    auto pipeline = MakePipeline(layout);
    Start();
    CommandValidation commands(device, frame);
    commands.SetPipeline(pipeline);
    EXPECT_THROW(commands.BindResourceSet(0, set, {}), RhiValidationError);
    const std::array<std::uint32_t, 1> unaligned{16}, pastEnd{512}, valid{256};
    EXPECT_THROW(commands.BindResourceSet(0, set, unaligned), RhiValidationError);
    EXPECT_THROW(commands.BindResourceSet(0, set, pastEnd), RhiValidationError);
    EXPECT_THROW(commands.BindResourceSet(1, set, valid), RhiValidationError);
    EXPECT_NO_THROW(commands.BindResourceSet(0, set, valid));
}
TEST_F(CommandValidationTest, CompatiblePipelineSwitchRetainsSetsAndChangedLayoutInvalidatesThem)
{
    auto a = Layout(), same = Layout(), different = Layout(false);
    auto set = Set(a);
    auto pa = MakePipeline(a), pb = MakePipeline(same), pc = MakePipeline(different);
    Start();
    CommandValidation commands(device, frame);
    commands.SetPipeline(pa);
    const std::array<std::uint32_t, 1> zero{0};
    commands.BindResourceSet(0, set, zero);
    commands.SetPipeline(pb);
    EXPECT_TRUE(commands.HasSet(0));
    commands.SetPipeline(pc);
    EXPECT_FALSE(commands.HasSet(0));
    EXPECT_THROW(commands.BindResourceSet(0, set, zero), RhiValidationError);
}
TEST_F(CommandValidationTest, InvalidPipelineAndFrameCreationNeverReachNativeCallback)
{
    auto vs = MakeShader(Shader(ShaderStage::Vertex)), ps = MakeShader(Shader(ShaderStage::Pixel));
    auto layout = Keep(device.CreatePipelineLayout({}, Make()));
    auto p = Pipeline();
    p.vertexShader = vs;
    p.pixelShader = ps;
    p.layout = layout;
    p.colorFormats[0] = Format::Unknown;
    bool invoked = false;
    EXPECT_THROW(device.CreateGraphicsPipeline(p,
                                               [&]
                                               {
                                                   invoked = true;
                                                   return Make()();
                                               }),
                 RhiValidationError);
    EXPECT_FALSE(invoked);
    p.colorFormats[0] = Format::Rgba8Unorm;
    Start();
    const auto before = device.PipelineCreationCount();
    EXPECT_THROW(device.CreateGraphicsPipeline(p,
                                               [&]
                                               {
                                                   invoked = true;
                                                   return Make()();
                                               }),
                 RhiValidationError);
    EXPECT_THROW(device.CreateShader(Shader(ShaderStage::Vertex),
                                     [&]
                                     {
                                         invoked = true;
                                         return Make()();
                                     }),
                 RhiValidationError);
    EXPECT_THROW(device.CreateSampler({},
                                      [&]
                                      {
                                          invoked = true;
                                          return Make()();
                                      }),
                 RhiValidationError);
    EXPECT_FALSE(invoked);
    EXPECT_EQ(before, device.PipelineCreationCount());
}
TEST_F(CommandValidationTest, StaleCommandFrameAndDestroyedPipelineAreRejected)
{
    auto pipeline = MakePipeline();
    auto vertex = Vertex();
    Start();
    CommandValidation commands(device, frame);
    Rendering(commands);
    commands.SetPipeline(pipeline);
    commands.BindVertexBuffer(0, {vertex, 0, 36}, 12);
    device.Destroy(pipeline);
    EXPECT_THROW(commands.Draw(3), RhiValidationError);
    commands.EndRendering();
    device.EndFrame(frame, chain);
    EXPECT_THROW(commands.SetPipeline(pipeline), RhiValidationError);
}
TEST_F(CommandValidationTest, AttachmentMismatchAndIndexBufferBoundsAreChecked)
{
    auto pipeline = MakePipeline();
    auto vertex = Vertex();
    auto indices = Keep(device.CreateBuffer({6, BufferUsage::Index, MemoryDomain::GpuOnly, ""}, Make()));
    auto color = Keep(device.CreateTexture(
        {TextureDimension::Texture2D, {32, 32}, 1, 1, 1, Format::Rgba16Float, TextureUsage::ColorAttachment, ""},
        Make()));
    Start();
    CommandValidation commands(device, frame);
    const ColorAttachment hdr{color};
    commands.BeginRendering({std::span(&hdr, 1), nullptr, {32, 32}});
    commands.SetPipeline(pipeline);
    commands.BindVertexBuffer(0, {vertex, 0, 36}, 12);
    commands.BindIndexBuffer({indices, 0, 6}, IndexType::UInt16);
    EXPECT_THROW(commands.DrawIndexed(3), RhiValidationError);
    commands.EndRendering();
    Rendering(commands);
    EXPECT_NO_THROW(commands.DrawIndexed(3));
    EXPECT_THROW(commands.DrawIndexed(4), RhiValidationError);
    EXPECT_THROW(commands.BindIndexBuffer({vertex, 0, 6}, IndexType::UInt16), RhiValidationError);
}
} // namespace
