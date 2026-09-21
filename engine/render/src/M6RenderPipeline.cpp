#include <MiniEngine/Render/M6RenderPipeline.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace MiniEngine::Render
{
namespace RG = RenderGraph;
using namespace Rhi;
namespace
{
struct DrawPass final
{
    struct BindingRefs final
    {
        RG::RgBuffer buffer;
        RG::RgTexture texture;
    };
    struct DrawRefs final
    {
        M6Draw draw;
        RG::RgBuffer vertex;
        RG::RgBuffer index;
        std::array<std::vector<BindingRefs>, 3> bindings;
    };
    std::vector<M6Draw> draws;
    RG::RgTexture sampled;
    Extent2D extent;
    // 每个 draw 的 exact graph handle 在 setup 中保存，execute 不再按物理句柄猜测来源。
    std::vector<DrawRefs> references;
};
struct ImportTable final
{
    const M6PipelineResources& resources;
    std::map<TextureHandle, RG::RgTexture> textures;
    std::map<BufferHandle, RG::RgBuffer> buffers;

    RG::RgBuffer ReadBuffer(RG::RgBuilder& builder, BufferHandle handle, ResourceAccess access)
    {
        if (!handle)
            return {};
        const auto found = std::find_if(resources.buffers.begin(), resources.buffers.end(),
                                        [handle](const auto& entry) { return entry.imported.physical == handle; });
        if (found == resources.buffers.end() || found->access != access)
            throw std::invalid_argument("M6 buffer import missing or access mismatch");
        auto it = buffers.find(handle);
        if (it == buffers.end())
            it = buffers.emplace(handle, builder.ImportBuffer(found->name, found->imported)).first;
        return builder.Read(it->second, access, found->stages);
    }
    RG::RgTexture ReadTexture(RG::RgBuilder& builder, TextureHandle handle)
    {
        if (!handle)
            return {};
        const auto found = std::find_if(resources.textures.begin(), resources.textures.end(),
                                        [handle](const auto& entry) { return entry.imported.physical == handle; });
        if (found == resources.textures.end())
            throw std::invalid_argument("M6 sampled texture import missing");
        auto it = textures.find(handle);
        if (it == textures.end())
            it = textures.emplace(handle, builder.ImportTexture(found->name, found->imported)).first;
        return builder.Read(it->second, ResourceAccess::SampledRead, ShaderStage::Pixel);
    }
    void Declare(RG::RgBuilder& builder, DrawPass& pass)
    {
        pass.references.clear();
        pass.references.reserve(pass.draws.size());
        for (const auto& draw : pass.draws)
        {
            DrawPass::DrawRefs refs;
            refs.draw = draw;
            refs.vertex = ReadBuffer(builder, draw.vertices.buffer, ResourceAccess::VertexRead);
            refs.index = ReadBuffer(builder, draw.indices.buffer, ResourceAccess::IndexRead);
            for (std::uint8_t set = 0; set < draw.setCount; ++set)
            {
                refs.bindings[set].reserve(draw.sets[set].bindings.size());
                for (const auto& binding : draw.sets[set].bindings)
                {
                    DrawPass::BindingRefs bindingRefs;
                    if (binding.type == BindingType::UniformBuffer)
                        bindingRefs.buffer = ReadBuffer(builder, binding.buffer.buffer, ResourceAccess::UniformRead);
                    else if (binding.type == BindingType::SampledTexture)
                    {
                        if (binding.texture)
                            bindingRefs.texture = ReadTexture(builder, binding.texture);
                        else if (pass.sampled)
                            bindingRefs.texture = pass.sampled;
                        else
                            throw std::invalid_argument("M6 sampled draw binding has no graph source");
                    }
                    refs.bindings[set].push_back(bindingRefs);
                }
            }
            pass.references.push_back(std::move(refs));
        }
    }
};
void ExecuteDraws(const DrawPass& data, const RG::RgResources& resources, IRhiCommandList& commands)
{
    commands.SetViewport({0, 0, static_cast<float>(data.extent.width), static_cast<float>(data.extent.height), 0, 1});
    commands.SetScissor({0, 0, data.extent.width, data.extent.height});
    for (const auto& refs : data.references)
    {
        const auto& draw = refs.draw;
        commands.BeginLabel(draw.stableId);
        commands.SetPipeline(draw.pipeline);
        for (std::uint8_t slot = 0; slot < draw.setCount; ++slot)
        {
            auto set = draw.sets[slot];
            for (std::size_t bindingIndex = 0; bindingIndex < set.bindings.size(); ++bindingIndex)
            {
                auto& binding = set.bindings[bindingIndex];
                const auto& bindingRefs = refs.bindings[slot].at(bindingIndex);
                if (binding.type == BindingType::UniformBuffer)
                    binding.buffer.buffer = resources.Get(bindingRefs.buffer);
                else if (binding.type == BindingType::SampledTexture)
                    binding.texture = resources.Get(bindingRefs.texture);
            }
            commands.BindResourceSet(slot, resources.CreateResourceSet(set), draw.dynamicOffsets[slot]);
        }
        if (draw.vertices.buffer)
        {
            auto vertices = draw.vertices;
            vertices.buffer = resources.Get(refs.vertex);
            commands.BindVertexBuffer(0, vertices, draw.stride);
        }
        if (draw.indexCount)
        {
            auto indices = draw.indices;
            indices.buffer = resources.Get(refs.index);
            commands.BindIndexBuffer(indices, draw.indexType);
            commands.DrawIndexed(draw.indexCount, 1, 0, 0, 0);
        }
        else
            commands.Draw(draw.vertexCount, 1, 0, 0);
        commands.EndLabel();
    }
}
struct CopyPass final
{
    RG::RgTexture source;
    RG::RgBuffer destination;
    Extent2D extent;
};
} // namespace

void DeclareM6RenderGraph(const World::RenderPacket& packet, const M6PipelineResources& resources,
                          RG::RenderGraph& graph)
{
    if (resources.level < 1 || resources.level > 9 || !resources.extent.width || !resources.extent.height)
        throw std::invalid_argument("invalid M6 migration level or extent");
    if (resources.level >= 4 && resources.opaque.size() != packet.mainOpaque.size())
        throw std::invalid_argument("M6 prepared draws do not match RenderPacket");
    if (resources.level >= 6 && resources.shadow.size() != packet.shadowCasters.size())
        throw std::invalid_argument("M6 prepared shadow draws do not match RenderPacket");
    ImportTable imports{resources};
    auto back = graph.ImportTexture("BackBuffer", resources.backBuffer);
    if (resources.timestamps)
        graph.AddPass<TimestampQueryHandle>(
            "Timestamp.Begin",
            [&](RG::RgBuilder& builder, auto& query)
            {
                query = resources.beginQuery;
                builder.SideEffect("frame timestamp begin");
            },
            [](const auto& query, const RG::RgResources&, IRhiCommandList& commands)
            { commands.WriteTimestamp(query); });

    RG::RgTexture hdr;
    if (resources.level >= 4)
    {
        auto shadow = graph.CreateTexture("ShadowMap", resources.shadowDesc);
        graph.AddPass<DrawPass>(
            "Shadow",
            [&](RG::RgBuilder& builder, DrawPass& data)
            {
                shadow = builder.Write(shadow, ResourceAccess::DepthWrite);
                builder.SetDepthAttachment(shadow, LoadOp::Clear, StoreOp::Store);
                data.extent = resources.shadowDesc.extent;
                if (resources.level >= 6)
                    data.draws = resources.shadow;
                imports.Declare(builder, data);
            },
            ExecuteDraws);
        auto depth = graph.CreateTexture("SceneDepth", resources.depthDesc);
        hdr = graph.CreateTexture("HdrColor", resources.hdrDesc);
        graph.AddPass<DrawPass>(
            "ForwardHDR",
            [&](RG::RgBuilder& builder, DrawPass& data)
            {
                data.sampled = builder.Read(shadow, ResourceAccess::SampledRead);
                hdr = builder.Write(hdr, ResourceAccess::ColorWrite);
                depth = builder.Write(depth, ResourceAccess::DepthWrite);
                builder.SetColorAttachment(hdr, LoadOp::Clear, StoreOp::Store, resources.hdrDesc.clearColorHint);
                builder.SetDepthAttachment(depth, LoadOp::Clear, StoreOp::Store);
                data.draws = resources.opaque;
                data.extent = resources.extent;
                imports.Declare(builder, data);
            },
            ExecuteDraws);
        if (resources.level >= 5)
            graph.AddPass<DrawPass>(
                "Skybox",
                [&](RG::RgBuilder& builder, DrawPass& data)
                {
                    hdr = builder.Write(hdr, ResourceAccess::ColorWrite);
                    builder.SetColorAttachment(hdr, LoadOp::Load, StoreOp::Store);
                    (void)builder.Read(depth, ResourceAccess::DepthRead);
                    builder.SetDepthAttachment(depth, LoadOp::Load, StoreOp::Store);
                    data.draws = {resources.sky};
                    data.extent = resources.extent;
                    imports.Declare(builder, data);
                },
                ExecuteDraws);
    }
    else if (resources.level == 3)
        hdr = graph.ImportTexture("ToneSource", resources.toneSource);

    graph.AddPass<DrawPass>(
        resources.level == 1   ? "Clear"
        : resources.level == 2 ? "Triangle"
                               : "ToneMap",
        [&](RG::RgBuilder& builder, DrawPass& data)
        {
            back = builder.Write(back, ResourceAccess::ColorWrite);
            builder.SetColorAttachment(back, LoadOp::Clear, StoreOp::Store, {0.02F, 0.03F, 0.05F, 1});
            data.extent = resources.extent;
            if (resources.level >= 3)
            {
                data.sampled = builder.Read(hdr, ResourceAccess::SampledRead);
                data.draws = {resources.tone};
            }
            else if (resources.level == 2)
                data.draws = {resources.triangle};
            imports.Declare(builder, data);
        },
        ExecuteDraws);
    if (resources.capture)
    {
        auto cpu = graph.ImportBuffer("ScreenshotReadback", resources.readback);
        graph.AddPass<CopyPass>(
            "Screenshot",
            [&](RG::RgBuilder& builder, CopyPass& data)
            {
                data.source = builder.Read(back, ResourceAccess::CopySource);
                cpu = data.destination = builder.Write(cpu, ResourceAccess::CopyDestination, RG::WriteCoverage::Full);
                data.extent = resources.extent;
                builder.SideEffect("requested screenshot");
            },
            [](const CopyPass& data, const RG::RgResources& resolver, IRhiCommandList& commands)
            {
                commands.CopyTextureForReadback(resolver.Get(data.source), resolver.Get(data.destination), data.extent);
            });
        graph.Export(cpu);
    }
    if (resources.timestamps)
        graph.AddPass<TimestampQueryHandle>(
            "Timestamp.End",
            [&](RG::RgBuilder& builder, auto& query)
            {
                query = resources.endQuery;
                builder.SideEffect("frame timestamp end");
            },
            [](const auto& query, const RG::RgResources&, IRhiCommandList& commands)
            { commands.WriteTimestamp(query); });
    graph.Present(back);
}
RG::CompiledRenderGraph BuildM6RenderGraph(const World::RenderPacket& packet, const M6PipelineResources& resources,
                                           RG::RenderGraph& graph)
{
    DeclareM6RenderGraph(packet, resources, graph);
    return graph.Compile();
}
} // namespace MiniEngine::Render
