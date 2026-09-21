#pragma once

#include "../../src/NativeRhiBackend.h"

#include "D3D12Capabilities.h"
#include "D3D12DescriptorHeap.h"
#include "D3D12PsoFactory.h"
#include "D3D12Queue.h"
#include "D3D12ResourceStateTracker.h"
#include "D3D12RootSignature.h"
#include "D3D12SwapChain.h"
#include "D3D12TextureUpload.h"
#include "D3D12UploadManager.h"
#include "D3D12UploadRing.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
// backend-private GPU 观测（D3D12GpuProfiler.h）；这里只前向声明，Tracy/D3D 类型不外泄。
class GpuProfiler;

class D3D12RhiBackend final : public NativeRhiBackend
{
  public:
    explicit D3D12RhiBackend(const RhiDeviceCreateInfo& createInfo);
    ~D3D12RhiBackend() override;
    D3D12RhiBackend(const D3D12RhiBackend&) = delete;
    D3D12RhiBackend& operator=(const D3D12RhiBackend&) = delete;

    const RhiCapabilities& Capabilities() const override;
    void Attach(DeviceLifetime& owner) override;
    std::unique_ptr<ResourcePayload> CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateSampler(const SamplerDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateShader(const ShaderDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateResourceSetLayout(const ResourceSetLayoutDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateResourceSet(const ResourceSetDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreatePipelineLayout(const PipelineLayoutDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateTimestampQuery(std::string_view name) override;
    std::unique_ptr<ResourcePayload> CreateSwapChain(const SwapChainDesc& desc) override;
    std::vector<DeviceLifetime::BackBufferCandidate> ResizeBackBuffers(ResourcePayload& chain,
                                                                       const SwapChainDesc& desc) override;
    std::uint32_t CurrentBackBufferIndex(ResourcePayload& chain) override;
    void BeginFrame(const FrameToken& frame, ResourcePayload& chain) override;
    void Consume(const CommandEvent& event) override;
    void Submit(const FrameToken& frame) override;
    void Present(ResourcePayload& chain) override;
    std::unique_ptr<ResourcePayload> CreateDynamicBuffer(const BufferDesc& desc,
                                                         std::span<const std::byte> bytes) override;
    void UploadBuffer(ResourcePayload& destination, std::uint64_t offset, std::span<const std::byte> bytes,
                      std::uint64_t serial) override;
    void UploadTexture(ResourcePayload& destination, std::span<const TextureSubresourceData> data,
                       std::uint64_t serial) override;
    std::uint64_t PollCompleted() override;
    std::uint64_t WaitFor(std::uint64_t serial) override;
    void WaitIdle() override;
    std::optional<TimestampResult> TryReadTimestamp(ResourcePayload& query) override;
    std::optional<TextureReadbackResult> TryReadTextureReadback(ResourcePayload& buffer) override;
    void ResetDiagnosticTrace() override
    {
        (void)Report();
        m_diagnosticTrace.clear();
    }
    // 负向通道：跳过下一次必要 state transition，让 GBV/Debug Layer 捕获漏 barrier。
    void InjectBackendFaultForTesting(std::string_view fault) override;
    NativeBackendReport Report(bool census = false) override;
    std::array<std::unique_ptr<ResourcePayload>, 4> PrepareEnvironment(
        ResourcePayload& panorama, std::string_view shaderRoot, std::uint64_t revision,
        const std::array<TextureDesc, 4>& descriptors) override;

  private:
    enum class PayloadKind : std::uint8_t
    {
        Buffer,
        Texture,
        Sampler,
        Shader,
        SetLayout,
        Set,
        PipelineLayout,
        Pipeline,
        Timestamp,
        SwapChain
    };

    enum class DescriptorGroupKind : std::uint8_t
    {
        Cbv,
        Srv,
        Sampler,
        FixedSrv
    };

    struct DescriptorGroup final
    {
        DescriptorGroupKind kind = DescriptorGroupKind::Srv;
        std::uint32_t rootParameter = 0;
        DescriptorRange source{};
        std::vector<std::uint32_t> sourceSlots;
        std::vector<std::uint32_t> bindingIndices;
    };

    struct Binding final
    {
        BindingLayoutEntry layout{};
        ResourceBinding value{};
        std::uint32_t group = UINT32_MAX;
        std::uint32_t groupSlot = 0;
        std::uint32_t dynamicIndex = UINT32_MAX;
        bool fixedRootCbv = false;
        std::uint32_t rootParameter = 0;
        bool fixedStaticSampler = false;
        std::uint32_t staticSamplerRegister = 0;
    };

    struct Payload final : ResourcePayload
    {
        explicit Payload(D3D12RhiBackend* backend, PayloadKind kind, std::string name)
            : backend(backend), kind(kind), debugName(std::move(name))
        {
        }
        ~Payload() override;

        D3D12RhiBackend* backend = nullptr;
        PayloadKind kind;
        std::string debugName;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        std::shared_ptr<AdaptedPso> cachedPipeline;
        std::unique_ptr<D3D12SwapChain> swapChain;
        ResourceKey stateKey{};
        bool hasStateKey = false;
        bool ownsStateKey = false;
        bool dynamic = false;
        bool backBuffer = false;
        bool uploadHeap = false;
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        std::uint64_t nativeSize = 0;
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
        DescriptorRange srvSource{};
        DescriptorRange samplerSource{};
        DescriptorRange rtvSource{};
        DescriptorRange dsvSource{};
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{};
        std::vector<std::byte> bytecode;
        std::vector<BindingLayoutEntry> layoutEntries;
        std::vector<Binding> bindings;
        std::vector<DescriptorGroup> groups;
        std::uint8_t set = 0;
        PipelineLayoutHandle layoutHandle{};
        bool fixedProfile = false;
        std::array<std::uint32_t, 3> setRootBase{};
        std::uint32_t queryIndex = UINT32_MAX;
        Microsoft::WRL::ComPtr<ID3D12Resource> queryReadback;
        // 公共 readback 可仅有紧凑像素容量，而 D3D12 placed footprint
        // 要求每行补齐 256 B；本后端私有资源承载 padding，
        // 并持有到 owner 完成退休。
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackStaging;
        std::uint64_t readbackStagingSize = 0;
        bool timestampWritten = false;
        bool timestampResolved = false;
        std::uint64_t timestampOwnerSerial = 0;
        std::uint64_t timestampFence = 0;
        std::uint64_t timestampFrequency = 0;
        TextureHandle readbackSource{};
        std::uint64_t readbackOwnerSerial = 0;
        std::uint64_t readbackFence = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint{};
        std::uint64_t readbackTotalBytes = 0;
        std::uint32_t readbackRowCount = 0;
        std::uint64_t readbackRowSize = 0;
        Extent2D readbackExtent{};
        Format readbackFormat = Format::Unknown;
        bool readbackPending = false;
        bool readbackConsumed = false;
        std::uint32_t nativeResourceCount = 0;
    };

    struct RetiredRange final
    {
        D3D12DescriptorHeap* heap = nullptr;
        DescriptorRange range{};
    };

    struct Attachment final
    {
        TextureHandle texture{};
        StoreOp store = StoreOp::Store;
        bool depth = false;
    };

    static Payload& AsPayload(ResourcePayload& payload);
    static const Payload& AsPayload(const ResourcePayload& payload);
    static std::wstring ToWide(std::string_view text);
    static DXGI_FORMAT ToNativeFormat(Format format);
    static DXGI_FORMAT ToResourceFormat(Format format);
    static DXGI_FORMAT ToDepthSrvFormat(Format format);
    static D3D12_RESOURCE_STATES AccessState(ResourceAccess access, ShaderStage stages = ShaderStage::Pixel);
    static D3D12_COMPARISON_FUNC CompareFunction(CompareOp op);
    static D3D12_FILTER FilterMode(const SamplerDesc& desc);
    static D3D12_TEXTURE_ADDRESS_MODE AddressMode(AddressMode mode);
    static D3D12_STATIC_BORDER_COLOR BorderColor(const std::array<float, 4>& color);
    static std::optional<std::pair<std::uint32_t, std::uint32_t>> FixedSrvSlot(std::uint8_t set, std::uint16_t binding);
    static std::optional<std::uint32_t> FixedCbvRoot(std::uint8_t set, std::uint16_t binding);
    static std::optional<std::uint32_t> FixedSamplerSlot(std::uint8_t set, std::uint16_t binding);

    void DestroyPayload(Payload& payload) noexcept;
    void RequireAttached(const char* operation) const;
    Payload& RequirePayload(ResourcePayload& payload, PayloadKind kind);
    const Payload& RequirePayload(const ResourcePayload& payload, PayloadKind kind) const;
    D3D12_RESOURCE_DESC MakeBufferDescription(const BufferDesc& desc) const;
    D3D12_RESOURCE_DESC MakeTextureDescription(const TextureDesc& desc) const;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateGenericRootSignature(const PipelineLayoutDesc& desc,
                                                                           bool& fixedProfile);
    void MakeSetDescriptors(Payload& setPayload, const ResourceSetDesc& desc);
    void CreateSourceCbv(Payload& setPayload, Binding& binding, std::uint32_t slot);
    void CreateSourceSrv(Payload& setPayload, Binding& binding, std::uint32_t slot);
    void CreateSourceSampler(Payload& setPayload, Binding& binding, std::uint32_t slot);
    void Transition(Payload& payload, ResourceAccess access, ShaderStage stages = ShaderStage::Pixel);
    std::uint32_t FlushBarriers();
    void BindResourceSet(std::uint32_t setIndex, Payload& setPayload, std::span<const std::uint32_t> offsets);
    void ResolveActiveQueries();
    void SetSubmissionFence(std::uint64_t ownerSerial, std::uint64_t actualFence);
    void RecordStandaloneUpload(Payload& destination, std::uint64_t offset, std::span<const std::byte> bytes,
                                std::uint64_t serial);
    void RecordStandaloneTextureUpload(Payload& destination, std::span<const TextureSubresourceData> data,
                                       std::uint64_t serial);
    void FinishStandaloneSubmission(std::uint64_t serial, D3D12FrameContext& context);
    std::uint64_t CompletedActual();

    std::unique_ptr<D3D12Device> m_device;
    D3D12ResourceStateTracker m_stateTracker;
    D3D12Queue m_queue;
    // M7-02：GPU 观测与 queue 同生命周期；Tracy 关闭时为 nullptr。
    std::unique_ptr<GpuProfiler> m_gpuProfiler;
    D3D12UploadRing m_uploadRing;
    D3D12UploadManager m_uploadManager;
    D3D12DescriptorHeap m_srvStaging;
    D3D12DescriptorHeap m_srvVisible;
    D3D12DescriptorHeap m_samplerStaging;
    D3D12DescriptorHeap m_samplerVisible;
    D3D12DescriptorHeap m_rtvHeap;
    D3D12DescriptorHeap m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_m5RootSignature;
    RootSignatureFacts m_rootFacts{};
    D3D12PsoFactory m_psoFactory;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap;
    RhiCapabilities m_capabilities{};
    DeviceLifetime* m_owner = nullptr;
    Payload* m_activeChain = nullptr;
    D3D12FrameContext* m_activeContext = nullptr;
    std::optional<FrameToken> m_activeFrame;
    Payload* m_currentPipeline = nullptr;
    std::vector<RetiredRange> m_activeRanges;
    std::vector<Payload*> m_activeQueries;
    std::vector<Payload*> m_activeReadbacks;
    std::vector<std::pair<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> m_retiredReadbacks;
    std::vector<Attachment> m_attachments;
    std::map<ResourceIdentity, ResourceAccess> m_logicalAccess;
    std::map<std::uint64_t, std::uint64_t> m_ownerToFence;
    std::uint64_t m_completedActual = 0;
    std::uint64_t m_completedOwner = 0;
    std::uint64_t m_lastOwnerSerial = 0;
    std::uint64_t m_queryFrequency = 0;
    std::uint32_t m_nextQueryIndex = 0;
    std::vector<std::uint32_t> m_freeQueryIndices;
    std::uint64_t m_livePayloadResources = 0;
    std::uint64_t m_submittedBatches = 0;
    std::uint64_t m_barriers = 0;
    std::uint64_t m_discardNoOps = 0;
    bool m_rendering = false;
    bool m_closed = false;
    std::uint64_t m_diagnosticWarnings = 0;
    std::uint64_t m_injectedFaults = 0;
    bool m_skipNextTransition = false;
    std::string m_diagnosticTrace;
    bool m_initialized = false;
};

std::unique_ptr<NativeRhiBackend> CreateD3D12RhiBackend(const RhiDeviceCreateInfo& createInfo);
} // namespace MiniEngine::Rhi::D3D12
