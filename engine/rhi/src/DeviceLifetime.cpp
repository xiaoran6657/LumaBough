#include "DeviceLifetime.h"
#include <MiniEngine/Rhi/RhiValidation.h>
#include <algorithm>
#include <bit>
#include <limits>
#include <set>

namespace MiniEngine::Rhi
{
DeviceLifetime::DeviceLifetime(RhiCapabilities capabilities)
    : m_capabilities(std::move(capabilities)), m_owner(Detail::AcquireRegistryOwner())
{
    constexpr std::array names{
        "Buffer",      "Texture",        "Sampler",          "Shader",         "ResourceSetLayout",
        "ResourceSet", "PipelineLayout", "GraphicsPipeline", "TimestampQuery", "SwapChain"};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        m_registries[i] = std::make_unique<Registry>(names[i], std::string(ToString(m_capabilities.backend)));
    }
}
DeviceLifetime::Key DeviceLifetime::ToKey(const ResourceIdentity& handle)
{
    return std::visit([](auto h) { return Key{h.Index(), h.Generation(), h.Owner()}; }, handle);
}
DeviceLifetime::Record& DeviceLifetime::Get(const ResourceIdentity& handle)
{
    return m_registries[handle.index()]->Get(ToKey(handle));
}
const DeviceLifetime::Record& DeviceLifetime::Get(const ResourceIdentity& handle) const
{
    return m_registries[handle.index()]->Get(ToKey(handle));
}
void DeviceLifetime::ValidateAlive(ResourceIdentity handle) const
{
    (void)Get(handle);
}
[[noreturn]] void DeviceLifetime::Fail(const char* operation, const char* message) const
{
    throw RhiValidationError({RhiErrorCode::InvalidState, operation, "DeviceLifetime", "",
                              std::string(ToString(m_capabilities.backend)), message});
}
BufferHandle DeviceLifetime::CreateBuffer(const BufferDesc& desc, const PayloadFactory& create)
{
    ValidateBufferDesc(desc);
    Record record;
    record.buffer = desc;
    return Register<BufferHandle>(std::move(record), desc.debugName, create);
}
TextureHandle DeviceLifetime::CreateTexture(const TextureDesc& desc, const PayloadFactory& create)
{
    ValidateTextureDesc(desc, m_capabilities);
    Record record;
    record.texture = desc;
    return Register<TextureHandle>(std::move(record), desc.debugName, create);
}
SamplerHandle DeviceLifetime::CreateSampler(const SamplerDesc& desc, const PayloadFactory& create)
{
    RequireCreationBoundary("CreateSampler");
    ValidateSamplerDesc(desc, m_capabilities);
    Record record;
    record.sampler = desc;
    return Register<SamplerHandle>(std::move(record), desc.debugName, create);
}
ShaderHandle DeviceLifetime::CreateShader(const ShaderDesc& desc, const PayloadFactory& create)
{
    RequireCreationBoundary("CreateShader");
    ValidateShaderDesc(desc);
    Record record;
    record.shaderStage = desc.stage;
    record.shader = desc;
    // manifest/identity 按值保存；借用 bytecode 由后端在创建期间复制，owner 不保留 span。
    record.shader->bytecode = {};
    return Register<ShaderHandle>(std::move(record), desc.debugName, create);
}
ResourceSetLayoutHandle DeviceLifetime::CreateResourceSetLayout(const ResourceSetLayoutDesc& desc,
                                                                const PayloadFactory& create)
{
    RequireCreationBoundary("CreateSetLayout");
    ValidateResourceSetLayout(desc);
    Record record;
    record.setLayout = desc;
    return Register<ResourceSetLayoutHandle>(std::move(record), desc.debugName, create);
}
ResourceSetHandle DeviceLifetime::CreateResourceSet(const ResourceSetDesc& desc, const PayloadFactory& create)
{
    return CreateResourceSetInternal(desc, create, 0);
}
ResourceSetHandle DeviceLifetime::CreateFrameResourceSet(const FrameToken& frame, const ResourceSetDesc& desc,
                                                         const PayloadFactory& create)
{
    ValidateFrame(frame);
    m_frameSets.reserve(m_frameSets.size() + 1);
    const auto handle = CreateResourceSetInternal(desc, create, frame.serial);
    m_frameSets.push_back(handle);
    return handle;
}
ResourceSetHandle DeviceLifetime::CreateResourceSetInternal(const ResourceSetDesc& desc, const PayloadFactory& create,
                                                            std::uint64_t frameSerial)
{
    const auto layout = *Get(desc.layout).setLayout;
    Record record;
    record.resourceSet = desc;
    record.frameSerial = frameSerial;
    record.dependencies.emplace_back(desc.layout);
    std::size_t expected = 0;
    for (const auto& entry : layout.entries)
    {
        expected += entry.count;
    }
    if (desc.bindings.size() != expected)
    {
        Fail("CreateResourceSet", "binding coverage does not match layout");
    }
    std::set<std::pair<std::uint16_t, std::uint16_t>> seen;
    for (const auto& binding : desc.bindings)
    {
        const auto entry = std::find_if(layout.entries.begin(), layout.entries.end(),
                                        [&](const auto& e) { return e.binding == binding.binding; });
        if (entry == layout.entries.end() || binding.type != entry->type || binding.arrayElement >= entry->count ||
            !seen.emplace(binding.binding, binding.arrayElement).second)
        {
            Fail("CreateResourceSet", "binding type/array/uniqueness does not match layout");
        }
        if (binding.type == BindingType::UniformBuffer)
        {
            const auto& buffer = Get(binding.buffer.buffer);
            const auto& d = *buffer.buffer;
            if (binding.texture || binding.sampler || (buffer.frameSerial != 0 && buffer.frameSerial != frameSerial) ||
                !HasFlag(d.usage, BufferUsage::Uniform) || binding.buffer.size == 0 ||
                binding.buffer.size < entry->uniformBytes || binding.buffer.size % 16 != 0 ||
                binding.buffer.offset > d.size || binding.buffer.size > d.size - binding.buffer.offset ||
                m_capabilities.uniformBufferOffsetAlignment == 0 ||
                binding.buffer.offset % m_capabilities.uniformBufferOffsetAlignment != 0)
            {
                Fail("CreateResourceSet", "invalid uniform buffer view or frame ownership");
            }
            record.dependencies.emplace_back(binding.buffer.buffer);
        }
        else if (binding.type == BindingType::SampledTexture)
        {
            const auto& texture = Get(binding.texture);
            if (binding.buffer.buffer || binding.buffer.offset || binding.buffer.size || binding.sampler ||
                texture.backBuffer || texture.texture->dimension != entry->textureDimension ||
                !HasFlag(texture.texture->usage, TextureUsage::Sampled))
            {
                Fail("CreateResourceSet", "sampled binding needs an owned persistent sampled texture");
            }
            record.dependencies.emplace_back(binding.texture);
        }
        else
        {
            const auto& sampler = *Get(binding.sampler).sampler;
            if (sampler.comparisonEnabled != entry->comparisonSampler || binding.buffer.buffer ||
                binding.buffer.offset || binding.buffer.size || binding.texture)
            {
                Fail("CreateResourceSet", "ambiguous sampler binding");
            }
            record.dependencies.emplace_back(binding.sampler);
        }
    }
    return Register<ResourceSetHandle>(std::move(record), desc.debugName, create);
}
PipelineLayoutHandle DeviceLifetime::CreatePipelineLayout(const PipelineLayoutDesc& desc, const PayloadFactory& create)
{
    RequireCreationBoundary("CreatePipelineLayout");
    if (desc.setCount > desc.sets.size())
    {
        Fail("CreatePipelineLayout", "set count out of range");
    }
    Record record;
    record.pipelineLayout = desc;
    for (std::size_t i = 0; i < desc.sets.size(); ++i)
    {
        if (i < desc.setCount)
        {
            if (Get(desc.sets[i]).setLayout->set != i)
            {
                Fail("CreatePipelineLayout", "logical set index mismatch");
            }
            record.dependencies.emplace_back(desc.sets[i]);
        }
        else if (desc.sets[i])
        {
            Fail("CreatePipelineLayout", "unused set must be invalid");
        }
    }
    return Register<PipelineLayoutHandle>(std::move(record), desc.debugName, create);
}
GraphicsPipelineHandle DeviceLifetime::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc,
                                                              const PayloadFactory& create)
{
    RequireCreationBoundary("CreateGraphicsPipeline");
    const auto& vertex = *Get(desc.vertexShader).shader;
    const ShaderDesc* pixel = desc.pixelShader ? &*Get(desc.pixelShader).shader : nullptr;
    auto layouts = ResolvedLayouts(desc.layout);
    ValidateGraphicsPipeline(desc, vertex, pixel, layouts, m_capabilities);
    Record record;
    record.pipeline = desc;
    record.pipelineKey = PipelineSemanticKey(desc, vertex, pixel, layouts);
    record.dependencies = {desc.vertexShader, desc.layout};
    if (desc.pixelShader)
        record.dependencies.emplace_back(desc.pixelShader);
    auto handle = Register<GraphicsPipelineHandle>(std::move(record), desc.debugName, create);
    ++m_pipelineCreations;
    return handle;
}
void DeviceLifetime::RequireCreationBoundary(const char* operation) const
{
    if (m_frame || m_uploadActive)
        Fail(operation, "shader/pipeline/state creation requires a frame boundary");
}
const BufferDesc& DeviceLifetime::Describe(BufferHandle h) const
{
    return *Get(h).buffer;
}
const TextureDesc& DeviceLifetime::Describe(TextureHandle h) const
{
    return *Get(h).texture;
}
const ShaderDesc& DeviceLifetime::Describe(ShaderHandle h) const
{
    return *Get(h).shader;
}
const ResourceSetDesc& DeviceLifetime::Describe(ResourceSetHandle h) const
{
    return *Get(h).resourceSet;
}
const ResourceSetLayoutDesc& DeviceLifetime::Describe(ResourceSetLayoutHandle h) const
{
    return *Get(h).setLayout;
}
const PipelineLayoutDesc& DeviceLifetime::Describe(PipelineLayoutHandle h) const
{
    return *Get(h).pipelineLayout;
}
const GraphicsPipelineDesc& DeviceLifetime::Describe(GraphicsPipelineHandle h) const
{
    return *Get(h).pipeline;
}
const std::string& DeviceLifetime::PipelineKey(GraphicsPipelineHandle h) const
{
    return Get(h).pipelineKey;
}
std::vector<ResourceSetLayoutDesc> DeviceLifetime::ResolvedLayouts(PipelineLayoutHandle h) const
{
    const auto& desc = Describe(h);
    std::vector<ResourceSetLayoutDesc> values;
    for (std::size_t i = 0; i < desc.setCount; ++i)
        values.push_back(Describe(desc.sets[i]));
    return values;
}
void DeviceLifetime::ValidateFrameToken(const FrameToken& frame) const
{
    ValidateFrame(frame);
}
void DeviceLifetime::ValidateSetBinding(const FrameToken& frame, ResourceSetHandle handle,
                                        std::span<const std::uint32_t> offsets) const
{
    ValidateFrame(frame);
    const auto& desc = Describe(handle);
    auto entries = Describe(desc.layout).entries;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
    std::size_t count = 0;
    for (const auto& e : entries)
        if (e.dynamicOffset)
            count += e.count;
    if (offsets.size() != count)
        Fail("BindResourceSet", "dynamic offset count does not match sorted binding/array order");
    std::size_t index = 0;
    for (const auto& e : entries)
    {
        for (std::uint16_t array = 0; array < e.count; ++array)
        {
            const auto found = std::find_if(desc.bindings.begin(), desc.bindings.end(), [&](const auto& b)
                                            { return b.binding == e.binding && b.arrayElement == array; });
            if (found == desc.bindings.end())
                Fail("BindResourceSet", "immutable set lost binding coverage");
            if (e.type == BindingType::UniformBuffer)
            {
                const auto& buffer = Describe(found->buffer.buffer);
                const auto offset = e.dynamicOffset ? offsets[index++] : 0;
                const auto alignment = m_capabilities.uniformBufferOffsetAlignment;
                if (!alignment || offset % alignment || found->buffer.offset > buffer.size ||
                    offset > buffer.size - found->buffer.offset ||
                    found->buffer.size > buffer.size - found->buffer.offset - offset)
                    Fail("BindResourceSet", "dynamic offset alignment/range exceeds buffer");
            }
            else if (e.type == BindingType::SampledTexture)
                (void)Describe(found->texture);
            else
                (void)Get(found->sampler);
        }
    }
}
TimestampQueryHandle DeviceLifetime::CreateTimestampQuery(std::string_view name, const PayloadFactory& create)
{
    RequireCreationBoundary("CreateTimestampQuery");
    return Register<TimestampQueryHandle>({}, std::string(name), create);
}
std::string DeviceLifetime::DebugName(ResourceIdentity handle) const
{
    return Get(handle).debugName;
}
std::uint64_t DeviceLifetime::LastUse(ResourceIdentity handle) const
{
    return m_registries[handle.index()]->LastUse(ToKey(handle));
}
std::uint64_t DeviceLifetime::RequiredFrameCompletion(SwapChainHandle handle,
                                                      std::optional<std::uint32_t> acquiredIndex) const
{
    if (m_frame || m_uploadActive)
        Fail("RequiredFrameCompletion", "frame already active");
    const auto& chain = Get(handle);
    if (chain.swapChain->extent.width == 0 || chain.swapChain->extent.height == 0)
        return 0;
    if (chain.dependencies.empty() || m_issued == std::numeric_limits<std::uint64_t>::max())
        Fail("RequiredFrameCompletion", "backbuffers unavailable or serial exhausted");
    const auto index = acquiredIndex.value_or(static_cast<std::uint32_t>(m_issued % chain.dependencies.size()));
    if (index >= chain.dependencies.size())
        Fail("RequiredFrameCompletion", "native acquired index exceeds swap chain buffers");
    const auto& buffer = chain.dependencies[index];
    return std::max(m_lanes[m_issued % m_lanes.size()], m_registries[1]->LastUse(ToKey(buffer)));
}
SwapChainHandle DeviceLifetime::CreateSwapChain(const SwapChainDesc& desc, const PayloadFactory& create)
{
    if (desc.bufferCount < 2 || desc.bufferCount > 3 || desc.format != Format::Rgba8Unorm)
    {
        Fail("CreateSwapChain", "unsupported M6 swap chain description");
    }
    Record record;
    record.swapChain = desc;
    return Register<SwapChainHandle>(std::move(record), desc.debugName, create);
}
void DeviceLifetime::DestroyInternal(ResourceIdentity handle, bool owned, bool completed)
{
    const auto& record = Get(handle);
    if (record.references != 0 || ((record.backBuffer || record.frameSerial != 0) && !owned))
    {
        Fail("Destroy", "resource is retained by a live owner");
    }
    auto dependencies = record.dependencies;
    if (completed)
    {
        m_registries[handle.index()]->DestroyCompleted(ToKey(handle));
    }
    else
    {
        m_registries[handle.index()]->Destroy(ToKey(handle));
    }
    for (const auto& dependency : dependencies)
    {
        --Get(dependency).references;
    }
}
void DeviceLifetime::Destroy(ResourceIdentity handle)
{
    if (m_uploadActive)
    {
        Fail("Destroy", "upload callback cannot reenter device mutation");
    }
    if (handle.index() == kSwapChainIdentityIndex)
    {
        if (m_frame || m_completed != m_lastSubmitted)
        {
            Fail("DestroySwapChain", "swap chain requires idle boundary");
        }
        auto& chain = Get(handle);
        const auto buffers = chain.dependencies;
        for (const auto& buffer : buffers)
        {
            if (Get(buffer).references != 1)
            {
                Fail("DestroySwapChain", "backbuffer still has a live import owner");
            }
        }
        chain.dependencies.clear();
        for (const auto& buffer : buffers)
        {
            --Get(buffer).references;
            DestroyInternal(buffer, true, true);
        }
        DestroyInternal(handle, false, true);
        return;
    }
    DestroyInternal(handle, false);
}
void DeviceLifetime::MarkTree(const ResourceIdentity& handle, std::uint64_t serial)
{
    const auto& dependencies = Get(handle).dependencies;
    for (const auto& dependency : dependencies)
    {
        MarkTree(dependency, serial);
    }
    m_registries[handle.index()]->MarkUsed(ToKey(handle), serial);
}
void DeviceLifetime::ValidateFrameResource(const FrameToken& frame, ResourceIdentity handle) const
{
    ValidateFrame(frame);
    const auto& record = Get(handle);
    if (record.backBuffer && handle != ResourceIdentity(frame.backBuffer))
        Fail("Use", "only the current frame's acquired backbuffer can be used");
}
void DeviceLifetime::Use(const FrameToken& frame, ResourceIdentity handle)
{
    ValidateFrameResource(frame, handle);
    MarkTree(handle, frame.serial);
}
void DeviceLifetime::CommitReplacement(ResourceSetHandle& current, ResourceSetHandle replacement)
{
    if (m_frame || m_uploadActive)
    {
        Fail("CommitReplacement", "revision commit requires a frame boundary");
    }
    (void)Get(replacement);
    if (current == replacement)
    {
        Fail("CommitReplacement", "replacement must be a new immutable set");
    }
    if (current)
    {
        if (Get(current).dependencies.front() != Get(replacement).dependencies.front())
        {
            Fail("CommitReplacement", "replacement layout must match current revision");
        }
        DestroyInternal(current, false);
    }
    current = replacement;
}
void DeviceLifetime::ResizeBackBuffers(SwapChainHandle handle, Extent2D extent, const BackBufferFactory& replace)
{
    if (m_frame || m_uploadActive)
    {
        Fail("ResizeBackBuffers", "cannot resize while recording");
    }
    auto& chain = Get(handle);
    if (extent.width == 0 || extent.height == 0)
    {
        chain.swapChain->extent = extent;
        return;
    }
    if (m_completed != m_lastSubmitted)
    {
        Fail("ResizeBackBuffers", "backend must establish idle completion");
    }
    for (const auto& buffer : chain.dependencies)
    {
        if (Get(buffer).references != 1)
        {
            Fail("ResizeBackBuffers", "backbuffer still has an external owner");
        }
    }
    const auto old = chain.dependencies;
    chain.dependencies.clear();
    chain.swapChain->extent = {};
    for (const auto& buffer : old)
    {
        --Get(buffer).references;
        DestroyInternal(buffer, true, true);
    }
    Collect(m_completed); // 必须先真正释放旧 payload，backend 才能 ResizeBuffers。
    if (!replace)
    {
        Fail("ResizeBackBuffers", "backend resize callback is missing");
    }
    auto candidates = replace();
    auto& replacementChain = Get(handle);
    if (candidates.size() != replacementChain.swapChain->bufferCount)
    {
        Fail("ResizeBackBuffers", "backbuffer count mismatch");
    }
    for (const auto& candidate : candidates)
    {
        const auto& desc = candidate.desc;
        ValidateTextureDesc(desc, m_capabilities);
        if (!candidate.payload || desc.extent != extent || desc.format != replacementChain.swapChain->format ||
            desc.dimension != TextureDimension::Texture2D || desc.arrayLayers != 1 || desc.mipLevels != 1 ||
            !HasFlag(desc.usage, TextureUsage::ColorAttachment))
        {
            Fail("ResizeBackBuffers", "invalid backbuffer candidate");
        }
    }
    std::vector<ResourceIdentity> registered;
    registered.reserve(candidates.size());
    try
    {
        for (auto& candidate : candidates)
        {
            Record record;
            record.texture = candidate.desc;
            record.backBuffer = true;
            record.references = 1;
            auto texture = Register<TextureHandle>(std::move(record), candidate.desc.debugName,
                                                   [&candidate] { return std::move(candidate.payload); });
            registered.emplace_back(texture);
        }
    }
    catch (...)
    {
        for (const auto& texture : registered)
        {
            m_registries[1]->DestroyCompleted(ToKey(texture));
        }
        throw;
    }
    replacementChain.dependencies = std::move(registered);
    replacementChain.swapChain->extent = extent;
}

FrameToken DeviceLifetime::BeginFrame(SwapChainHandle handle, std::optional<std::uint32_t> acquiredIndex)
{
    if (m_frame || m_uploadActive)
    {
        Fail("BeginFrame", "frame already active");
    }
    const auto& chain = Get(handle);
    if (chain.swapChain->extent.width == 0 || chain.swapChain->extent.height == 0)
    {
        return {};
    }
    if (chain.dependencies.empty() || m_issued == std::numeric_limits<std::uint64_t>::max())
    {
        Fail("BeginFrame", "backbuffers unavailable or frame serial exhausted");
    }
    const auto serial = m_issued + 1;
    const auto lane = static_cast<std::uint32_t>((serial - 1) % m_lanes.size());
    if (RequiredFrameCompletion(handle, acquiredIndex) > m_completed)
    {
        Fail("BeginFrame", "recycle lane is still in flight");
    }
    FrameToken frame{serial, m_owner, lane,
                     std::get<TextureHandle>(chain.dependencies[acquiredIndex.value_or(
                         static_cast<std::uint32_t>((serial - 1) % chain.dependencies.size()))])};
    m_issued = serial;
    m_frame = frame;
    m_frameChain = handle;
    // 帧持有 chain 本身与本次 acquire 的 backbuffer；不能把全部 backbuffer 都标成已使用。
    m_registries[9]->MarkUsed(ToKey(handle), serial);
    MarkTree(frame.backBuffer, serial);
    return frame;
}
void DeviceLifetime::ValidateFrame(const FrameToken& frame) const
{
    if (!m_frame || frame.serial == 0 || frame.owner != m_owner || frame.serial != m_frame->serial ||
        frame.recycleLane != m_frame->recycleLane || frame.backBuffer != m_frame->backBuffer)
    {
        Fail("ValidateFrame", "stale, forged or foreign frame token");
    }
    (void)Get(frame.backBuffer);
}
DynamicBufferSlice DeviceLifetime::WriteDynamicBuffer(const FrameToken& frame, std::span<const std::byte> bytes,
                                                      std::uint32_t alignment, const DynamicPayloadFactory& create)
{
    ValidateFrame(frame);
    if (bytes.empty() || bytes.size() > 65536 || !std::has_single_bit(alignment))
    {
        Fail("WriteDynamicBuffer", "invalid dynamic size or alignment");
    }
    m_dynamic.reserve(m_dynamic.size() + 1);
    BufferDesc desc{(bytes.size() + 15) & ~std::uint64_t{15}, BufferUsage::Uniform, MemoryDomain::CpuToGpu,
                    "frame constants"};
    if (!create)
    {
        Fail("WriteDynamicBuffer", "backend dynamic allocator is missing");
    }
    const auto buffer = CreateBuffer(desc, [&] { return create(desc, bytes); });
    Get(buffer).frameSerial = frame.serial;
    Get(buffer).dynamicSize = bytes.size();
    m_dynamic.push_back(buffer);
    Use(frame, buffer);
    // 此处管理 slice 身份；create 回调必须在返回前复制 bytes 到 backend-owned upload storage。
    return {{buffer, 0, bytes.size()}, frame.serial};
}
ResourcePayload& DeviceLifetime::Payload(ResourceIdentity handle)
{
    return *Get(handle).payload;
}
std::vector<TextureHandle> DeviceLifetime::BackBuffers(SwapChainHandle chain) const
{
    std::vector<TextureHandle> result;
    for (const auto& resource : Get(chain).dependencies)
        result.push_back(std::get<TextureHandle>(resource));
    return result;
}
std::uint64_t DeviceLifetime::InitializeBuffer(BufferHandle destination, std::span<const std::byte> bytes,
                                               const BufferUpload& submit)
{
    if (LastUse(destination) != 0 || bytes.empty())
        Fail("InitializeBuffer", "initial data requires a new unused resource and nonempty bytes");
    return SubmitBufferUpload(destination, 0, bytes, submit, true);
}
std::uint64_t DeviceLifetime::UploadBuffer(BufferHandle destination, std::uint64_t offset,
                                           std::span<const std::byte> bytes, const BufferUpload& submit)
{
    return SubmitBufferUpload(destination, offset, bytes, submit, false);
}
std::uint64_t DeviceLifetime::SubmitBufferUpload(BufferHandle destination, std::uint64_t offset,
                                                 std::span<const std::byte> bytes, const BufferUpload& submit,
                                                 bool initial)
{
    auto& record = Get(destination);
    if (initial)
        ValidateBufferInitialData(*record.buffer, bytes);
    else
        ValidateBufferUpload(*record.buffer, offset, bytes);
    if (m_frame || m_uploadActive || !submit || record.references || record.frameSerial ||
        m_registries[0]->LastUse(ToKey(destination)) > m_completed ||
        m_issued == std::numeric_limits<std::uint64_t>::max())
    {
        Fail("UploadBuffer", "upload needs an unreferenced resource and a frame boundary");
    }
    const auto serial = m_issued + 1;
    m_uploadActive = true;
    try
    {
        submit(*record.payload, offset, bytes, serial);
    }
    catch (...)
    {
        m_uploadActive = false;
        throw;
    }
    m_uploadActive = false;
    m_registries[0]->MarkUsed(ToKey(destination), serial);
    m_issued = serial;
    m_lastSubmitted = serial;
    return serial;
}
std::uint64_t DeviceLifetime::UploadTexture(TextureHandle destination,
                                            std::span<const TextureSubresourceData> subresources,
                                            const TextureUpload& submit)
{
    auto& record = Get(destination);
    ValidateTextureUpload(*record.texture, subresources);
    if (m_frame || m_uploadActive || !submit || record.references || record.backBuffer ||
        m_registries[1]->LastUse(ToKey(destination)) > m_completed ||
        m_issued == std::numeric_limits<std::uint64_t>::max())
    {
        Fail("UploadTexture", "upload needs an unreferenced resource and a frame boundary");
    }
    const auto serial = m_issued + 1;
    m_uploadActive = true;
    try
    {
        submit(*record.payload, subresources, serial);
    }
    catch (...)
    {
        m_uploadActive = false;
        throw;
    }
    m_uploadActive = false;
    m_registries[1]->MarkUsed(ToKey(destination), serial);
    m_issued = serial;
    m_lastSubmitted = serial;
    return serial;
}
void DeviceLifetime::ValidateDynamicSlice(const FrameToken& frame, const DynamicBufferSlice& slice) const
{
    ValidateFrame(frame);
    const auto& record = Get(slice.view.buffer);
    if (slice.frameSerial != frame.serial || record.frameSerial != frame.serial || slice.view.size == 0 ||
        slice.view.offset != 0 || slice.view.size != record.dynamicSize)
    {
        Fail("ValidateDynamicSlice", "slice is stale or outside frame allocation");
    }
}
void DeviceLifetime::EndFrame(const FrameToken& frame, SwapChainHandle chain)
{
    ValidateFrame(frame);
    if (chain != m_frameChain)
    {
        Fail("EndFrame", "frame belongs to a different swap chain");
    }
    // backend 只有在成功提交后调用；尚未提交的引用已由 Use 保护，但不能提前 Collect。
    m_lastSubmitted = frame.serial;
    m_lanes[frame.recycleLane] = frame.serial;
    m_frame.reset();
    m_frameChain = {};
    // 帧集合先释放逻辑依赖，随后其动态 buffer 才能退休。
    for (auto set : m_frameSets)
        DestroyInternal(set, true);
    m_frameSets.clear();
    for (auto buffer : m_dynamic)
    {
        DestroyInternal(buffer, true);
    }
    m_dynamic.clear();
}
void DeviceLifetime::Collect(std::uint64_t completed)
{
    if (m_uploadActive)
    {
        Fail("Collect", "upload callback cannot change completion");
    }
    if (completed < m_completed || completed > m_lastSubmitted)
    {
        Fail("Collect", "completion must be monotonic and no later than actual submission");
    }
    m_completed = completed;
    // 按依赖所有者先释放；swap chain 最后，不能持有已销毁 backbuffer 的外部 view。
    constexpr std::array<std::size_t, 10> order{7, 5, 6, 4, 3, 2, 1, 0, 8, 9};
    for (auto kind : order)
    {
        m_registries[kind]->Collect(completed);
    }
}
RegistryCounts DeviceLifetime::Stats() const
{
    RegistryCounts result;
    for (const auto& registry : m_registries)
    {
        const auto count = registry->Stats();
        result.alive += count.alive;
        result.retiring += count.retiring;
        result.slots += count.slots;
        result.exhausted += count.exhausted;
    }
    return result;
}
DeviceDiagnostics DeviceLifetime::Diagnostics() const
{
    const auto counts = Stats();
    return {counts.alive,
            counts.retiring,
            counts.slots,
            counts.exhausted,
            m_lastSubmitted,
            m_completed,
            m_frame ? m_frame->serial : 0};
}
void DeviceLifetime::CheckShutdown() const
{
    const auto count = Stats();
    if (m_frame || m_completed != m_lastSubmitted || count.alive || count.retiring)
    {
        Fail("CheckShutdown", "active frames, GPU work or resource owners remain");
    }
}
} // namespace MiniEngine::Rhi
