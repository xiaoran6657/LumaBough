#pragma once
#include "CommandRecording.h"
#include <MiniEngine/Rhi/RhiResults.h>
#include <functional>
#include <map>
#include <memory>

namespace MiniEngine::Tests
{
using namespace Rhi;
// 测试替身没有 GPU；Wait 的完成号由测试 source 明确提供。
struct TraceCompletion final
{
    std::uint64_t completed = 0;
    std::vector<std::uint64_t> waits;
    std::function<std::uint64_t(std::uint64_t)> onWait;
    std::uint64_t Wait(std::uint64_t required);
};
// 每帧 import 的测试契约，不是 Render Graph 编译器。
struct TraceImportHandle final
{
    std::uint64_t owner = 0;
    std::size_t index = 0;
};
class TraceRhiDevice final : public IRhiDevice
{
  public:
    explicit TraceRhiDevice(RhiBackend backend = RhiBackend::D3D12);
    const RhiCapabilities& Capabilities() const override;
    DeviceDiagnostics Diagnostics() const override;
    TextureStateSnapshot QueryTextureState(const FrameToken& frame, TextureHandle texture) const override;
    BufferStateSnapshot QueryBufferState(const FrameToken& frame, BufferHandle buffer) const override;
    BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    ShaderHandle CreateShader(const ShaderDesc& desc) override;
    ResourceSetLayoutHandle CreateResourceSetLayout(const ResourceSetLayoutDesc& desc) override;
    ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc) override;
    ResourceSetHandle CreateFrameResourceSet(const FrameToken& frame, const ResourceSetDesc& desc) override;
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override;
    GraphicsPipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    TimestampQueryHandle CreateTimestampQuery(std::string_view debugName) override;
    SwapChainHandle CreateSwapChain(const SwapChainDesc& desc) override;
    void Destroy(BufferHandle handle) override;
    void Destroy(TextureHandle handle) override;
    void Destroy(SamplerHandle handle) override;
    void Destroy(ShaderHandle handle) override;
    void Destroy(ResourceSetLayoutHandle handle) override;
    void Destroy(ResourceSetHandle handle) override;
    void Destroy(PipelineLayoutHandle handle) override;
    void Destroy(GraphicsPipelineHandle handle) override;
    void Destroy(TimestampQueryHandle handle) override;
    void Destroy(SwapChainHandle handle) override;
    FrameToken BeginFrame(SwapChainHandle swapChain) override;
    TextureHandle AcquireBackBuffer(const FrameToken& frame) override;
    IRhiCommandList& BeginGraphics(const FrameToken& frame) override;
    IRhiGraphCommandSink& GraphCommandSink(const FrameToken& frame) override;
    void EndGraphics(const FrameToken& frame, IRhiCommandList& commands) override;
    void EndFrame(const FrameToken& frame, SwapChainHandle swapChain) override;
    DynamicBufferSlice WriteDynamicBuffer(const FrameToken& frame, std::span<const std::byte> bytes,
                                          std::uint32_t alignment) override;
    void UploadBuffer(BufferHandle destination, std::uint64_t destinationOffset,
                      std::span<const std::byte> bytes) override;
    void UploadTexture(TextureHandle destination, std::span<const TextureSubresourceData> subresources) override;
    std::optional<TimestampResult> TryReadTimestamp(TimestampQueryHandle query) override;
    std::optional<TextureReadbackResult> TryReadTextureReadback(BufferHandle readback) override;
    void ResizeSwapChain(SwapChainHandle swapChain, Extent2D extent) override;
    void WaitIdle() override;
    void CompleteThrough(std::uint64_t serial);
    void FailNextBackBufferCreation()
    {
        m_failNextBackBuffers = true;
    }
    void Shutdown();
    TraceCompletion& Completion()
    {
        return m_completion;
    }
    std::size_t WaitIdleCount() const
    {
        return m_waitIdleCount;
    }
    std::size_t ConsumedCommands() const
    {
        return m_consumed;
    }
    const std::vector<std::string>& Events() const
    {
        return m_events;
    }
    std::string CanonicalTrace() const;
    std::uint64_t StableHash() const;
    DeviceLifetime& Lifetime()
    {
        return m_lifetime;
    }
    const CommandResourceStates& ResourceStates() const
    {
        return m_resources;
    }
    TraceImportHandle Import(const FrameToken& frame, ResourceIdentity resource);
    ResourceIdentity Resolve(TraceImportHandle handle);

  private:
    struct ImportRecord
    {
        FrameToken frame;
        ResourceIdentity resource;
    };
    std::vector<DeviceLifetime::BackBufferCandidate> BuildBackBuffers(const SwapChainDesc& desc, Extent2D extent);
    void OnCommand(const CommandEvent& event);
    void Record(const CommandEvent& event);
    void Retire(ResourceIdentity handle);
    void PurgeDeadStates();
    void WaitFor(std::uint64_t required);
    std::uint64_t Id(ResourceIdentity handle);
    template <class Handle> Handle Own(Handle handle)
    {
        m_owned.emplace_back(handle);
        (void)Id(handle);
        return handle;
    }
    [[noreturn]] void Fail(const char* operation, const char* message) const;
    RhiCapabilities m_capabilities;
    DeviceLifetime m_lifetime;
    CommandResourceStates m_resources;
    TraceCompletion m_completion;
    std::vector<ResourceIdentity> m_owned;
    std::map<ResourceIdentity, std::uint64_t> m_ids;
    std::map<SwapChainHandle, SwapChainDesc> m_chains;
    std::vector<std::string> m_events;
    // Trace 审计保留已关闭代理和事件，旧引用可报告错误；这不是生产帧池。
    std::vector<std::unique_ptr<CommandRecording>> m_recordings;
    CommandRecording* m_commands = nullptr;
    std::optional<FrameToken> m_frame;
    SwapChainHandle m_chain;
    std::size_t m_pending = 0;
    std::size_t m_consumed = 0;
    std::size_t m_waitIdleCount = 0;
    bool m_failNextBackBuffers = false;
    std::map<TimestampQueryHandle, TimestampResult> m_queries;
    std::map<BufferHandle, std::pair<std::uint64_t, TextureReadbackResult>> m_readbacks;
    const std::uint64_t m_importOwner;
    std::vector<ImportRecord> m_imports;
    std::set<ResourceIdentity> m_frameImports;
};
} // namespace MiniEngine::Tests
