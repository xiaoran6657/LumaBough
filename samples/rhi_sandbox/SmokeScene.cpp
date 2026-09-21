#include "SmokeScene.h"
#include <MiniEngine/Rhi/RhiPipeline.h>
#include <algorithm>
#include <cstring>

namespace MiniEngine::Sandbox
{
using namespace Rhi;
namespace
{
template <class T> std::span<const std::byte> Bytes(const T& data)
{
    return std::as_bytes(std::span(&data, 1));
}
struct ToneConstants
{
    float exposure = 0;
    std::uint32_t debug = 0;
    float inverseWidth = 0;
    float inverseHeight = 0;
};
static_assert(sizeof(ToneConstants) == 16);
} // namespace
SmokeScene::Resources::~Resources()
{
    for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it)
    {
        try
        {
            (*it)();
        }
        catch (const RhiException&)
        { /* 被热重载撤销的 handle 无需再次销毁。 */
        }
    }
}
SmokeScene::~SmokeScene() = default;
SmokeScene::SmokeScene(IRhiDevice& device, const SmokeShaders& shaders, Extent2D extent, std::uint32_t level)
    : m_device(device), m_resources(device), m_extent(extent), m_level(level)
{
    if (level < 1 || level > 6)
        throw std::invalid_argument("smoke level must be 1..6");
    if (level >= 2)
    {
        ResourceSetLayoutDesc toneLayout;
        toneLayout.set = 0;
        toneLayout.entries = {{7, BindingType::Sampler, 1, ShaderStage::Pixel, false},
                              {9, BindingType::UniformBuffer, 1, ShaderStage::Pixel, false, 16},
                              {10, BindingType::SampledTexture, 1, ShaderStage::Pixel, false}};
        toneLayout.debugName = "M6.ToneMap.layout";
        m_toneLayout = m_resources.Own(device.CreateResourceSetLayout(toneLayout));
        PipelineLayoutDesc pl;
        pl.sets[0] = m_toneLayout;
        pl.setCount = 1;
        pl.debugName = "M6.ToneMap.pipeline-layout";
        const auto layout = m_resources.Own(device.CreatePipelineLayout(pl));
        const auto vs = m_resources.Own(device.CreateShader(shaders.toneVertex));
        const auto ps = m_resources.Own(device.CreateShader(shaders.tonePixel));
        GraphicsPipelineDesc pipeline;
        pipeline.vertexShader = vs;
        pipeline.pixelShader = ps;
        pipeline.layout = layout;
        pipeline.colorAttachmentCount = 1;
        pipeline.colorFormats[0] = Format::Rgba8Unorm;
        pipeline.cullMode = CullMode::None;
        pipeline.depthTest = pipeline.depthWrite = false;
        pipeline.debugName = "M6.ToneMap.pipeline";
        m_tone.pipeline = m_resources.Own(device.CreateGraphicsPipeline(pipeline));
        m_toneDescriptor = pipeline;
        m_toneVertex = vs;
        m_tonePixel = ps;
        m_tone.setCount = 1;
        SamplerDesc sampler;
        sampler.addressU = sampler.addressV = sampler.addressW = AddressMode::Clamp;
        sampler.debugName = "M6.LinearClamp";
        m_sampler = m_resources.Own(device.CreateSampler(sampler));
    }
    if (level >= 3)
    {
        PipelineLayoutDesc pl;
        for (std::uint8_t i = 0; i < 3; ++i)
        {
            ResourceSetLayoutDesc layout;
            layout.set = i;
            if (i == 2)
                layout.entries.push_back({0, BindingType::UniformBuffer, 1, ShaderStage::Vertex, true, 208});
            pl.sets[i] = m_resources.Own(device.CreateResourceSetLayout(layout));
            if (i == 2)
                m_drawLayout = pl.sets[i];
            else
                m_depth.sets[i] = m_resources.Own(device.CreateResourceSet({pl.sets[i], {}, "empty depth set"}));
        }
        pl.setCount = 3;
        pl.debugName = "M6.DepthMesh.pipeline-layout";
        const auto layout = m_resources.Own(device.CreatePipelineLayout(pl));
        const auto vs = m_resources.Own(device.CreateShader(shaders.depthVertex));
        GraphicsPipelineDesc pipeline;
        pipeline.vertexShader = vs;
        pipeline.layout = layout;
        pipeline.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
        pipeline.depthFormat = Format::D32Float;
        pipeline.cullMode = CullMode::None;
        pipeline.debugName = "M6.DepthMesh.pipeline";
        m_depth.pipeline = m_resources.Own(device.CreateGraphicsPipeline(pipeline));
        m_depth.setCount = 3;
        m_depth.dynamicOffsets[2] = {0};
        // 固定 NDC 三角形，中心 depth=0.25；矩阵全部 identity，排除相机差异。
        const std::array<float, 9> vertices{-0.75F, -0.75F, 0.25F, 0, 0.75F, 0.25F, 0.75F, -0.75F, 0.25F};
        const std::array<std::uint16_t, 3> indices{0, 1, 2};
        m_vertices = m_resources.Own(device.CreateBuffer(
            {sizeof(vertices), BufferUsage::Vertex | BufferUsage::CopySource, MemoryDomain::GpuOnly, "M6.vertices"},
            Bytes(vertices)));
        m_indices = m_resources.Own(device.CreateBuffer(
            {sizeof(indices), BufferUsage::Index, MemoryDomain::GpuOnly, "M6.indices"}, Bytes(indices)));
        for (std::size_t matrix = 0; matrix < 3; ++matrix)
            for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
                m_object[matrix * 16 + diagonal * 5] = 1;
        m_object[48] = 1;
    }
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        m_beginQueries[i] = m_resources.Own(device.CreateTimestampQuery("M6.frame.begin"));
        m_endQueries[i] = m_resources.Own(device.CreateTimestampQuery("M6.frame.end"));
    }
    BuildSizeResources();
}
void SmokeScene::BuildToneSet()
{
    const ToneConstants constants{m_exposure, 0, 1.0F / m_extent.width, 1.0F / m_extent.height};
    auto buffer = m_resources.Own(m_device.CreateBuffer(
        {sizeof(constants), BufferUsage::Uniform, MemoryDomain::GpuOnly, "M6.exposure"}, Bytes(constants)));
    ResourceSetDesc desc;
    desc.layout = m_toneLayout;
    desc.debugName = "M6.ToneMap.set";
    desc.bindings = {{7, 0, BindingType::Sampler, {}, {}, m_sampler},
                     {9, 0, BindingType::UniformBuffer, {buffer, 0, sizeof(constants)}},
                     {10, 0, BindingType::SampledTexture, {}, m_source}};
    const auto set = m_resources.Own(m_device.CreateResourceSet(desc));
    const auto oldSet = m_tone.sets[0];
    const auto oldBuffer = m_constants;
    m_tone.sets[0] = set;
    m_constants = buffer;
    // 完整候选成功之后才撤销旧 revision；owner 的 lastUse 负责实际退休。
    if (oldSet)
        m_device.Destroy(oldSet);
    if (oldBuffer)
    {
        m_device.Destroy(oldBuffer);
        m_bufferAccess.erase(oldBuffer);
    }
}
void SmokeScene::BuildSizeResources()
{
    if (m_level >= 2)
    {
        const bool depth = m_level >= 3;
        TextureDesc desc{
            TextureDimension::Texture2D,
            depth ? m_extent : Extent2D{4, 4},
            1,
            1,
            1,
            depth ? Format::D32Float : Format::Rgba16Float,
            TextureUsage::Sampled |
                (depth ? TextureUsage::DepthStencil : TextureUsage::ColorAttachment | TextureUsage::CopyDestination),
            depth ? "M6.depth source" : "M6.HDR source"};
        if (!depth)
            desc.clearColorHint = {4, 2, 1, 1};
        m_source = m_resources.Own(m_device.CreateTexture(desc));
        if (!depth)
        {
            // 有意带行 padding 的 FP16 上传，随后 clear/rewrite 检验读写 hazard。
            std::array<std::byte, 152> data{};
            const std::array<std::uint16_t, 4> pixel{0x4400, 0x4000, 0x3c00, 0x3c00};
            for (std::size_t y = 0; y < 4; ++y)
                for (std::size_t x = 0; x < 4; ++x)
                    std::memcpy(data.data() + y * 40 + x * 8, pixel.data(), 8);
            const TextureSubresourceData row{data, 40, data.size()};
            m_device.UploadTexture(m_source, std::span(&row, 1));
        }
        BuildToneSet();
    }
    const auto pitch = (static_cast<std::uint64_t>(m_extent.width) * 4 + 255) & ~std::uint64_t{255};
    for (auto& readback : m_readbacks)
        readback = m_resources.Own(m_device.CreateBuffer(
            {pitch * m_extent.height, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "M6.readback"}, {}));
}
void SmokeScene::Transition(IRhiGraphCommandSink& sink, TextureHandle handle, ResourceAccess after)
{
    const GraphResourceImport imported{handle, {}};
    sink.ImportResources(std::span(&imported, 1));
    const auto before = m_textureAccess.contains(handle) ? m_textureAccess.at(handle) : ResourceAccess::None;
    const AccessTransition transition{handle, {}, before, after};
    sink.ApplyTransitions(std::span(&transition, 1));
    m_textureAccess[handle] = after;
}
void SmokeScene::Transition(IRhiGraphCommandSink& sink, BufferHandle handle, ResourceAccess after)
{
    const GraphResourceImport imported{{}, handle};
    sink.ImportResources(std::span(&imported, 1));
    const auto before = m_bufferAccess.contains(handle) ? m_bufferAccess.at(handle) : ResourceAccess::None;
    const AccessTransition transition{{}, handle, before, after};
    sink.ApplyTransitions(std::span(&transition, 1));
    m_bufferAccess[handle] = after;
}
SmokeFrameResult SmokeScene::Render(SwapChainHandle chain, bool capture)
{
    const auto frame = m_device.BeginFrame(chain);
    if (!frame.serial)
        return {};
    auto& commands = m_device.BeginGraphics(frame);
    auto& sink = m_device.GraphCommandSink(frame);
    m_textureAccess.try_emplace(frame.backBuffer, ResourceAccess::Present);
    SmokeFrameResult result{frame, m_readbacks[frame.recycleLane], m_beginQueries[frame.recycleLane],
                            m_endQueries[frame.recycleLane]};
    if (capture)
        commands.WriteTimestamp(result.beginQuery);
    std::optional<DynamicBufferSlice> constants;
    if (m_level >= 3)
    {
        constants = m_device.WriteDynamicBuffer(frame, Bytes(m_object), 256);
        ResourceSetDesc desc;
        desc.layout = m_drawLayout;
        desc.bindings.push_back({0, 0, BindingType::UniformBuffer, constants->view});
        desc.debugName = "M6.frame draw set";
        m_depth.sets[2] = m_device.CreateFrameResourceSet(frame, desc);
        Transition(sink, constants->view.buffer, ResourceAccess::UniformRead);
        Transition(sink, m_vertices, ResourceAccess::VertexRead);
        Transition(sink, m_indices, ResourceAccess::IndexRead);
    }
    if (m_level == 1)
    {
        Transition(sink, frame.backBuffer, ResourceAccess::ColorWrite);
        Render::ClearPass(commands, frame.backBuffer, m_extent, {0.04F, 0.08F, 0.14F, 1});
    }
    else
    {
        Transition(sink, m_constants, ResourceAccess::UniformRead);
        // 同帧两次 SRV→attachment→SRV：不能靠每帧 ClearState 掩盖 hazard。
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            if (m_level >= 3)
            {
                Transition(sink, m_source, ResourceAccess::DepthWrite);
                Render::DepthMeshPass(commands, m_source, m_extent, m_depth, {m_vertices, 0, 36}, {m_indices, 0, 6});
            }
            else
            {
                Transition(sink, m_source, ResourceAccess::ColorWrite);
                Render::ClearPass(commands, m_source, {4, 4}, {4, 2, 1, 1});
            }
            Transition(sink, m_source, ResourceAccess::SampledRead);
            Transition(sink, frame.backBuffer, ResourceAccess::ColorWrite);
            Render::FullscreenPass(commands, frame.backBuffer, m_extent, m_tone);
        }
    }
    if (capture)
    {
        Transition(sink, frame.backBuffer, ResourceAccess::CopySource);
        Transition(sink, result.readback, ResourceAccess::CopyDestination);
        commands.CopyTextureForReadback(frame.backBuffer, result.readback, m_extent);
        commands.WriteTimestamp(result.endQuery);
    }
    Transition(sink, frame.backBuffer, ResourceAccess::Present);
    m_device.EndGraphics(frame, commands);
    m_device.EndFrame(frame, chain);
    if (constants)
        m_bufferAccess.erase(constants->view.buffer);
    return result;
}
void SmokeScene::Resize(Extent2D extent)
{
    if (!extent.width || !extent.height)
        throw std::invalid_argument("scene resize requires positive extent");
    // composition 已在 device.ResizeSwapChain 完成真实 idle/resize；这里只替换尺寸资源。
    if (m_tone.sets[0])
    {
        m_device.Destroy(m_tone.sets[0]);
        m_tone.sets[0] = {};
    }
    if (m_constants)
    {
        m_device.Destroy(m_constants);
        m_bufferAccess.erase(m_constants);
        m_constants = {};
    }
    if (m_source)
    {
        m_device.Destroy(m_source);
        m_source = {};
    }
    for (auto& buffer : m_readbacks)
    {
        m_device.Destroy(buffer);
        m_bufferAccess.erase(buffer);
        buffer = {};
    }
    m_textureAccess.clear();
    m_extent = extent;
    BuildSizeResources();
}
void SmokeScene::ReloadToneShaders(const ShaderDesc& vertex, const ShaderDesc& pixel)
{
    if (m_level < 2)
        throw std::invalid_argument("shader reload requires ToneMap");
    ShaderHandle candidateVertex, candidatePixel;
    GraphicsPipelineHandle candidatePipeline;
    try
    {
        candidateVertex = m_device.CreateShader(vertex);
        candidatePixel = m_device.CreateShader(pixel);
        auto descriptor = m_toneDescriptor;
        descriptor.vertexShader = candidateVertex;
        descriptor.pixelShader = candidatePixel;
        candidatePipeline = m_device.CreateGraphicsPipeline(descriptor);
        m_resources.Own(candidateVertex);
        m_resources.Own(candidatePixel);
        m_resources.Own(candidatePipeline);
        const auto oldPipeline = m_tone.pipeline;
        const auto oldVertex = m_toneVertex, oldPixel = m_tonePixel;
        m_tone.pipeline = candidatePipeline;
        m_toneVertex = candidateVertex;
        m_tonePixel = candidatePixel;
        m_toneDescriptor = descriptor;
        // 发布完整候选后撤销旧句柄；旧 shader/PSO 仍由 owner 保留至真实完成。
        m_device.Destroy(oldPipeline);
        m_device.Destroy(oldVertex);
        m_device.Destroy(oldPixel);
    }
    catch (...)
    {
        // 创建失败不改变当前 revision；尚未提交的候选可以立即交回 owner。
        if (candidatePipeline)
            m_device.Destroy(candidatePipeline);
        if (candidatePixel)
            m_device.Destroy(candidatePixel);
        if (candidateVertex)
            m_device.Destroy(candidateVertex);
        throw;
    }
}
void SmokeScene::ReloadExposure(float exposure)
{
    if (m_level < 2)
        throw std::invalid_argument("exposure requires ToneMap");
    const auto old = m_exposure;
    m_exposure = exposure;
    try
    {
        BuildToneSet();
    }
    catch (...)
    {
        m_exposure = old;
        throw;
    }
}
} // namespace MiniEngine::Sandbox
