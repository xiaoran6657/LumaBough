#pragma once

// M6-03 设备资源与生命周期契约；完整 graphics/adapter 接线见 M6-05/06。
// 所有调用限渲染线程；句柄不拥有对象，span/string_view 仅在调用期间借用。

#include <MiniEngine/Rhi/IRhiCommandList.h>
#include <MiniEngine/Rhi/IRhiGraphCommandSink.h>
#include <MiniEngine/Rhi/RhiCapabilities.h>
#include <MiniEngine/Rhi/RhiError.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace MiniEngine::Rhi
{
struct FrameToken final
{
    std::uint64_t serial = 0;
    std::uint64_t owner = 0; // device 签发身份，防止另一个设备的同 serial token 误用。
    std::uint32_t recycleLane = 0;
    TextureHandle backBuffer;
};

struct SwapChainDesc final
{
    Extent2D extent;
    Format format = Format::Rgba8Unorm;
    std::uint8_t bufferCount = 3;
    bool vsync = true;
    std::string debugName;
    // 仅 composition 注入，调用者持有窗口；不返回图形 API 对象。
    void* nativeWindow = nullptr;
};

// 引擎对象统计，不包括 native device/context 自身；shutdown 必须 alive/retiring=0。
struct DeviceDiagnostics final
{
    std::size_t aliveObjects = 0;
    std::size_t retiringObjects = 0;
    std::size_t registrySlots = 0;
    std::size_t exhaustedSlots = 0;
    std::uint64_t lastSubmittedSerial = 0;
    std::uint64_t completedSerial = 0;
    std::uint64_t activeFrameSerial = 0;

    // 提交路径段级计量（M7-SUBMIT-001）。**只累计计数，不改变任何行为**：
    //   native*  = 后端 Consume 逐事件的原生调用耗时（按操作类别分桶）；
    //   submit   = EndFrame 内的队列提交（ExecuteCommandLists + fence signal）；
    //   recorded = 进入命令记录层的有效事件数。
    // 口径：native 发生在命令录制期（录制层 sink 同步调用后端），因此 native 是
    // rhiSubmitMs 的子集；"非原生部分" = rhiSubmitMs − native（图记账 + 记录层校验/事件构造）。
    // 全部为累计值，调用方（runner）用逐帧差分得到每帧段耗时。
    struct SubmitSegment final
    {
        std::uint64_t micros = 0;
        std::uint64_t events = 0;
    };
    struct SubmitProfile final
    {
        SubmitSegment nativeRendering; // Begin/EndRendering、label 作用域
        SubmitSegment nativeBarrier;   // 资源状态转换/屏障
        SubmitSegment nativeBind;      // pipeline / viewport / scissor / 各类绑定
        SubmitSegment nativeDraw;      // Draw / DrawIndexed
        SubmitSegment nativeOther;     // 其余原生操作（clear/copy/query 等）
        SubmitSegment submit;          // 队列提交 + fence
        std::uint64_t recordedEvents = 0;

        [[nodiscard]] std::uint64_t NativeMicros() const
        {
            return nativeRendering.micros + nativeBarrier.micros + nativeBind.micros + nativeDraw.micros +
                   nativeOther.micros;
        }
        [[nodiscard]] std::uint64_t NativeEvents() const
        {
            return nativeRendering.events + nativeBarrier.events + nativeBind.events + nativeDraw.events +
                   nativeOther.events;
        }
    };
    SubmitProfile submitProfile{};
};

struct TimestampResult final
{
    std::uint64_t ticks = 0;
    std::uint64_t frequency = 0;
    std::uint64_t frameSerial = 0;
    bool disjoint = false;
    std::uint8_t validBits = 64;
    bool unavailable = false;
};

// 回读成功后数据归调用者；rowPitch 是 bytes，bytes 含完整各行。
struct TextureReadbackResult final
{
    std::vector<std::byte> bytes;
    std::uint64_t rowPitch = 0;
    Extent2D extent;
    Format format = Format::Unknown;
    // 已完成但不可取像素（例如无 GPU trace）必须明确表示，不能伪造黑图。
    bool unavailable = false;
    std::uint64_t frameSerial = 0;
    TextureHandle source;
};

// 图在执行前读取当前帧的真实 descriptor/access/内容证明；查询不导入、不创建、
// 不标记 GPU 使用。fullyDefined 对 buffer 表示整个 [0,size) 区间已定义。
struct TextureStateSnapshot final
{
    TextureDesc descriptor;
    ResourceAccess access = ResourceAccess::None;
    bool fullyDefined = false;
};
struct BufferStateSnapshot final
{
    BufferDesc descriptor;
    ResourceAccess access = ResourceAccess::None;
    bool fullyDefined = false;
};

// 失败抛 RhiException；descriptor/upload 验证与生命周期 owner 在公共 CPU 层。
// backend 必须在原生创建前验证，录制引用时标记最后使用，真实完成后才回收。
class IRhiDevice
{
  public:
    virtual ~IRhiDevice() = default;

    [[nodiscard]] virtual const RhiCapabilities& Capabilities() const = 0;
    [[nodiscard]] virtual DeviceDiagnostics Diagnostics() const = 0;
    [[nodiscard]] virtual TextureStateSnapshot QueryTextureState(const FrameToken& frame,
                                                                 TextureHandle texture) const = 0;
    [[nodiscard]] virtual BufferStateSnapshot QueryBufferState(const FrameToken& frame, BufferHandle buffer) const = 0;

    virtual BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) = 0;
    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
    virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
    virtual ResourceSetLayoutHandle CreateResourceSetLayout(const ResourceSetLayoutDesc& desc) = 0;
    virtual ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc) = 0;
    // 当前帧专属集合可以引用 WriteDynamicBuffer 的切片，EndFrame 自动撤销并退休。
    // persistent CreateResourceSet 仍拒绝 frame-local buffer，不能跨帧保存此集合。
    virtual ResourceSetHandle CreateFrameResourceSet(const FrameToken& frame, const ResourceSetDesc& desc) = 0;
    virtual PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) = 0;
    virtual GraphicsPipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual TimestampQueryHandle CreateTimestampQuery(std::string_view debugName) = 0;
    virtual SwapChainHandle CreateSwapChain(const SwapChainDesc& desc) = 0;

    // Destroy 立即撤销逻辑可用性，底层对象待最后一次 GPU 使用完成后退休。
    // 资源集合及 pipeline 依赖须先销毁；验证失败不得部分修改注册表。
    virtual void Destroy(BufferHandle handle) = 0;
    virtual void Destroy(TextureHandle handle) = 0;
    virtual void Destroy(SamplerHandle handle) = 0;
    virtual void Destroy(ShaderHandle handle) = 0;
    virtual void Destroy(ResourceSetLayoutHandle handle) = 0;
    virtual void Destroy(ResourceSetHandle handle) = 0;
    virtual void Destroy(PipelineLayoutHandle handle) = 0;
    virtual void Destroy(GraphicsPipelineHandle handle) = 0;
    virtual void Destroy(TimestampQueryHandle handle) = 0;
    virtual void Destroy(SwapChainHandle handle) = 0;

    // token 只属于本 device 当前帧；serial=0 表示零尺寸/挂起，不得录制或 EndFrame。
    // recycleLane 是后端已等待安全的回收槽位，可用于 transient pool，不得决定 pass 顺序。
    virtual FrameToken BeginFrame(SwapChainHandle swapChain) = 0;
    // frame 已携带 backbuffer；此入口统一校验 token，不提供额外 native acquire。
    virtual TextureHandle AcquireBackBuffer(const FrameToken& frame) = 0;
    virtual IRhiCommandList& BeginGraphics(const FrameToken& frame) = 0;
    virtual IRhiGraphCommandSink& GraphCommandSink(const FrameToken& frame) = 0;
    virtual void EndGraphics(const FrameToken& frame, IRhiCommandList& commands) = 0;
    virtual void EndFrame(const FrameToken& frame, SwapChainHandle swapChain) = 0;

    virtual DynamicBufferSlice WriteDynamicBuffer(const FrameToken& frame, std::span<const std::byte> bytes,
                                                  std::uint32_t alignment) = 0;
    virtual void UploadBuffer(BufferHandle destination, std::uint64_t destinationOffset,
                              std::span<const std::byte> bytes) = 0;
    virtual void UploadTexture(TextureHandle destination, std::span<const TextureSubresourceData> subresources) = 0;
    virtual std::optional<TimestampResult> TryReadTimestamp(TimestampQueryHandle query) = 0;
    // nullopt 表示 GPU 尚未完成；成功结果拥有副本，调用方无需 Map/等待 fence。
    virtual std::optional<TextureReadbackResult> TryReadTextureReadback(BufferHandle readback) = 0;
    virtual void ResizeSwapChain(SwapChainHandle swapChain, Extent2D extent) = 0;
    // 仅在 resize/shutdown/recovery 使用，不得作为逐帧同步策略。
    virtual void WaitIdle() = 0;
};
} // namespace MiniEngine::Rhi
