#include "TraceRhi.h"
#include <MiniEngine/Rhi/RhiValidation.h>
#include <algorithm>
#include <bit>
#include <iomanip>
#include <locale>
#include <sstream>

namespace MiniEngine::Tests
{
namespace
{
struct TracePayload final : ResourcePayload
{
    std::vector<std::byte> bytes;
};
std::unique_ptr<ResourcePayload> Payload()
{
    return std::make_unique<TracePayload>();
}
RhiCapabilities TraceCapabilities(RhiBackend backend)
{
    RhiCapabilities result;
    result.backend = backend;
    result.adapterName = "CPU semantic trace (no GPU)";
    result.maxTextureDimension2D = 16384;
    result.maxColorAttachments = 4;
    result.maxAnisotropy = 16;
    result.formatSupport.fill(31);
    result.cubeFormatSupport.fill(31);
    return result;
}
std::uint64_t HashBytes(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes)
    {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace
std::uint64_t TraceCompletion::Wait(std::uint64_t required)
{
    waits.push_back(required);
    // 这是显式测试调度器，不从 CPU 帧号推断 GPU 完成。
    return onWait ? onWait(required) : completed;
}
TraceRhiDevice::TraceRhiDevice(RhiBackend backend)
    : m_capabilities(TraceCapabilities(backend)), m_lifetime(m_capabilities),
      m_importOwner(Detail::AcquireRegistryOwner())
{
}
const RhiCapabilities& TraceRhiDevice::Capabilities() const
{
    return m_capabilities;
}
DeviceDiagnostics TraceRhiDevice::Diagnostics() const
{
    return m_lifetime.Diagnostics();
}
TextureStateSnapshot TraceRhiDevice::QueryTextureState(const FrameToken& frame, TextureHandle texture) const
{
    m_lifetime.ValidateFrameResource(frame, texture);
    const auto state = m_resources.find(texture);
    return {m_lifetime.Describe(texture), state == m_resources.end() ? ResourceAccess::None : state->second.access,
            state != m_resources.end() && state->second.defined};
}
BufferStateSnapshot TraceRhiDevice::QueryBufferState(const FrameToken& frame, BufferHandle buffer) const
{
    m_lifetime.ValidateFrameResource(frame, buffer);
    const auto& desc = m_lifetime.Describe(buffer);
    const auto state = m_resources.find(buffer);
    return {desc, state == m_resources.end() ? ResourceAccess::None : state->second.access,
            state != m_resources.end() && state->second.Contains(0, desc.size)};
}
[[noreturn]] void TraceRhiDevice::Fail(const char* operation, const char* message) const
{
    throw RhiValidationError(
        {RhiErrorCode::InvalidState, operation, "TraceRhiDevice", "", "trace",
         "frame=" + std::to_string(m_frame ? m_frame->serial : 0) + " pass=<device> command=0: " + message});
}
std::uint64_t TraceRhiDevice::Id(ResourceIdentity handle)
{
    auto [it, added] = m_ids.try_emplace(handle, m_ids.size() + 1);
    (void)added;
    return it->second;
}
void TraceRhiDevice::Record(const CommandEvent& event)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    // 长度前缀防止名称/标签制造分隔符歧义；逻辑 ID 排除 registry owner、地址与 padding。
    out << event.operation.size() << ':' << event.operation << " r" << event.resources.size();
    for (const auto& handle : event.resources)
        out << ' ' << handle.index() << ':' << Id(handle);
    out << " i" << event.integers.size();
    for (auto value : event.integers)
        out << ' ' << value;
    out << " f" << event.scalars.size();
    for (auto value : event.scalars)
        out << ' ' << std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value);
    const auto text = event.operation == "SetPipeline"
                          ? m_lifetime.PipelineKey(std::get<GraphicsPipelineHandle>(event.resources.at(0)))
                          : event.text;
    out << " t" << text.size() << ':' << text;
    m_events.push_back(out.str());
}
void TraceRhiDevice::OnCommand(const CommandEvent& event)
{
    if (event.operation == "WriteTimestamp")
        m_queries[std::get<TimestampQueryHandle>(event.resources.at(0))] = {0, 0, m_frame->serial, false, 0, true};
    if (event.operation == "CopyTextureForReadback")
    {
        TextureReadbackResult result;
        result.extent = {static_cast<std::uint32_t>(event.integers[0]), static_cast<std::uint32_t>(event.integers[1])};
        result.format = Format::Rgba8Unorm;
        result.rowPitch = static_cast<std::uint64_t>(result.extent.width) * 4;
        result.unavailable = true;
        result.frameSerial = m_frame->serial;
        result.source = std::get<TextureHandle>(event.resources.at(0));
        m_readbacks[std::get<BufferHandle>(event.resources.at(1))] = {m_frame->serial, std::move(result)};
    }
    if (event.operation == "CopyBuffer")
        m_readbacks.erase(std::get<BufferHandle>(event.resources.at(1)));
    Record(event);
    if (m_capabilities.backend == RhiBackend::D3D11)
        ++m_consumed;
    else
        ++m_pending;
}
BufferHandle TraceRhiDevice::CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initial)
{
    ValidateBufferInitialData(desc, initial);
    // initialData 是创建契约，不能错误地套用随后 Upload 的 CopyDestination usage 要求。
    auto handle = Own(m_lifetime.CreateBuffer(desc,
                                              [&]
                                              {
                                                  auto payload = std::make_unique<TracePayload>();
                                                  payload->bytes.assign(initial.begin(), initial.end());
                                                  return payload;
                                              }));
    if (!initial.empty())
        m_resources[handle].Define(0, initial.size());
    Record({"CreateBuffer", {handle}, {SemanticHash(desc), initial.size(), HashBytes(initial)}});
    return handle;
}
TextureHandle TraceRhiDevice::CreateTexture(const TextureDesc& desc)
{
    auto handle = Own(m_lifetime.CreateTexture(desc, Payload));
    m_resources[handle] = {};
    Record({"CreateTexture", {handle}, {SemanticHash(desc)}});
    return handle;
}
SamplerHandle TraceRhiDevice::CreateSampler(const SamplerDesc& desc)
{
    auto handle = Own(m_lifetime.CreateSampler(desc, Payload));
    Record({"CreateSampler", {handle}, {SemanticHash(desc)}});
    return handle;
}
ShaderHandle TraceRhiDevice::CreateShader(const ShaderDesc& desc)
{
    auto handle = Own(m_lifetime.CreateShader(desc, Payload));
    // 离线 backend bytecode 不相同；公共 semantic identity 相同才应有同一 trace。
    Record({"CreateShader", {handle}, {static_cast<std::uint64_t>(desc.stage)}, {}, desc.semanticHash});
    return handle;
}
ResourceSetLayoutHandle TraceRhiDevice::CreateResourceSetLayout(const ResourceSetLayoutDesc& desc)
{
    auto handle = Own(m_lifetime.CreateResourceSetLayout(desc, Payload));
    Record({"CreateResourceSetLayout", {handle}, {SemanticHash(desc)}});
    return handle;
}
ResourceSetHandle TraceRhiDevice::CreateResourceSet(const ResourceSetDesc& desc)
{
    auto handle = Own(m_lifetime.CreateResourceSet(desc, Payload));
    CommandEvent event{"CreateResourceSet", {handle, desc.layout}};
    auto sorted = desc.bindings;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
              { return std::pair{a.binding, a.arrayElement} < std::pair{b.binding, b.arrayElement}; });
    for (const auto& binding : sorted)
    {
        event.integers.insert(event.integers.end(),
                              {binding.binding, binding.arrayElement, static_cast<std::uint64_t>(binding.type)});
        if (binding.type == BindingType::UniformBuffer)
        {
            event.resources.emplace_back(binding.buffer.buffer);
            event.integers.insert(event.integers.end(), {binding.buffer.offset, binding.buffer.size});
        }
        else if (binding.type == BindingType::SampledTexture)
            event.resources.emplace_back(binding.texture);
        else
            event.resources.emplace_back(binding.sampler);
    }
    Record(event);
    return handle;
}
ResourceSetHandle TraceRhiDevice::CreateFrameResourceSet(const FrameToken& frame, const ResourceSetDesc& desc)
{
    auto handle = m_lifetime.CreateFrameResourceSet(frame, desc, Payload);
    Record({"CreateFrameResourceSet", {handle}, {frame.serial}});
    return handle;
}
PipelineLayoutHandle TraceRhiDevice::CreatePipelineLayout(const PipelineLayoutDesc& desc)
{
    auto handle = Own(m_lifetime.CreatePipelineLayout(desc, Payload));
    CommandEvent event{"CreatePipelineLayout", {handle}};
    for (std::uint32_t i = 0; i < desc.setCount; ++i)
        event.resources.emplace_back(desc.sets[i]);
    Record(event);
    return handle;
}
GraphicsPipelineHandle TraceRhiDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    auto handle = Own(m_lifetime.CreateGraphicsPipeline(desc, Payload));
    Record({"CreateGraphicsPipeline", {handle}, {}, {}, m_lifetime.PipelineKey(handle)});
    return handle;
}
TimestampQueryHandle TraceRhiDevice::CreateTimestampQuery(std::string_view name)
{
    auto handle = Own(m_lifetime.CreateTimestampQuery(name, Payload));
    Record({"CreateTimestampQuery", {handle}});
    return handle;
}
SwapChainHandle TraceRhiDevice::CreateSwapChain(const SwapChainDesc& desc)
{
    if (m_frame)
        Fail("CreateSwapChain", "creation requires a frame boundary");
    if (desc.extent.width != 0 && desc.extent.height != 0)
        ValidateTextureDesc({TextureDimension::Texture2D, desc.extent, 1, 1, 1, desc.format,
                             TextureUsage::ColorAttachment | TextureUsage::CopySource, desc.debugName},
                            m_capabilities);
    WaitIdle();
    auto handle = m_lifetime.CreateSwapChain(desc, Payload);
    try
    {
        m_lifetime.ResizeBackBuffers(handle, desc.extent, [&] { return BuildBackBuffers(desc, desc.extent); });
    }
    catch (...)
    {
        m_lifetime.Destroy(handle);
        m_lifetime.Collect(m_lifetime.Completed());
        throw;
    }
    Own(handle);
    m_chains[handle] = desc;
    Record({"CreateSwapChain",
            {handle},
            {desc.extent.width, desc.extent.height, static_cast<std::uint64_t>(desc.format), desc.bufferCount,
             desc.vsync}});
    Record({"ResizeSwapChain", {handle}, {desc.extent.width, desc.extent.height}});
    return handle;
}
void TraceRhiDevice::Retire(ResourceIdentity handle)
{
    if (m_frame && m_commands && m_commands->HasActiveAttachment(handle))
        Fail("Destroy", "attachment is retained by active rendering; EndRendering first");
    m_lifetime.Destroy(handle);
    Record({"Destroy", {handle}});
    m_resources.erase(handle);
}
void TraceRhiDevice::Destroy(BufferHandle handle)
{
    Retire(handle);
    m_readbacks.erase(handle);
}
void TraceRhiDevice::Destroy(TextureHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(SamplerHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(ShaderHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(ResourceSetLayoutHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(ResourceSetHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(PipelineLayoutHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(GraphicsPipelineHandle handle)
{
    Retire(handle);
}
void TraceRhiDevice::Destroy(TimestampQueryHandle handle)
{
    Retire(handle);
    m_queries.erase(handle);
}
void TraceRhiDevice::Destroy(SwapChainHandle handle)
{
    Retire(handle);
    m_chains.erase(handle);
    PurgeDeadStates();
}

void TraceRhiDevice::CompleteThrough(std::uint64_t serial)
{
    m_lifetime.Collect(serial);
    m_completion.completed = serial;
    PurgeDeadStates();
}
void TraceRhiDevice::WaitFor(std::uint64_t required)
{
    if (required <= m_lifetime.Completed())
        return;
    const auto observed = m_completion.Wait(required);
    if (observed < required)
        Fail("WaitFor", "completion source has not proved required work complete");
    CompleteThrough(observed);
}
FrameToken TraceRhiDevice::BeginFrame(SwapChainHandle chain)
{
    if (m_frame)
        Fail("BeginFrame", "a frame is already active");
    WaitFor(m_lifetime.RequiredFrameCompletion(chain));
    auto frame = m_lifetime.BeginFrame(chain);
    if (!frame.serial)
        return frame;
    m_frameImports.clear();
    m_frame = frame;
    m_chain = chain;
    m_commands = nullptr;
    m_pending = 0;
    (void)Id(frame.backBuffer);
    Record({"BeginFrame", {chain, frame.backBuffer}, {frame.serial, frame.recycleLane}});
    return frame;
}
TextureHandle TraceRhiDevice::AcquireBackBuffer(const FrameToken& frame)
{
    m_lifetime.ValidateFrameToken(frame);
    return frame.backBuffer;
}
IRhiCommandList& TraceRhiDevice::BeginGraphics(const FrameToken& frame)
{
    m_lifetime.ValidateFrameToken(frame);
    if (m_commands)
        Fail("BeginGraphics", "frame already opened its graphics command list");
    auto commands = std::make_unique<CommandRecording>(
        m_lifetime, frame, m_resources, [this](const auto& event) { OnCommand(event); },
        [this](ResourceIdentity resource)
        {
            if (!m_frameImports.contains(resource))
                Fail("GraphImport", "resource is not imported for this frame");
        },
        [this](std::span<const ResourceIdentity> resources)
        {
            auto candidate = m_frameImports;
            candidate.insert(resources.begin(), resources.end());
            m_frameImports.swap(candidate);
        });
    m_commands = commands.get();
    m_recordings.push_back(std::move(commands));
    Record({"BeginGraphics", {}, {frame.serial}});
    return *m_commands;
}
IRhiGraphCommandSink& TraceRhiDevice::GraphCommandSink(const FrameToken& frame)
{
    m_lifetime.ValidateFrameToken(frame);
    if (!m_commands || m_commands->CurrentState() == CommandRecording::State::Closed)
        Fail("GraphCommandSink", "graphics is not recording");
    return *m_commands;
}
void TraceRhiDevice::EndGraphics(const FrameToken& frame, IRhiCommandList& commands)
{
    m_lifetime.ValidateFrameToken(frame);
    if (!m_commands || &commands != m_commands)
        Fail("EndGraphics", "foreign command list");
    const auto state = m_resources.find(frame.backBuffer);
    if (state == m_resources.end() || state->second.access != ResourceAccess::Present || !state->second.defined ||
        state->second.declaredFrame != frame.serial)
        Fail("EndGraphics", "declare defined backbuffer Present before closing; recording remains open");
    m_commands->Close();
}
void TraceRhiDevice::EndFrame(const FrameToken& frame, SwapChainHandle chain)
{
    m_lifetime.ValidateFrameToken(frame);
    if (chain != m_chain || !m_commands || m_commands->CurrentState() != CommandRecording::State::Closed)
        Fail("EndFrame", "submission requires the frame's already closed command list and matching chain");
    const auto state = m_resources.find(frame.backBuffer);
    if (state == m_resources.end() || state->second.access != ResourceAccess::Present || !state->second.defined)
        Fail("EndFrame", "backbuffer must be defined and have final Present access");
    // Close 只由 EndGraphics 进行；这里消费封闭 batch、记录提交，不推断 completion。
    m_lifetime.EndFrame(frame, chain);
    if (m_capabilities.backend == RhiBackend::D3D12)
        m_consumed += m_pending;
    m_pending = 0;
    Record({"EndFrame", {chain}, {frame.serial}});
    m_frame.reset();
    m_chain = {};
    PurgeDeadStates();
}
DynamicBufferSlice TraceRhiDevice::WriteDynamicBuffer(const FrameToken& frame, std::span<const std::byte> bytes,
                                                      std::uint32_t alignment)
{
    auto slice = m_lifetime.WriteDynamicBuffer(frame, bytes, alignment,
                                               [](const BufferDesc&, std::span<const std::byte> input)
                                               {
                                                   auto payload = std::make_unique<TracePayload>();
                                                   payload->bytes.assign(input.begin(), input.end());
                                                   return payload;
                                               });
    m_resources[slice.view.buffer].Define(0, bytes.size());
    Record({"WriteDynamicBuffer", {slice.view.buffer}, {bytes.size(), alignment, HashBytes(bytes)}});
    return slice;
}
void TraceRhiDevice::UploadBuffer(BufferHandle destination, std::uint64_t offset, std::span<const std::byte> bytes)
{
    m_lifetime.UploadBuffer(
        destination, offset, bytes,
        [](ResourcePayload& base, std::uint64_t start, std::span<const std::byte> input, std::uint64_t)
        {
            auto& payload = static_cast<TracePayload&>(base);
            payload.bytes.resize(std::max(payload.bytes.size(), static_cast<std::size_t>(start + input.size())));
            std::copy(input.begin(), input.end(), payload.bytes.begin() + static_cast<std::ptrdiff_t>(start));
        });
    m_resources[destination].Define(offset, bytes.size());
    Record({"UploadBuffer", {destination}, {offset, bytes.size(), HashBytes(bytes)}});
}
void TraceRhiDevice::UploadTexture(TextureHandle destination, std::span<const TextureSubresourceData> subresources)
{
    m_lifetime.UploadTexture(destination, subresources,
                             [](ResourcePayload&, std::span<const TextureSubresourceData>, std::uint64_t) {});
    m_resources[destination].defined = true;
    CommandEvent event{"UploadTexture", {destination}};
    for (const auto& subresource : subresources)
        event.integers.insert(event.integers.end(),
                              {subresource.rowPitch, subresource.slicePitch, HashBytes(subresource.bytes)});
    Record(event);
}
std::optional<TimestampResult> TraceRhiDevice::TryReadTimestamp(TimestampQueryHandle query)
{
    m_lifetime.ValidateAlive(query);
    auto found = m_queries.find(query);
    if (found == m_queries.end() || found->second.frameSerial > m_lifetime.Completed())
        return std::nullopt;
    return found->second;
}
std::optional<TextureReadbackResult> TraceRhiDevice::TryReadTextureReadback(BufferHandle readback)
{
    const auto& desc = m_lifetime.Describe(readback);
    if (desc.memory != MemoryDomain::GpuToCpu)
        Fail("TryReadTextureReadback", "buffer is not readback memory");
    auto found = m_readbacks.find(readback);
    if (found == m_readbacks.end() || found->second.first > m_lifetime.Completed())
        return std::nullopt;
    return found->second.second;
}
void TraceRhiDevice::PurgeDeadStates()
{
    for (auto it = m_resources.begin(); it != m_resources.end();)
    {
        try
        {
            m_lifetime.ValidateAlive(it->first);
            ++it;
        }
        catch (const RhiException&)
        {
            it = m_resources.erase(it);
        }
    }
}
std::vector<DeviceLifetime::BackBufferCandidate> TraceRhiDevice::BuildBackBuffers(const SwapChainDesc& desc,
                                                                                  Extent2D extent)
{
    if (m_failNextBackBuffers)
    {
        m_failNextBackBuffers = false;
        Fail("BackBufferFactory", "injected backbuffer allocation failure");
    }
    std::vector<DeviceLifetime::BackBufferCandidate> result;
    for (std::uint32_t i = 0; i < desc.bufferCount; ++i)
        result.push_back({{TextureDimension::Texture2D, extent, 1, 1, 1, desc.format,
                           TextureUsage::ColorAttachment | TextureUsage::CopySource, "backbuffer " + std::to_string(i)},
                          Payload()});
    return result;
}
void TraceRhiDevice::ResizeSwapChain(SwapChainHandle chain, Extent2D extent)
{
    m_lifetime.ValidateAlive(chain);
    if (m_frame)
        Fail("ResizeSwapChain", "resize requires frame boundary");
    WaitIdle();
    const auto desc = m_chains.at(chain);
    m_lifetime.ResizeBackBuffers(chain, extent, [&] { return BuildBackBuffers(desc, extent); });
    m_chains.at(chain).extent = extent;
    PurgeDeadStates();
    Record({"ResizeSwapChain", {chain}, {extent.width, extent.height}});
}
void TraceRhiDevice::WaitIdle()
{
    if (m_frame)
        Fail("WaitIdle", "WaitIdle cannot flush an active recording");
    ++m_waitIdleCount;
    WaitFor(m_lifetime.LastSubmitted());
}
TraceImportHandle TraceRhiDevice::Import(const FrameToken& frame, ResourceIdentity resource)
{
    m_lifetime.ValidateFrameToken(frame);
    m_lifetime.ValidateAlive(resource);
    if (resource.index() != 0 && resource.index() != 1)
        Fail("Import", "graph imports are buffers/textures");
    m_frameImports.insert(resource);
    m_imports.push_back({frame, resource});
    return {m_importOwner, m_imports.size() - 1};
}
ResourceIdentity TraceRhiDevice::Resolve(TraceImportHandle handle)
{
    if (handle.owner != m_importOwner || handle.index >= m_imports.size())
        Fail("Resolve", "foreign or forged import handle");
    const auto& record = m_imports[handle.index];
    m_lifetime.ValidateFrameToken(record.frame);
    m_lifetime.ValidateAlive(record.resource);
    return record.resource;
}
void TraceRhiDevice::Shutdown()
{
    if (m_frame)
        Fail("Shutdown", "active frame must be ended first");
    WaitIdle();
    for (auto it = m_owned.rbegin(); it != m_owned.rend(); ++it)
    {
        try
        {
            m_lifetime.ValidateAlive(*it);
        }
        catch (const RhiException&)
        {
            continue;
        }
        Retire(*it);
    }
    CompleteThrough(m_lifetime.LastSubmitted());
    m_lifetime.CheckShutdown();
}
std::string TraceRhiDevice::CanonicalTrace() const
{
    std::string result = "miniengine.command-trace.v1\n";
    for (const auto& event : m_events)
        result += event + '\n';
    return result;
}
std::uint64_t TraceRhiDevice::StableHash() const
{
    const auto text = CanonicalTrace();
    return HashBytes(std::as_bytes(std::span(text.data(), text.size())));
}
} // namespace MiniEngine::Tests
