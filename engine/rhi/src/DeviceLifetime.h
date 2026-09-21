#pragma once

#include "DeferredRegistry.h"
#include <MiniEngine/Rhi/IRhiDevice.h>
#include <MiniEngine/Rhi/RhiPipeline.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace MiniEngine::Rhi
{
// 后端私有资源的析构边界；公共 API 不返回此对象。派生类在 backend 内持有实际对象。
struct ResourcePayload
{
    virtual ~ResourcePayload() = default;
};
using PayloadFactory = std::function<std::unique_ptr<ResourcePayload>()>;
using ResourceIdentity =
    std::variant<BufferHandle, TextureHandle, SamplerHandle, ShaderHandle, ResourceSetLayoutHandle, ResourceSetHandle,
                 PipelineLayoutHandle, GraphicsPipelineHandle, TimestampQueryHandle, SwapChainHandle>;
// 变体顺序固定：index 9 = SwapChainHandle。Retire 需要在销毁 swap chain 前等待空闲边界；
// 用命名常量而非魔数，顺序由 tests/rhi/DeviceLifetimeTests.cpp 的 ResourceIdentityOrder 断言守护。
constexpr std::size_t kSwapChainIdentityIndex = 9;

// 供后续 IRhiDevice adapters 组合使用的 CPU owner，不是第三个图形后端。
// backend 负责实际创建、提交与完成证明；本类负责公开引用、依赖及退休顺序。
// 所有调用限渲染线程。析构前 owner 必须停录制、等 GPU、按依赖 Destroy 并 CheckShutdown。
class DeviceLifetime final
{
  public:
    explicit DeviceLifetime(RhiCapabilities capabilities);
    BufferHandle CreateBuffer(const BufferDesc& desc, const PayloadFactory& create);
    TextureHandle CreateTexture(const TextureDesc& desc, const PayloadFactory& create);
    SamplerHandle CreateSampler(const SamplerDesc& desc, const PayloadFactory& create);
    ShaderHandle CreateShader(const ShaderDesc& desc, const PayloadFactory& create);
    ResourceSetLayoutHandle CreateResourceSetLayout(const ResourceSetLayoutDesc& desc, const PayloadFactory& create);
    ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc, const PayloadFactory& create);
    ResourceSetHandle CreateFrameResourceSet(const FrameToken& frame, const ResourceSetDesc& desc,
                                             const PayloadFactory& create);
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc, const PayloadFactory& create);
    GraphicsPipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc, const PayloadFactory& create);
    SwapChainHandle CreateSwapChain(const SwapChainDesc& desc, const PayloadFactory& create);
    TimestampQueryHandle CreateTimestampQuery(std::string_view debugName, const PayloadFactory& create);
    void Destroy(ResourceIdentity handle);
    std::string DebugName(ResourceIdentity handle) const;
    std::uint64_t LastUse(ResourceIdentity handle) const;
    // adapter 只等待即将复用的 lane/backbuffer，完成证明再交给 Collect。
    std::uint64_t RequiredFrameCompletion(SwapChainHandle chain, std::optional<std::uint32_t> acquiredIndex = {}) const;
    ResourcePayload& Payload(ResourceIdentity handle);
    std::vector<TextureHandle> BackBuffers(SwapChainHandle chain) const;
    void ValidateAlive(ResourceIdentity handle) const;
    // 先完整验证引用，再标记当前帧及其传递依赖。禁止在 Destroy 后追加 GPU 使用。
    void Use(const FrameToken& frame, ResourceIdentity handle);
    // 与 Use 相同的身份前置验证，但不标记提交号，供 graph 的只读 import 核验。
    void ValidateFrameResource(const FrameToken& frame, ResourceIdentity handle) const;
    void CommitReplacement(ResourceSetHandle& current, ResourceSetHandle replacement);

    // 返回的引用只在下一次 owner 修改前借用；command validator 不保存这些引用。
    const BufferDesc& Describe(BufferHandle handle) const;
    const TextureDesc& Describe(TextureHandle handle) const;
    const ShaderDesc& Describe(ShaderHandle handle) const;
    const ResourceSetDesc& Describe(ResourceSetHandle handle) const;
    const ResourceSetLayoutDesc& Describe(ResourceSetLayoutHandle handle) const;
    const PipelineLayoutDesc& Describe(PipelineLayoutHandle handle) const;
    const GraphicsPipelineDesc& Describe(GraphicsPipelineHandle handle) const;
    std::vector<ResourceSetLayoutDesc> ResolvedLayouts(PipelineLayoutHandle handle) const;
    const std::string& PipelineKey(GraphicsPipelineHandle handle) const;
    void ValidateFrameToken(const FrameToken& frame) const;
    void ValidateSetBinding(const FrameToken& frame, ResourceSetHandle set,
                            std::span<const std::uint32_t> dynamicOffsets) const;
    RhiBackend Backend() const
    {
        return m_capabilities.backend;
    }
    std::uint64_t PipelineCreationCount() const
    {
        return m_pipelineCreations;
    }

    // Swap chain 的真实 ResizeBuffers 在 M6-05/06；这里管理外部 owner 的 backbuffer 身份。
    // replace 返回 RAII 候选 payload，验证/注册失败自动释放，不遗留无人持有的 handle。
    // 失败保持 suspended，旧 handles 已失效；回调不得修改当前 swap chain owner。
    struct BackBufferCandidate final
    {
        TextureDesc desc;
        std::unique_ptr<ResourcePayload> payload;
    };
    using BackBufferFactory = std::function<std::vector<BackBufferCandidate>()>;
    void ResizeBackBuffers(SwapChainHandle chain, Extent2D extent, const BackBufferFactory& replace);
    FrameToken BeginFrame(SwapChainHandle chain, std::optional<std::uint32_t> acquiredIndex = {});
    void EndFrame(const FrameToken& frame, SwapChainHandle chain);
    using DynamicPayloadFactory =
        std::function<std::unique_ptr<ResourcePayload>(const BufferDesc&, std::span<const std::byte>)>;
    DynamicBufferSlice WriteDynamicBuffer(const FrameToken& frame, std::span<const std::byte> bytes,
                                          std::uint32_t alignment, const DynamicPayloadFactory& create);
    // 上传回调必须复制输入并成功提交后才返回；抛出时必须尚未提交任何 GPU 工作。
    // 原生创建回调不得提交上传，先注册对象，再通过此入口绑定上传的最后使用序号。
    using BufferUpload =
        std::function<void(ResourcePayload&, std::uint64_t, std::span<const std::byte>, std::uint64_t)>;
    using TextureUpload = std::function<void(ResourcePayload&, std::span<const TextureSubresourceData>, std::uint64_t)>;
    std::uint64_t InitializeBuffer(BufferHandle destination, std::span<const std::byte> bytes,
                                   const BufferUpload& submit);
    std::uint64_t UploadBuffer(BufferHandle destination, std::uint64_t offset, std::span<const std::byte> bytes,
                               const BufferUpload& submit);
    std::uint64_t UploadTexture(TextureHandle destination, std::span<const TextureSubresourceData> subresources,
                                const TextureUpload& submit);
    void ValidateDynamicSlice(const FrameToken& frame, const DynamicBufferSlice& slice) const;
    // 此入口只接受 backend query/fence 得到的单流完成号，不主动等待 GPU。
    void Collect(std::uint64_t completed);
    RegistryCounts Stats() const;
    DeviceDiagnostics Diagnostics() const;
    void CheckShutdown() const;
    std::uint64_t LastSubmitted() const
    {
        return m_lastSubmitted;
    }
    std::uint64_t Completed() const
    {
        return m_completed;
    }

  private:
    using Key = RhiHandle<struct DeviceLifetimeKeyTag>;
    struct Record final
    {
        std::unique_ptr<ResourcePayload> payload;
        std::string debugName;
        std::vector<ResourceIdentity> dependencies;
        std::size_t references = 0;
        std::uint64_t frameSerial = 0;
        std::uint64_t dynamicSize = 0;
        bool backBuffer = false;
        std::optional<BufferDesc> buffer;
        std::optional<TextureDesc> texture;
        std::optional<ResourceSetLayoutDesc> setLayout;
        std::optional<SamplerDesc> sampler;
        std::optional<ShaderDesc> shader;
        std::optional<ResourceSetDesc> resourceSet;
        std::optional<PipelineLayoutDesc> pipelineLayout;
        std::optional<GraphicsPipelineDesc> pipeline;
        std::string pipelineKey;
        ShaderStage shaderStage = ShaderStage::Vertex;
        std::optional<SwapChainDesc> swapChain;
    };
    using Registry = DeferredRegistry<Key, Record>;
    static Key ToKey(const ResourceIdentity& handle);
    Record& Get(const ResourceIdentity& handle);
    const Record& Get(const ResourceIdentity& handle) const;
    template <class Handle> Handle Register(Record record, const std::string& name, const PayloadFactory& create)
    {
        if (m_uploadActive)
        {
            Fail("Create", "upload callback cannot reenter device mutation");
        }
        constexpr std::size_t kind = ResourceIdentity(Handle{}).index();
        for (const auto& dependency : record.dependencies)
        {
            (void)Get(dependency);
        }
        // 分配/原生创建/registry 插入全部成功后再增加引用，失败不改旧 revision。
        if (!create)
        {
            Fail("Create", "backend factory is missing");
        }
        record.debugName = name;
        record.payload = create();
        if (!record.payload)
        {
            Fail("Create", "backend returned an empty payload");
        }
        auto dependencies = record.dependencies;
        const Key key = m_registries[kind]->Create(std::move(record), name);
        for (const auto& dependency : dependencies)
        {
            ++Get(dependency).references;
        }
        return Handle{key.Index(), key.Generation(), key.Owner()};
    }
    void ValidateFrame(const FrameToken& frame) const;
    void RequireCreationBoundary(const char* operation) const;
    ResourceSetHandle CreateResourceSetInternal(const ResourceSetDesc& desc, const PayloadFactory& create,
                                                std::uint64_t frameSerial);
    std::uint64_t SubmitBufferUpload(BufferHandle destination, std::uint64_t offset, std::span<const std::byte> bytes,
                                     const BufferUpload& submit, bool initial);
    void MarkTree(const ResourceIdentity& handle, std::uint64_t serial);
    void DestroyInternal(ResourceIdentity handle, bool owned, bool completed = false);
    [[noreturn]] void Fail(const char* operation, const char* message) const;
    RhiCapabilities m_capabilities;
    std::array<std::unique_ptr<Registry>, 10> m_registries;
    const std::uint64_t m_owner;
    bool m_uploadActive = false;
    std::uint64_t m_issued = 0;
    std::uint64_t m_pipelineCreations = 0;
    std::uint64_t m_lastSubmitted = 0;
    std::uint64_t m_completed = 0;
    std::array<std::uint64_t, 3> m_lanes{};
    std::optional<FrameToken> m_frame;
    SwapChainHandle m_frameChain;
    std::vector<BufferHandle> m_dynamic;
    std::vector<ResourceSetHandle> m_frameSets;
};
} // namespace MiniEngine::Rhi
