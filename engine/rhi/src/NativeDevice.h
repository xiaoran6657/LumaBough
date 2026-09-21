#pragma once
#include "NativeRhiBackend.h"
#include <map>
#include <set>

namespace MiniEngine::Rhi
{
// 生产 adapter 的 CPU 组合层。所有 API 特定对象/命令都委托 NativeRhiBackend。
// command 引用只借用至本帧结束；测试替身可保留历史，但生产层不累积每帧代理。
class NativeDevice final : public IRhiDevice
{
  public:
    NativeDevice(std::unique_ptr<NativeRhiBackend> backend, const RhiDeviceCreateInfo& info);
    ~NativeDevice() override;
    const RhiCapabilities& Capabilities() const override;
    DeviceDiagnostics Diagnostics() const override;
    TextureStateSnapshot QueryTextureState(const FrameToken& frame, TextureHandle texture) const override;
    BufferStateSnapshot QueryBufferState(const FrameToken& frame, BufferHandle buffer) const override;
    BufferHandle CreateBuffer(const BufferDesc&, std::span<const std::byte>) override;
    TextureHandle CreateTexture(const TextureDesc&) override;
    SamplerHandle CreateSampler(const SamplerDesc&) override;
    ShaderHandle CreateShader(const ShaderDesc&) override;
    ResourceSetLayoutHandle CreateResourceSetLayout(const ResourceSetLayoutDesc&) override;
    ResourceSetHandle CreateResourceSet(const ResourceSetDesc&) override;
    ResourceSetHandle CreateFrameResourceSet(const FrameToken&, const ResourceSetDesc&) override;
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc&) override;
    GraphicsPipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override;
    TimestampQueryHandle CreateTimestampQuery(std::string_view) override;
    SwapChainHandle CreateSwapChain(const SwapChainDesc&) override;
    void Destroy(BufferHandle) override;
    void Destroy(TextureHandle) override;
    void Destroy(SamplerHandle) override;
    void Destroy(ShaderHandle) override;
    void Destroy(ResourceSetLayoutHandle) override;
    void Destroy(ResourceSetHandle) override;
    void Destroy(PipelineLayoutHandle) override;
    void Destroy(GraphicsPipelineHandle) override;
    void Destroy(TimestampQueryHandle) override;
    void Destroy(SwapChainHandle) override;
    FrameToken BeginFrame(SwapChainHandle) override;
    TextureHandle AcquireBackBuffer(const FrameToken&) override;
    IRhiCommandList& BeginGraphics(const FrameToken&) override;
    IRhiGraphCommandSink& GraphCommandSink(const FrameToken&) override;
    void EndGraphics(const FrameToken&, IRhiCommandList&) override;
    void EndFrame(const FrameToken&, SwapChainHandle) override;
    DynamicBufferSlice WriteDynamicBuffer(const FrameToken&, std::span<const std::byte>, std::uint32_t) override;
    void UploadBuffer(BufferHandle, std::uint64_t, std::span<const std::byte>) override;
    void UploadTexture(TextureHandle, std::span<const TextureSubresourceData>) override;
    std::optional<TimestampResult> TryReadTimestamp(TimestampQueryHandle) override;
    std::optional<TextureReadbackResult> TryReadTextureReadback(BufferHandle) override;
    void ResizeSwapChain(SwapChainHandle, Extent2D) override;
    void WaitIdle() override;
    // 仅内部诊断/测试入口；工厂对外只返回 IRhiDevice。
    NativeBackendReport NativeReport(bool census = false);
    // 仅负向通道测试入口：通过内部 backend 接口注入一次性 native 缺陷，不进入公共 RHI 契约。
    void InjectBackendFaultForTesting(std::string_view fault)
    {
        m_backend->InjectBackendFaultForTesting(fault);
    }
    void Shutdown();
    const std::string& SemanticTrace() const
    {
        return m_trace;
    }
    // composition 在帧录制外重置诊断窗口，避免长运行截断目标帧；不改变 GPU 状态。
    void ResetFrameDiagnostics(bool captureTrace = false);
    PreparedEnvironment PrepareEnvironment(TextureHandle panorama, std::string_view shaderRoot, std::uint64_t revision);
    std::uint64_t FrameCommandCount() const
    {
        return m_frameCommandCount;
    }
    // 距上次 ResetFrameDiagnostics 的 frame ResourceSet 创建数；稳定性诊断使用。
    std::uint64_t FrameResourceSetCount() const
    {
        return m_frameResourceSets;
    }
    DeviceLifetime& Lifetime()
    {
        return m_lifetime;
    }

  private:
    template <class Function> decltype(auto) Native(const char* operation, Function&& function)
    {
        try
        {
            return std::forward<Function>(function)();
        }
        catch (const RhiException&)
        {
            throw;
        }
        catch (const std::bad_alloc&)
        {
            throw RhiException({RhiErrorCode::OutOfMemory, operation, "Device", "",
                                std::string(ToString(Capabilities().backend)), "native allocation failed"});
        }
        catch (const std::exception& error)
        {
            throw RhiException({RhiErrorCode::BackendFailure, operation, "Device", "",
                                std::string(ToString(Capabilities().backend)), error.what()});
        }
    }
    template <class Handle> Handle Own(Handle handle)
    {
        m_owned.emplace(handle, ++m_sequence);
        return handle;
    }
    void EnsureUsable(const char* operation) const;
    void Retire(ResourceIdentity);
    void Poll();
    void WaitFor(std::uint64_t);
    void PurgeDeadStates();
    void TrackBackBuffers(SwapChainHandle);
    void Record(const CommandEvent&);
    std::uint64_t Id(ResourceIdentity);
    [[noreturn]] void Fail(const char* operation, const char* message) const;
    // backend 先构造最后析构：payload 的 descriptor/tracker 析构仍可访问其 owner。
    std::unique_ptr<NativeRhiBackend> m_backend;
    DeviceLifetime m_lifetime;
    const RhiDeviceCreateInfo m_info;
    CommandResourceStates m_resources;
    std::map<ResourceIdentity, std::uint64_t> m_owned;
    std::uint64_t m_sequence = 0;
    std::map<SwapChainHandle, SwapChainDesc> m_chains;
    std::set<ResourceIdentity> m_frameImports;
    std::unique_ptr<CommandRecording> m_commands;
    std::optional<FrameToken> m_frame;
    SwapChainHandle m_chain;
    bool m_shutdown = false;
    bool m_faulted = false;
    // 仅显式诊断模式保留 trace，普通运行不累积历史身份/文本。
    std::map<ResourceIdentity, std::uint64_t> m_ids;
    std::uint64_t m_nextTraceId = 0;
    std::uint64_t m_frameCommandCount = 0;
    std::uint64_t m_frameResourceSets = 0;
    // M7-SUBMIT-001：提交路径段级计量（累计值，逐帧差分由调用方做）。只计数不改行为。
    DeviceDiagnostics::SubmitProfile m_submitProfile{};
    bool m_captureTrace = false;
    std::string m_trace = "miniengine.native-rhi-semantic.v1\n";
};
} // namespace MiniEngine::Rhi
