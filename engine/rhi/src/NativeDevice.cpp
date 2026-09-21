#include "NativeDevice.h"
#include <MiniEngine/Rhi/RhiResults.h>
#include <MiniEngine/Rhi/RhiValidation.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <exception>
#include <intrin.h>
#include <locale>
#include <sstream>
#include <thread>

namespace MiniEngine::Rhi
{
namespace
{
// M7-SUBMIT-001：提交路径的事件级计时用 TSC 而不是 steady_clock/QPC。
// 原因（实测教训，见 E-M7-SUBMIT-001 的 INVALID 记录）：QPC 读取在 ~3k 事件/帧的热路径上
// 单次约 0.5 µs，一对读就是 ~1 µs/事件，既把整帧拖慢 ~4.7%（同会话无仪表对照），
// 也把 native 段本身污染掉（测得 0.56 µs/事件 < 开销 1.02 µs/事件 ⇒ 不可事后修正）。
// TSC 读是 ~20-30 ns 级；换算系数在首次使用时用 steady_clock 标定一次（不变 TSC 前提）。
class SubmitClock final
{
  public:
    SubmitClock()
    {
        const auto startClock = std::chrono::steady_clock::now();
        const std::uint64_t startTicks = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::uint64_t endTicks = __rdtsc();
        const double elapsedMicros =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - startClock).count();
        m_microsPerTick = elapsedMicros > 0.0 && endTicks > startTicks
                              ? elapsedMicros / static_cast<double>(endTicks - startTicks)
                              : 0.0;
    }

    [[nodiscard]] std::uint64_t Ticks() const noexcept
    {
        return __rdtsc();
    }

    [[nodiscard]] std::uint64_t MicrosSince(const std::uint64_t startTicks) const noexcept
    {
        const std::uint64_t delta = __rdtsc() - startTicks;
        return static_cast<std::uint64_t>(static_cast<double>(delta) * m_microsPerTick);
    }

  private:
    double m_microsPerTick = 0.0;
};

const SubmitClock& SubmitTimer()
{
    static const SubmitClock clock;
    return clock;
}
} // namespace

NativeDevice::NativeDevice(std::unique_ptr<NativeRhiBackend> backend, const RhiDeviceCreateInfo& info)
    : m_backend(std::move(backend)), m_lifetime(m_backend->Capabilities()), m_info(info)
{
    m_backend->Attach(m_lifetime);
}
NativeDevice::~NativeDevice()
{
    try
    {
        if (!m_frame && !m_faulted)
            Shutdown();
        else
            m_backend->WaitIdle();
    }
    catch (...)
    {
        // 原操作保留错误与 DRED；析构仍须完成 GPU 等待后才释放 payload。
        try
        {
            m_backend->WaitIdle();
        }
        catch (...)
        {
        }
    }
}
const RhiCapabilities& NativeDevice::Capabilities() const
{
    return m_backend->Capabilities();
}
DeviceDiagnostics NativeDevice::Diagnostics() const
{
    DeviceDiagnostics result = m_lifetime.Diagnostics();
    result.submitProfile = m_submitProfile;
    return result;
}
TextureStateSnapshot NativeDevice::QueryTextureState(const FrameToken& frame, TextureHandle texture) const
{
    m_lifetime.ValidateFrameResource(frame, texture);
    const auto state = m_resources.find(texture);
    return {m_lifetime.Describe(texture), state == m_resources.end() ? ResourceAccess::None : state->second.access,
            state != m_resources.end() && state->second.defined};
}
BufferStateSnapshot NativeDevice::QueryBufferState(const FrameToken& frame, BufferHandle buffer) const
{
    m_lifetime.ValidateFrameResource(frame, buffer);
    const auto& desc = m_lifetime.Describe(buffer);
    const auto state = m_resources.find(buffer);
    return {desc, state == m_resources.end() ? ResourceAccess::None : state->second.access,
            state != m_resources.end() && state->second.Contains(0, desc.size)};
}
[[noreturn]] void NativeDevice::Fail(const char* operation, const char* message) const
{
    throw RhiValidationError(
        {RhiErrorCode::InvalidState, operation, "Device", "", std::string(ToString(Capabilities().backend)),
         "frame=" + std::to_string(m_frame ? m_frame->serial : 0) + " pass=<device> command=0: " + message});
}
void NativeDevice::EnsureUsable(const char* operation) const
{
    if (m_shutdown || m_faulted)
        Fail(operation, "device is shut down or failed; fallback/reuse is prohibited");
}
std::uint64_t NativeDevice::Id(ResourceIdentity resource)
{
    const auto found = m_ids.find(resource);
    if (found != m_ids.end())
        return found->second;
    return m_ids.emplace(resource, ++m_nextTraceId).first->second;
}
PreparedEnvironment NativeDevice::PrepareEnvironment(TextureHandle panorama, std::string_view shaderRoot,
                                                     std::uint64_t revision)
{
    EnsureUsable("PrepareEnvironment");
    if (m_frame || shaderRoot.empty() || revision == 0)
        Fail("PrepareEnvironment", "preparation requires a frame boundary, shader root and revision");
    m_lifetime.ValidateAlive(panorama);
    const auto source = m_lifetime.Describe(panorama);
    if (source.dimension != TextureDimension::Texture2D || source.format != Format::Rgba16Float ||
        !HasFlag(source.usage, TextureUsage::Sampled) || !m_resources.at(panorama).defined)
        Fail("PrepareEnvironment", "panorama must be a completely uploaded sampled HDR texture");
    WaitIdle();
    PreparedEnvironment result;
    result.sourceRevision = revision;
    constexpr std::array<std::uint32_t, 4> sizes{512, 32, 128, 256};
    constexpr std::array<std::uint16_t, 4> mips{10, 1, 8, 1};
    constexpr std::array<const char*, 4> names{"EnvironmentCube", "IrradianceCube", "PrefilteredCube", "BrdfLut"};
    for (std::size_t i = 0; i < 4; ++i)
    {
        auto& d = result.descriptors[i];
        d.dimension = i == 3 ? TextureDimension::Texture2D : TextureDimension::TextureCube;
        d.extent = {sizes[i], sizes[i]};
        d.mipLevels = mips[i];
        d.arrayLayers = i == 3 ? 1 : 6;
        d.format = i == 3 ? Format::Rg16Float : Format::Rgba16Float;
        d.usage = TextureUsage::Sampled;
        d.debugName = names[i];
        ValidateTextureDesc(d, Capabilities());
    }
    std::array<std::unique_ptr<ResourcePayload>, 4> payloads;
    try
    {
        payloads = Native("PrepareEnvironment",
                          [&]
                          {
                              return m_backend->PrepareEnvironment(m_lifetime.Payload(panorama), shaderRoot, revision,
                                                                   result.descriptors);
                          });
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (!payloads[i])
                throw std::runtime_error("native IBL preparation returned an empty texture");
            result.textures[i] =
                Own(m_lifetime.CreateTexture(result.descriptors[i], [&] { return std::move(payloads[i]); }));
            auto& state = m_resources[result.textures[i]];
            state.access = ResourceAccess::SampledRead;
            state.defined = true;
        }
        if (m_backend->Report().warningErrors)
            throw std::runtime_error("IBL preparation emitted native diagnostics");
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        for (const auto texture : result.textures)
            if (texture)
            {
                try
                {
                    Destroy(texture);
                }
                catch (...)
                {
                }
            }
        // Report drains D3D12's info queue and D3D11's debug queue. Diagnostics are
        // best effort here: neither a failing report nor cleanup may replace the
        // first environment-preparation exception.
        try
        {
            static_cast<void>(m_backend->Report());
        }
        catch (...)
        {
        }
        std::rethrow_exception(failure);
    }
    return result;
}
void NativeDevice::ResetFrameDiagnostics(bool captureTrace)
{
    EnsureUsable("ResetFrameDiagnostics");
    if (m_commands && m_commands->CurrentState() != CommandRecording::State::Closed)
        Fail("ResetFrameDiagnostics", "cannot reset an active command stream");
    m_captureTrace = captureTrace;
    m_backend->ResetDiagnosticTrace();
    m_trace = "miniengine.native-rhi-semantic.v1\n";
    m_ids.clear();
    m_nextTraceId = 0;
    m_frameCommandCount = 0;
    m_frameResourceSets = 0;
}
void NativeDevice::Record(const CommandEvent& event)
{
    ++m_frameCommandCount;
    if ((!m_info.enableDebugLayer && !m_captureTrace) || m_trace.size() >= 4 * 1024 * 1024)
        return;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << event.operation.size() << ':' << event.operation << " r" << event.resources.size();
    for (const auto& resource : event.resources)
        out << ' ' << resource.index() << ':' << Id(resource);
    out << " i" << event.integers.size();
    for (auto value : event.integers)
        out << ' ' << value;
    out << " f" << event.scalars.size();
    for (auto value : event.scalars)
        out << ' ' << std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value);
    const auto text = event.operation == "SetPipeline"
                          ? m_lifetime.PipelineKey(std::get<GraphicsPipelineHandle>(event.resources.at(0)))
                          : event.text;
    out << " t" << text.size() << ':' << text << '\n';
    m_trace += out.str();
    if (m_trace.size() >= 4 * 1024 * 1024)
        m_trace += "trace-truncated: 4 MiB capture budget reached\n";
}
BufferHandle NativeDevice::CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initial)
{
    EnsureUsable("CreateBuffer");
    ValidateBufferInitialData(desc, initial);
    auto handle = m_lifetime.CreateBuffer(
        desc, [&] { return Native("CreateBuffer", [&] { return m_backend->CreateBuffer(desc); }); });
    try
    {
        if (!initial.empty())
        {
            m_lifetime.InitializeBuffer(
                handle, initial, [&](ResourcePayload& payload, auto offset, auto bytes, auto serial)
                { Native("InitializeBuffer", [&] { m_backend->UploadBuffer(payload, offset, bytes, serial); }); });
            m_resources[handle].Define(0, initial.size());
        }
        Own(handle);
    }
    catch (...)
    {
        m_lifetime.Destroy(handle);
        m_lifetime.Collect(m_lifetime.Completed());
        PurgeDeadStates();
        throw;
    }
    Record({"CreateBuffer", {handle}, {SemanticHash(desc), initial.size()}});
    return handle;
}
TextureHandle NativeDevice::CreateTexture(const TextureDesc& desc)
{
    EnsureUsable("CreateTexture");
    auto handle = Own(m_lifetime.CreateTexture(
        desc, [&] { return Native("CreateTexture", [&] { return m_backend->CreateTexture(desc); }); }));
    m_resources[handle] = {};
    Record({"CreateTexture", {handle}, {SemanticHash(desc)}});
    return handle;
}
SamplerHandle NativeDevice::CreateSampler(const SamplerDesc& desc)
{
    EnsureUsable("CreateSampler");
    auto handle = Own(m_lifetime.CreateSampler(
        desc, [&] { return Native("CreateSampler", [&] { return m_backend->CreateSampler(desc); }); }));
    Record({"CreateSampler", {handle}, {}, {}, std::string{}});
    return handle;
}
ShaderHandle NativeDevice::CreateShader(const ShaderDesc& desc)
{
    EnsureUsable("CreateShader");
    auto handle = Own(m_lifetime.CreateShader(
        desc, [&] { return Native("CreateShader", [&] { return m_backend->CreateShader(desc); }); }));
    Record({"CreateShader", {handle}, {static_cast<std::uint64_t>(desc.stage)}, {}, desc.semanticHash});
    return handle;
}
ResourceSetLayoutHandle NativeDevice::CreateResourceSetLayout(const ResourceSetLayoutDesc& desc)
{
    EnsureUsable("CreateResourceSetLayout");
    auto handle = Own(m_lifetime.CreateResourceSetLayout(
        desc,
        [&] { return Native("CreateResourceSetLayout", [&] { return m_backend->CreateResourceSetLayout(desc); }); }));
    Record({"CreateResourceSetLayout", {handle}, {}, {}, std::string{}});
    return handle;
}
ResourceSetHandle NativeDevice::CreateResourceSet(const ResourceSetDesc& desc)
{
    EnsureUsable("CreateResourceSet");
    auto handle = Own(m_lifetime.CreateResourceSet(
        desc, [&] { return Native("CreateResourceSet", [&] { return m_backend->CreateResourceSet(desc); }); }));
    Record({"CreateResourceSet", {handle}, {}, {}, std::string{}});
    return handle;
}
ResourceSetHandle NativeDevice::CreateFrameResourceSet(const FrameToken& frame, const ResourceSetDesc& desc)
{
    EnsureUsable("CreateFrameResourceSet");
    auto handle = m_lifetime.CreateFrameResourceSet(
        frame, desc,
        [&] { return Native("CreateFrameResourceSet", [&] { return m_backend->CreateResourceSet(desc); }); });
    ++m_frameResourceSets;
    Record({"CreateFrameResourceSet", {handle}, {frame.serial}});
    return handle;
}
PipelineLayoutHandle NativeDevice::CreatePipelineLayout(const PipelineLayoutDesc& desc)
{
    EnsureUsable("CreatePipelineLayout");
    auto handle = Own(m_lifetime.CreatePipelineLayout(
        desc, [&] { return Native("CreatePipelineLayout", [&] { return m_backend->CreatePipelineLayout(desc); }); }));
    Record({"CreatePipelineLayout", {handle}, {}, {}, std::string{}});
    return handle;
}
GraphicsPipelineHandle NativeDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    EnsureUsable("CreateGraphicsPipeline");
    auto handle = Own(m_lifetime.CreateGraphicsPipeline(
        desc,
        [&] { return Native("CreateGraphicsPipeline", [&] { return m_backend->CreateGraphicsPipeline(desc); }); }));
    Record({"CreateGraphicsPipeline", {handle}, {}, {}, m_lifetime.PipelineKey(handle)});
    return handle;
}
TimestampQueryHandle NativeDevice::CreateTimestampQuery(std::string_view name)
{
    EnsureUsable("CreateTimestampQuery");
    auto handle = Own(m_lifetime.CreateTimestampQuery(
        name, [&] { return Native("CreateTimestampQuery", [&] { return m_backend->CreateTimestampQuery(name); }); }));
    Record({"CreateTimestampQuery", {handle}});
    return handle;
}
SwapChainHandle NativeDevice::CreateSwapChain(const SwapChainDesc& requested)
{
    EnsureUsable("CreateSwapChain");
    if (m_frame || !m_chains.empty())
        Fail("CreateSwapChain", "M6 supports one swap chain at a frame boundary");
    auto desc = requested;
    if (!desc.nativeWindow)
        desc.nativeWindow = m_info.nativeWindow;
    if (!desc.nativeWindow)
        Fail("CreateSwapChain", "a live composition-owned window is required");
    if (desc.extent.width && desc.extent.height)
        ValidateTextureDesc({TextureDimension::Texture2D, desc.extent, 1, 1, 1, desc.format,
                             TextureUsage::ColorAttachment | TextureUsage::CopySource, desc.debugName},
                            Capabilities());
    WaitIdle();
    auto handle = m_lifetime.CreateSwapChain(
        desc, [&] { return Native("CreateSwapChain", [&] { return m_backend->CreateSwapChain(desc); }); });
    try
    {
        m_lifetime.ResizeBackBuffers(
            handle, desc.extent,
            [&]
            {
                return Native("AcquireBackBuffers",
                              [&] { return m_backend->ResizeBackBuffers(m_lifetime.Payload(handle), desc); });
            });
        Own(handle);
        m_chains.emplace(handle, desc);
        TrackBackBuffers(handle);
    }
    catch (...)
    {
        m_lifetime.Destroy(handle);
        m_lifetime.Collect(m_lifetime.Completed());
        throw;
    }
    Record({"CreateSwapChain", {handle}, {desc.extent.width, desc.extent.height, desc.bufferCount, desc.vsync}});
    return handle;
}
void NativeDevice::TrackBackBuffers(SwapChainHandle chain)
{
    for (const auto buffer : m_lifetime.BackBuffers(chain))
    {
        m_resources[buffer] = {};
        // DXGI 的初始逻辑访问是 Present，但初次 acquire 的内容未定义。
        m_resources[buffer].access = ResourceAccess::Present;
    }
}
void NativeDevice::Retire(ResourceIdentity handle)
{
    EnsureUsable("Destroy");
    if (m_commands && m_commands->HasActiveAttachment(handle))
        Fail("Destroy", "attachment is retained by active rendering; EndRendering first");
    if (handle.index() == kSwapChainIdentityIndex)
        WaitIdle();
    m_lifetime.Destroy(handle);
    m_owned.erase(handle);
    Record({"Destroy", {handle}});
    m_resources.erase(handle);
}
void NativeDevice::Destroy(BufferHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(TextureHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(SamplerHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(ShaderHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(ResourceSetLayoutHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(ResourceSetHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(PipelineLayoutHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(GraphicsPipelineHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(TimestampQueryHandle handle)
{
    Retire(handle);
}
void NativeDevice::Destroy(SwapChainHandle handle)
{
    Retire(handle);
    m_chains.erase(handle);
    PurgeDeadStates();
}
void NativeDevice::PurgeDeadStates()
{
    for (auto it = m_ids.begin(); it != m_ids.end();)
    {
        try
        {
            m_lifetime.ValidateAlive(it->first);
            ++it;
        }
        catch (const RhiException&)
        {
            it = m_ids.erase(it);
        }
    }
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
void NativeDevice::Poll()
{
    const auto completed = Native("PollCompleted", [&] { return m_backend->PollCompleted(); });
    m_lifetime.Collect(completed);
    PurgeDeadStates();
}
void NativeDevice::WaitFor(std::uint64_t required)
{
    Poll();
    if (required <= m_lifetime.Completed())
        return;
    const auto completed = Native("WaitFor", [&] { return m_backend->WaitFor(required); });
    if (completed < required)
        Fail("WaitFor", "backend did not prove the required serial completed");
    m_lifetime.Collect(completed);
    PurgeDeadStates();
}
FrameToken NativeDevice::BeginFrame(SwapChainHandle chain)
{
    EnsureUsable("BeginFrame");
    m_lifetime.ValidateAlive(chain);
    if (m_frame)
        Fail("BeginFrame", "another frame is active");
    const auto& desc = m_chains.at(chain);
    if (!desc.extent.width || !desc.extent.height)
        return {};
    const auto index = Native("Acquire", [&] { return m_backend->CurrentBackBufferIndex(m_lifetime.Payload(chain)); });
    WaitFor(m_lifetime.RequiredFrameCompletion(chain, index));
    auto frame = m_lifetime.BeginFrame(chain, index);
    m_frame = frame;
    m_chain = chain;
    // 两个原生 adapter 都使用 FLIP_DISCARD，Present 后不得承诺保留上一帧内容。
    m_resources.at(frame.backBuffer).defined = false;
    m_commands.reset();
    m_frameImports.clear();
    try
    {
        Native("BeginFrame", [&] { m_backend->BeginFrame(frame, m_lifetime.Payload(chain)); });
    }
    catch (...)
    {
        m_faulted = true;
        throw;
    }
    Record({"BeginFrame", {chain, frame.backBuffer}, {frame.serial, frame.recycleLane}});
    return frame;
}
TextureHandle NativeDevice::AcquireBackBuffer(const FrameToken& frame)
{
    EnsureUsable("AcquireBackBuffer");
    m_lifetime.ValidateFrameToken(frame);
    return frame.backBuffer;
}
IRhiCommandList& NativeDevice::BeginGraphics(const FrameToken& frame)
{
    EnsureUsable("BeginGraphics");
    m_lifetime.ValidateFrameToken(frame);
    if (m_commands)
        Fail("BeginGraphics", "this frame already opened graphics");
    m_commands = std::make_unique<CommandRecording>(
        m_lifetime, frame, m_resources,
        [this](const CommandEvent& event)
        {
            try
            {
                if (event.operation != "ImportResources")
                {
                    // M7-SUBMIT-001：只加一次计时与分桶（TSC 计时，~20-30 ns/读），
                    // 不改变任何调用顺序或状态。
                    const std::uint64_t consumeStart = SubmitTimer().Ticks();
                    Native(event.operation.c_str(), [&] { m_backend->Consume(event); });
                    const std::uint64_t consumeMicros = SubmitTimer().MicrosSince(consumeStart);
                    DeviceDiagnostics::SubmitSegment* bucket = &m_submitProfile.nativeOther;
                    if (event.operation == "Draw" || event.operation == "DrawIndexed")
                        bucket = &m_submitProfile.nativeDraw;
                    else if (event.operation == "BeginRendering" || event.operation == "EndRendering" ||
                             event.operation == "BeginLabel" || event.operation == "EndLabel")
                        bucket = &m_submitProfile.nativeRendering;
                    else if (event.operation == "ApplyTransitions" || event.operation == "ResetTransientContents")
                        bucket = &m_submitProfile.nativeBarrier;
                    else if (event.operation == "SetPipeline" || event.operation == "SetViewport" ||
                             event.operation == "SetScissor" || event.operation == "BindVertexBuffer" ||
                             event.operation == "BindIndexBuffer" || event.operation == "BindResourceSet")
                        bucket = &m_submitProfile.nativeBind;
                    bucket->micros += consumeMicros;
                    ++bucket->events;
                    ++m_submitProfile.recordedEvents;
                }
            }
            catch (...)
            {
                m_faulted = true;
                throw;
            }
            Record(event);
        },
        [this](ResourceIdentity resource)
        {
            if (!m_frameImports.contains(resource))
                Fail("GraphImport", "resource was not imported by this frame's graph executor");
        },
        [this](std::span<const ResourceIdentity> imports)
        {
            auto candidate = m_frameImports;
            candidate.insert(imports.begin(), imports.end());
            m_frameImports.swap(candidate);
        });
    Record({"BeginGraphics", {}, {frame.serial}});
    return *m_commands;
}
IRhiGraphCommandSink& NativeDevice::GraphCommandSink(const FrameToken& frame)
{
    EnsureUsable("GraphCommandSink");
    m_lifetime.ValidateFrameToken(frame);
    if (!m_commands || m_commands->CurrentState() == CommandRecording::State::Closed)
        Fail("GraphCommandSink", "graphics is not recording");
    return *m_commands;
}
void NativeDevice::EndGraphics(const FrameToken& frame, IRhiCommandList& commands)
{
    EnsureUsable("EndGraphics");
    m_lifetime.ValidateFrameToken(frame);
    if (!m_commands || m_commands.get() != &commands)
        Fail("EndGraphics", "foreign command list");
    const auto state = m_resources.find(frame.backBuffer);
    if (state == m_resources.end() || state->second.access != ResourceAccess::Present || !state->second.defined ||
        state->second.declaredFrame != frame.serial)
        Fail("EndGraphics", "declare a defined Present backbuffer before closing");
    m_commands->Close();
}
void NativeDevice::EndFrame(const FrameToken& frame, SwapChainHandle chain)
{
    EnsureUsable("EndFrame");
    m_lifetime.ValidateFrameToken(frame);
    if (chain != m_chain || !m_commands || m_commands->CurrentState() != CommandRecording::State::Closed)
        Fail("EndFrame", "matching chain and already closed command list are required");
    try
    {
        // M7-SUBMIT-001：队列提交（ExecuteCommandLists + fence）单列一段（TSC 计时）。
        {
            const std::uint64_t submitStart = SubmitTimer().Ticks();
            Native("Submit", [&] { m_backend->Submit(frame); });
            m_submitProfile.submit.micros += SubmitTimer().MicrosSince(submitStart);
            ++m_submitProfile.submit.events;
        }
        m_lifetime.EndFrame(frame, chain);
        // 发布提交后才 Present：呈现失败仍保留已提交资源的退休依据。
        m_frame.reset();
        m_chain = {};
        Native("Present", [&] { m_backend->Present(m_lifetime.Payload(chain)); });
        Record({"EndFrame", {chain}, {frame.serial}});
        PurgeDeadStates();
        const auto report = Native("Diagnostics", [&] { return m_backend->Report(); });
        if (report.warningErrors)
            throw RhiException({RhiErrorCode::BackendFailure, "EndFrame", "Diagnostics", "",
                                std::string(ToString(Capabilities().backend)),
                                "native warning/error detected:\n" + report.trace});
    }
    catch (...)
    {
        m_faulted = true;
        throw;
    }
}
DynamicBufferSlice NativeDevice::WriteDynamicBuffer(const FrameToken& frame, std::span<const std::byte> bytes,
                                                    std::uint32_t alignment)
{
    EnsureUsable("WriteDynamicBuffer");
    auto slice = m_lifetime.WriteDynamicBuffer(
        frame, bytes, alignment, [&](const BufferDesc& desc, auto input)
        { return Native("WriteDynamicBuffer", [&] { return m_backend->CreateDynamicBuffer(desc, input); }); });
    m_resources[slice.view.buffer].Define(0, bytes.size());
    Record({"WriteDynamicBuffer", {slice.view.buffer}, {bytes.size(), alignment}});
    return slice;
}
void NativeDevice::UploadBuffer(BufferHandle destination, std::uint64_t offset, std::span<const std::byte> bytes)
{
    EnsureUsable("UploadBuffer");
    Poll();
    m_lifetime.UploadBuffer(
        destination, offset, bytes, [&](ResourcePayload& payload, auto start, auto data, auto serial)
        { Native("UploadBuffer", [&] { m_backend->UploadBuffer(payload, start, data, serial); }); });
    m_resources[destination].Define(offset, bytes.size());
    Record({"UploadBuffer", {destination}, {offset, bytes.size()}});
}
void NativeDevice::UploadTexture(TextureHandle destination, std::span<const TextureSubresourceData> subresources)
{
    EnsureUsable("UploadTexture");
    Poll();
    m_lifetime.UploadTexture(destination, subresources, [&](ResourcePayload& payload, auto data, auto serial)
                             { Native("UploadTexture", [&] { m_backend->UploadTexture(payload, data, serial); }); });
    m_resources[destination].defined = true;
    Record({"UploadTexture", {destination}, {subresources.size()}});
}
std::optional<TimestampResult> NativeDevice::TryReadTimestamp(TimestampQueryHandle query)
{
    EnsureUsable("TryReadTimestamp");
    m_lifetime.ValidateAlive(query);
    Poll();
    return Native("TryReadTimestamp", [&] { return m_backend->TryReadTimestamp(m_lifetime.Payload(query)); });
}
std::optional<TextureReadbackResult> NativeDevice::TryReadTextureReadback(BufferHandle buffer)
{
    EnsureUsable("TryReadTextureReadback");
    if (m_lifetime.Describe(buffer).memory != MemoryDomain::GpuToCpu)
        Fail("TryReadTextureReadback", "buffer is not readback memory");
    Poll();
    return Native("TryReadTextureReadback",
                  [&] { return m_backend->TryReadTextureReadback(m_lifetime.Payload(buffer)); });
}
void NativeDevice::ResizeSwapChain(SwapChainHandle chain, Extent2D extent)
{
    EnsureUsable("ResizeSwapChain");
    m_lifetime.ValidateAlive(chain);
    if (m_frame)
        Fail("ResizeSwapChain", "resize requires a frame boundary");
    auto& desc = m_chains.at(chain);
    if (!extent.width || !extent.height)
    {
        m_lifetime.ResizeBackBuffers(chain, extent, {});
        desc.extent = extent;
        Record({"ResizeSwapChain", {chain}, {extent.width, extent.height}});
        return;
    }
    ValidateTextureDesc({TextureDimension::Texture2D, extent, 1, 1, 1, desc.format,
                         TextureUsage::ColorAttachment | TextureUsage::CopySource, desc.debugName},
                        Capabilities());
    WaitIdle();
    auto candidate = desc;
    candidate.extent = extent;
    try
    {
        m_lifetime.ResizeBackBuffers(
            chain, extent,
            [&]
            {
                return Native("ResizeSwapChain",
                              [&] { return m_backend->ResizeBackBuffers(m_lifetime.Payload(chain), candidate); });
            });
        desc = candidate;
        PurgeDeadStates();
        TrackBackBuffers(chain);
    }
    catch (...)
    {
        desc.extent = {};
        PurgeDeadStates();
        throw;
    }
    Record({"ResizeSwapChain", {chain}, {extent.width, extent.height}});
}
void NativeDevice::WaitIdle()
{
    EnsureUsable("WaitIdle");
    if (m_frame)
        Fail("WaitIdle", "cannot flush an active recording");
    Native("WaitIdle", [&] { m_backend->WaitIdle(); });
    Poll();
    if (m_lifetime.Completed() < m_lifetime.LastSubmitted())
        Fail("WaitIdle", "native completion did not reach the last submitted serial");
}
NativeBackendReport NativeDevice::NativeReport(bool census)
{
    return Native("Report", [&] { return m_backend->Report(census); });
}
void NativeDevice::Shutdown()
{
    if (m_shutdown)
        return;
    EnsureUsable("Shutdown");
    WaitIdle();
    std::vector<std::pair<std::uint64_t, ResourceIdentity>> reverse;
    for (const auto& [resource, sequence] : m_owned)
        reverse.emplace_back(sequence, resource);
    std::sort(reverse.rbegin(), reverse.rend());
    for (const auto& [sequence, resource] : reverse)
    {
        (void)sequence;
        m_lifetime.Destroy(resource);
        m_owned.erase(resource);
    }
    m_lifetime.Collect(m_lifetime.Completed());
    m_commands.reset();
    m_resources.clear();
    m_chains.clear();
    m_lifetime.CheckShutdown();
    m_shutdown = true;
}
} // namespace MiniEngine::Rhi
