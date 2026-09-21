#include "CommandValidation.h"
#include <algorithm>
#include <limits>
#include <set>

namespace MiniEngine::Rhi
{
CommandValidation::CommandValidation(DeviceLifetime& device, FrameToken frame) : m_device(device), m_frame(frame)
{
    device.ValidateFrameToken(frame);
}
[[noreturn]] void CommandValidation::Fail(const char* message)
{
    throw RhiValidationError({RhiErrorCode::InvalidState, "CommandValidation", "CommandList", "", "", message});
}
void CommandValidation::BeginRendering(const RenderingInfo& rendering)
{
    m_device.ValidateFrameToken(m_frame);
    if (m_rendering || rendering.colors.size() > 4 || (rendering.colors.empty() && !rendering.depth) ||
        rendering.extent.width == 0 || rendering.extent.height == 0)
        Fail("invalid or nested rendering scope");
    std::array<Format, 4> colors{};
    Format depth = Format::Unknown;
    std::vector<TextureHandle> attachments;
    std::set<TextureHandle> seen;
    auto check = [&](TextureHandle texture, TextureUsage usage)
    {
        const auto& desc = m_device.Describe(texture);
        if (!seen.insert(texture).second || !HasFlag(desc.usage, usage) || desc.extent != rendering.extent ||
            desc.sampleCount != 1)
            Fail("attachment usage/extent/sample count or aliasing is invalid");
        attachments.push_back(texture);
        return desc.format;
    };
    for (std::size_t i = 0; i < rendering.colors.size(); ++i)
        colors[i] = check(rendering.colors[i].texture, TextureUsage::ColorAttachment);
    if (rendering.depth)
        depth = check(rendering.depth->texture, TextureUsage::DepthStencil);
    for (auto h : attachments)
        m_device.Use(m_frame, h);
    m_colors = colors;
    m_colorCount = static_cast<std::uint8_t>(rendering.colors.size());
    m_depth = depth;
    m_attachments = std::move(attachments);
    m_rendering = true;
}
void CommandValidation::EndRendering()
{
    m_device.ValidateFrameToken(m_frame);
    if (!m_rendering)
        Fail("rendering scope is not active");
    m_rendering = false;
    m_attachments.clear();
}
void CommandValidation::SetPipeline(GraphicsPipelineHandle pipeline)
{
    m_device.ValidateFrameToken(m_frame);
    const auto& desc = m_device.Describe(pipeline);
    auto layouts = m_device.ResolvedLayouts(desc.layout);
    auto key = PipelineLayoutKey(layouts);
    m_device.Use(m_frame, pipeline);
    if (key != m_layoutKey)
        for (auto& bound : m_sets)
            bound.reset();
    m_layoutKey = std::move(key);
    m_pipeline = pipeline;
}
void CommandValidation::BindResourceSet(std::uint32_t index, ResourceSetHandle set,
                                        std::span<const std::uint32_t> offsets)
{
    m_device.ValidateFrameToken(m_frame);
    if (!m_pipeline || index >= 3)
        Fail("bind needs a pipeline and valid set index");
    const auto& pipeline = m_device.Describe(m_pipeline);
    auto layouts = m_device.ResolvedLayouts(pipeline.layout);
    const auto& desc = m_device.Describe(set);
    if (index >= layouts.size() || !LayoutCompatible(layouts[index], m_device.Describe(desc.layout)))
        Fail("set layout is incompatible with current pipeline slot");
    m_device.ValidateSetBinding(m_frame, set, offsets);
    BoundSet bound{set, {offsets.begin(), offsets.end()}};
    m_device.Use(m_frame, set);
    m_sets[index] = std::move(bound);
}
void CommandValidation::ValidateView(BufferView view, BufferUsage usage) const
{
    const auto& desc = m_device.Describe(view.buffer);
    if (!HasFlag(desc.usage, usage) || view.size == 0 || view.offset > desc.size || view.size > desc.size - view.offset)
        Fail("buffer usage or view range is invalid");
}
void CommandValidation::BindVertexBuffer(std::uint32_t slot, BufferView view, std::uint32_t stride)
{
    m_device.ValidateFrameToken(m_frame);
    if (slot >= m_vertices.size() || stride == 0 || stride > 2048 || stride % 4 || view.offset % 4)
        Fail("vertex slot/stride/alignment is invalid");
    ValidateView(view, BufferUsage::Vertex);
    m_device.Use(m_frame, view.buffer);
    m_vertices[slot] = VertexBuffer{view, stride};
}
void CommandValidation::BindIndexBuffer(BufferView view, IndexType type)
{
    m_device.ValidateFrameToken(m_frame);
    if (type != IndexType::UInt16 && type != IndexType::UInt32)
        Fail("unknown index format");
    const std::uint32_t bytes = type == IndexType::UInt16 ? 2 : 4;
    if (view.offset % bytes || view.size % bytes)
        Fail("index alignment is invalid");
    ValidateView(view, BufferUsage::Index);
    m_device.Use(m_frame, view.buffer);
    m_indices = view;
    m_indexType = type;
}
void CommandValidation::ValidateDraw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex,
                                     std::uint32_t firstInstance, bool indexed)
{
    m_device.ValidateFrameToken(m_frame);
    if (!m_rendering || !m_pipeline)
        Fail("draw needs active rendering and pipeline");
    if (vertices == 0 || instances == 0)
        Fail("draw count must be nonzero");
    const auto& pipeline = m_device.Describe(m_pipeline);
    if (pipeline.colorAttachmentCount != m_colorCount || pipeline.colorFormats != m_colors ||
        pipeline.depthFormat != m_depth)
        Fail("pipeline attachment signature does not match rendering");
    for (auto handle : m_attachments)
        m_device.ValidateAlive(handle);
    auto layouts = m_device.ResolvedLayouts(pipeline.layout);
    for (std::size_t i = 0; i < layouts.size(); ++i)
    {
        if (layouts[i].entries.empty())
            continue;
        if (!m_sets[i])
            Fail("draw is missing a required resource set");
        const auto& set = m_device.Describe(m_sets[i]->handle);
        if (!LayoutCompatible(layouts[i], m_device.Describe(set.layout)))
            Fail("bound set no longer matches pipeline");
        m_device.ValidateSetBinding(m_frame, m_sets[i]->handle, m_sets[i]->offsets);
    }
    for (const auto& attribute : pipeline.vertexAttributes)
    {
        if (!m_vertices[attribute.bufferSlot])
            Fail("draw is missing a required vertex buffer");
        const auto& buffer = *m_vertices[attribute.bufferSlot];
        ValidateView(buffer.view, BufferUsage::Vertex);
        const auto end = static_cast<std::uint64_t>(attribute.offset) + VertexFormatBytes(attribute.format);
        if (end > buffer.stride || end > buffer.view.size)
            Fail("vertex attribute exceeds stride/view");
        // Indexed 的 vertexOffset 与 GPU index 内容共同决定地址，CPU 不猜未回读的 index 值。
        if (indexed && attribute.instanceStepRate == 0)
            continue;
        const auto count = attribute.instanceStepRate ? instances : vertices;
        const auto first = attribute.instanceStepRate ? firstInstance : firstVertex;
        const std::uint64_t last = static_cast<std::uint64_t>(first) + count - 1;
        // StartInstanceLocation 是 instance buffer 的起始元素；step rate 只作用于本 draw 的实例序号。
        const auto element = attribute.instanceStepRate
                                 ? static_cast<std::uint64_t>(firstInstance) +
                                       (static_cast<std::uint64_t>(instances) - 1) / attribute.instanceStepRate
                                 : last;
        if (element > (buffer.view.size - end) / buffer.stride)
            Fail("draw vertex/instance range exceeds buffer view");
    }
    // 只有全套前置条件成立后，才发布本次 draw 对引用树的使用。
    m_device.Use(m_frame, m_pipeline);
    for (std::size_t i = 0; i < layouts.size(); ++i)
        if (m_sets[i])
            m_device.Use(m_frame, m_sets[i]->handle);
    for (const auto& a : pipeline.vertexAttributes)
        m_device.Use(m_frame, m_vertices[a.bufferSlot]->view.buffer);
}
void CommandValidation::Draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex,
                             std::uint32_t firstInstance)
{
    ValidateDraw(vertices, instances, firstVertex, firstInstance, false);
}
void CommandValidation::DrawIndexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t firstIndex,
                                    std::int32_t vertexOffset, std::uint32_t firstInstance)
{
    m_device.ValidateFrameToken(m_frame);
    if (!m_indices)
        Fail("indexed draw is missing index buffer");
    ValidateView(*m_indices, BufferUsage::Index);
    const std::uint64_t bytes = m_indexType == IndexType::UInt16 ? 2 : 4;
    const auto capacity = m_indices->size / bytes;
    if (firstIndex > capacity || indices > capacity - firstIndex)
        Fail("indexed draw range exceeds index buffer view");
    // M5 两端均使用 base vertex 0。未持有 index min/max 元数据前不接受带偏移的寻址。
    if (vertexOffset != 0)
        Fail("M6 indexed draw requires zero vertexOffset; index values remain the mesh owner's responsibility");
    ValidateDraw(indices, instances, 0, firstInstance, true);
    m_device.Use(m_frame, m_indices->buffer);
}
bool CommandValidation::HasSet(std::uint32_t index) const
{
    m_device.ValidateFrameToken(m_frame);
    return index < m_sets.size() && m_sets[index].has_value();
}
} // namespace MiniEngine::Rhi
