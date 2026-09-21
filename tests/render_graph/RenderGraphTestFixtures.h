#pragma once
#include <MiniEngine/RenderGraph/RenderGraph.h>
#include <array>

namespace MiniEngine::RenderGraph::Tests
{
struct TexturePassData
{
    RgTexture texture;
};
struct BufferPassData
{
    RgBuffer buffer;
};
inline Rhi::TextureDesc MakeColorDesc()
{
    Rhi::TextureDesc desc;
    desc.extent = {32, 24};
    desc.format = Rhi::Format::Rgba8Unorm;
    desc.usage = Rhi::TextureUsage::ColorAttachment | Rhi::TextureUsage::Sampled | Rhi::TextureUsage::CopySource |
                 Rhi::TextureUsage::CopyDestination;
    return desc;
}
inline Rhi::TextureDesc MakeDepthDesc()
{
    auto desc = MakeColorDesc();
    desc.format = Rhi::Format::D32Float;
    desc.usage = Rhi::TextureUsage::DepthStencil | Rhi::TextureUsage::Sampled;
    return desc;
}
inline Rhi::BufferDesc MakeBufferDesc()
{
    return {32,
            Rhi::BufferUsage::CopySource | Rhi::BufferUsage::CopyDestination | Rhi::BufferUsage::Vertex,
            Rhi::MemoryDomain::GpuOnly,
            {}};
}
inline RgTexture AddAttachmentPass(RenderGraph& graph, std::string_view name, RgTexture input,
                                   Rhi::LoadOp load = Rhi::LoadOp::Clear, Rhi::StoreOp store = Rhi::StoreOp::Store,
                                   bool depth = false)
{
    RgTexture output;
    graph.AddPass<TexturePassData>(
        name,
        [&](RgBuilder& builder, TexturePassData& data)
        {
            data.texture =
                builder.Write(input, depth ? Rhi::ResourceAccess::DepthWrite : Rhi::ResourceAccess::ColorWrite);
            if (depth)
                builder.SetDepthAttachment(data.texture, load, store);
            else
                builder.SetColorAttachment(data.texture, load, store);
            output = data.texture;
        },
        [](const TexturePassData&, const RgResources&, Rhi::IRhiCommandList&) {});
    return output;
}
inline void AddSamplePass(RenderGraph& graph, std::string_view name, RgTexture input)
{
    graph.AddPass<TexturePassData>(
        name, [&](RgBuilder& builder, TexturePassData& data)
        { data.texture = builder.Read(input, Rhi::ResourceAccess::SampledRead); },
        [](const TexturePassData&, const RgResources&, Rhi::IRhiCommandList&) {});
}
inline RgBuffer AddBufferCopyWrite(RenderGraph& graph, std::string_view name, RgBuffer input,
                                   WriteCoverage coverage = WriteCoverage::Full)
{
    RgBuffer output;
    graph.AddPass<BufferPassData>(
        name, [&](RgBuilder& builder, BufferPassData& data)
        { output = data.buffer = builder.Write(input, Rhi::ResourceAccess::CopyDestination, coverage); },
        [](const BufferPassData&, const RgResources&, Rhi::IRhiCommandList&) {});
    return output;
}
} // namespace MiniEngine::RenderGraph::Tests
