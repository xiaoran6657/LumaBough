#include "CommandRecording.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace MiniEngine::Rhi
{
bool ResourceContent::Contains(std::uint64_t offset, std::uint64_t size) const
{
    if (size == 0 || offset > std::numeric_limits<std::uint64_t>::max() - size)
        return false;
    return std::any_of(ranges.begin(), ranges.end(),
                       [&](auto range) { return offset >= range.first && offset + size <= range.second; });
}
void ResourceContent::Define(std::uint64_t offset, std::uint64_t size)
{
    if (size == 0 || offset > std::numeric_limits<std::uint64_t>::max() - size)
        throw RhiValidationError({RhiErrorCode::InvalidArgument, "Define", "Buffer", "", "", "invalid byte range"});
    ranges.emplace_back(offset, offset + size);
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (auto range : ranges)
    {
        if (!merged.empty() && range.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, range.second);
        else
            merged.push_back(range);
    }
    ranges = std::move(merged);
}
CommandRecording::CommandRecording(DeviceLifetime& device, FrameToken frame, CommandResourceStates& resources,
                                   Emit emit, ValidateImport imports, DeclareImports declareImports)
    : m_device(device), m_frame(frame), m_resources(resources), m_emit(std::move(emit)),
      m_validateImport(std::move(imports)), m_declareImports(std::move(declareImports)), m_bindings(device, frame)
{
    if (!m_emit || !m_validateImport)
        Fail("missing command consumer or graph import ledger");
    m_state = State::Recording;
}
[[noreturn]] void CommandRecording::Fail(const char* message)
{
    throw RhiValidationError({RhiErrorCode::InvalidState, "CommandRecording", "CommandList", "", "", message});
}
[[noreturn]] void CommandRecording::FailResource(ResourceIdentity handle, const char* message) const
{
    throw RhiValidationError({RhiErrorCode::InvalidState, "CommandRecording", "Resource", m_device.DebugName(handle),
                              std::string(ToString(m_device.Backend())), message});
}
void CommandRecording::OutsideRendering() const
{
    if (m_state != State::Recording)
        Fail("operation requires recording outside rendering");
}
void CommandRecording::RequireAccess(ResourceIdentity handle, ResourceAccess access) const
{
    const auto found = m_resources.find(handle);
    if (found == m_resources.end() || found->second.access != access || found->second.declaredFrame != m_frame.serial)
        FailResource(handle, "resource access does not match graph declaration");
}
void CommandRecording::RequireDefined(TextureHandle texture) const
{
    auto found = m_resources.find(texture);
    if (found == m_resources.end() || !found->second.defined)
        FailResource(texture, "texture contents are undefined");
}
void CommandRecording::RequireDefined(BufferView view) const
{
    auto found = m_resources.find(view.buffer);
    if (found == m_resources.end() || !found->second.Contains(view.offset, view.size))
        FailResource(view.buffer, "buffer range is undefined");
}
bool CommandRecording::HasActiveAttachment(ResourceIdentity handle) const
{
    if (m_state != State::Rendering)
        return false;
    const auto* texture = std::get_if<TextureHandle>(&handle);
    return texture && std::any_of(m_attachments.begin(), m_attachments.end(),
                                  [&](const auto& attachment) { return attachment.first == *texture; });
}
void CommandRecording::Close()
{
    Run({"EndGraphics"},
        [&]
        {
            OutsideRendering();
            if (!m_labels.empty())
                Fail("unclosed label scope");
            m_state = State::Closed;
        });
}
void CommandRecording::BeginLabel(std::string_view name)
{
    Run({"BeginLabel", {}, {}, {}, std::string(name)},
        [&]
        {
            if (name.empty())
                Fail("label must have a name");
            m_labels.emplace_back(name);
        });
}
void CommandRecording::EndLabel()
{
    Run({"EndLabel"},
        [&]
        {
            if (m_labels.empty())
                Fail("label stack is empty");
            m_labels.pop_back();
        });
}
void CommandRecording::BeginRendering(const RenderingInfo& info)
{
    CommandEvent event{
        "BeginRendering", {}, {info.extent.width, info.extent.height, info.colors.size(), info.depth != nullptr}};
    for (const auto& color : info.colors)
    {
        event.resources.emplace_back(color.texture);
        event.integers.push_back(static_cast<std::uint64_t>(color.load));
        event.integers.push_back(static_cast<std::uint64_t>(color.store));
        event.scalars.insert(event.scalars.end(), color.clearColor.begin(), color.clearColor.end());
    }
    if (info.depth)
    {
        event.resources.emplace_back(info.depth->texture);
        event.integers.insert(event.integers.end(),
                              {static_cast<std::uint64_t>(info.depth->load),
                               static_cast<std::uint64_t>(info.depth->store), info.depth->clearStencil});
        event.scalars.push_back(info.depth->clearDepth);
    }
    Run(std::move(event),
        [&]
        {
            OutsideRendering();
            // 先校验所有 attachment；后一个失败不能改变前一个的定义性。
            auto candidate = m_resources;
            std::vector<std::pair<TextureHandle, StoreOp>> attachments;
            auto validate = [&](TextureHandle texture, LoadOp load, StoreOp store, bool depth)
            {
                if (load != LoadOp::Load && load != LoadOp::Clear && load != LoadOp::DontCare)
                    Fail("unknown load operation");
                if (store != StoreOp::Store && store != StoreOp::DontCare)
                    Fail("unknown store operation");
                const auto& desc = m_device.Describe(texture);
                if (desc.dimension != TextureDimension::Texture2D || desc.arrayLayers != 1 || desc.mipLevels != 1)
                    FailResource(texture,
                                 "attachment requires one 2D subresource; view selection is not in this profile");
                const auto state = candidate[texture].access;
                if (depth && state == ResourceAccess::DepthRead)
                {
                    RequireAccess(texture, ResourceAccess::DepthRead);
                    if (load != LoadOp::Load || store != StoreOp::Store)
                        Fail("read-only depth requires Load/Store");
                }
                else
                    RequireAccess(texture, depth ? ResourceAccess::DepthWrite : ResourceAccess::ColorWrite);
                if (load == LoadOp::Load)
                    RequireDefined(texture);
                candidate[texture].defined =
                    load == LoadOp::Clear || (load == LoadOp::Load && candidate[texture].defined);
                attachments.emplace_back(texture, store);
            };
            for (const auto& color : info.colors)
            {
                if (!std::all_of(color.clearColor.begin(), color.clearColor.end(),
                                 [](float value) { return std::isfinite(value); }))
                    Fail("clear color must be finite");
                validate(color.texture, color.load, color.store, false);
            }
            if (info.depth)
            {
                if (!std::isfinite(info.depth->clearDepth) || info.depth->clearDepth < 0 || info.depth->clearDepth > 1)
                    Fail("clear depth outside normalized range");
                // 当前 profile 没有 stencil commands，不接受被静默忽略的非零 clear。
                if (info.depth->clearStencil != 0)
                    Fail("nonzero stencil clear is outside current profile");
                validate(info.depth->texture, info.depth->load, info.depth->store, true);
            }
            m_bindings.BeginRendering(info);
            m_resources = std::move(candidate);
            m_attachments = std::move(attachments);
            m_depth = info.depth ? std::optional(info.depth->texture) : std::nullopt;
            m_extent = info.extent;
            m_state = State::Rendering;
        });
}
void CommandRecording::EndRendering()
{
    Run({"EndRendering"},
        [&]
        {
            // 先验证全部附件，再修改状态；失效附件不能重新写入内容账本。
            for (const auto& attachment : m_attachments)
            {
                m_device.ValidateAlive(attachment.first);
                if (!m_resources.contains(attachment.first))
                    FailResource(attachment.first, "missing attachment content state");
            }
            m_bindings.EndRendering();
            for (auto [texture, store] : m_attachments)
                if (store == StoreOp::DontCare)
                    m_resources.at(texture).defined = false;
            m_attachments.clear();
            m_depth.reset();
            m_state = State::Recording;
        });
}
void CommandRecording::SetPipeline(GraphicsPipelineHandle pipeline)
{
    Run({"SetPipeline", {pipeline}},
        [&]
        {
            m_bindings.SetPipeline(pipeline);
            for (std::uint32_t i = 0; i < m_sets.size(); ++i)
                if (!m_bindings.HasSet(i))
                    m_sets[i].reset();
            m_pipeline = pipeline;
        });
}
void CommandRecording::SetViewport(const Viewport& viewport)
{
    Run({"SetViewport",
         {},
         {},
         {viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth}},
        [&]
        {
            const std::array values{viewport.x,      viewport.y,        viewport.width,
                                    viewport.height, viewport.minDepth, viewport.maxDepth};
            if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }) ||
                viewport.x < 0 || viewport.y < 0 || viewport.width <= 0 || viewport.height <= 0 ||
                viewport.minDepth < 0 || viewport.maxDepth > 1 || viewport.minDepth > viewport.maxDepth)
                Fail("invalid viewport");
            m_viewport = viewport;
        });
}
void CommandRecording::SetScissor(const Rect& scissor)
{
    Run({"SetScissor",
         {},
         {static_cast<std::uint64_t>(scissor.x), static_cast<std::uint64_t>(scissor.y), scissor.width, scissor.height}},
        [&]
        {
            if (scissor.x < 0 || scissor.y < 0 || scissor.width == 0 || scissor.height == 0)
                Fail("invalid scissor");
            m_scissor = scissor;
        });
}
void CommandRecording::BindVertexBuffer(std::uint32_t slot, const BufferView& view, std::uint32_t stride)
{
    Run({"BindVertexBuffer", {view.buffer}, {slot, view.offset, view.size, stride}},
        [&]
        {
            m_bindings.BindVertexBuffer(slot, view, stride);
            m_vertices[slot] = view;
        });
}
void CommandRecording::BindIndexBuffer(const BufferView& view, IndexType type)
{
    Run({"BindIndexBuffer", {view.buffer}, {view.offset, view.size, static_cast<std::uint64_t>(type)}},
        [&]
        {
            m_bindings.BindIndexBuffer(view, type);
            m_indices = view;
        });
}
void CommandRecording::BindResourceSet(std::uint32_t set, ResourceSetHandle resources,
                                       std::span<const std::uint32_t> offsets)
{
    CommandEvent event{"BindResourceSet", {resources}, {set}};
    event.integers.insert(event.integers.end(), offsets.begin(), offsets.end());
    Run(std::move(event),
        [&]
        {
            m_bindings.BindResourceSet(set, resources, offsets);
            m_sets[set] = std::pair{resources, std::vector<std::uint32_t>(offsets.begin(), offsets.end())};
        });
}
void CommandRecording::ValidateDrawState()
{
    if (m_state != State::Rendering || !m_viewport || !m_scissor || !m_pipeline)
        Fail("draw requires rendering, pipeline, viewport and scissor");
    if (static_cast<double>(m_viewport->x) + m_viewport->width > m_extent.width ||
        static_cast<double>(m_viewport->y) + m_viewport->height > m_extent.height ||
        static_cast<std::uint64_t>(m_scissor->x) + m_scissor->width > m_extent.width ||
        static_cast<std::uint64_t>(m_scissor->y) + m_scissor->height > m_extent.height)
        Fail("viewport/scissor exceeds render extent");
    // 在访问内容账本前验证 handle，避免失效 depth 变成未结构化的 map::at 异常。
    for (const auto& attachment : m_attachments)
        m_device.ValidateAlive(attachment.first);
    if (m_depth && !m_resources.contains(*m_depth))
        FailResource(*m_depth, "missing depth content state");
    if (m_depth && m_resources.at(*m_depth).access == ResourceAccess::DepthRead &&
        m_device.Describe(*m_pipeline).depthWrite)
        Fail("pipeline writes read-only depth");
    for (const auto& attribute : m_device.Describe(*m_pipeline).vertexAttributes)
    {
        auto it = m_vertices.find(attribute.bufferSlot);
        if (it != m_vertices.end())
        {
            RequireAccess(it->second.buffer, ResourceAccess::VertexRead);
            RequireDefined(it->second);
        }
    }
    for (const auto& slot : m_sets)
    {
        if (!slot)
            continue;
        const auto& desc = m_device.Describe(slot->first);
        auto entries = m_device.Describe(desc.layout).entries;
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
        std::size_t dynamic = 0;
        for (const auto& layout : entries)
        {
            for (std::uint32_t element = 0; element < layout.count; ++element)
            {
                const auto binding =
                    std::find_if(desc.bindings.begin(), desc.bindings.end(), [&](const auto& entry)
                                 { return entry.binding == layout.binding && entry.arrayElement == element; });
                if (binding == desc.bindings.end())
                    Fail("resource set binding is missing");
                if (layout.type == BindingType::SampledTexture)
                {
                    RequireAccess(binding->texture, ResourceAccess::SampledRead);
                    RequireDefined(binding->texture);
                }
                else if (layout.type == BindingType::UniformBuffer)
                {
                    auto view = binding->buffer;
                    if (layout.dynamicOffset)
                        view.offset += slot->second.at(dynamic++);
                    RequireAccess(view.buffer, ResourceAccess::UniformRead);
                    RequireDefined(view);
                }
            }
        }
    }
}
void CommandRecording::Draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex,
                            std::uint32_t firstInstance)
{
    Run({"Draw", {}, {vertices, instances, firstVertex, firstInstance}},
        [&]
        {
            ValidateDrawState();
            m_bindings.Draw(vertices, instances, firstVertex, firstInstance);
            // 不把任意 triangle 的覆盖范围假装成整张 attachment 的显式初始化。
        });
}
void CommandRecording::DrawIndexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t firstIndex,
                                   std::int32_t vertexOffset, std::uint32_t firstInstance)
{
    Run({"DrawIndexed", {}, {indices, instances, firstIndex, static_cast<std::uint64_t>(vertexOffset), firstInstance}},
        [&]
        {
            ValidateDrawState();
            if (m_indices)
            {
                RequireAccess(m_indices->buffer, ResourceAccess::IndexRead);
                RequireDefined(*m_indices);
            }
            m_bindings.DrawIndexed(indices, instances, firstIndex, vertexOffset, firstInstance);
        });
}
void CommandRecording::CopyBuffer(const BufferView& source, const BufferView& destination, std::uint64_t size)
{
    Run({"CopyBuffer",
         {source.buffer, destination.buffer},
         {source.offset, source.size, destination.offset, destination.size, size}},
        [&]
        {
            OutsideRendering();
            const auto& src = m_device.Describe(source.buffer);
            const auto& dst = m_device.Describe(destination.buffer);
            if (!HasFlag(src.usage, BufferUsage::CopySource) || !HasFlag(dst.usage, BufferUsage::CopyDestination) ||
                size == 0 || size > source.size || size > destination.size || source.offset > src.size ||
                source.size > src.size - source.offset || destination.offset > dst.size ||
                destination.size > dst.size - destination.offset || source.buffer == destination.buffer)
                Fail("invalid buffer copy usage/range or self-copy");
            RequireAccess(source.buffer, ResourceAccess::CopySource);
            RequireAccess(destination.buffer, ResourceAccess::CopyDestination);
            RequireDefined(BufferView{source.buffer, source.offset, size});
            m_device.Use(m_frame, source.buffer);
            m_device.Use(m_frame, destination.buffer);
            m_resources[destination.buffer].Define(destination.offset, size);
        });
}
void CommandRecording::CopyTextureForReadback(TextureHandle source, BufferHandle readback, Extent2D extent)
{
    Run({"CopyTextureForReadback", {source, readback}, {extent.width, extent.height}},
        [&]
        {
            OutsideRendering();
            const auto& src = m_device.Describe(source);
            const auto& dst = m_device.Describe(readback);
            const std::uint64_t bytes = static_cast<std::uint64_t>(extent.width) * extent.height * 4;
            if (extent.width == 0 || extent.height == 0 || extent != src.extent || src.sampleCount != 1 ||
                src.arrayLayers != 1 || src.mipLevels != 1 ||
                (src.format != Format::Rgba8Unorm && src.format != Format::Rgba8UnormSrgb) ||
                !HasFlag(src.usage, TextureUsage::CopySource) || !HasFlag(dst.usage, BufferUsage::CopyDestination) ||
                dst.memory != MemoryDomain::GpuToCpu || dst.size < bytes)
                Fail("invalid RGBA8 readback description");
            RequireAccess(source, ResourceAccess::CopySource);
            RequireAccess(readback, ResourceAccess::CopyDestination);
            RequireDefined(source);
            m_device.Use(m_frame, source);
            m_device.Use(m_frame, readback);
            m_resources[readback].Define(0, bytes);
        });
}
void CommandRecording::WriteTimestamp(TimestampQueryHandle query)
{
    Run({"WriteTimestamp", {query}},
        [&]
        {
            if (m_device.LastUse(query) > m_device.Completed())
                Fail("timestamp query is still in flight");
            if (m_writtenQueries.contains(query))
                Fail("query point written twice in one frame");
            m_device.Use(m_frame, query);
            m_writtenQueries.insert(query);
        });
}
void CommandRecording::ValidateTransition(const AccessTransition& transition, CommandResourceStates& candidate) const
{
    if (static_cast<bool>(transition.texture) == static_cast<bool>(transition.buffer))
        Fail("transition must name exactly one texture or buffer");
    const ResourceIdentity handle =
        transition.texture ? ResourceIdentity(transition.texture) : ResourceIdentity(transition.buffer);
    m_device.ValidateAlive(handle);
    m_validateImport(handle);
    auto& state = candidate[handle];
    if (state.access != transition.before)
        Fail("transition before-access disagrees with resource state");
    if (transition.after == ResourceAccess::None)
        Fail("transition cannot erase access history");
    if (transition.texture)
    {
        const auto& desc = m_device.Describe(transition.texture);
        TextureUsage usage = TextureUsage::None;
        switch (transition.after)
        {
        case ResourceAccess::SampledRead:
            usage = TextureUsage::Sampled;
            if (transition.stages != ShaderStage::Pixel && transition.stages != ShaderStage::Vertex &&
                transition.stages != (ShaderStage::Pixel | ShaderStage::Vertex))
                Fail("invalid sampled stages");
            break;
        case ResourceAccess::ColorWrite:
            usage = TextureUsage::ColorAttachment;
            break;
        case ResourceAccess::DepthRead:
        case ResourceAccess::DepthWrite:
            usage = TextureUsage::DepthStencil;
            break;
        case ResourceAccess::CopySource:
            usage = TextureUsage::CopySource;
            break;
        case ResourceAccess::CopyDestination:
            usage = TextureUsage::CopyDestination;
            break;
        case ResourceAccess::Present:
            if (transition.texture != m_frame.backBuffer)
                Fail("only current backbuffer may become Present");
            break;
        default:
            Fail("unsupported texture access");
        }
        if (usage != TextureUsage::None && !HasFlag(desc.usage, usage))
            Fail("texture transition exceeds usage");
        if ((transition.after == ResourceAccess::SampledRead || transition.after == ResourceAccess::DepthRead ||
             transition.after == ResourceAccess::CopySource || transition.after == ResourceAccess::Present) &&
            !state.defined)
            Fail("transition reads undefined texture contents");
    }
    else
    {
        const auto& desc = m_device.Describe(transition.buffer);
        BufferUsage usage = BufferUsage::None;
        switch (transition.after)
        {
        case ResourceAccess::CopySource:
            usage = BufferUsage::CopySource;
            break;
        case ResourceAccess::CopyDestination:
            usage = BufferUsage::CopyDestination;
            break;
        case ResourceAccess::VertexRead:
            usage = BufferUsage::Vertex;
            break;
        case ResourceAccess::IndexRead:
            usage = BufferUsage::Index;
            break;
        case ResourceAccess::UniformRead:
            usage = BufferUsage::Uniform;
            break;
        default:
            Fail("unsupported buffer transition access");
        }
        if (!HasFlag(desc.usage, usage))
            Fail("unsupported buffer transition usage");
    }
    state.access = transition.after;
    state.declaredFrame = m_frame.serial;
}
void CommandRecording::ImportResources(std::span<const GraphResourceImport> resources)
{
    CommandEvent event{"ImportResources"};
    for (const auto& resource : resources)
        event.resources.emplace_back(resource.texture ? ResourceIdentity(resource.texture)
                                                      : ResourceIdentity(resource.buffer));
    auto identities = event.resources;
    Run(std::move(event),
        [&]
        {
            OutsideRendering();
            if (!m_declareImports)
                Fail("import declaration callback is not configured");
            for (const auto& resource : resources)
                if (static_cast<bool>(resource.texture) == static_cast<bool>(resource.buffer))
                    Fail("import must identify exactly one resource");
            m_declareImports(identities);
        });
}
void CommandRecording::ResetTransientContents(std::span<const GraphResourceImport> resources)
{
    CommandEvent event{"ResetTransientContents"};
    for (const auto& resource : resources)
    {
        if (resource.texture)
            event.resources.emplace_back(resource.texture);
        else if (resource.buffer)
            event.resources.emplace_back(resource.buffer);
    }
    Run(std::move(event),
        [&]
        {
            OutsideRendering();
            if (resources.empty())
                Fail("reset batch cannot be empty");
            auto candidate = m_resources;
            for (const auto& resource : resources)
            {
                if (static_cast<bool>(resource.texture) == static_cast<bool>(resource.buffer))
                    Fail("reset must identify exactly one resource");
                const ResourceIdentity handle =
                    resource.texture ? ResourceIdentity(resource.texture) : ResourceIdentity(resource.buffer);
                if (handle == ResourceIdentity(m_frame.backBuffer))
                    Fail("backbuffer content cannot be reset as transient");
                m_validateImport(handle);
                m_device.ValidateFrameResource(m_frame, handle);
                // 新建未上传资源可能尚无账本项，默认 None/Undefined 正是其初始状态。
                auto& content = candidate[handle];
                // 全批验证后一次发布；已有 access 与 declaredFrame 保持不变。
                content.defined = false;
                content.ranges.clear();
            }
            m_resources = std::move(candidate);
        });
}
void CommandRecording::ApplyTransitions(std::span<const AccessTransition> transitions)
{
    CommandEvent event{"ApplyTransitions"};
    for (const auto& transition : transitions)
    {
        if (transition.texture)
            event.resources.emplace_back(transition.texture);
        else
            event.resources.emplace_back(transition.buffer);
        event.integers.insert(event.integers.end(), {static_cast<std::uint64_t>(transition.before),
                                                     static_cast<std::uint64_t>(transition.after),
                                                     static_cast<std::uint64_t>(transition.stages)});
    }
    Run(std::move(event),
        [&]
        {
            OutsideRendering();
            auto candidate = m_resources;
            for (const auto& transition : transitions)
                ValidateTransition(transition, candidate);
            for (const auto& transition : transitions)
                m_device.Use(m_frame, transition.texture ? ResourceIdentity(transition.texture)
                                                         : ResourceIdentity(transition.buffer));
            m_resources = std::move(candidate);
        });
}
} // namespace MiniEngine::Rhi
