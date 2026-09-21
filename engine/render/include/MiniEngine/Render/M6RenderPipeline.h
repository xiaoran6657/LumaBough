#pragma once
#include <MiniEngine/RenderGraph/RenderGraph.h>
#include <MiniEngine/World/RenderPacket.h>

namespace MiniEngine::Render
{
// 资源 owner 在帧边界生成不可变绑定快照；Pass 不持有 device 或 native 状态。
struct M6Draw final
{
    std::string stableId;
    Rhi::GraphicsPipelineHandle pipeline;
    std::array<Rhi::ResourceSetDesc, 3> sets{};
    std::uint8_t setCount = 0;
    std::array<std::vector<std::uint32_t>, 3> dynamicOffsets;
    Rhi::BufferView vertices, indices;
    std::uint32_t stride = 48, vertexCount = 3, indexCount = 0;
    Rhi::IndexType indexType = Rhi::IndexType::UInt32;
};
struct M6TextureInput final
{
    std::string name;
    RenderGraph::TextureImport imported;
};
struct M6BufferInput final
{
    std::string name;
    RenderGraph::BufferImport imported;
    Rhi::ResourceAccess access = Rhi::ResourceAccess::UniformRead;
    Rhi::ShaderStage stages = Rhi::ShaderStage::Vertex;
};
struct M6PipelineResources final
{
    Rhi::Extent2D extent;
    RenderGraph::TextureImport backBuffer, toneSource;
    std::vector<M6TextureInput> textures;
    std::vector<M6BufferInput> buffers;
    std::vector<M6Draw> opaque, shadow;
    M6Draw triangle, tone, sky;
    Rhi::TextureDesc shadowDesc, depthDesc, hdrDesc;
    RenderGraph::BufferImport readback;
    Rhi::TimestampQueryHandle beginQuery, endQuery;
    std::uint32_t level = 9;
    bool capture = false, timestamps = false;
};
// 调用方必须保持 graph 活到 Execute 完成；编译结果与绑定快照由 graph 按值拥有。
// level 仅用于九级迁移取证；两 backend 使用同一个声明与执行函数。
void DeclareM6RenderGraph(const World::RenderPacket& packet, const M6PipelineResources& resources,
                         RenderGraph::RenderGraph& graph);
RenderGraph::CompiledRenderGraph BuildM6RenderGraph(const World::RenderPacket& packet,
                                                    const M6PipelineResources& resources,
                                                    RenderGraph::RenderGraph& graph);
} // namespace MiniEngine::Render
