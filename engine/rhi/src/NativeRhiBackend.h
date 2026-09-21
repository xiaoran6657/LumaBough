#pragma once

#include "CommandRecording.h"
#include <MiniEngine/Rhi/RhiFactory.h>
#include <stdexcept>
#include <string_view>

namespace MiniEngine::Rhi
{
// 只在 factory/adapter 内组合；没有 native 类型，也不向 pass 暴露 payload。
// DeviceLifetime 是唯一公开句柄 owner，payload 析构发生在真实完成后。
struct NativeBackendReport final
{
    std::uint64_t warningErrors = 0;
    std::uint64_t liveResources = 0;
    std::uint64_t submittedBatches = 0;
    std::uint64_t completedSerial = 0;
    std::uint64_t explicitUnbinds = 0;
    std::uint64_t barriers = 0;
    std::uint64_t discardNoOps = 0;
    // 稳定性诊断：活跃 descriptor range/绑定槽计数（D3D11 记实际绑定的 view/常量槽）。
    std::uint64_t descriptorRanges = 0;
    // 稳定性诊断：当前占用的上传/动态缓冲字节（D3D11 记本帧动态 buffer 字节）。
    std::uint64_t uploadBytes = 0;
    // 负向通道注入计数；正常 sample/pass 恒为 0，用于证明注入真实生效。
    std::uint64_t injectedFaults = 0;
    std::string trace;
};
class NativeRhiBackend
{
  public:
    virtual ~NativeRhiBackend() = default;
    virtual const RhiCapabilities& Capabilities() const = 0;
    // Attach 在任何资源创建前执行一次；backend 不拥有 owner。
    virtual void Attach(DeviceLifetime& owner) = 0;
    virtual std::array<std::unique_ptr<ResourcePayload>, 4> PrepareEnvironment(
        ResourcePayload& panorama, std::string_view shaderRoot, std::uint64_t revision,
        const std::array<TextureDesc, 4>& descriptors)
    {
        (void)panorama;
        (void)shaderRoot;
        (void)revision;
        (void)descriptors;
        throw std::runtime_error("native environment preparation is unavailable");
    }
    virtual std::unique_ptr<ResourcePayload> CreateBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateTexture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateShader(const ShaderDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateResourceSetLayout(const ResourceSetLayoutDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateResourceSet(const ResourceSetDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreatePipelineLayout(const PipelineLayoutDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateTimestampQuery(std::string_view name) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateSwapChain(const SwapChainDesc& desc) = 0;
    // 初次调用取得真实 buffers；resize 调用前公共 owner 已释放所有旧 buffer payload。
    virtual std::vector<DeviceLifetime::BackBufferCandidate> ResizeBackBuffers(ResourcePayload& chain,
                                                                               const SwapChainDesc& desc) = 0;
    virtual std::uint32_t CurrentBackBufferIndex(ResourcePayload& chain) = 0;
    virtual void BeginFrame(const FrameToken& frame, ResourcePayload& chain) = 0;
    // CommandEvent 已经完整验证；EndGraphics 事件只 Close，Submit 绝不再次 Close。
    virtual void Consume(const CommandEvent& event) = 0;
    virtual void Submit(const FrameToken& frame) = 0;
    // Submit 返回后公共 owner 先发布提交，再调用 Present；失败不得触发 fallback。
    virtual void Present(ResourcePayload& chain) = 0;
    virtual std::unique_ptr<ResourcePayload> CreateDynamicBuffer(const BufferDesc& desc,
                                                                 std::span<const std::byte> bytes) = 0;
    // 回调成功返回代表该 serial 已提交；输入须复制，staging 保留至实际完成。
    virtual void UploadBuffer(ResourcePayload& destination, std::uint64_t offset, std::span<const std::byte> bytes,
                              std::uint64_t serial) = 0;
    virtual void UploadTexture(ResourcePayload& destination, std::span<const TextureSubresourceData> data,
                               std::uint64_t serial) = 0;
    virtual std::uint64_t PollCompleted() = 0;
    virtual std::uint64_t WaitFor(std::uint64_t serial) = 0;
    // 含 fatal 路径已经提交但未能发布到 owner 的工作；析构释放资源前调用。
    virtual void WaitIdle() = 0;
    virtual std::optional<TimestampResult> TryReadTimestamp(ResourcePayload& query) = 0;
    virtual std::optional<TextureReadbackResult> TryReadTextureReadback(ResourcePayload& buffer) = 0;
    // 正常诊断不得吞 warning/error；census=true 时独立检查 native 资源，不把 device 本身计入资源泄漏。
    // 仅重置有界单帧文本，累计 warning/error 与资源计数不得清零。
    virtual void ResetDiagnosticTrace()
    {
    }
    // 测试专用负向通道：一次性注入一个 native lowering 缺陷（例如跳过 transition/unbind）。
    // 只在负向测试进程内调用，正常 sample/pass 不使用；未知名称必须失败，避免静默无效注入。
    virtual void InjectBackendFaultForTesting(std::string_view fault)
    {
        (void)fault;
        throw std::runtime_error("backend fault injection is unsupported");
    }
    virtual NativeBackendReport Report(bool census = false) = 0;
};
} // namespace MiniEngine::Rhi
