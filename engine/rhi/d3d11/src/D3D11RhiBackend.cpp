#include "D3D11RhiBackend.h"
#include "D3D11Capabilities.h"
#include "D3D11Error.h"
#include "D3D11GpuProfiler.h"
#include <MiniEngine/Rhi/D3D11/D3D11IblResources.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <MiniEngine/Rhi/RhiValidation.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <deque>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace MiniEngine::Rhi::D3D11
{
namespace
{
using Microsoft::WRL::ComPtr;

// D3D11 会复用等价 state 的 native identity；保留首次命名，避免重复改名触发 warning 55。
void NameNativeObject(ID3D11DeviceChild* object, std::string_view name)
{
    if (!object || name.empty())
        return;
    UINT size = 0;
    object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, nullptr);
    if (size == 0)
        MiniEngine::SetDebugObjectName(object, name);
}

[[noreturn]] void Fail(RhiErrorCode code, std::string_view operation, std::string_view object, std::string_view message)
{
    throw RhiValidationError({code, std::string(operation), std::string(object), {}, "d3d11", std::string(message)});
}

void Check(HRESULT result, std::string_view operation)
{
    if (FAILED(result))
    {
        std::ostringstream out;
        out << operation << " failed with HRESULT 0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result);
        throw RhiException({RhiErrorCode::BackendFailure, std::string(operation), "Native", {}, "d3d11", out.str()});
    }
}

std::wstring Wide(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

DXGI_FORMAT DxgiFormat(Format format, bool depthResource = false)
{
    switch (format)
    {
    case Format::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8UnormSrgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Rg16Float:
        return DXGI_FORMAT_R16G16_FLOAT;
    case Format::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case Format::D32Float:
        return depthResource ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
    case Format::D24UnormS8Uint:
        return depthResource ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    default:
        break;
    }
    Fail(RhiErrorCode::Unsupported, "DxgiFormat", "Format", "format is outside the D3D11 M6 profile");
}

DXGI_FORMAT ShaderResourceFormat(Format format)
{
    switch (format)
    {
    case Format::D32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case Format::D24UnormS8Uint:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:
        return DxgiFormat(format);
    }
}

std::uint32_t BytesPerPixel(Format format)
{
    switch (format)
    {
    case Format::Rgba8Unorm:
    case Format::Rgba8UnormSrgb:
    case Format::R32Float:
    case Format::D32Float:
    case Format::D24UnormS8Uint:
        return 4;
    case Format::Rg16Float:
        return 4;
    case Format::Rgba16Float:
        return 8;
    default:
        break;
    }
    return 0;
}

D3D11_COMPARISON_FUNC Compare(CompareOp value)
{
    switch (value)
    {
    case CompareOp::Never:
        return D3D11_COMPARISON_NEVER;
    case CompareOp::Less:
        return D3D11_COMPARISON_LESS;
    case CompareOp::LessEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOp::Equal:
        return D3D11_COMPARISON_EQUAL;
    case CompareOp::GreaterEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOp::Greater:
        return D3D11_COMPARISON_GREATER;
    case CompareOp::Always:
        return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_ALWAYS;
}

D3D11_TEXTURE_ADDRESS_MODE Address(AddressMode value)
{
    switch (value)
    {
    case AddressMode::Repeat:
        return D3D11_TEXTURE_ADDRESS_WRAP;
    case AddressMode::Clamp:
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    case AddressMode::Border:
        return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

D3D11_FILTER FilterMode(const SamplerDesc& desc)
{
    if (desc.comparisonEnabled)
    {
        if (desc.minMagFilter == Filter::Anisotropic || desc.mipFilter == Filter::Anisotropic)
            return D3D11_FILTER_COMPARISON_ANISOTROPIC;
        if (desc.minMagFilter == Filter::Nearest && desc.mipFilter == Filter::Nearest)
            return D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        if (desc.minMagFilter == Filter::Nearest)
            return D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR;
        if (desc.mipFilter == Filter::Nearest)
            return D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        return D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    }
    if (desc.minMagFilter == Filter::Anisotropic || desc.mipFilter == Filter::Anisotropic)
        return D3D11_FILTER_ANISOTROPIC;
    if (desc.minMagFilter == Filter::Nearest && desc.mipFilter == Filter::Nearest)
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (desc.minMagFilter == Filter::Nearest)
        return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    if (desc.mipFilter == Filter::Nearest)
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D11_FILL_MODE FillMode()
{
    return D3D11_FILL_SOLID;
}

struct NativeCounters final
{
    std::uint64_t livePayloads = 0;
    std::uint64_t liveObjects = 0;
};

struct NativeHold final
{
    std::shared_ptr<NativeCounters> counters;
    bool object = false;
    NativeHold() = default;
    NativeHold(std::shared_ptr<NativeCounters> value, bool native) : counters(std::move(value)), object(native)
    {
        if (counters)
        {
            ++counters->livePayloads;
            if (object)
                ++counters->liveObjects;
        }
    }
    NativeHold(const NativeHold&) = delete;
    NativeHold& operator=(const NativeHold&) = delete;
    NativeHold(NativeHold&& other) noexcept : counters(std::move(other.counters)), object(other.object)
    {
        other.object = false;
    }
    NativeHold& operator=(NativeHold&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            counters = std::move(other.counters);
            object = other.object;
            other.object = false;
        }
        return *this;
    }
    ~NativeHold()
    {
        Reset();
    }
    void Reset() noexcept
    {
        if (counters)
        {
            if (object && counters->liveObjects)
                --counters->liveObjects;
            if (counters->livePayloads)
                --counters->livePayloads;
            counters.reset();
        }
        object = false;
    }
};

struct PayloadBase : ResourcePayload
{
    std::shared_ptr<NativeCounters> counters;
    NativeHold hold;
    explicit PayloadBase(std::shared_ptr<NativeCounters> value, bool native)
        : counters(std::move(value)), hold(counters, native)
    {
    }
};

struct ReadbackInfo final
{
    ComPtr<ID3D11Texture2D> staging;
    NativeHold hold;
    TextureHandle source;
    TextureDesc desc;
    Extent2D extent;
    std::uint64_t frameSerial = 0;
    std::uint64_t completionSerial = 0;
    bool valid = true;
};

struct BufferPayload final : PayloadBase
{
    BufferDesc desc;
    ComPtr<ID3D11Buffer> buffer;
    std::optional<ReadbackInfo> readback;
    std::vector<std::byte> uploadShadow;
    BufferPayload(std::shared_ptr<NativeCounters> countersValue, const BufferDesc& value, bool native = true)
        : PayloadBase(std::move(countersValue), native), desc(value)
    {
    }
};

struct TexturePayload final : PayloadBase
{
    TextureDesc desc;
    DXGI_FORMAT nativeFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    TexturePayload(std::shared_ptr<NativeCounters> countersValue, const TextureDesc& value, bool native = true)
        : PayloadBase(std::move(countersValue), native), desc(value)
    {
    }
};

struct SamplerPayload final : PayloadBase
{
    SamplerDesc desc;
    ComPtr<ID3D11SamplerState> sampler;
    SamplerPayload(std::shared_ptr<NativeCounters> countersValue, const SamplerDesc& value)
        : PayloadBase(std::move(countersValue), true), desc(value)
    {
    }
};

struct ShaderPayload final : PayloadBase
{
    ShaderDesc desc;
    std::vector<std::byte> bytecode;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ShaderPayload(std::shared_ptr<NativeCounters> countersValue, const ShaderDesc& value)
        : PayloadBase(std::move(countersValue), true), desc(value),
          bytecode(value.bytecode.begin(), value.bytecode.end())
    {
        desc.bytecode = {};
    }
};

struct ResourceSetLayoutPayload final : PayloadBase
{
    ResourceSetLayoutDesc desc;
    ResourceSetLayoutPayload(std::shared_ptr<NativeCounters> countersValue, const ResourceSetLayoutDesc& value)
        : PayloadBase(std::move(countersValue), false), desc(value)
    {
    }
};

struct ResourceSetPayload final : PayloadBase
{
    ResourceSetDesc desc;
    ResourceSetPayload(std::shared_ptr<NativeCounters> countersValue, const ResourceSetDesc& value)
        : PayloadBase(std::move(countersValue), false), desc(value)
    {
    }
};

struct PipelineLayoutPayload final : PayloadBase
{
    PipelineLayoutDesc desc;
    PipelineLayoutPayload(std::shared_ptr<NativeCounters> countersValue, const PipelineLayoutDesc& value)
        : PayloadBase(std::move(countersValue), false), desc(value)
    {
    }
};

struct GraphicsPipelinePayload final : PayloadBase
{
    GraphicsPipelineDesc desc;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depthStencil;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    GraphicsPipelinePayload(std::shared_ptr<NativeCounters> countersValue, const GraphicsPipelineDesc& value)
        : PayloadBase(std::move(countersValue), true), desc(value)
    {
    }
};

struct TimestampPayload final : PayloadBase
{
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> timestamp;
    bool armed = false;
    std::uint64_t frameSerial = 0;
    std::uint64_t completionSerial = 0;
    TimestampPayload(std::shared_ptr<NativeCounters> countersValue) : PayloadBase(std::move(countersValue), true)
    {
    }
};

struct SwapChainPayload final : PayloadBase
{
    SwapChainDesc desc;
    ComPtr<IDXGISwapChain3> swapChain;
    SwapChainPayload(std::shared_ptr<NativeCounters> countersValue, const SwapChainDesc& value)
        : PayloadBase(std::move(countersValue), true), desc(value)
    {
    }
};

struct PendingUpload final
{
    std::uint64_t serial = 0;
    ComPtr<ID3D11Resource> staging;
    std::vector<std::byte> input;
    NativeHold hold;
    PendingUpload(std::shared_ptr<NativeCounters> countersValue, std::uint64_t value, bool native)
        : serial(value), hold(std::move(countersValue), native)
    {
    }
    PendingUpload(PendingUpload&&) noexcept = default;
    PendingUpload& operator=(PendingUpload&&) noexcept = default;
    PendingUpload(const PendingUpload&) = delete;
    PendingUpload& operator=(const PendingUpload&) = delete;
};

struct Completion final
{
    std::uint64_t serial = 0;
    ComPtr<ID3D11Query> query;
    NativeHold hold;
    Completion(std::shared_ptr<NativeCounters> countersValue, std::uint64_t value, ComPtr<ID3D11Query> event)
        : serial(value), query(std::move(event)), hold(std::move(countersValue), true)
    {
    }
    Completion(Completion&&) noexcept = default;
    Completion& operator=(Completion&&) noexcept = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
};

struct NativeBinding final
{
    BindingType type = BindingType::UniformBuffer;
    std::uint32_t slot = 0;
};

std::optional<NativeBinding> LowerBinding(std::uint8_t set, std::uint16_t binding, BindingType type)
{
    // 本表对应已校验的 M6 shader compiler 契约；逻辑 binding 不通过
    // 算术或 set/binding 身份直接推导原生 register。
    if (set == 0)
    {
        switch (binding)
        {
        case 0:
            if (type == BindingType::UniformBuffer)
                return NativeBinding{type, 0};
            break;
        case 1:
            if (type == BindingType::UniformBuffer)
                return NativeBinding{type, 3};
            break;
        case 2:
            if (type == BindingType::SampledTexture)
                return NativeBinding{type, 5};
            break;
        case 3:
            if (type == BindingType::SampledTexture)
                return NativeBinding{type, 6};
            break;
        case 4:
            if (type == BindingType::SampledTexture)
                return NativeBinding{type, 7};
            break;
        case 5:
            if (type == BindingType::SampledTexture)
                return NativeBinding{type, 8};
            break;
        case 6:
            if (type == BindingType::Sampler)
                return NativeBinding{type, 0};
            break;
        case 7:
            if (type == BindingType::Sampler)
                return NativeBinding{type, 1};
            break;
        case 8:
            if (type == BindingType::Sampler)
                return NativeBinding{type, 2};
            break;
        case 9:
            if (type == BindingType::UniformBuffer)
                return NativeBinding{type, 0};
            break;
        case 10:
            if (type == BindingType::SampledTexture)
                return NativeBinding{type, 0};
            break;
        case 11:
            if (type == BindingType::UniformBuffer)
                return NativeBinding{type, 0};
            break;
        case 12:
            if (type == BindingType::SampledTexture)
                return NativeBinding{type, 0};
            break;
        default:
            break;
        }
    }
    else if (set == 1)
    {
        if (binding == 0 && type == BindingType::UniformBuffer)
            return NativeBinding{type, 2};
        if (binding >= 1 && binding <= 5 && type == BindingType::SampledTexture)
            return NativeBinding{type, static_cast<std::uint32_t>(binding - 1)};
    }
    else if (set == 2 && binding == 0 && type == BindingType::UniformBuffer)
    {
        return NativeBinding{type, 1};
    }
    return std::nullopt;
}

void ValidateNativeProfile(const ShaderDesc& shader)
{
    for (const auto& requirement : shader.manifest.bindings)
    {
        const auto lowered = LowerBinding(requirement.set, requirement.binding, requirement.type);
        if (!lowered || lowered->slot >= 128)
            Fail(RhiErrorCode::Unsupported, "CreateGraphicsPipeline", shader.debugName,
                 "shader logical binding has no explicit D3D11 register lowering");
    }
}

} // namespace

class D3D11RhiBackend final : public NativeRhiBackend
{
  public:
    explicit D3D11RhiBackend(const RhiDeviceCreateInfo& createInfo);
    ~D3D11RhiBackend() override;
    const RhiCapabilities& Capabilities() const override
    {
        return m_capabilities;
    }
    void Attach(DeviceLifetime& owner) override
    {
        m_owner = &owner;
    }
    std::unique_ptr<ResourcePayload> CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<ResourcePayload> CreateTexture(const TextureDesc& desc) override;
    std::array<std::unique_ptr<ResourcePayload>, 4> PrepareEnvironment(
        ResourcePayload& panorama, std::string_view shaderRoot, std::uint64_t revision,
        const std::array<TextureDesc, 4>& descriptors) override;
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
        RecordMessageDiagnostics();
        m_trace = "miniengine.d3d11-native.v1\n";
    }
    // 负向通道：跳过下一次冲突 output 的主动 unbind，让 Debug Layer 捕获 hazard。
    void InjectBackendFaultForTesting(std::string_view fault) override;
    NativeBackendReport Report(bool census = false) override;

  private:
    template <class T> T& PayloadAs(ResourcePayload& payload, const char* operation)
    {
        auto* result = dynamic_cast<T*>(&payload);
        if (!result)
            Fail(RhiErrorCode::InvalidArgument, operation, "ResourcePayload", "payload type does not match operation");
        return *result;
    }
    void CreateTextureViews(TexturePayload& payload);
    void SubmitCompletion(std::uint64_t serial);
    void ReleaseCompleted();
    void ClearContext();
    void UnbindShaderReads(ID3D11Resource* resource);
    void UnbindOutputs(ID3D11Resource* resource);
    void BindSet(const ResourceSetPayload& set, std::span<const std::uint32_t> offsets);
    void Discard(ID3D11View* view);
    void RecordMessageDiagnostics();
    std::uint32_t ConstantCount(std::uint64_t bytes) const;
    void SetConstantBuffer(ShaderStage visibility, std::uint32_t slot, ID3D11Buffer* buffer,
                           std::uint32_t firstConstant, std::uint32_t numConstants);
    ID3D11Device* Device() const
    {
        return m_device.Get();
    }
    ID3D11DeviceContext* Context() const
    {
        return m_context.Get();
    }

    RhiDeviceCreateInfo m_createInfo;
    RhiCapabilities m_capabilities;
    std::shared_ptr<NativeCounters> m_counters = std::make_shared<NativeCounters>();
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11DeviceContext1> m_context1;
    ComPtr<ID3DUserDefinedAnnotation> m_annotation;
    ComPtr<ID3D11InfoQueue> m_infoQueue;
    ComPtr<ID3D11Debug> m_debug;
    // M7-02：backend-private GPU 观测（Tracy 关闭时为 nullptr，调用点短路）。
    std::unique_ptr<GpuProfiler> m_gpuProfiler;
    DeviceLifetime* m_owner = nullptr;
    std::deque<Completion> m_completions;
    std::vector<PendingUpload> m_pending;
    std::uint64_t m_completedSerial = 0;
    std::uint64_t m_lastSubmittedSerial = 0;
    std::uint64_t m_submittedBatches = 0;
    std::uint64_t m_explicitUnbinds = 0;
    std::uint64_t m_barriers = 0;
    std::uint64_t m_discardNoOps = 0;
    std::uint64_t m_warningErrors = 0;
    std::uint64_t m_injectedFaults = 0;
    std::uint64_t m_frameDynamicBytes = 0;
    bool m_skipNextHazardUnbind = false;
    ID3D11Resource* m_faultSuppressedResource = nullptr;
    std::size_t m_scannedMessages = 0;
    std::string m_trace = "miniengine.d3d11-native.v1\n";
    std::optional<FrameToken> m_frame;
    SwapChainPayload* m_frameChain = nullptr;
    bool m_rendering = false;
    std::uint32_t m_outputCount = 0;
    std::vector<StoreOp> m_outputStores;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> m_rtvs;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    std::array<ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_vsSrvs;
    std::array<ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_psSrvs;
    struct ConstantBinding
    {
        ComPtr<ID3D11Buffer> buffer;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    std::array<ConstantBinding, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> m_vsConstants, m_psConstants;
    std::array<ComPtr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> m_vsSamplers, m_psSamplers;
    std::vector<TimestampPayload*> m_frameQueries;
    std::vector<BufferPayload*> m_frameReadbacks;
    std::vector<ReadbackInfo> m_retiredReadbacks;
    GraphicsPipelinePayload* m_currentPipeline = nullptr;
};

D3D11RhiBackend::D3D11RhiBackend(const RhiDeviceCreateInfo& createInfo) : m_createInfo(createInfo)
{
    if (createInfo.enableGpuValidation)
        Fail(RhiErrorCode::Unsupported, "CreateD3D11RhiBackend", "Device", "D3D11 has no GPU-based validation mode");

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (createInfo.enableDebugLayer)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_DRIVER_TYPE driver = createInfo.useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT result =
        D3D11CreateDevice(nullptr, driver, nullptr, flags, requested, static_cast<UINT>(std::size(requested)),
                          D3D11_SDK_VERSION, &m_device, &selected, &m_context);
    if (FAILED(result))
    {
        std::ostringstream out;
        out << "D3D11CreateDevice failed with HRESULT 0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result) << (createInfo.useWarp ? " (WARP)" : " (hardware)");
        throw RhiException({RhiErrorCode::BackendFailure, "D3D11CreateDevice", "Device", {}, "d3d11", out.str()});
    }
    if (selected < D3D_FEATURE_LEVEL_11_0)
        Fail(RhiErrorCode::Unsupported, "D3D11CreateDevice", "Device", "feature level 11_0 is required");

    // 创建设备可没有 D3D11.1；只有实际消费非零动态常量范围
    // 时才要求对应的 offset API。
    (void)m_context.As(&m_context1);
    (void)m_context.As(&m_annotation);
    (void)m_device.As(&m_infoQueue);
    (void)m_device.As(&m_debug);
    if (m_infoQueue.Get() != nullptr)
    {
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
        m_infoQueue->ClearStoredMessages();
    }
    m_capabilities = QueryCapabilities(*m_device.Get(), *m_context.Get(), 2000);
    if (m_capabilities.backend != RhiBackend::D3D11 || m_capabilities.debugLayerEnabled != createInfo.enableDebugLayer)
        Fail(RhiErrorCode::BackendFailure, "CreateD3D11RhiBackend", "Capabilities",
             "reported D3D11 capabilities do not match the selected device");
    m_trace += "driver=" + m_capabilities.adapterName +
               " debug=" + std::string(m_capabilities.debugLayerEnabled ? "true\n" : "false\n");
    // GPU profiler 与 device/context 同生共死，且只在 render thread 上创建（本构造函数
    // 由 RHI 组合根在 render thread 调用）。
    m_gpuProfiler = CreateGpuProfiler(m_device.Get(), m_context.Get());
}

D3D11RhiBackend::~D3D11RhiBackend()
{
    try
    {
        WaitIdle();
    }
    catch (...)
    {
        // 析构不能再抛第二个异常；释放原生对象前 WaitIdle 已尝试
        // 有界的真实 EVENT 完成证明。
    }
    // 先于 device/context 释放：Tracy 的析构会回读未完成的 timestamp 查询。
    m_gpuProfiler.reset();
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateBuffer(const BufferDesc& desc)
{
    if (desc.size == 0 || desc.size > std::numeric_limits<UINT>::max())
        Fail(RhiErrorCode::InvalidArgument, "CreateBuffer", desc.debugName,
             "buffer size is outside D3D11's UINT range");
    D3D11_BUFFER_DESC native{};
    native.ByteWidth = static_cast<UINT>(desc.size);
    native.Usage = D3D11_USAGE_DEFAULT;
    native.BindFlags = 0;
    native.CPUAccessFlags = 0;
    if (desc.memory == MemoryDomain::CpuToGpu)
    {
        native.Usage = D3D11_USAGE_DYNAMIC;
        native.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }
    else if (desc.memory == MemoryDomain::GpuToCpu)
    {
        native.Usage = D3D11_USAGE_STAGING;
        native.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    }
    if (desc.memory != MemoryDomain::GpuToCpu)
    {
        if (HasFlag(desc.usage, BufferUsage::Vertex))
            native.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        if (HasFlag(desc.usage, BufferUsage::Index))
            native.BindFlags |= D3D11_BIND_INDEX_BUFFER;
        if (HasFlag(desc.usage, BufferUsage::Uniform))
            native.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
    }
    auto payload = std::make_unique<BufferPayload>(m_counters, desc);
    Check(m_device->CreateBuffer(&native, nullptr, &payload->buffer), "ID3D11Device::CreateBuffer");
    NameNativeObject(payload->buffer.Get(), desc.debugName);
    return payload;
}

std::array<std::unique_ptr<ResourcePayload>, 4> D3D11RhiBackend::PrepareEnvironment(
    ResourcePayload& panorama, std::string_view shaderRoot, std::uint64_t revision,
    const std::array<TextureDesc, 4>& descriptors)
{
    auto& source = PayloadAs<TexturePayload>(panorama, "PrepareEnvironment");
    ClearContext();
    try
    {
        D3D11IblResources candidate;
        std::string error;
        if (!source.srv || !candidate.BuildTemporary(*Device(), *Context(), *source.srv.Get(),
                                                     std::filesystem::path(shaderRoot) / "d3d11", revision, error))
        {
            throw std::runtime_error("D3D11 IBL preparation failed: " + error);
        }
        candidate.CommitTemporary();
        const auto* active = candidate.Active();
        if (!active || candidate.State() != IblState::Ready)
            throw std::runtime_error("D3D11 IBL not ready");
        std::array<ComPtr<ID3D11Texture2D>, 4> textures{active->environment, active->irradiance, active->prefiltered,
                                                        active->brdfLut};
        std::array<ComPtr<ID3D11ShaderResourceView>, 4> views{active->environmentSrv, active->irradianceSrv,
                                                              active->prefilteredSrv, active->brdfLutSrv};
        std::array<std::unique_ptr<ResourcePayload>, 4> result;
        for (std::size_t i = 0; i < 4; ++i)
        {
            auto texture = std::make_unique<TexturePayload>(m_counters, descriptors[i]);
            texture->nativeFormat = DxgiFormat(descriptors[i].format);
            texture->texture = textures[i];
            texture->srv = views[i];
            result[i] = std::move(texture);
        }
        ClearContext();
        RecordMessageDiagnostics();
        return result;
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        ClearContext();
        try
        {
            RecordMessageDiagnostics();
        }
        catch (...)
        {
        }
        std::rethrow_exception(failure);
    }
}
std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateTexture(const TextureDesc& desc)
{
    if (desc.extent.width == 0 || desc.extent.height == 0 || desc.mipLevels == 0 || desc.arrayLayers == 0)
        Fail(RhiErrorCode::InvalidArgument, "CreateTexture", desc.debugName,
             "empty texture extent or subresource count");
    if (desc.dimension == TextureDimension::TextureCube && desc.arrayLayers != 6)
        Fail(RhiErrorCode::InvalidArgument, "CreateTexture", desc.debugName, "D3D11 cube resources require six faces");
    if (desc.sampleCount != 1)
        Fail(RhiErrorCode::Unsupported, "CreateTexture", desc.debugName,
             "the M6 D3D11 adapter supports single-sample views only");

    D3D11_TEXTURE2D_DESC native{};
    native.Width = desc.extent.width;
    native.Height = desc.extent.height;
    native.MipLevels = desc.mipLevels;
    native.ArraySize = desc.dimension == TextureDimension::TextureCube ? 6 : desc.arrayLayers;
    native.Format = DxgiFormat(desc.format, HasFlag(desc.usage, TextureUsage::DepthStencil));
    native.SampleDesc = {1, 0};
    native.Usage = D3D11_USAGE_DEFAULT;
    native.BindFlags = 0;
    if (HasFlag(desc.usage, TextureUsage::Sampled))
        native.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (HasFlag(desc.usage, TextureUsage::ColorAttachment))
        native.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (HasFlag(desc.usage, TextureUsage::DepthStencil))
        native.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    if (desc.dimension == TextureDimension::TextureCube)
        native.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

    auto payload = std::make_unique<TexturePayload>(m_counters, desc);
    payload->nativeFormat = native.Format;
    Check(m_device->CreateTexture2D(&native, nullptr, &payload->texture), "ID3D11Device::CreateTexture2D");
    NameNativeObject(payload->texture.Get(), desc.debugName);
    CreateTextureViews(*payload);
    return payload;
}

void D3D11RhiBackend::CreateTextureViews(TexturePayload& payload)
{
    const auto& desc = payload.desc;
    if (HasFlag(desc.usage, TextureUsage::Sampled))
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = ShaderResourceFormat(desc.format);
        if (desc.dimension == TextureDimension::TextureCube)
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MostDetailedMip = 0;
            view.TextureCube.MipLevels = desc.mipLevels;
        }
        else
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MostDetailedMip = 0;
            view.Texture2D.MipLevels = desc.mipLevels;
        }
        Check(m_device->CreateShaderResourceView(payload.texture.Get(), &view, &payload.srv),
              "ID3D11Device::CreateShaderResourceView");
        NameNativeObject(payload.srv.Get(), desc.debugName);
    }
    if (HasFlag(desc.usage, TextureUsage::ColorAttachment))
    {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DxgiFormat(desc.format);
        if (desc.arrayLayers == 1)
        {
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = 0;
        }
        else
        {
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipSlice = 0;
            view.Texture2DArray.ArraySize = desc.arrayLayers;
        }
        Check(m_device->CreateRenderTargetView(payload.texture.Get(), &view, &payload.rtv),
              "ID3D11Device::CreateRenderTargetView");
        NameNativeObject(payload.rtv.Get(), desc.debugName);
    }
    if (HasFlag(desc.usage, TextureUsage::DepthStencil))
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DxgiFormat(desc.format);
        if (desc.arrayLayers == 1)
        {
            view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = 0;
        }
        else
        {
            view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipSlice = 0;
            view.Texture2DArray.ArraySize = desc.arrayLayers;
        }
        Check(m_device->CreateDepthStencilView(payload.texture.Get(), &view, &payload.dsv),
              "ID3D11Device::CreateDepthStencilView");
        NameNativeObject(payload.dsv.Get(), desc.debugName);
    }
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateSampler(const SamplerDesc& desc)
{
    D3D11_SAMPLER_DESC native{};
    native.Filter = FilterMode(desc);
    native.AddressU = Address(desc.addressU);
    native.AddressV = Address(desc.addressV);
    native.AddressW = Address(desc.addressW);
    native.MipLODBias = 0.0F;
    native.MaxAnisotropy = static_cast<UINT>(std::clamp(desc.maxAnisotropy, 1.0F, m_capabilities.maxAnisotropy));
    native.ComparisonFunc = Compare(desc.comparison);
    std::copy(desc.borderColor.begin(), desc.borderColor.end(), native.BorderColor);
    native.MinLOD = desc.minLod;
    native.MaxLOD = desc.maxLod;
    auto payload = std::make_unique<SamplerPayload>(m_counters, desc);
    Check(m_device->CreateSamplerState(&native, &payload->sampler), "ID3D11Device::CreateSamplerState");
    NameNativeObject(payload->sampler.Get(), desc.debugName);
    return payload;
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateShader(const ShaderDesc& desc)
{
    if (desc.bytecode.empty())
        Fail(RhiErrorCode::InvalidArgument, "CreateShader", desc.debugName, "D3D11 shader bytecode must be present");
    auto payload = std::make_unique<ShaderPayload>(m_counters, desc);
    if (desc.stage == ShaderStage::Vertex)
    {
        Check(
            m_device->CreateVertexShader(payload->bytecode.data(), payload->bytecode.size(), nullptr, &payload->vertex),
            "ID3D11Device::CreateVertexShader");
        NameNativeObject(payload->vertex.Get(), desc.debugName);
    }
    else if (desc.stage == ShaderStage::Pixel)
    {
        Check(m_device->CreatePixelShader(payload->bytecode.data(), payload->bytecode.size(), nullptr, &payload->pixel),
              "ID3D11Device::CreatePixelShader");
        NameNativeObject(payload->pixel.Get(), desc.debugName);
    }
    else
    {
        Fail(RhiErrorCode::Unsupported, "CreateShader", desc.debugName, "only vertex and pixel shaders are supported");
    }
    return payload;
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateResourceSetLayout(const ResourceSetLayoutDesc& desc)
{
    return std::make_unique<ResourceSetLayoutPayload>(m_counters, desc);
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateResourceSet(const ResourceSetDesc& desc)
{
    return std::make_unique<ResourceSetPayload>(m_counters, desc);
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreatePipelineLayout(const PipelineLayoutDesc& desc)
{
    return std::make_unique<PipelineLayoutPayload>(m_counters, desc);
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateTimestampQuery(std::string_view name)
{
    D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC timestampDesc{D3D11_QUERY_TIMESTAMP, 0};
    auto payload = std::make_unique<TimestampPayload>(m_counters);
    Check(m_device->CreateQuery(&disjointDesc, &payload->disjoint), "ID3D11Device::CreateQuery(disjoint)");
    Check(m_device->CreateQuery(&timestampDesc, &payload->timestamp), "ID3D11Device::CreateQuery(timestamp)");
    NameNativeObject(payload->disjoint.Get(), name);
    NameNativeObject(payload->timestamp.Get(), name);
    return payload;
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    if (!m_owner)
        Fail(RhiErrorCode::InvalidState, "CreateGraphicsPipeline", desc.debugName,
             "backend is not attached to its owner");
    auto& vertex = PayloadAs<ShaderPayload>(m_owner->Payload(desc.vertexShader), "CreateGraphicsPipeline");
    if (vertex.desc.stage != ShaderStage::Vertex)
        Fail(RhiErrorCode::InvalidArgument, "CreateGraphicsPipeline", desc.debugName, "vertex shader stage is wrong");
    ValidateNativeProfile(vertex.desc);
    ShaderPayload* pixel = nullptr;
    if (desc.pixelShader)
    {
        pixel = &PayloadAs<ShaderPayload>(m_owner->Payload(desc.pixelShader), "CreateGraphicsPipeline");
        if (pixel->desc.stage != ShaderStage::Pixel)
            Fail(RhiErrorCode::InvalidArgument, "CreateGraphicsPipeline", desc.debugName,
                 "pixel shader stage is wrong");
        ValidateNativeProfile(pixel->desc);
    }

    auto payload = std::make_unique<GraphicsPipelinePayload>(m_counters, desc);
    payload->vertex = vertex.vertex;
    if (pixel)
        payload->pixel = pixel->pixel;

    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    elements.reserve(desc.vertexAttributes.size());
    auto semanticName = [](VertexSemantic semantic) -> const char*
    {
        switch (semantic)
        {
        case VertexSemantic::Position:
            return "POSITION";
        case VertexSemantic::Normal:
            return "NORMAL";
        case VertexSemantic::Tangent:
            return "TANGENT";
        case VertexSemantic::TexCoord:
            return "TEXCOORD";
        case VertexSemantic::Color:
            return "COLOR";
        }
        return "POSITION";
    };
    auto vertexFormat = [](VertexFormat format) -> DXGI_FORMAT
    {
        switch (format)
        {
        case VertexFormat::Float2:
            return DXGI_FORMAT_R32G32_FLOAT;
        case VertexFormat::Float3:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexFormat::Float4:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        return DXGI_FORMAT_UNKNOWN;
    };
    for (const auto& attribute : desc.vertexAttributes)
    {
        D3D11_INPUT_ELEMENT_DESC native{};
        native.SemanticName = semanticName(attribute.semantic);
        native.SemanticIndex = attribute.semanticIndex;
        native.Format = vertexFormat(attribute.format);
        native.InputSlot = attribute.bufferSlot;
        native.AlignedByteOffset = attribute.offset;
        native.InputSlotClass =
            attribute.instanceStepRate ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        native.InstanceDataStepRate = attribute.instanceStepRate;
        elements.push_back(native);
    }
    if (!elements.empty())
    {
        Check(m_device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), vertex.bytecode.data(),
                                          vertex.bytecode.size(), &payload->inputLayout),
              "ID3D11Device::CreateInputLayout");
        NameNativeObject(payload->inputLayout.Get(), desc.debugName);
    }

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = FillMode();
    raster.CullMode = desc.cullMode == CullMode::None    ? D3D11_CULL_NONE
                      : desc.cullMode == CullMode::Front ? D3D11_CULL_FRONT
                                                         : D3D11_CULL_BACK;
    raster.FrontCounterClockwise = desc.frontFace == FrontFace::CounterClockwise;
    raster.DepthBias = desc.depthBias;
    raster.DepthBiasClamp = desc.depthBiasClamp;
    raster.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
    raster.DepthClipEnable = desc.depthClip;
    Check(m_device->CreateRasterizerState(&raster, &payload->rasterizer), "ID3D11Device::CreateRasterizerState");
    NameNativeObject(payload->rasterizer.Get(), desc.debugName);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = desc.depthTest;
    depth.DepthWriteMask = desc.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = Compare(desc.depthCompare);
    depth.StencilEnable = FALSE;
    Check(m_device->CreateDepthStencilState(&depth, &payload->depthStencil), "ID3D11Device::CreateDepthStencilState");
    NameNativeObject(payload->depthStencil.Get(), desc.debugName);

    D3D11_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    auto& target = blend.RenderTarget[0];
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    target.BlendEnable = desc.alphaBlend;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    Check(m_device->CreateBlendState(&blend, &payload->blend), "ID3D11Device::CreateBlendState");
    NameNativeObject(payload->blend.Get(), desc.debugName);
    return payload;
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateSwapChain(const SwapChainDesc& desc)
{
    if (!desc.nativeWindow)
        Fail(RhiErrorCode::InvalidArgument, "CreateSwapChain", desc.debugName,
             "nativeWindow is required for an HWND swap chain");
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    Check(m_device.As(&dxgiDevice), "ID3D11Device::QueryInterface(IDXGIDevice)");
    Check(dxgiDevice->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");
    Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIAdapter::GetParent(IDXGIFactory2)");

    DXGI_SWAP_CHAIN_DESC1 native{};
    native.Width = desc.extent.width;
    native.Height = desc.extent.height;
    native.Format = DxgiFormat(desc.format);
    native.Stereo = FALSE;
    native.SampleDesc = {1, 0};
    native.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    native.BufferCount = std::max<std::uint8_t>(2, desc.bufferCount);
    native.Scaling = DXGI_SCALING_STRETCH;
    native.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    native.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    native.Flags = 0;

    ComPtr<IDXGISwapChain1> chain1;
    Check(factory->CreateSwapChainForHwnd(m_device.Get(), static_cast<HWND>(desc.nativeWindow), &native, nullptr,
                                          nullptr, &chain1),
          "IDXGIFactory2::CreateSwapChainForHwnd");
    auto payload = std::make_unique<SwapChainPayload>(m_counters, desc);
    Check(chain1.As(&payload->swapChain), "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)");
    if (payload->swapChain.Get() == nullptr)
        Fail(RhiErrorCode::Unsupported, "CreateSwapChain", desc.debugName,
             "DXGI flip-model index query is unavailable");
    return payload;
}

std::vector<DeviceLifetime::BackBufferCandidate> D3D11RhiBackend::ResizeBackBuffers(ResourcePayload& chain,
                                                                                    const SwapChainDesc& desc)
{
    auto& payload = PayloadAs<SwapChainPayload>(chain, "ResizeBackBuffers");
    if (desc.extent.width == 0 || desc.extent.height == 0)
        return {};
    if (payload.swapChain.Get() == nullptr)
        Fail(RhiErrorCode::InvalidState, "ResizeBackBuffers", desc.debugName, "swap chain is not initialized");
    Check(payload.swapChain->ResizeBuffers(std::max<std::uint8_t>(2, desc.bufferCount), desc.extent.width,
                                           desc.extent.height, DxgiFormat(desc.format), 0),
          "IDXGISwapChain::ResizeBuffers");
    std::vector<DeviceLifetime::BackBufferCandidate> candidates;
    candidates.reserve(std::max<std::uint8_t>(2, desc.bufferCount));
    const TextureDesc textureDesc{TextureDimension::Texture2D,
                                  desc.extent,
                                  1,
                                  1,
                                  1,
                                  desc.format,
                                  TextureUsage::ColorAttachment | TextureUsage::CopySource,
                                  desc.debugName};
    for (UINT index = 0; index < std::max<std::uint8_t>(2, desc.bufferCount); ++index)
    {
        auto candidate = std::make_unique<TexturePayload>(m_counters, textureDesc);
        candidate->nativeFormat = DxgiFormat(desc.format);
        // D3D11 由 runtime 自动轮换 buffer 0 的身份；它不是 D3D12 的物理 buffer 数组。
        // 公共三个 acquire 槽借用同一轮换接口，只允许当前 FrameToken 的 backBuffer 被消费。
        Check(payload.swapChain->GetBuffer(0, IID_PPV_ARGS(&candidate->texture)), "IDXGISwapChain::GetBuffer");
        NameNativeObject(candidate->texture.Get(), desc.debugName);
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DxgiFormat(desc.format);
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        Check(m_device->CreateRenderTargetView(candidate->texture.Get(), &view, &candidate->rtv),
              "ID3D11Device::CreateRenderTargetView(backbuffer)");
        NameNativeObject(candidate->rtv.Get(), desc.debugName);
        candidates.push_back({textureDesc, std::move(candidate)});
    }
    return candidates;
}

std::uint32_t D3D11RhiBackend::CurrentBackBufferIndex(ResourcePayload& chain)
{
    auto& payload = PayloadAs<SwapChainPayload>(chain, "CurrentBackBufferIndex");
    if (payload.swapChain.Get() == nullptr)
        Fail(RhiErrorCode::InvalidState, "CurrentBackBufferIndex", "SwapChain", "swap chain is not initialized");
    return payload.swapChain->GetCurrentBackBufferIndex();
}

void D3D11RhiBackend::BeginFrame(const FrameToken& frame, ResourcePayload& chain)
{
    if (m_frame)
        Fail(RhiErrorCode::InvalidState, "BeginFrame", "Frame", "a D3D11 frame is already active");
    auto& payload = PayloadAs<SwapChainPayload>(chain, "BeginFrame");
    if (payload.swapChain.Get() == nullptr)
        Fail(RhiErrorCode::InvalidState, "BeginFrame", "SwapChain", "swap chain is not initialized");
    ClearContext();
    m_frame = frame;
    m_frameChain = &payload;
    m_frameDynamicBytes = 0;
    m_frameQueries.clear();
    m_frameReadbacks.clear();
    if (m_capabilities.debugLayerEnabled && m_trace.size() < 262144)
        m_trace += "begin=" + std::to_string(frame.serial) + ":lane=" + std::to_string(frame.recycleLane) + "\n";
}

void D3D11RhiBackend::ClearContext()
{
    if (m_context.Get() != nullptr)
        m_context->ClearState();
    for (auto& view : m_vsSrvs)
        view.Reset();
    for (auto& view : m_psSrvs)
        view.Reset();
    for (auto& binding : m_vsConstants)
        binding = {};
    for (auto& binding : m_psConstants)
        binding = {};
    for (auto& sampler : m_vsSamplers)
        sampler.Reset();
    for (auto& sampler : m_psSamplers)
        sampler.Reset();
    for (auto& view : m_rtvs)
        view.Reset();
    m_dsv.Reset();
    m_outputStores.clear();
    m_outputCount = 0;
    m_rendering = false;
    m_currentPipeline = nullptr;
    m_faultSuppressedResource = nullptr;
}

void D3D11RhiBackend::UnbindShaderReads(ID3D11Resource* resource)
{
    if (!resource)
        return;
    // 注入后同一资源保持冲突绑定，直到它再次作为 attachment 绑定；由 Debug Layer 捕获。
    if (m_faultSuppressedResource == resource)
        return;
    for (UINT slot = 0; slot < m_vsSrvs.size(); ++slot)
    {
        if (!m_vsSrvs[slot])
            continue;
        ComPtr<ID3D11Resource> bound;
        m_vsSrvs[slot]->GetResource(&bound);
        if (bound.Get() == resource)
        {
            if (m_skipNextHazardUnbind)
            {
                // 测试专用负向通道：关闭该资源的 hazard unbind 保护，让 Debug Layer 精确捕获。
                m_skipNextHazardUnbind = false;
                m_faultSuppressedResource = resource;
                ++m_injectedFaults;
                m_trace += "injected-fault=skip-next-hazard-unbind\n";
                return;
            }
            ID3D11ShaderResourceView* nullView = nullptr;
            m_context->VSSetShaderResources(slot, 1, &nullView);
            m_vsSrvs[slot].Reset();
            ++m_explicitUnbinds;
        }
    }
    for (UINT slot = 0; slot < m_psSrvs.size(); ++slot)
    {
        if (!m_psSrvs[slot])
            continue;
        ComPtr<ID3D11Resource> bound;
        m_psSrvs[slot]->GetResource(&bound);
        if (bound.Get() == resource)
        {
            if (m_skipNextHazardUnbind)
            {
                m_skipNextHazardUnbind = false;
                m_faultSuppressedResource = resource;
                ++m_injectedFaults;
                m_trace += "injected-fault=skip-next-hazard-unbind\n";
                return;
            }
            ID3D11ShaderResourceView* nullView = nullptr;
            m_context->PSSetShaderResources(slot, 1, &nullView);
            m_psSrvs[slot].Reset();
            ++m_explicitUnbinds;
        }
    }
}

void D3D11RhiBackend::UnbindOutputs(ID3D11Resource* resource)
{
    if (!resource)
        return;
    bool changed = false;
    for (UINT slot = 0; slot < m_outputCount; ++slot)
    {
        if (!m_rtvs[slot])
            continue;
        ComPtr<ID3D11Resource> bound;
        m_rtvs[slot]->GetResource(&bound);
        if (bound.Get() == resource)
        {
            if (m_skipNextHazardUnbind)
            {
                // 测试专用负向通道：故意保留冲突的 output 绑定，让 Debug Layer 精确捕获
                // hazard；正常 sample/pass 不设置该开关。
                m_skipNextHazardUnbind = false;
                ++m_injectedFaults;
                m_trace += "injected-fault=skip-next-hazard-unbind\n";
                continue;
            }
            m_rtvs[slot].Reset();
            ++m_explicitUnbinds;
            changed = true;
        }
    }
    if (m_dsv)
    {
        ComPtr<ID3D11Resource> bound;
        m_dsv->GetResource(&bound);
        if (bound.Get() == resource)
        {
            m_dsv.Reset();
            ++m_explicitUnbinds;
            changed = true;
        }
    }
    if (changed)
    {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
        for (UINT slot = 0; slot < m_outputCount; ++slot)
            outputs[slot] = m_rtvs[slot].Get();
        m_context->OMSetRenderTargets(m_outputCount, outputs.data(), m_dsv.Get());
    }
}

void D3D11RhiBackend::Discard(ID3D11View* view)
{
    if (!view)
        return;
    if (m_context1.Get() != nullptr)
    {
        m_context1->DiscardView(view);
    }
    else
    {
        // D3D11.0 没有 discard-view API；DontCare 的正确性保持不变，
        // 此优化记为 no-op，并由诊断计数明确记录。
        ++m_discardNoOps;
    }
}

std::uint32_t D3D11RhiBackend::ConstantCount(std::uint64_t bytes) const
{
    if (bytes == 0 || bytes > static_cast<std::uint64_t>(std::numeric_limits<UINT>::max()) * 16ULL)
        Fail(RhiErrorCode::InvalidArgument, "BindResourceSet", "UniformBuffer",
             "constant range is outside D3D11 limits");
    return static_cast<std::uint32_t>(((bytes + 255) / 256) * 16);
}

void D3D11RhiBackend::SetConstantBuffer(ShaderStage visibility, std::uint32_t slot, ID3D11Buffer* buffer,
                                        std::uint32_t firstConstant, std::uint32_t numConstants)
{
    if (firstConstant != 0 && !m_context1)
        Fail(RhiErrorCode::Unsupported, "BindResourceSet", "UniformBuffer",
             "D3D11.1 constant-buffer offset API is required for a nonzero dynamic offset");
    if (HasFlag(visibility, ShaderStage::Vertex) &&
        (m_vsConstants.at(slot).buffer.Get() != buffer || m_vsConstants[slot].first != firstConstant ||
         m_vsConstants[slot].count != numConstants))
    {
        if (m_context1.Get() != nullptr)
            m_context1->VSSetConstantBuffers1(slot, 1, &buffer, &firstConstant, &numConstants);
        else
            m_context->VSSetConstantBuffers(slot, 1, &buffer);
        m_vsConstants[slot] = {buffer, firstConstant, numConstants};
    }
    if (HasFlag(visibility, ShaderStage::Pixel) &&
        (m_psConstants.at(slot).buffer.Get() != buffer || m_psConstants[slot].first != firstConstant ||
         m_psConstants[slot].count != numConstants))
    {
        if (m_context1.Get() != nullptr)
            m_context1->PSSetConstantBuffers1(slot, 1, &buffer, &firstConstant, &numConstants);
        else
            m_context->PSSetConstantBuffers(slot, 1, &buffer);
        m_psConstants[slot] = {buffer, firstConstant, numConstants};
    }
}

void D3D11RhiBackend::BindSet(const ResourceSetPayload& set, std::span<const std::uint32_t> offsets)
{
    if (!m_owner)
        Fail(RhiErrorCode::InvalidState, "BindResourceSet", "ResourceSet", "backend is not attached to its owner");
    const auto& layout = m_owner->Describe(set.desc.layout);
    std::size_t dynamicIndex = 0;
    for (const auto& entry : layout.entries)
    {
        for (std::uint16_t arrayElement = 0; arrayElement < entry.count; ++arrayElement)
        {
            const auto lowered = LowerBinding(layout.set, entry.binding, entry.type);
            if (!lowered)
                Fail(RhiErrorCode::Unsupported, "BindResourceSet",
                     entry.type == BindingType::Sampler ? "Sampler" : "Binding",
                     "logical binding is absent from the frozen D3D11 register manifest");
            if (lowered->slot + arrayElement >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
                Fail(RhiErrorCode::Unsupported, "BindResourceSet", "Binding",
                     "native register exceeds D3D11 slot limits");
            const auto found = std::find_if(set.desc.bindings.begin(), set.desc.bindings.end(),
                                            [&](const ResourceBinding& value)
                                            {
                                                return value.binding == entry.binding &&
                                                       value.arrayElement == arrayElement && value.type == entry.type;
                                            });
            if (found == set.desc.bindings.end())
                continue;
            if (entry.dynamicOffset)
            {
                if (dynamicIndex >= offsets.size())
                    Fail(RhiErrorCode::InvalidArgument, "BindResourceSet", "ResourceSet", "missing dynamic offset");
            }
            const std::uint64_t dynamicOffset = entry.dynamicOffset ? offsets[dynamicIndex++] : 0;
            const UINT slot = lowered->slot + arrayElement;
            if (entry.type == BindingType::UniformBuffer)
            {
                auto& buffer = PayloadAs<BufferPayload>(m_owner->Payload(found->buffer.buffer), "BindResourceSet");
                if (buffer.buffer.Get() == nullptr)
                    Fail(RhiErrorCode::InvalidState, "BindResourceSet", "UniformBuffer", "buffer has no native object");
                if (found->buffer.offset > std::numeric_limits<std::uint64_t>::max() - dynamicOffset)
                    Fail(RhiErrorCode::InvalidArgument, "BindResourceSet", "UniformBuffer", "dynamic offset overflow");
                const auto byteOffset = found->buffer.offset + dynamicOffset;
                if (byteOffset > buffer.desc.size || found->buffer.size > buffer.desc.size - byteOffset)
                    Fail(RhiErrorCode::InvalidArgument, "BindResourceSet", "UniformBuffer",
                         "constant range is outside buffer");
                const auto first = static_cast<std::uint32_t>(byteOffset / 16);
                const auto count = ConstantCount(entry.uniformBytes ? entry.uniformBytes : found->buffer.size);
                SetConstantBuffer(entry.visibility, slot, buffer.buffer.Get(), first, count);
            }
            else if (entry.type == BindingType::SampledTexture)
            {
                auto& texture = PayloadAs<TexturePayload>(m_owner->Payload(found->texture), "BindResourceSet");
                if (texture.srv.Get() == nullptr)
                    Fail(RhiErrorCode::InvalidState, "BindResourceSet", "Texture",
                         "texture has no shader-resource view");
                UnbindOutputs(texture.texture.Get());
                if (HasFlag(entry.visibility, ShaderStage::Vertex))
                {
                    m_context->VSSetShaderResources(slot, 1, texture.srv.GetAddressOf());
                    m_vsSrvs[slot] = texture.srv;
                }
                if (HasFlag(entry.visibility, ShaderStage::Pixel))
                {
                    m_context->PSSetShaderResources(slot, 1, texture.srv.GetAddressOf());
                    m_psSrvs[slot] = texture.srv;
                }
            }
            else
            {
                auto& sampler = PayloadAs<SamplerPayload>(m_owner->Payload(found->sampler), "BindResourceSet");
                if (sampler.sampler.Get() == nullptr)
                    Fail(RhiErrorCode::InvalidState, "BindResourceSet", "Sampler", "sampler has no native object");
                if (HasFlag(entry.visibility, ShaderStage::Vertex) &&
                    m_vsSamplers.at(slot).Get() != sampler.sampler.Get())
                {
                    m_context->VSSetSamplers(slot, 1, sampler.sampler.GetAddressOf());
                    m_vsSamplers[slot] = sampler.sampler;
                }
                if (HasFlag(entry.visibility, ShaderStage::Pixel) &&
                    m_psSamplers.at(slot).Get() != sampler.sampler.Get())
                {
                    m_context->PSSetSamplers(slot, 1, sampler.sampler.GetAddressOf());
                    m_psSamplers[slot] = sampler.sampler;
                }
            }
        }
    }
}

void D3D11RhiBackend::Consume(const CommandEvent& event)
{
    if (!m_frame)
        Fail(RhiErrorCode::InvalidState, event.operation, "Frame", "command received outside an active frame");
    if (event.operation == "BeginLabel")
    {
        if (m_annotation.Get() != nullptr)
        {
            const auto name = Wide(event.text);
            m_annotation->BeginEvent(name.c_str());
        }
        // GPU zone 与 pass label 一一对应：名字来自图/Pass 的稳定名称。
        if (m_gpuProfiler)
            m_gpuProfiler->BeginZone(event.text.c_str());
        return;
    }
    if (event.operation == "EndLabel")
    {
        if (m_gpuProfiler)
            m_gpuProfiler->EndZone();
        if (m_annotation.Get() != nullptr)
            m_annotation->EndEvent();
        return;
    }
    if (event.operation == "ImportResources" || event.operation == "EndGraphics")
        return;
    if (event.operation == "BeginRendering")
    {
        if (m_rendering || event.integers.size() < 4)
            Fail(RhiErrorCode::InvalidState, event.operation, "Rendering",
                 "rendering scope is already active or malformed");
        const UINT colorCount = static_cast<UINT>(event.integers[2]);
        const bool hasDepth = event.integers[3] != 0;
        if (colorCount > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ||
            event.resources.size() != colorCount + (hasDepth ? 1 : 0))
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Rendering",
                 "attachment count is outside D3D11 limits");
        m_outputCount = colorCount;
        m_outputStores.clear();
        m_outputStores.reserve(colorCount);
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
        for (UINT i = 0; i < colorCount; ++i)
        {
            auto& texture = PayloadAs<TexturePayload>(m_owner->Payload(event.resources[i]), event.operation.c_str());
            if (!texture.rtv)
                Fail(RhiErrorCode::InvalidArgument, event.operation, "Texture", "color attachment has no RTV");
            UnbindShaderReads(texture.texture.Get());
            if (m_faultSuppressedResource == texture.texture.Get())
                m_faultSuppressedResource = nullptr;
            m_rtvs[i] = texture.rtv;
            outputs[i] = m_rtvs[i].Get();
            const auto integer = 4 + static_cast<std::size_t>(i) * 2;
            if (integer + 1 >= event.integers.size())
                Fail(RhiErrorCode::InvalidArgument, event.operation, "Rendering",
                     "color load/store payload is malformed");
            const auto load = static_cast<LoadOp>(event.integers[integer]);
            const auto store = static_cast<StoreOp>(event.integers[integer + 1]);
            m_outputStores.push_back(store);
            if (load == LoadOp::Clear)
            {
                if (event.scalars.size() < static_cast<std::size_t>(i + 1) * 4)
                    Fail(RhiErrorCode::InvalidArgument, event.operation, "Rendering",
                         "clear color payload is malformed");
                m_context->ClearRenderTargetView(texture.rtv.Get(), event.scalars.data() + i * 4);
            }
            else if (load == LoadOp::DontCare)
            {
                Discard(texture.rtv.Get());
            }
        }
        m_dsv.Reset();
        if (hasDepth)
        {
            auto& depth =
                PayloadAs<TexturePayload>(m_owner->Payload(event.resources[colorCount]), event.operation.c_str());
            if (!depth.dsv)
                Fail(RhiErrorCode::InvalidArgument, event.operation, "Texture", "depth attachment has no DSV");
            UnbindShaderReads(depth.texture.Get());
            if (m_faultSuppressedResource == depth.texture.Get())
                m_faultSuppressedResource = nullptr;
            m_dsv = depth.dsv;
            const auto integer = 4 + static_cast<std::size_t>(colorCount) * 2;
            if (integer + 2 >= event.integers.size() || event.scalars.size() < colorCount * 4 + 1)
                Fail(RhiErrorCode::InvalidArgument, event.operation, "Rendering",
                     "depth load/store payload is malformed");
            const auto load = static_cast<LoadOp>(event.integers[integer]);
            if (load == LoadOp::Clear)
                m_context->ClearDepthStencilView(depth.dsv.Get(), D3D11_CLEAR_DEPTH, event.scalars[colorCount * 4], 0);
            else if (load == LoadOp::DontCare)
                Discard(depth.dsv.Get());
        }
        m_context->OMSetRenderTargets(colorCount, outputs.data(), m_dsv.Get());
        m_rendering = true;
        return;
    }
    if (event.operation == "EndRendering")
    {
        if (!m_rendering)
            Fail(RhiErrorCode::InvalidState, event.operation, "Rendering", "rendering scope is not active");
        for (UINT i = 0; i < m_outputCount; ++i)
            if (m_outputStores[i] == StoreOp::DontCare)
                Discard(m_rtvs[i].Get());
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        for (auto& view : m_rtvs)
            view.Reset();
        m_dsv.Reset();
        m_outputCount = 0;
        m_outputStores.clear();
        m_rendering = false;
        return;
    }
    if (event.operation == "SetPipeline")
    {
        auto& pipeline =
            PayloadAs<GraphicsPipelinePayload>(m_owner->Payload(event.resources.at(0)), event.operation.c_str());
        if (m_currentPipeline == &pipeline)
            return;
        m_context->IASetInputLayout(pipeline.inputLayout.Get());
        m_context->VSSetShader(pipeline.vertex.Get(), nullptr, 0);
        m_context->PSSetShader(pipeline.pixel.Get(), nullptr, 0);
        m_context->RSSetState(pipeline.rasterizer.Get());
        m_context->OMSetDepthStencilState(pipeline.depthStencil.Get(), 0);
        const float blendFactor[4] = {0, 0, 0, 0};
        m_context->OMSetBlendState(pipeline.blend.Get(), blendFactor, 0xffffffffU);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_currentPipeline = &pipeline;
        return;
    }
    if (event.operation == "SetViewport")
    {
        if (event.scalars.size() != 6)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Viewport", "viewport payload is malformed");
        D3D11_VIEWPORT viewport{event.scalars[0], event.scalars[1], event.scalars[2],
                                event.scalars[3], event.scalars[4], event.scalars[5]};
        m_context->RSSetViewports(1, &viewport);
        return;
    }
    if (event.operation == "SetScissor")
    {
        if (event.integers.size() != 4 || event.integers[0] > std::numeric_limits<LONG>::max() ||
            event.integers[1] > std::numeric_limits<LONG>::max())
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Scissor", "scissor payload is malformed");
        D3D11_RECT rect{static_cast<LONG>(event.integers[0]), static_cast<LONG>(event.integers[1]),
                        static_cast<LONG>(event.integers[0] + event.integers[2]),
                        static_cast<LONG>(event.integers[1] + event.integers[3])};
        m_context->RSSetScissorRects(1, &rect);
        return;
    }
    if (event.operation == "BindVertexBuffer")
    {
        if (event.resources.size() != 1 || event.integers.size() != 4)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "VertexBuffer", "vertex binding payload is malformed");
        auto& buffer = PayloadAs<BufferPayload>(m_owner->Payload(event.resources[0]), event.operation.c_str());
        const UINT slot = static_cast<UINT>(event.integers[0]);
        const UINT offset = static_cast<UINT>(event.integers[1]);
        const UINT stride = static_cast<UINT>(event.integers[3]);
        ID3D11Buffer* native = buffer.buffer.Get();
        m_context->IASetVertexBuffers(slot, 1, &native, &stride, &offset);
        return;
    }
    if (event.operation == "BindIndexBuffer")
    {
        if (event.resources.size() != 1 || event.integers.size() != 3)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "IndexBuffer", "index binding payload is malformed");
        auto& buffer = PayloadAs<BufferPayload>(m_owner->Payload(event.resources[0]), event.operation.c_str());
        const auto type = static_cast<IndexType>(event.integers[2]);
        m_context->IASetIndexBuffer(buffer.buffer.Get(),
                                    type == IndexType::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
                                    static_cast<UINT>(event.integers[0]));
        return;
    }
    if (event.operation == "BindResourceSet")
    {
        if (event.resources.size() != 1 || event.integers.empty())
            Fail(RhiErrorCode::InvalidArgument, event.operation, "ResourceSet", "resource-set payload is malformed");
        auto& set = PayloadAs<ResourceSetPayload>(m_owner->Payload(event.resources[0]), event.operation.c_str());
        std::vector<std::uint32_t> offsets;
        offsets.reserve(event.integers.size() - 1);
        for (std::size_t i = 1; i < event.integers.size(); ++i)
        {
            if (event.integers[i] > std::numeric_limits<std::uint32_t>::max())
                Fail(RhiErrorCode::InvalidArgument, event.operation, "ResourceSet",
                     "dynamic offset exceeds UINT range");
            offsets.push_back(static_cast<std::uint32_t>(event.integers[i]));
        }
        BindSet(set, offsets);
        return;
    }
    if (event.operation == "Draw")
    {
        if (event.integers.size() != 4)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Draw", "draw payload is malformed");
        m_context->DrawInstanced(static_cast<UINT>(event.integers[0]), static_cast<UINT>(event.integers[1]),
                                 static_cast<UINT>(event.integers[2]), static_cast<UINT>(event.integers[3]));
        return;
    }
    if (event.operation == "DrawIndexed")
    {
        if (event.integers.size() != 5)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "DrawIndexed", "indexed draw payload is malformed");
        m_context->DrawIndexedInstanced(static_cast<UINT>(event.integers[0]), static_cast<UINT>(event.integers[1]),
                                        static_cast<UINT>(event.integers[2]), static_cast<INT>(event.integers[3]),
                                        static_cast<UINT>(event.integers[4]));
        return;
    }
    if (event.operation == "CopyBuffer")
    {
        if (event.resources.size() != 2 || event.integers.size() != 5)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Buffer", "copy payload is malformed");
        auto& source = PayloadAs<BufferPayload>(m_owner->Payload(event.resources[0]), event.operation.c_str());
        auto& destination = PayloadAs<BufferPayload>(m_owner->Payload(event.resources[1]), event.operation.c_str());
        const auto sourceOffset = event.integers[0];
        const auto destinationOffset = event.integers[2];
        const auto size = event.integers[4];
        if (sourceOffset > source.desc.size || size > source.desc.size - sourceOffset ||
            destinationOffset > destination.desc.size || size > destination.desc.size - destinationOffset ||
            sourceOffset > std::numeric_limits<UINT>::max() || destinationOffset > std::numeric_limits<UINT>::max() ||
            size > std::numeric_limits<UINT>::max())
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Buffer", "copy range is outside D3D11 limits");
        D3D11_BOX box{static_cast<UINT>(sourceOffset), 0, 0, static_cast<UINT>(sourceOffset + size), 1, 1};
        m_context->CopySubresourceRegion(destination.buffer.Get(), 0, static_cast<UINT>(destinationOffset), 0, 0,
                                         source.buffer.Get(), 0, &box);
        if (destination.readback)
            destination.readback->valid = false;
        return;
    }
    if (event.operation == "CopyTextureForReadback")
    {
        if (event.resources.size() != 2 || event.integers.size() != 2)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Readback", "texture readback payload is malformed");
        auto& source = PayloadAs<TexturePayload>(m_owner->Payload(event.resources[0]), event.operation.c_str());
        auto& destination = PayloadAs<BufferPayload>(m_owner->Payload(event.resources[1]), event.operation.c_str());
        const Extent2D extent{static_cast<std::uint32_t>(event.integers[0]),
                              static_cast<std::uint32_t>(event.integers[1])};
        if (source.texture.Get() == nullptr || extent.width == 0 || extent.height == 0 ||
            extent.width > source.desc.extent.width || extent.height > source.desc.extent.height ||
            source.desc.sampleCount != 1)
            Fail(RhiErrorCode::Unsupported, event.operation, "Readback",
                 "texture readback is outside the single-sample D3D11 profile");
        if (destination.readback)
        {
            if (!destination.readback->completionSerial || destination.readback->completionSerial > m_completedSerial)
            {
                if (!destination.readback->completionSerial)
                    destination.readback->completionSerial = destination.readback->frameSerial;
                m_retiredReadbacks.push_back(std::move(*destination.readback));
            }
            destination.readback.reset();
        }
        UnbindShaderReads(source.texture.Get());
        UnbindOutputs(source.texture.Get());
        D3D11_TEXTURE2D_DESC native{};
        source.texture->GetDesc(&native);
        native.Width = extent.width;
        native.Height = extent.height;
        native.MipLevels = 1;
        native.ArraySize = 1;
        native.SampleDesc = {1, 0};
        native.Usage = D3D11_USAGE_STAGING;
        native.BindFlags = 0;
        native.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        native.MiscFlags = 0;
        ReadbackInfo info;
        info.hold = NativeHold(m_counters, true);
        info.source = std::get<TextureHandle>(event.resources[0]);
        info.desc = source.desc;
        info.extent = extent;
        info.frameSerial = m_frame->serial;
        Check(m_device->CreateTexture2D(&native, nullptr, &info.staging), "ID3D11Device::CreateTexture2D(readback)");
        D3D11_BOX box{0, 0, 0, extent.width, extent.height, 1};
        m_context->CopySubresourceRegion(info.staging.Get(), 0, 0, 0, 0, source.texture.Get(), 0, &box);
        destination.readback = std::move(info);
        m_frameReadbacks.push_back(&destination);
        return;
    }
    if (event.operation == "WriteTimestamp")
    {
        if (event.resources.size() != 1)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "TimestampQuery", "timestamp payload is malformed");
        auto& query = PayloadAs<TimestampPayload>(m_owner->Payload(event.resources[0]), event.operation.c_str());
        if (query.armed && (query.completionSerial == 0 || query.completionSerial > m_completedSerial))
            Fail(RhiErrorCode::InvalidState, event.operation, "TimestampQuery", "timestamp query is still in flight");
        if (query.armed)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT previousDisjoint{};
            UINT64 previousTicks = 0;
            Check(m_context->GetData(query.disjoint.Get(), &previousDisjoint, sizeof(previousDisjoint),
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH),
                  "GetData recycled disjoint");
            Check(m_context->GetData(query.timestamp.Get(), &previousTicks, sizeof(previousTicks),
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH),
                  "GetData recycled timestamp");
        }
        query.completionSerial = 0;
        m_context->Begin(query.disjoint.Get());
        m_context->End(query.timestamp.Get());
        m_context->End(query.disjoint.Get());
        query.armed = true;
        query.frameSerial = m_frame->serial;
        m_frameQueries.push_back(&query);
        return;
    }
    // 内容重置只修改公共账本，不改变 native resource state 或绑定。
    if (event.operation == "ResetTransientContents")
        return;
    if (event.operation == "ApplyTransitions")
    {
        if (event.integers.size() != event.resources.size() * 3)
            Fail(RhiErrorCode::InvalidArgument, event.operation, "Transition", "transition payload is malformed");
        for (std::size_t i = 0; i < event.resources.size(); ++i)
        {
            const auto unbindsBefore = m_explicitUnbinds;
            const auto after = static_cast<ResourceAccess>(event.integers[i * 3 + 1]);
            if (const auto* textureHandle = std::get_if<TextureHandle>(&event.resources[i]))
            {
                auto& texture = PayloadAs<TexturePayload>(m_owner->Payload(*textureHandle), event.operation.c_str());
                if (after == ResourceAccess::SampledRead || after == ResourceAccess::ColorWrite ||
                    after == ResourceAccess::DepthRead || after == ResourceAccess::DepthWrite ||
                    after == ResourceAccess::CopySource || after == ResourceAccess::CopyDestination ||
                    after == ResourceAccess::Present)
                {
                    if (after != ResourceAccess::SampledRead)
                        UnbindShaderReads(texture.texture.Get());
                    if (after != ResourceAccess::SampledRead)
                        UnbindOutputs(texture.texture.Get());
                }
            }
            if (m_capabilities.debugLayerEnabled && m_trace.size() < 262144)
                m_trace += "graph-access frame=" + std::to_string(m_frame->serial) +
                           " resource=" + m_owner->DebugName(event.resources[i]) +
                           " logicalBefore=" + std::to_string(event.integers[i * 3]) +
                           " logicalAfter=" + std::to_string(event.integers[i * 3 + 1]) +
                           " unbinds=" + std::to_string(m_explicitUnbinds - unbindsBefore) + "\n";
            ++m_barriers;
        }
        return;
    }
    Fail(RhiErrorCode::Unsupported, event.operation, "CommandEvent",
         "D3D11 command lowering is not implemented for this operation");
}

void D3D11RhiBackend::SubmitCompletion(std::uint64_t serial)
{
    if (serial == 0 || serial <= m_lastSubmittedSerial)
        Fail(RhiErrorCode::InvalidArgument, "Submit", "Completion", "completion serials must increase");
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    Check(m_device->CreateQuery(&queryDesc, &query), "ID3D11Device::CreateQuery(event)");
    NameNativeObject(query.Get(), "MiniEngine.D3D11.Completion." + std::to_string(serial));
    m_context->End(query.Get());
    m_context->Flush();
    m_completions.emplace_back(m_counters, serial, std::move(query));
    m_lastSubmittedSerial = serial;
    ++m_submittedBatches;
}

void D3D11RhiBackend::Submit(const FrameToken& frame)
{
    if (!m_frame || m_frame->serial != frame.serial || m_rendering)
        Fail(RhiErrorCode::InvalidState, "Submit", "Frame", "frame is not active or rendering is still open");
    SubmitCompletion(frame.serial);
    for (auto* query : m_frameQueries)
        if (query)
            query->completionSerial = frame.serial;
    for (auto* buffer : m_frameReadbacks)
        if (buffer && buffer->readback)
            buffer->readback->completionSerial = frame.serial;
    m_frameQueries.clear();
    m_frameReadbacks.clear();
    m_frame.reset();
    m_frameChain = nullptr;
    m_currentPipeline = nullptr;
    // 提交之后回读已完成的 GPU timestamp（渲染线程上的唯一 collect 点）。
    if (m_gpuProfiler)
    {
        m_gpuProfiler->NewFrame();
        m_gpuProfiler->Collect();
    }
    if (m_capabilities.debugLayerEnabled && m_trace.size() < 262144)
        m_trace += "submit=" + std::to_string(frame.serial) + "\n";
}

void D3D11RhiBackend::Present(ResourcePayload& chain)
{
    auto& payload = PayloadAs<SwapChainPayload>(chain, "Present");
    if (payload.swapChain.Get() == nullptr)
        Fail(RhiErrorCode::InvalidState, "Present", "SwapChain", "swap chain is not initialized");
    const UINT syncInterval = payload.desc.vsync ? 1U : 0U;
    const UINT flags = 0;
    const HRESULT result = payload.swapChain->Present(syncInterval, flags);
    if (FAILED(result))
    {
        const auto code = (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                              ? RhiErrorCode::DeviceLost
                              : RhiErrorCode::BackendFailure;
        std::ostringstream out;
        out << "IDXGISwapChain::Present failed with HRESULT 0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result);
        throw RhiException({code, "Present", "SwapChain", payload.desc.debugName, "d3d11", out.str()});
    }
}

std::unique_ptr<ResourcePayload> D3D11RhiBackend::CreateDynamicBuffer(const BufferDesc& desc,
                                                                      std::span<const std::byte> bytes)
{
    if (!m_frame)
        throw std::logic_error{"CreateDynamicBuffer requires an open frame"};
    if (desc.memory != MemoryDomain::CpuToGpu || bytes.empty() || bytes.size() > desc.size)
        Fail(RhiErrorCode::InvalidArgument, "CreateDynamicBuffer", desc.debugName, "dynamic buffer input is invalid");
    auto payload = std::make_unique<BufferPayload>(m_counters, desc);
    D3D11_BUFFER_DESC native{};
    native.ByteWidth = static_cast<UINT>(desc.size);
    native.Usage = D3D11_USAGE_DYNAMIC;
    native.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (HasFlag(desc.usage, BufferUsage::Vertex))
        native.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
    if (HasFlag(desc.usage, BufferUsage::Index))
        native.BindFlags |= D3D11_BIND_INDEX_BUFFER;
    native.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Check(m_device->CreateBuffer(&native, nullptr, &payload->buffer), "ID3D11Device::CreateBuffer(dynamic)");
    NameNativeObject(payload->buffer.Get(), desc.debugName);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(m_context->Map(payload->buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
          "ID3D11DeviceContext::Map(dynamic)");
    std::memcpy(mapped.pData, bytes.data(), bytes.size());
    m_context->Unmap(payload->buffer.Get(), 0);
    m_frameDynamicBytes += desc.size;
    return payload;
}

void D3D11RhiBackend::UploadBuffer(ResourcePayload& destination, std::uint64_t offset, std::span<const std::byte> bytes,
                                   std::uint64_t serial)
{
    auto& payload = PayloadAs<BufferPayload>(destination, "UploadBuffer");
    if (payload.buffer.Get() == nullptr || bytes.empty() || offset > payload.desc.size ||
        bytes.size() > payload.desc.size - offset)
        Fail(RhiErrorCode::InvalidArgument, "UploadBuffer", payload.desc.debugName,
             "upload range is outside the buffer");
    if (bytes.size() > std::numeric_limits<UINT>::max())
        Fail(RhiErrorCode::Unsupported, "UploadBuffer", payload.desc.debugName, "upload staging exceeds D3D11 limits");
    PendingUpload pending(m_counters, serial, false);
    pending.input.assign(bytes.begin(), bytes.end());
    if (payload.desc.memory == MemoryDomain::CpuToGpu)
    {
        // WRITE_DISCARD 替换整块；先准备完整影子，保留局部更新范围之外的有效字节。
        auto shadow = payload.uploadShadow;
        shadow.resize(static_cast<std::size_t>(payload.desc.size));
        std::copy(bytes.begin(), bytes.end(), shadow.begin() + static_cast<std::ptrdiff_t>(offset));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(m_context->Map(payload.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
              "ID3D11DeviceContext::Map(upload)");
        std::memcpy(mapped.pData, shadow.data(), shadow.size());
        m_context->Unmap(payload.buffer.Get(), 0);
        payload.uploadShadow = std::move(shadow);
    }
    else
    {
        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(bytes.size());
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> staging;
        Check(m_device->CreateBuffer(&stagingDesc, nullptr, &staging), "ID3D11Device::CreateBuffer(upload-staging)");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(m_context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped),
              "ID3D11DeviceContext::Map(upload-staging)");
        std::memcpy(mapped.pData, bytes.data(), bytes.size());
        m_context->Unmap(staging.Get(), 0);
        D3D11_BOX box{0, 0, 0, static_cast<UINT>(bytes.size()), 1, 1};
        m_context->CopySubresourceRegion(payload.buffer.Get(), 0, static_cast<UINT>(offset), 0, 0, staging.Get(), 0,
                                         &box);
        pending.staging = staging;
        pending.hold = NativeHold(m_counters, true);
    }
    m_pending.push_back(std::move(pending));
    SubmitCompletion(serial);
}

void D3D11RhiBackend::UploadTexture(ResourcePayload& destination, std::span<const TextureSubresourceData> data,
                                    std::uint64_t serial)
{
    auto& payload = PayloadAs<TexturePayload>(destination, "UploadTexture");
    if (payload.texture.Get() == nullptr || data.empty())
        Fail(RhiErrorCode::InvalidArgument, "UploadTexture", payload.desc.debugName,
             "texture upload has no destination or data");
    const auto bytesPerPixel = BytesPerPixel(payload.desc.format);
    if (!bytesPerPixel)
        Fail(RhiErrorCode::Unsupported, "UploadTexture", payload.desc.debugName,
             "texture format has no D3D11 upload lowering");
    D3D11_TEXTURE2D_DESC native{};
    payload.texture->GetDesc(&native);
    native.Usage = D3D11_USAGE_STAGING;
    native.BindFlags = 0;
    native.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    native.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    Check(m_device->CreateTexture2D(&native, nullptr, &staging), "ID3D11Device::CreateTexture2D(upload-staging)");
    PendingUpload pending(m_counters, serial, true);
    for (std::uint32_t layer = 0; layer < payload.desc.arrayLayers; ++layer)
    {
        for (std::uint32_t mip = 0; mip < payload.desc.mipLevels; ++mip)
        {
            const std::size_t subresource = static_cast<std::size_t>(layer) * payload.desc.mipLevels + mip;
            const auto& input = data[subresource];
            const std::uint32_t width = std::max(1U, payload.desc.extent.width >> mip);
            const std::uint32_t height = std::max(1U, payload.desc.extent.height >> mip);
            const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) * bytesPerPixel;
            if (input.rowPitch < rowBytes || input.rowPitch > std::numeric_limits<UINT>::max())
                Fail(RhiErrorCode::InvalidArgument, "UploadTexture", payload.desc.debugName, "row pitch is invalid");
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const UINT nativeSubresource = D3D11CalcSubresource(mip, layer, payload.desc.mipLevels);
            Check(m_context->Map(staging.Get(), nativeSubresource, D3D11_MAP_WRITE, 0, &mapped),
                  "ID3D11DeviceContext::Map(texture-upload)");
            for (std::uint32_t row = 0; row < height; ++row)
                std::memcpy(static_cast<std::byte*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                            input.bytes.data() + static_cast<std::size_t>(row) * input.rowPitch,
                            static_cast<std::size_t>(rowBytes));
            m_context->Unmap(staging.Get(), nativeSubresource);
            pending.input.insert(pending.input.end(), input.bytes.begin(), input.bytes.end());
        }
    }
    m_context->CopyResource(payload.texture.Get(), staging.Get());
    pending.staging = staging;
    m_pending.push_back(std::move(pending));
    SubmitCompletion(serial);
}

void D3D11RhiBackend::ReleaseCompleted()
{
    std::erase_if(m_retiredReadbacks,
                  [&](const ReadbackInfo& value) { return value.completionSerial <= m_completedSerial; });
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [&](const PendingUpload& value) { return value.serial <= m_completedSerial; }),
                    m_pending.end());
}

std::uint64_t D3D11RhiBackend::PollCompleted()
{
    while (!m_completions.empty())
    {
        auto& completion = m_completions.front();
        const HRESULT result = m_context->GetData(completion.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE)
            break;
        if (FAILED(result))
        {
            throw RhiException({RhiErrorCode::DeviceLost,
                                "PollCompleted",
                                "EVENT",
                                {},
                                "d3d11",
                                "ID3D11DeviceContext::GetData failed for completion query"});
        }
        m_completedSerial = std::max(m_completedSerial, completion.serial);
        m_completions.pop_front();
    }
    ReleaseCompleted();
    return m_completedSerial;
}

std::uint64_t D3D11RhiBackend::WaitFor(std::uint64_t serial)
{
    if (serial <= m_completedSerial)
        return m_completedSerial;
    if (serial > m_lastSubmittedSerial)
        Fail(RhiErrorCode::InvalidArgument, "WaitFor", "EVENT", "requested serial was never submitted");
    m_context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (m_completedSerial < serial)
    {
        PollCompleted();
        if (m_completedSerial >= serial)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            throw RhiException({RhiErrorCode::BackendFailure,
                                "WaitFor",
                                "EVENT",
                                {},
                                "d3d11",
                                "bounded EVENT completion wait timed out"});
        ::Sleep(1);
    }
    return m_completedSerial;
}

void D3D11RhiBackend::WaitIdle()
{
    // 只有故障析构能从 active recording 到这里；immediate context 已消费部分命令，
    // 必须插入真实 EVENT 并等待，不能因 owner 尚未 EndFrame 而直接释放 payload。
    if (m_frame)
    {
        if (m_frame->serial > m_lastSubmittedSerial)
            SubmitCompletion(m_frame->serial);
        m_frame.reset();
        m_rendering = false;
    }
    if (m_context.Get() != nullptr)
        m_context->Flush();
    if (m_lastSubmittedSerial > m_completedSerial)
        WaitFor(m_lastSubmittedSerial);
    ClearContext();
}

std::optional<TimestampResult> D3D11RhiBackend::TryReadTimestamp(ResourcePayload& queryPayload)
{
    auto& query = PayloadAs<TimestampPayload>(queryPayload, "TryReadTimestamp");
    PollCompleted();
    if (!query.armed || query.completionSerial == 0 || query.completionSerial > m_completedSerial)
        return std::nullopt;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    const HRESULT disjointResult =
        m_context->GetData(query.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (disjointResult == S_FALSE)
        return std::nullopt;
    if (FAILED(disjointResult))
        throw RhiException({RhiErrorCode::BackendFailure,
                            "TryReadTimestamp",
                            "TimestampQuery",
                            {},
                            "d3d11",
                            "timestamp disjoint query failed"});
    UINT64 ticks = 0;
    const HRESULT timestampResult =
        m_context->GetData(query.timestamp.Get(), &ticks, sizeof(ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (timestampResult == S_FALSE)
        return std::nullopt;
    if (FAILED(timestampResult))
        throw RhiException({RhiErrorCode::BackendFailure,
                            "TryReadTimestamp",
                            "TimestampQuery",
                            {},
                            "d3d11",
                            "timestamp query failed"});
    TimestampResult result;
    result.ticks = ticks;
    result.frequency = disjoint.Frequency;
    result.frameSerial = query.frameSerial;
    result.disjoint = disjoint.Disjoint != FALSE || disjoint.Frequency == 0;
    result.validBits = 64;
    result.unavailable = false;
    return result;
}

std::optional<TextureReadbackResult> D3D11RhiBackend::TryReadTextureReadback(ResourcePayload& bufferPayload)
{
    auto& buffer = PayloadAs<BufferPayload>(bufferPayload, "TryReadTextureReadback");
    PollCompleted();
    if (!buffer.readback || !buffer.readback->valid || buffer.readback->completionSerial == 0 ||
        buffer.readback->completionSerial > m_completedSerial)
        return std::nullopt;
    ReadbackInfo info = std::move(*buffer.readback);
    buffer.readback.reset();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(m_context->Map(info.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "ID3D11DeviceContext::Map(readback)");
    const auto bytesPerPixel = BytesPerPixel(info.desc.format);
    if (!bytesPerPixel || info.desc.format != Format::Rgba8Unorm && info.desc.format != Format::Rgba8UnormSrgb)
    {
        m_context->Unmap(info.staging.Get(), 0);
        Fail(RhiErrorCode::Unsupported, "TryReadTextureReadback", "Readback",
             "only RGBA8 readback is normalized by the M6 contract");
    }
    const auto rowPitch = static_cast<std::uint64_t>(mapped.RowPitch);
    if (info.extent.height > std::numeric_limits<std::size_t>::max() / rowPitch)
    {
        m_context->Unmap(info.staging.Get(), 0);
        Fail(RhiErrorCode::OutOfMemory, "TryReadTextureReadback", "Readback", "row-pitched result is too large");
    }
    std::vector<std::byte> raw(static_cast<std::size_t>(rowPitch * info.extent.height));
    for (std::uint32_t row = 0; row < info.extent.height; ++row)
        std::memcpy(raw.data() + static_cast<std::size_t>(row) * rowPitch,
                    static_cast<const std::byte*>(mapped.pData) + static_cast<std::size_t>(row) * rowPitch,
                    static_cast<std::size_t>(rowPitch));
    m_context->Unmap(info.staging.Get(), 0);
    auto result = NormalizeRgba8Readback(raw, rowPitch, info.extent, info.desc.format);
    result.frameSerial = info.frameSerial;
    result.source = info.source;
    return result;
}

void D3D11RhiBackend::RecordMessageDiagnostics()
{
    if (m_infoQueue.Get() == nullptr)
        return;
    const std::size_t count = m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if (count < m_scannedMessages)
        m_scannedMessages = 0;
    for (std::size_t index = m_scannedMessages; index < count; ++index)
    {
        SIZE_T size = 0;
        if (FAILED(m_infoQueue->GetMessage(index, nullptr, &size)) || size == 0)
            continue;
        std::vector<std::byte> bytes(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        if (FAILED(m_infoQueue->GetMessage(index, message, &size)))
            continue;
        if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING || message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)
        {
            ++m_warningErrors;
            m_trace += "native-message=" + std::to_string(static_cast<unsigned>(message->ID)) + ":" +
                       std::string(message->pDescription ? message->pDescription : "") + "\n";
        }
    }
    m_scannedMessages = count;
}

void D3D11RhiBackend::InjectBackendFaultForTesting(std::string_view fault)
{
    if (fault == "skip-next-hazard-unbind")
    {
        m_skipNextHazardUnbind = true;
        return;
    }
    // 未知名称交给基类默认实现（std::runtime_error），与 D3D12 及基类契约一致。
    NativeRhiBackend::InjectBackendFaultForTesting(fault);
}

NativeBackendReport D3D11RhiBackend::Report(bool census)
{
    RecordMessageDiagnostics();
    std::string censusTrace;
    std::uint64_t externalNativeObjects = 0;
    if (census && m_capabilities.debugLayerEnabled)
    {
        if (!m_debug)
            Fail(RhiErrorCode::BackendFailure, "Report", "Census", "debug device is missing");
        ComPtr<IDXGIInfoQueue> dxgiInfo;
        Check(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfo)), "query native census queue");
        dxgiInfo->ClearStoredMessages(DXGI_DEBUG_D3D11);
        Check(m_debug->ReportLiveDeviceObjects(
                  static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL)),
              "native census");
        bool sawDevice = false;
        const auto count = dxgiInfo->GetNumStoredMessages(DXGI_DEBUG_D3D11);
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T size = 0;
            Check(dxgiInfo->GetMessage(DXGI_DEBUG_D3D11, index, nullptr, &size), "census message size");
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(bytes.data());
            Check(dxgiInfo->GetMessage(DXGI_DEBUG_D3D11, index, message, &size), "census message");
            if (!message->pDescription)
                continue;
            const std::string text(message->pDescription);
            censusTrace += "census.message=" + text + "\n";
            const bool device = text.find("Live ID3D11Device at ") != std::string::npos;
            const bool context = text.find("Live ID3D11Context at ") != std::string::npos;
            sawDevice = sawDevice || device;
            const auto ref = text.find("Refcount: ");
            if (!device && !context && text.find("Live ") != std::string::npos && ref != std::string::npos &&
                std::stoull(text.substr(ref + 10)) > 0)
                ++externalNativeObjects;
        }
        if (!sawDevice)
        {
            // 把队列里实际看到的消息带进错误文本：没有它，"omitted its device" 无法定位
            // 是设备没被报出来，还是 debug 队列本身是空的（例如被外部工具清空/过滤）。
            Fail(RhiErrorCode::BackendFailure, "Report", "Census",
                 "native census omitted its device (messages=" + std::to_string(count) + ")\n" + censusTrace);
        }
        censusTrace += "census.sawDevice=true externalNativeObjects=" + std::to_string(externalNativeObjects) + "\n";
    }
    else if (census)
        censusTrace = "census.available=false debugLayer=false\n";
    NativeBackendReport report;
    report.warningErrors = m_warningErrors;
    report.liveResources = std::max(m_counters->liveObjects, externalNativeObjects);
    report.submittedBatches = m_submittedBatches;
    report.completedSerial = m_completedSerial;
    report.explicitUnbinds = m_explicitUnbinds;
    report.barriers = m_barriers;
    report.discardNoOps = m_discardNoOps;
    // D3D11 没有 descriptor heap；用当前实际绑定的 view/常量槽作为描述符使用量。
    std::uint64_t boundViews = 0;
    const auto countViews = [&boundViews](const auto& slots)
    {
        for (const auto& slot : slots)
            boundViews += slot.Get() != nullptr ? 1U : 0U;
    };
    countViews(m_vsSrvs);
    countViews(m_psSrvs);
    countViews(m_vsSamplers);
    countViews(m_psSamplers);
    countViews(m_rtvs);
    for (const auto& slot : m_vsConstants)
        boundViews += slot.buffer.Get() != nullptr ? 1U : 0U;
    for (const auto& slot : m_psConstants)
        boundViews += slot.buffer.Get() != nullptr ? 1U : 0U;
    boundViews += m_dsv.Get() != nullptr ? 1U : 0U;
    report.descriptorRanges = boundViews;
    report.uploadBytes = m_frameDynamicBytes;
    report.injectedFaults = m_injectedFaults;
    std::ostringstream out;
    out << m_trace << censusTrace << "stats live=" << report.liveResources << " submitted=" << report.submittedBatches
        << " completed=" << report.completedSerial << " unbinds=" << report.explicitUnbinds
        << " barriers=" << report.barriers << " discardNoOps=" << report.discardNoOps
        << " injectedFaults=" << m_injectedFaults << '\n';
    report.trace = out.str();
    return report;
}

} // namespace MiniEngine::Rhi::D3D11

namespace MiniEngine::Rhi::D3D11
{
std::unique_ptr<NativeRhiBackend> CreateD3D11RhiBackend(const RhiDeviceCreateInfo& createInfo)
{
    return std::make_unique<D3D11RhiBackend>(createInfo);
}
} // namespace MiniEngine::Rhi::D3D11
