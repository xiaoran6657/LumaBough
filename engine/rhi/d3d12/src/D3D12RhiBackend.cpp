#include "D3D12RhiBackend.h"
#include <MiniEngine/Rhi/RhiPipeline.h>

#include "D3D12Diagnostics.h"
#include "D3D12GpuProfiler.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <pix3.h>

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/RhiValidation.h>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
constexpr std::uint64_t kUploadRingBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDedicatedUploadBudget = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kGenericSetStride = 3U;
constexpr std::uint32_t kTimestampCapacity = 4096U;

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1U;
    properties.VisibleNodeMask = 1U;
    return properties;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuOffset(D3D12_CPU_DESCRIPTOR_HANDLE handle, std::uint32_t slot, std::uint32_t increment)
{
    handle.ptr += static_cast<SIZE_T>(slot) * increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuOffset(D3D12_GPU_DESCRIPTOR_HANDLE handle, std::uint32_t slot, std::uint32_t increment)
{
    handle.ptr += static_cast<UINT64>(slot) * increment;
    return handle;
}

void RequireIndex(std::span<const std::uint64_t> values, std::size_t index, const char* operation)
{
    if (index >= values.size())
        throw std::logic_error{std::string{operation} + " event payload is truncated"};
}

std::uint32_t AsUint(std::uint64_t value, const char* operation)
{
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::out_of_range{std::string{operation} + " integer exceeds UINT32"};
    return static_cast<std::uint32_t>(value);
}

std::uint64_t CheckedAdd(std::uint64_t left, std::uint64_t right, const char* message)
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        throw std::overflow_error{message};
    return left + right;
}

D3D12_RESOURCE_FLAGS TextureFlags(const TextureDesc& desc)
{
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (HasFlag(desc.usage, TextureUsage::ColorAttachment))
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (HasFlag(desc.usage, TextureUsage::DepthStencil))
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return flags;
}

bool IsDepthFormat(Format format)
{
    return format == Format::D32Float || format == Format::D24UnormS8Uint;
}

} // namespace

D3D12RhiBackend::Payload::~Payload()
{
    if (backend != nullptr)
        backend->DestroyPayload(*this);
}

D3D12RhiBackend::D3D12RhiBackend(const RhiDeviceCreateInfo& createInfo)
{
    DeviceCreateOptions options;
    options.debugLayer = createInfo.enableDebugLayer;
    options.gpuValidation = createInfo.enableGpuValidation;
    options.dred = createInfo.enableDebugLayer;
    options.warp = createInfo.useWarp;
    m_device = D3D12Device::Create(options);
    m_capabilities = QueryCapabilities(*m_device);
    const auto assessment = AssessM6Capabilities(m_capabilities);
    if (assessment.status != RhiCapabilityStatus::Ready)
    {
        if (assessment.error.has_value())
            throw std::runtime_error{"D3D12 RHI capability gate BLOCKED: " + assessment.error->message};
        throw std::runtime_error{"D3D12 RHI capability gate BLOCKED"};
    }

    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    m_queue.Initialize(device);
    m_queue.SetTraceEnabled(createInfo.enableDebugLayer);
    m_uploadRing.Initialize(device, kUploadRingBytes);
    m_uploadManager.Initialize(
        &device, m_uploadRing, [this](const std::uint64_t fence)
        { m_queue.WaitForSubmittedFence(fence, "upload-ring-pressure"); }, kDedicatedUploadBudget);
    m_srvStaging.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kCbvSrvUavHeapCapacity, false,
                            L"M6.D3D12.SrvStaging");
    m_srvVisible.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kCbvSrvUavHeapCapacity, true,
                            L"M6.D3D12.SrvVisible");
    m_samplerStaging.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048U, false, L"M6.D3D12.SamplerStaging");
    // D3D12 的 shader-visible sampler heap 上限为 2048 个 descriptor。
    m_samplerVisible.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048U, true, L"M6.D3D12.SamplerVisible");
    m_rtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity, false, L"M6.D3D12.RtvHeap");
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity, false, L"M6.D3D12.DsvHeap");
    m_m5RootSignature = CreateM5RootSignature(device, m_rootFacts);
    Internal::SetDebugName(m_m5RootSignature.Get(), L"M6.D3D12.RootSignature");
    m_psoFactory.Initialize(device, *m_m5RootSignature.Get(), 1U);

    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDescription.Count = kTimestampCapacity;
    ThrowIfFailed(device.CreateQueryHeap(&queryDescription, IID_PPV_ARGS(&m_timestampHeap)),
                  "ID3D12Device::CreateQueryHeap(M6 timestamp)");
    Internal::SetDebugName(m_timestampHeap.Get(), L"M6.D3D12.TimestampHeap");
    ThrowIfFailed(m_queue.NativeQueue().GetTimestampFrequency(&m_queryFrequency),
                  "ID3D12CommandQueue::GetTimestampFrequency");
    if (m_queryFrequency == 0U)
        throw std::runtime_error{"D3D12 timestamp frequency is zero"};
    m_freeQueryIndices.reserve(kTimestampCapacity);
    // GPU 观测必须在 queue 初始化之后创建（Tracy 需要 queue 做时间戳校准）。
    m_gpuProfiler = CreateGpuProfiler(&device, &m_queue.NativeQueue());
    m_initialized = true;
}

D3D12RhiBackend::~D3D12RhiBackend()
{
    m_owner = nullptr; // owner registry 已先释放全部 payload。
    try
    {
        WaitIdle();
    }
    catch (const std::exception& error)
    {
        MiniEngine::WriteLog(MiniEngine::LogLevel::Error,
                             "d3d12 native backend shutdown failed: " + std::string{error.what()});
    }
    // Tracy 的 D3D12 析构会忙等未完成的 payload：必须在 WaitIdle 之后、释放 queue 之前。
    m_gpuProfiler.reset();
    try
    {
        if (m_initialized)
        {
            const auto completed = m_queue.CompletedValue();
            m_uploadManager.Reclaim(completed);
            m_srvStaging.Reclaim(completed);
            m_srvVisible.Reclaim(completed);
            m_samplerStaging.Reclaim(completed);
            m_samplerVisible.Reclaim(completed);
            m_rtvHeap.Reclaim(completed);
            m_dsvHeap.Reclaim(completed);
            m_psoFactory.DeferredRelease().Reclaim(completed);
            if (m_uploadRing.IsIdle())
                m_uploadRing.Shutdown("backend-destructor");
        }
    }
    catch (const std::exception& error)
    {
        MiniEngine::WriteLog(MiniEngine::LogLevel::Error,
                             "d3d12 native backend deferred cleanup failed: " + std::string{error.what()});
    }
}

const RhiCapabilities& D3D12RhiBackend::Capabilities() const
{
    return m_capabilities;
}

void D3D12RhiBackend::Attach(DeviceLifetime& owner)
{
    if (m_owner != nullptr && m_owner != &owner)
        throw std::logic_error{"D3D12RhiBackend::Attach called with a different owner"};
    if (m_owner == &owner)
        throw std::logic_error{"D3D12RhiBackend::Attach called twice"};
    m_owner = &owner;
}

void D3D12RhiBackend::RequireAttached(const char* operation) const
{
    if (!m_initialized || m_owner == nullptr)
        throw std::logic_error{std::string{operation} + " before backend attach/initialization"};
}

D3D12RhiBackend::Payload& D3D12RhiBackend::AsPayload(ResourcePayload& payload)
{
    auto* result = dynamic_cast<Payload*>(&payload);
    if (result == nullptr)
        throw std::logic_error{"resource payload belongs to a different backend"};
    return *result;
}

const D3D12RhiBackend::Payload& D3D12RhiBackend::AsPayload(const ResourcePayload& payload)
{
    auto* result = dynamic_cast<const Payload*>(&payload);
    if (result == nullptr)
        throw std::logic_error{"resource payload belongs to a different backend"};
    return *result;
}

D3D12RhiBackend::Payload& D3D12RhiBackend::RequirePayload(ResourcePayload& payload, const PayloadKind kind)
{
    Payload& result = AsPayload(payload);
    if (result.kind != kind)
        throw std::logic_error{"native payload kind mismatch"};
    return result;
}

const D3D12RhiBackend::Payload& D3D12RhiBackend::RequirePayload(const ResourcePayload& payload,
                                                                const PayloadKind kind) const
{
    const Payload& result = AsPayload(payload);
    if (result.kind != kind)
        throw std::logic_error{"native payload kind mismatch"};
    return result;
}

std::wstring D3D12RhiBackend::ToWide(const std::string_view text)
{
    if (text.empty())
        return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        std::wstring result;
        result.reserve(text.size());
        for (const auto character : text)
            result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
        return result;
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

DXGI_FORMAT D3D12RhiBackend::ToNativeFormat(const Format format)
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
        return DXGI_FORMAT_D32_FLOAT;
    case Format::D24UnormS8Uint:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::Unknown:
    case Format::Count:
        return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT D3D12RhiBackend::ToResourceFormat(const Format format)
{
    switch (format)
    {
    case Format::D32Float:
        return DXGI_FORMAT_R32_TYPELESS;
    case Format::D24UnormS8Uint:
        return DXGI_FORMAT_R24G8_TYPELESS;
    default:
        return ToNativeFormat(format);
    }
}

DXGI_FORMAT D3D12RhiBackend::ToDepthSrvFormat(const Format format)
{
    switch (format)
    {
    case Format::D32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case Format::D24UnormS8Uint:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:
        return ToNativeFormat(format);
    }
}

D3D12_RESOURCE_STATES D3D12RhiBackend::AccessState(const ResourceAccess access, const ShaderStage stages)
{
    switch (access)
    {
    case ResourceAccess::SampledRead:
    {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (HasFlag(stages, ShaderStage::Vertex))
            state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return state;
    }
    case ResourceAccess::ColorWrite:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceAccess::DepthRead:
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    case ResourceAccess::DepthWrite:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceAccess::CopySource:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceAccess::CopyDestination:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case ResourceAccess::Present:
    case ResourceAccess::None:
        return D3D12_RESOURCE_STATE_COMMON;
    case ResourceAccess::VertexRead:
    case ResourceAccess::UniformRead:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case ResourceAccess::IndexRead:
        return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

D3D12_COMPARISON_FUNC D3D12RhiBackend::CompareFunction(const CompareOp op)
{
    switch (op)
    {
    case CompareOp::Never:
        return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less:
        return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::LessEqual:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Equal:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::GreaterEqual:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::Always:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_FILTER D3D12RhiBackend::FilterMode(const SamplerDesc& desc)
{
    const bool comparison = desc.comparisonEnabled;
    if (desc.minMagFilter == Filter::Anisotropic || desc.mipFilter == Filter::Anisotropic)
        return comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    const bool minMagLinear = desc.minMagFilter == Filter::Linear;
    const bool mipLinear = desc.mipFilter == Filter::Linear;
    if (comparison)
    {
        if (minMagLinear && mipLinear)
            return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        if (minMagLinear && !mipLinear)
            return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        if (!minMagLinear && mipLinear)
            return D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR;
        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    }
    if (minMagLinear && mipLinear)
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    if (minMagLinear && !mipLinear)
        return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (!minMagLinear && mipLinear)
        return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    return D3D12_FILTER_MIN_MAG_MIP_POINT;
}

D3D12_TEXTURE_ADDRESS_MODE D3D12RhiBackend::AddressMode(const MiniEngine::Rhi::AddressMode mode)
{
    switch (mode)
    {
    case AddressMode::Repeat:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::Clamp:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_STATIC_BORDER_COLOR D3D12RhiBackend::BorderColor(const std::array<float, 4>& color)
{
    if (color[0] == 1.0F && color[1] == 1.0F && color[2] == 1.0F && color[3] == 1.0F)
        return D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    if (color[0] == 0.0F && color[1] == 0.0F && color[2] == 0.0F && color[3] == 0.0F)
        return D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    return D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
}

D3D12_RESOURCE_DESC D3D12RhiBackend::MakeBufferDescription(const BufferDesc& desc) const
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = HasFlag(desc.usage, BufferUsage::Uniform) ? AlignUp(desc.size, 256U) : desc.size;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

D3D12_RESOURCE_DESC D3D12RhiBackend::MakeTextureDescription(const TextureDesc& desc) const
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = desc.extent.width;
    description.Height = desc.extent.height;
    const std::uint32_t layers =
        static_cast<std::uint32_t>(desc.arrayLayers) * (desc.dimension == TextureDimension::TextureCube ? 6U : 1U);
    if (layers == 0U || layers > std::numeric_limits<std::uint16_t>::max())
        throw std::out_of_range{"texture array layer count exceeds D3D12 representation"};
    description.DepthOrArraySize = static_cast<UINT16>(layers);
    description.MipLevels = desc.mipLevels;
    description.Format = ToResourceFormat(desc.format);
    description.SampleDesc.Count = desc.sampleCount;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = TextureFlags(desc);
    return description;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateBuffer(const BufferDesc& desc)
{
    RequireAttached("CreateBuffer");
    auto payload = std::make_unique<Payload>(this, PayloadKind::Buffer, desc.debugName);
    const auto description = MakeBufferDescription(desc);
    const auto heapType =
        desc.memory == MemoryDomain::GpuOnly
            ? D3D12_HEAP_TYPE_DEFAULT
            : (desc.memory == MemoryDomain::CpuToGpu ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK);
    const auto initialState = desc.memory == MemoryDomain::GpuOnly
                                  ? D3D12_RESOURCE_STATE_COMMON
                                  : (desc.memory == MemoryDomain::CpuToGpu ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                           : D3D12_RESOURCE_STATE_COPY_DEST);
    auto heapProperties = HeapProperties(heapType);
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, initialState,
                                                 nullptr, IID_PPV_ARGS(&payload->resource)),
                  "ID3D12Device::CreateCommittedResource(RHI buffer)");
    const auto wideName = ToWide(desc.debugName.empty() ? "M6.D3D12.Buffer" : desc.debugName);
    Internal::SetDebugName(payload->resource.Get(), wideName);
    payload->nativeSize = description.Width;
    payload->gpuAddress = payload->resource->GetGPUVirtualAddress();
    payload->uploadHeap = desc.memory == MemoryDomain::CpuToGpu;
    payload->initialState = initialState;
    payload->stateKey = m_stateTracker.Register(*payload->resource.Get(), 1U, initialState, wideName.c_str());
    payload->hasStateKey = true;
    payload->ownsStateKey = true;
    payload->nativeResourceCount = 1U;
    ++m_livePayloadResources;
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateTexture(const TextureDesc& desc)
{
    RequireAttached("CreateTexture");
    auto payload = std::make_unique<Payload>(this, PayloadKind::Texture, desc.debugName);
    const auto description = MakeTextureDescription(desc);
    const auto initialState =
        IsDepthFormat(desc.format) ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON;
    D3D12_CLEAR_VALUE clearValue{};
    D3D12_CLEAR_VALUE* clearValuePtr = nullptr;
    if (HasFlag(desc.usage, TextureUsage::ColorAttachment))
    {
        clearValue.Format = ToNativeFormat(desc.format);
        std::copy(desc.clearColorHint.begin(), desc.clearColorHint.end(), clearValue.Color);
        clearValuePtr = &clearValue;
    }
    else if (HasFlag(desc.usage, TextureUsage::DepthStencil))
    {
        clearValue.Format = ToNativeFormat(desc.format);
        clearValue.DepthStencil.Depth = desc.clearDepthHint;
        clearValue.DepthStencil.Stencil = desc.clearStencilHint;
        clearValuePtr = &clearValue;
    }
    auto heapProperties = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, initialState,
                                                 clearValuePtr, IID_PPV_ARGS(&payload->resource)),
                  "ID3D12Device::CreateCommittedResource(RHI texture)");
    const auto wideName = ToWide(desc.debugName.empty() ? "M6.D3D12.Texture" : desc.debugName);
    Internal::SetDebugName(payload->resource.Get(), wideName);
    const std::uint32_t depthOrArraySize = description.DepthOrArraySize;
    const std::uint32_t subresources = static_cast<std::uint32_t>(description.MipLevels) * depthOrArraySize;
    payload->initialState = initialState;
    payload->stateKey = m_stateTracker.Register(*payload->resource.Get(), subresources, initialState, wideName.c_str());
    payload->hasStateKey = true;
    payload->ownsStateKey = true;
    payload->nativeSize = description.Width * description.Height;
    payload->nativeResourceCount = 1U;
    ++m_livePayloadResources;

    if (HasFlag(desc.usage, TextureUsage::Sampled))
    {
        payload->srvSource = m_srvStaging.Allocate(1U);
        payload->srvCpu = m_srvStaging.Cpu(payload->srvSource);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = IsDepthFormat(desc.format) ? ToDepthSrvFormat(desc.format) : ToNativeFormat(desc.format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc.dimension == TextureDimension::TextureCube)
        {
            if (desc.arrayLayers == 1U)
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srv.TextureCube.MostDetailedMip = 0U;
                srv.TextureCube.MipLevels = desc.mipLevels;
                srv.TextureCube.ResourceMinLODClamp = 0.0F;
            }
            else
            {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srv.TextureCubeArray.MostDetailedMip = 0U;
                srv.TextureCubeArray.MipLevels = desc.mipLevels;
                srv.TextureCubeArray.First2DArrayFace = 0U;
                srv.TextureCubeArray.NumCubes = desc.arrayLayers;
                srv.TextureCubeArray.ResourceMinLODClamp = 0.0F;
            }
        }
        else if (desc.arrayLayers == 1U)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MostDetailedMip = 0U;
            srv.Texture2D.MipLevels = desc.mipLevels;
            srv.Texture2D.ResourceMinLODClamp = 0.0F;
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MostDetailedMip = 0U;
            srv.Texture2DArray.MipLevels = desc.mipLevels;
            srv.Texture2DArray.FirstArraySlice = 0U;
            srv.Texture2DArray.ArraySize = desc.arrayLayers;
            srv.Texture2DArray.ResourceMinLODClamp = 0.0F;
        }
        device.CreateShaderResourceView(payload->resource.Get(), &srv, payload->srvCpu);
    }
    if (HasFlag(desc.usage, TextureUsage::ColorAttachment))
    {
        payload->rtvSource = m_rtvHeap.Allocate(1U);
        payload->rtvCpu = m_rtvHeap.Cpu(payload->rtvSource);
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = ToNativeFormat(desc.format);
        if (desc.arrayLayers == 1U)
        {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rtv.Texture2D.MipSlice = 0U;
            rtv.Texture2D.PlaneSlice = 0U;
        }
        else
        {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray.MipSlice = 0U;
            rtv.Texture2DArray.FirstArraySlice = 0U;
            rtv.Texture2DArray.ArraySize = desc.arrayLayers;
        }
        device.CreateRenderTargetView(payload->resource.Get(), &rtv, payload->rtvCpu);
    }
    if (HasFlag(desc.usage, TextureUsage::DepthStencil))
    {
        payload->dsvSource = m_dsvHeap.Allocate(2U);
        payload->dsvCpu = m_dsvHeap.Cpu(payload->dsvSource);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = ToNativeFormat(desc.format);
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        if (desc.arrayLayers == 1U)
        {
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv.Texture2D.MipSlice = 0U;
        }
        else
        {
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv.Texture2DArray.MipSlice = 0U;
            dsv.Texture2DArray.FirstArraySlice = 0U;
            dsv.Texture2DArray.ArraySize = desc.arrayLayers;
        }
        device.CreateDepthStencilView(payload->resource.Get(), &dsv, payload->dsvCpu);
        dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device.CreateDepthStencilView(payload->resource.Get(), &dsv, m_dsvHeap.Cpu(payload->dsvSource.base + 1U));
    }
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateSampler(const SamplerDesc& desc)
{
    RequireAttached("CreateSampler");
    auto payload = std::make_unique<Payload>(this, PayloadKind::Sampler, desc.debugName);
    payload->samplerSource = m_samplerStaging.Allocate(1U);
    payload->samplerCpu = m_samplerStaging.Cpu(payload->samplerSource);
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = FilterMode(desc);
    sampler.AddressU = AddressMode(desc.addressU);
    sampler.AddressV = AddressMode(desc.addressV);
    sampler.AddressW = AddressMode(desc.addressW);
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = desc.maxAnisotropy > 0.0F
                                ? static_cast<UINT>((std::min)(desc.maxAnisotropy, m_capabilities.maxAnisotropy))
                                : 1U;
    sampler.ComparisonFunc = desc.comparisonEnabled ? CompareFunction(desc.comparison) : D3D12_COMPARISON_FUNC_NEVER;
    std::copy(desc.borderColor.begin(), desc.borderColor.end(), sampler.BorderColor);
    sampler.MinLOD = desc.minLod;
    sampler.MaxLOD = desc.maxLod;
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    device.CreateSampler(&sampler, payload->samplerCpu);
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateShader(const ShaderDesc& desc)
{
    RequireAttached("CreateShader");
    auto payload = std::make_unique<Payload>(this, PayloadKind::Shader, desc.debugName);
    payload->bytecode.assign(desc.bytecode.begin(), desc.bytecode.end());
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateResourceSetLayout(const ResourceSetLayoutDesc& desc)
{
    RequireAttached("CreateResourceSetLayout");
    auto payload = std::make_unique<Payload>(this, PayloadKind::SetLayout, desc.debugName);
    payload->set = desc.set;
    payload->layoutEntries = desc.entries;
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateTimestampQuery(const std::string_view name)
{
    RequireAttached("CreateTimestampQuery");
    if (m_nextQueryIndex >= kTimestampCapacity && m_freeQueryIndices.empty())
        throw std::runtime_error{"D3D12 timestamp query heap exhausted"};
    auto payload = std::make_unique<Payload>(this, PayloadKind::Timestamp, std::string{name});
    if (m_freeQueryIndices.empty())
        payload->queryIndex = m_nextQueryIndex++;
    else
    {
        payload->queryIndex = m_freeQueryIndices.back();
        m_freeQueryIndices.pop_back();
    }
    payload->timestampFrequency = m_queryFrequency;
    auto heapProperties = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = sizeof(std::uint64_t);
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&payload->queryReadback)),
                  "ID3D12Device::CreateCommittedResource(timestamp readback)");
    Internal::SetDebugName(payload->queryReadback.Get(), ToWide(name));
    payload->nativeResourceCount = 1U;
    ++m_livePayloadResources;
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateSwapChain(const SwapChainDesc& desc)
{
    RequireAttached("CreateSwapChain");
    if (desc.bufferCount != kSwapChainBufferCount)
        throw std::runtime_error{"D3D12 backend requires exactly three swap-chain buffers"};
    if (desc.format != Format::Rgba8Unorm)
        throw std::runtime_error{"D3D12 backend swap-chain format must be Rgba8Unorm"};
    auto payload = std::make_unique<Payload>(this, PayloadKind::SwapChain, desc.debugName);
    payload->swapChain = std::make_unique<D3D12SwapChain>();
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    auto& factory = *static_cast<IDXGIFactory7*>(m_device->NativeFactoryHandle());
    payload->swapChain->Initialize(device, factory, m_queue.NativeQueue(), m_stateTracker, desc.nativeWindow,
                                   desc.extent.width, desc.extent.height, desc.vsync);
    payload->nativeResourceCount = 1U + kSwapChainBufferCount;
    m_livePayloadResources += payload->nativeResourceCount;
    return payload;
}

void D3D12RhiBackend::DestroyPayload(Payload& payload) noexcept
{
    try
    {
        if (payload.kind == PayloadKind::SwapChain && payload.swapChain != nullptr)
        {
            for (std::uint32_t index = 0U; index < kSwapChainBufferCount; ++index)
            {
                try
                {
                    m_stateTracker.Unregister(payload.swapChain->BackBufferKey(index));
                }
                catch (...)
                {
                }
            }
        }
        else if (payload.hasStateKey && payload.ownsStateKey)
        {
            m_stateTracker.Unregister(payload.stateKey);
        }
        if (payload.srvSource)
            m_srvStaging.Free(payload.srvSource);
        if (payload.samplerSource)
            m_samplerStaging.Free(payload.samplerSource);
        if (payload.rtvSource)
            m_rtvHeap.Free(payload.rtvSource);
        if (payload.dsvSource)
            m_dsvHeap.Free(payload.dsvSource);
        for (const auto& group : payload.groups)
        {
            if (!group.source)
                continue;
            if (group.kind == DescriptorGroupKind::Sampler)
                m_samplerStaging.Free(group.source);
            else
                m_srvStaging.Free(group.source);
        }
        if (payload.kind == PayloadKind::Timestamp && payload.queryIndex != UINT32_MAX)
            m_freeQueryIndices.push_back(payload.queryIndex);
        if (payload.readbackStaging)
            ++payload.nativeResourceCount;
        if (payload.nativeResourceCount <= m_livePayloadResources)
            m_livePayloadResources -= payload.nativeResourceCount;
        payload.backend = nullptr;
    }
    catch (const std::exception& error)
    {
        MiniEngine::WriteLog(MiniEngine::LogLevel::Error, "d3d12 payload cleanup failed: " + std::string{error.what()});
    }
}

std::unique_ptr<NativeRhiBackend> CreateD3D12RhiBackend(const RhiDeviceCreateInfo& createInfo)
{
    return std::make_unique<D3D12RhiBackend>(createInfo);
}
} // namespace MiniEngine::Rhi::D3D12

namespace MiniEngine::Rhi::D3D12
{
std::optional<std::pair<std::uint32_t, std::uint32_t>> D3D12RhiBackend::FixedSrvSlot(const std::uint8_t set,
                                                                                     const std::uint16_t binding)
{
    if (set == 0U && binding >= 2U && binding <= 5U)
        return std::pair{RootIndex(RootParameter::GlobalSrvs), static_cast<std::uint32_t>(binding - 2U)};
    if (set == 0U && (binding == 10U || binding == 12U))
        return std::pair{RootIndex(RootParameter::MaterialSrvs), 0U};
    if (set == 1U && binding >= 1U && binding <= 5U)
        return std::pair{RootIndex(RootParameter::MaterialSrvs), static_cast<std::uint32_t>(binding - 1U)};
    return std::nullopt;
}

std::optional<std::uint32_t> D3D12RhiBackend::FixedCbvRoot(const std::uint8_t set, const std::uint16_t binding)
{
    if (set == 0U && (binding == 0U || binding == 9U || binding == 11U))
        return RootIndex(RootParameter::FrameCbv);
    if (set == 0U && binding == 1U)
        return RootIndex(RootParameter::IblCbv);
    if (set == 1U && binding == 0U)
        return RootIndex(RootParameter::MaterialCbv);
    if (set == 2U && binding == 0U)
        return RootIndex(RootParameter::ObjectCbv);
    return std::nullopt;
}

std::optional<std::uint32_t> D3D12RhiBackend::FixedSamplerSlot(const std::uint8_t set, const std::uint16_t binding)
{
    if (set == 0U && binding >= 6U && binding <= 8U)
        return static_cast<std::uint32_t>(binding - 6U);
    return std::nullopt;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> D3D12RhiBackend::CreateGenericRootSignature(const PipelineLayoutDesc& desc,
                                                                                        bool& fixedProfile)
{
    fixedProfile = true;
    std::vector<ResourceSetLayoutDesc> layouts;
    layouts.reserve(desc.setCount);
    for (std::size_t index = 0U; index < desc.setCount; ++index)
    {
        layouts.push_back(m_owner->Describe(desc.sets[index]));
        for (const auto& entry : layouts.back().entries)
        {
            const bool fixed = entry.count == 1U && ((entry.type == BindingType::UniformBuffer &&
                                                      FixedCbvRoot(layouts.back().set, entry.binding).has_value()) ||
                                                     (entry.type == BindingType::SampledTexture &&
                                                      FixedSrvSlot(layouts.back().set, entry.binding).has_value()) ||
                                                     (entry.type == BindingType::Sampler &&
                                                      FixedSamplerSlot(layouts.back().set, entry.binding).has_value()));
            fixedProfile = fixedProfile && fixed;
        }
    }
    if (fixedProfile)
        return m_m5RootSignature;

    // 未落入 M5 frozen profile 的布局使用后端私有、按 logical set 分区的 table。
    // register/space 只存在于此 lowering，绝不由 public descriptor 直接提供。
    const std::uint32_t setCount = desc.setCount;
    const std::uint32_t parameterCount = setCount * kGenericSetStride;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(parameterCount);
    std::vector<D3D12_ROOT_PARAMETER> parameters(parameterCount);
    for (std::uint32_t set = 0U; set < setCount; ++set)
    {
        std::uint32_t cbvCount = 0U;
        std::uint32_t srvCount = 0U;
        std::uint32_t samplerCount = 0U;
        for (const auto& entry : layouts[set].entries)
        {
            const auto count = static_cast<std::uint32_t>(entry.count);
            if (entry.type == BindingType::UniformBuffer)
                cbvCount += count;
            else if (entry.type == BindingType::SampledTexture)
                srvCount += count;
            else
                samplerCount += count;
        }
        const std::array counts{(std::max)(1U, cbvCount), (std::max)(1U, srvCount), (std::max)(1U, samplerCount)};
        const std::array types{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                               D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER};
        const std::array registers{0U, 0U, 0U};
        const std::array visibility{D3D12_SHADER_VISIBILITY_ALL, D3D12_SHADER_VISIBILITY_PIXEL,
                                    D3D12_SHADER_VISIBILITY_PIXEL};
        for (std::uint32_t group = 0U; group < kGenericSetStride; ++group)
        {
            const std::uint32_t parameter = set * kGenericSetStride + group;
            ranges[parameter].RangeType = types[group];
            ranges[parameter].NumDescriptors = counts[group];
            ranges[parameter].BaseShaderRegister = registers[group];
            ranges[parameter].RegisterSpace = set;
            ranges[parameter].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            parameters[parameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[parameter].DescriptorTable.NumDescriptorRanges = 1U;
            parameters[parameter].DescriptorTable.pDescriptorRanges = &ranges[parameter];
            parameters[parameter].ShaderVisibility = visibility[group];
        }
    }
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = parameterCount;
    signature.pParameters = parameters.data();
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                      D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                      D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                      D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult =
        D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult))
    {
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
        {
            MiniEngine::WriteLog(
                MiniEngine::LogLevel::Error,
                "d3d12 generic root signature: " +
                    std::string{static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()});
        }
        ThrowIfFailed(serializeResult, "D3D12SerializeRootSignature(generic)");
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> result;
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    ThrowIfFailed(device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&result)),
                  "ID3D12Device::CreateRootSignature(generic)");
    Internal::SetDebugName(result.Get(), L"M6.D3D12.GenericRootSignature");
    return result;
}

void D3D12RhiBackend::CreateSourceCbv(Payload& setPayload, Binding& binding, const std::uint32_t slot)
{
    if (binding.value.type != BindingType::UniformBuffer)
        throw std::logic_error{"CreateSourceCbv received a non-uniform binding"};
    auto& buffer = AsPayload(m_owner->Payload(ResourceIdentity(binding.value.buffer.buffer)));
    if (buffer.kind != PayloadKind::Buffer)
        throw std::logic_error{"uniform binding does not name a buffer payload"};
    const auto offset = binding.value.buffer.offset;
    const auto logicalSize = binding.value.buffer.size;
    const auto size = AlignUp(logicalSize, 256U);
    if (offset > buffer.nativeSize || size > buffer.nativeSize - offset)
        throw std::out_of_range{"uniform binding CBV exceeds native buffer allocation"};
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = buffer.gpuAddress + offset;
    cbv.SizeInBytes = AsUint(size, "CBV size");
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    const auto destination =
        CpuOffset(m_srvStaging.Cpu(setPayload.groups[binding.group].source), slot, m_srvStaging.Increment());
    device.CreateConstantBufferView(&cbv, destination);
}

void D3D12RhiBackend::CreateSourceSrv(Payload& setPayload, Binding& binding, const std::uint32_t slot)
{
    if (binding.value.type != BindingType::SampledTexture)
        throw std::logic_error{"CreateSourceSrv received a non-texture binding"};
    const auto& texture = AsPayload(m_owner->Payload(ResourceIdentity(binding.value.texture)));
    if (texture.kind != PayloadKind::Texture || texture.srvCpu.ptr == 0U)
        throw std::logic_error{"sampled binding does not name an SRV texture"};
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    const auto destination =
        CpuOffset(m_srvStaging.Cpu(setPayload.groups[binding.group].source), slot, m_srvStaging.Increment());
    device.CopyDescriptorsSimple(1U, destination, texture.srvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RhiBackend::CreateSourceSampler(Payload& setPayload, Binding& binding, const std::uint32_t slot)
{
    if (binding.value.type != BindingType::Sampler)
        throw std::logic_error{"CreateSourceSampler received a non-sampler binding"};
    const auto& sampler = AsPayload(m_owner->Payload(ResourceIdentity(binding.value.sampler)));
    if (sampler.kind != PayloadKind::Sampler || sampler.samplerCpu.ptr == 0U)
        throw std::logic_error{"sampler binding does not name a sampler payload"};
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    const auto destination =
        CpuOffset(m_samplerStaging.Cpu(setPayload.groups[binding.group].source), slot, m_samplerStaging.Increment());
    device.CopyDescriptorsSimple(1U, destination, sampler.samplerCpu, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

void D3D12RhiBackend::MakeSetDescriptors(Payload& setPayload, const ResourceSetDesc& desc)
{
    const auto& layout = m_owner->Describe(desc.layout);
    setPayload.set = layout.set;
    setPayload.layoutEntries = layout.entries;
    std::vector<BindingLayoutEntry> entries = layout.entries;
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) { return left.binding < right.binding; });
    setPayload.fixedProfile = true;
    for (const auto& entry : entries)
    {
        const bool known = entry.count == 1U &&
                           ((entry.type == BindingType::UniformBuffer && FixedCbvRoot(layout.set, entry.binding)) ||
                            (entry.type == BindingType::SampledTexture && FixedSrvSlot(layout.set, entry.binding)) ||
                            (entry.type == BindingType::Sampler && FixedSamplerSlot(layout.set, entry.binding)));
        setPayload.fixedProfile = setPayload.fixedProfile && known;
    }

    for (const auto& entry : entries)
    {
        for (std::uint16_t element = 0U; element < entry.count; ++element)
        {
            const auto found =
                std::find_if(desc.bindings.begin(), desc.bindings.end(), [&](const auto& value)
                             { return value.binding == entry.binding && value.arrayElement == element; });
            if (found == desc.bindings.end())
                throw std::logic_error{"resource set binding is missing after owner validation"};
            Binding binding;
            binding.layout = entry;
            binding.value = *found;
            if (entry.dynamicOffset)
            {
                binding.dynamicIndex = 0U;
                for (const auto& prior : setPayload.bindings)
                    if (prior.layout.dynamicOffset)
                        ++binding.dynamicIndex;
            }
            if (setPayload.fixedProfile)
            {
                if (entry.type == BindingType::UniformBuffer)
                {
                    binding.fixedRootCbv = true;
                    binding.rootParameter = *FixedCbvRoot(layout.set, entry.binding);
                }
                else if (entry.type == BindingType::SampledTexture)
                {
                    const auto [root, slot] = *FixedSrvSlot(layout.set, entry.binding);
                    binding.rootParameter = root;
                    binding.groupSlot = slot;
                    auto group = std::find_if(
                        setPayload.groups.begin(), setPayload.groups.end(), [&](const auto& value)
                        { return value.kind == DescriptorGroupKind::FixedSrv && value.rootParameter == root; });
                    if (group == setPayload.groups.end())
                    {
                        DescriptorGroup created;
                        created.kind = DescriptorGroupKind::FixedSrv;
                        created.rootParameter = root;
                        created.sourceSlots.resize(root == RootIndex(RootParameter::MaterialSrvs) ? kMaterialSrvCount
                                                                                                  : kGlobalSrvCount);
                        group = setPayload.groups.insert(setPayload.groups.end(), std::move(created));
                    }
                    binding.group = static_cast<std::uint32_t>(group - setPayload.groups.begin());
                }
                else
                {
                    binding.fixedStaticSampler = true;
                    binding.staticSamplerRegister = *FixedSamplerSlot(layout.set, entry.binding);
                }
            }
            else
            {
                DescriptorGroupKind groupKind =
                    entry.type == BindingType::UniformBuffer
                        ? DescriptorGroupKind::Cbv
                        : (entry.type == BindingType::SampledTexture ? DescriptorGroupKind::Srv
                                                                     : DescriptorGroupKind::Sampler);
                auto group = std::find_if(setPayload.groups.begin(), setPayload.groups.end(),
                                          [&](const auto& value) { return value.kind == groupKind; });
                if (group == setPayload.groups.end())
                {
                    DescriptorGroup created;
                    created.kind = groupKind;
                    created.rootParameter =
                        static_cast<std::uint32_t>(layout.set) * kGenericSetStride +
                        (groupKind == DescriptorGroupKind::Cbv ? 0U
                                                               : (groupKind == DescriptorGroupKind::Srv ? 1U : 2U));
                    group = setPayload.groups.insert(setPayload.groups.end(), std::move(created));
                }
                binding.group = static_cast<std::uint32_t>(group - setPayload.groups.begin());
                binding.groupSlot = static_cast<std::uint32_t>(group->bindingIndices.size());
            }
            setPayload.bindings.push_back(std::move(binding));
            if (!setPayload.fixedProfile)
            {
                auto& group = setPayload.groups[setPayload.bindings.back().group];
                group.bindingIndices.push_back(static_cast<std::uint32_t>(setPayload.bindings.size() - 1U));
            }
        }
    }

    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    for (auto& group : setPayload.groups)
    {
        const std::uint32_t count = group.kind == DescriptorGroupKind::FixedSrv
                                        ? static_cast<std::uint32_t>(group.sourceSlots.size())
                                        : static_cast<std::uint32_t>(group.bindingIndices.size());
        if (count == 0U)
            continue;
        if (group.kind == DescriptorGroupKind::Sampler)
            group.source = m_samplerStaging.Allocate(count);
        else
            group.source = m_srvStaging.Allocate(count);
    }

    // 固定表的未使用槽位必须是合法 null SRV；root table 的范围仍然是完整 t0-t4/t5-t8。
    for (auto& group : setPayload.groups)
    {
        if (group.kind != DescriptorGroupKind::FixedSrv)
            continue;
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1U;
        for (std::uint32_t slot = 0U; slot < group.sourceSlots.size(); ++slot)
            device.CreateShaderResourceView(nullptr, &nullSrv,
                                            CpuOffset(m_srvStaging.Cpu(group.source), slot, m_srvStaging.Increment()));
    }

    for (std::uint32_t index = 0U; index < setPayload.bindings.size(); ++index)
    {
        auto& binding = setPayload.bindings[index];
        if (binding.fixedStaticSampler)
            continue;
        if (binding.fixedRootCbv)
            continue;
        if (binding.layout.type == BindingType::UniformBuffer)
            CreateSourceCbv(setPayload, binding, binding.groupSlot);
        else if (binding.layout.type == BindingType::SampledTexture)
        {
            const auto slot = setPayload.fixedProfile ? binding.groupSlot : binding.groupSlot;
            CreateSourceSrv(setPayload, binding, slot);
        }
        else
            CreateSourceSampler(setPayload, binding, binding.groupSlot);
    }
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateResourceSet(const ResourceSetDesc& desc)
{
    RequireAttached("CreateResourceSet");
    auto payload = std::make_unique<Payload>(this, PayloadKind::Set, desc.debugName);
    MakeSetDescriptors(*payload, desc);
    return payload;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreatePipelineLayout(const PipelineLayoutDesc& desc)
{
    RequireAttached("CreatePipelineLayout");
    auto payload = std::make_unique<Payload>(this, PayloadKind::PipelineLayout, desc.debugName);
    bool fixedProfile = false;
    payload->rootSignature = CreateGenericRootSignature(desc, fixedProfile);
    payload->fixedProfile = fixedProfile;
    for (std::uint32_t set = 0U; set < desc.setCount; ++set)
        payload->setRootBase[set] = fixedProfile ? 0U : set * kGenericSetStride;
    return payload;
}

DXGI_FORMAT VertexFormatNative(const VertexFormat format)
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
}

const char* SemanticName(const VertexSemantic semantic)
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
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    RequireAttached("CreateGraphicsPipeline");
    const auto& vertex = AsPayload(m_owner->Payload(ResourceIdentity(desc.vertexShader)));
    const auto& layout = AsPayload(m_owner->Payload(ResourceIdentity(desc.layout)));
    if (vertex.kind != PayloadKind::Shader || layout.kind != PayloadKind::PipelineLayout || vertex.bytecode.empty() ||
        layout.rootSignature == nullptr)
        throw std::logic_error{"graphics pipeline dependency payload kind is invalid"};
    const Payload* pixel = nullptr;
    if (desc.pixelShader)
    {
        pixel = &AsPayload(m_owner->Payload(ResourceIdentity(desc.pixelShader)));
        if (pixel->kind != PayloadKind::Shader || pixel->bytecode.empty())
            throw std::logic_error{"pixel shader payload kind is invalid"};
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = layout.rootSignature.Get();
    native.VS = {vertex.bytecode.data(), vertex.bytecode.size()};
    if (pixel != nullptr)
        native.PS = {pixel->bytecode.data(), pixel->bytecode.size()};
    std::vector<D3D12_INPUT_ELEMENT_DESC> input;
    input.reserve(desc.vertexAttributes.size());
    for (const auto& attribute : desc.vertexAttributes)
    {
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = SemanticName(attribute.semantic);
        element.SemanticIndex = attribute.semanticIndex;
        element.Format = VertexFormatNative(attribute.format);
        element.InputSlot = attribute.bufferSlot;
        element.AlignedByteOffset = attribute.offset;
        element.InputSlotClass = attribute.instanceStepRate == 0U ? D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
                                                                  : D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        element.InstanceDataStepRate = attribute.instanceStepRate;
        input.push_back(element);
    }
    native.InputLayout = {input.data(), static_cast<UINT>(input.size())};
    native.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    native.NumRenderTargets = desc.colorAttachmentCount;
    for (std::uint32_t index = 0U; index < desc.colorAttachmentCount; ++index)
        native.RTVFormats[index] = ToNativeFormat(desc.colorFormats[index]);
    native.DSVFormat = ToNativeFormat(desc.depthFormat);
    native.SampleDesc.Count = desc.sampleCount;
    native.SampleMask = UINT_MAX;
    native.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    native.RasterizerState.CullMode =
        desc.cullMode == CullMode::None
            ? D3D12_CULL_MODE_NONE
            : (desc.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK);
    native.RasterizerState.FrontCounterClockwise = desc.frontFace == FrontFace::CounterClockwise;
    native.RasterizerState.DepthClipEnable = desc.depthClip;
    native.RasterizerState.DepthBias = desc.depthBias;
    native.RasterizerState.DepthBiasClamp = desc.depthBiasClamp;
    native.RasterizerState.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
    native.RasterizerState.MultisampleEnable = desc.sampleCount > 1U;
    native.BlendState.AlphaToCoverageEnable = FALSE;
    native.BlendState.IndependentBlendEnable = FALSE;
    for (std::uint32_t index = 0U; index < 8U; ++index)
    {
        auto& blend = native.BlendState.RenderTarget[index];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.BlendEnable = desc.alphaBlend;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    }
    native.DepthStencilState.DepthEnable = desc.depthFormat != Format::Unknown && desc.depthTest;
    native.DepthStencilState.DepthWriteMask =
        desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    native.DepthStencilState.DepthFunc = CompareFunction(desc.depthCompare);
    native.DepthStencilState.StencilEnable = FALSE;
    auto payload = std::make_unique<Payload>(this, PayloadKind::Pipeline, desc.debugName);
    payload->layoutHandle = desc.layout;
    payload->fixedProfile = layout.fixedProfile;
    std::vector<ResourceSetLayoutDesc> semanticLayouts;
    const auto& pipelineLayout = m_owner->Describe(desc.layout);
    for (std::uint32_t index = 0; index < pipelineLayout.setCount; ++index)
        semanticLayouts.push_back(m_owner->Describe(pipelineLayout.sets[index]));
    const auto* pixelDesc = desc.pixelShader ? &m_owner->Describe(desc.pixelShader) : nullptr;
    auto cacheKey = PipelineSemanticKey(desc, m_owner->Describe(desc.vertexShader), pixelDesc, semanticLayouts);
    // Root signature 身份与自持有 bytecode 防止不同 revision 错用同一 native PSO。
    cacheKey += "|root=" + std::to_string(reinterpret_cast<std::uintptr_t>(layout.rootSignature.Get()));
    cacheKey.append(reinterpret_cast<const char*>(vertex.bytecode.data()), vertex.bytecode.size());
    if (pixel != nullptr)
        cacheKey.append(reinterpret_cast<const char*>(pixel->bytecode.data()), pixel->bytecode.size());
    payload->cachedPipeline = m_psoFactory.AcquireAdapted(std::move(cacheKey), native);
    payload->pipeline = payload->cachedPipeline->pipeline;
    Internal::SetDebugName(payload->pipeline.Get(),
                           ToWide(desc.debugName.empty() ? "M6.D3D12.Pipeline" : desc.debugName));
    return payload;
}
} // namespace MiniEngine::Rhi::D3D12

namespace MiniEngine::Rhi::D3D12
{
void D3D12RhiBackend::Transition(Payload& payload, const ResourceAccess access, const ShaderStage stages)
{
    if (payload.dynamic && payload.uploadHeap && access == ResourceAccess::UniformRead)
        return; // 帧内 slice 借用 M5 upload ring，底层始终处于 GENERIC_READ。
    if (!payload.hasStateKey || payload.resource == nullptr)
        throw std::logic_error{"native transition requires a tracked resource"};
    auto desired = AccessState(access, stages);
    // Upload heap 固定处于 GENERIC_READ，已包含 vertex/constant/copy-source，
    // 不允许把它转换到其它资源状态。
    if (payload.uploadHeap)
        desired = D3D12_RESOURCE_STATE_GENERIC_READ;
    const bool shaderRead =
        (desired & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) != 0;
    if (m_skipNextTransition && shaderRead && m_stateTracker.CurrentState(payload.stateKey) != desired)
    {
        // 测试专用负向通道：故意漏掉一次 read 方向 transition，让 GBV 捕获 shader 读取
        // 写状态资源；正常 sample/pass 不设置该开关。
        m_skipNextTransition = false;
        ++m_injectedFaults;
        m_diagnosticTrace += "injected-fault=skip-next-read-transition resource=" + payload.debugName + "\n";
        return;
    }
    m_stateTracker.Transition(payload.stateKey, desired);
}

std::uint32_t D3D12RhiBackend::FlushBarriers()
{
    if (!m_activeFrame.has_value())
        throw std::logic_error{"barrier flush requires an active frame"};
    const auto emitted = m_stateTracker.FlushBarriersTo(m_queue.CommandList());
    m_barriers += emitted;
    return emitted;
}

void D3D12RhiBackend::Consume(const CommandEvent& event)
{
    RequireAttached("Consume");
    if (!m_activeFrame.has_value() || m_activeContext == nullptr || m_closed)
        throw std::logic_error{"command event received outside an open native frame"};
    auto& list = m_queue.CommandList();
    const auto operation = std::string_view{event.operation};
    if (operation == "BeginLabel")
    {
        if (event.text.empty())
            throw std::logic_error{"BeginLabel event has an empty text"};
        const auto wide = ToWide(event.text);
        PIXBeginEvent(&list, PIX_COLOR_DEFAULT, L"%s", wide.c_str());
        // GPU zone 与 pass label 一一对应：zone 写进当前录制的 command list。
        if (m_gpuProfiler)
            m_gpuProfiler->BeginZone(event.text.c_str());
        return;
    }
    if (operation == "EndLabel")
    {
        if (m_gpuProfiler)
            m_gpuProfiler->EndZone();
        PIXEndEvent(&list);
        return;
    }
    if (operation == "BeginRendering")
    {
        if (m_rendering || event.integers.size() < 4U)
            throw std::logic_error{"BeginRendering native event is out of order"};
        const auto colorCount = AsUint(event.integers[2], "BeginRendering color count");
        const bool hasDepth = event.integers[3] != 0U;
        if (event.resources.size() != static_cast<std::size_t>(colorCount + (hasDepth ? 1U : 0U)) ||
            event.integers.size() < 4U + static_cast<std::size_t>(colorCount) * 2U ||
            event.scalars.size() < static_cast<std::size_t>(colorCount) * 4U + (hasDepth ? 1U : 0U))
            throw std::logic_error{"BeginRendering native event payload is truncated"};
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
        rtvs.reserve(colorCount);
        m_attachments.clear();
        for (std::uint32_t index = 0U; index < colorCount; ++index)
        {
            const auto texture = std::get<TextureHandle>(event.resources[index]);
            auto& payload = RequirePayload(m_owner->Payload(ResourceIdentity(texture)), PayloadKind::Texture);
            if (payload.rtvCpu.ptr == 0U)
                throw std::logic_error{"color attachment has no RTV"};
            Transition(payload, ResourceAccess::ColorWrite);
            m_logicalAccess[texture] = ResourceAccess::ColorWrite;
            rtvs.push_back(payload.rtvCpu);
            m_attachments.push_back({texture, static_cast<StoreOp>(event.integers[5U + index * 2U]), false});
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (hasDepth)
        {
            const auto texture = std::get<TextureHandle>(event.resources[colorCount]);
            auto& payload = RequirePayload(m_owner->Payload(ResourceIdentity(texture)), PayloadKind::Texture);
            if (payload.dsvCpu.ptr == 0U)
                throw std::logic_error{"depth attachment has no DSV"};
            const auto logical =
                m_logicalAccess.contains(texture) ? m_logicalAccess.at(texture) : ResourceAccess::DepthWrite;
            const auto state =
                logical == ResourceAccess::DepthRead ? ResourceAccess::DepthRead : ResourceAccess::DepthWrite;
            Transition(payload, state);
            m_logicalAccess[texture] = state;
            dsv = state == ResourceAccess::DepthRead ? m_dsvHeap.Cpu(payload.dsvSource.base + 1U) : payload.dsvCpu;
            m_attachments.push_back({texture, static_cast<StoreOp>(event.integers[5U + colorCount * 2U]), true});
        }
        FlushBarriers();
        list.OMSetRenderTargets(colorCount, rtvs.empty() ? nullptr : rtvs.data(), FALSE, hasDepth ? &dsv : nullptr);
        for (std::uint32_t index = 0U; index < colorCount; ++index)
        {
            const auto load = static_cast<LoadOp>(event.integers[4U + index * 2U]);
            const auto texture = std::get<TextureHandle>(event.resources[index]);
            auto& payload = RequirePayload(m_owner->Payload(ResourceIdentity(texture)), PayloadKind::Texture);
            if (load == LoadOp::DontCare)
            {
                list.DiscardResource(payload.resource.Get(), nullptr);
            }
            if (load == LoadOp::Clear)
            {
                const float clear[4]{event.scalars[index * 4U], event.scalars[index * 4U + 1U],
                                     event.scalars[index * 4U + 2U], event.scalars[index * 4U + 3U]};
                list.ClearRenderTargetView(payload.rtvCpu, clear, 0U, nullptr);
            }
        }
        if (hasDepth)
        {
            const auto load = static_cast<LoadOp>(event.integers[4U + colorCount * 2U]);
            const auto texture = std::get<TextureHandle>(event.resources[colorCount]);
            auto& payload = RequirePayload(m_owner->Payload(ResourceIdentity(texture)), PayloadKind::Texture);
            if (load == LoadOp::DontCare)
            {
                list.DiscardResource(payload.resource.Get(), nullptr);
            }
            if (load == LoadOp::Clear)
            {
                const auto clearDepth = event.scalars[colorCount * 4U];
                const auto clearStencil = AsUint(event.integers[6U + colorCount * 2U], "depth clear stencil");
                list.ClearDepthStencilView(payload.dsvCpu,
                                           static_cast<D3D12_CLEAR_FLAGS>(
                                               D3D12_CLEAR_FLAG_DEPTH | (clearStencil ? D3D12_CLEAR_FLAG_STENCIL : 0U)),
                                           clearDepth, static_cast<UINT8>(clearStencil), 0U, nullptr);
            }
        }
        m_rendering = true;
        return;
    }
    if (operation == "EndRendering")
    {
        if (!m_rendering)
            throw std::logic_error{"EndRendering native event without BeginRendering"};
        for (const auto& attachment : m_attachments)
        {
            auto& payload =
                RequirePayload(m_owner->Payload(ResourceIdentity(attachment.texture)), PayloadKind::Texture);
            if (attachment.store == StoreOp::DontCare)
            {
                list.DiscardResource(payload.resource.Get(), nullptr);
            }
        }
        FlushBarriers();
        m_attachments.clear();
        m_rendering = false;
        return;
    }
    if (operation == "SetPipeline")
    {
        if (event.resources.size() != 1U)
            throw std::logic_error{"SetPipeline native event payload is invalid"};
        const auto pipelineHandle = std::get<GraphicsPipelineHandle>(event.resources[0]);
        auto& pipeline = RequirePayload(m_owner->Payload(ResourceIdentity(pipelineHandle)), PayloadKind::Pipeline);
        if (pipeline.pipeline == nullptr)
            throw std::logic_error{"graphics pipeline has no native PSO"};
        auto& layout =
            RequirePayload(m_owner->Payload(ResourceIdentity(pipeline.layoutHandle)), PayloadKind::PipelineLayout);
        list.SetGraphicsRootSignature(layout.rootSignature.Get());
        list.SetPipelineState(pipeline.pipeline.Get());
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_currentPipeline = &pipeline;
        return;
    }
    if (operation == "SetViewport")
    {
        if (event.scalars.size() != 6U)
            throw std::logic_error{"SetViewport native event payload is invalid"};
        D3D12_VIEWPORT viewport{event.scalars[0], event.scalars[1], event.scalars[2],
                                event.scalars[3], event.scalars[4], event.scalars[5]};
        list.RSSetViewports(1U, &viewport);
        return;
    }
    if (operation == "SetScissor")
    {
        if (event.integers.size() != 4U)
            throw std::logic_error{"SetScissor native event payload is invalid"};
        D3D12_RECT rectangle{static_cast<LONG>(event.integers[0]), static_cast<LONG>(event.integers[1]),
                             static_cast<LONG>(event.integers[0] + event.integers[2]),
                             static_cast<LONG>(event.integers[1] + event.integers[3])};
        list.RSSetScissorRects(1U, &rectangle);
        return;
    }
    if (operation == "BindVertexBuffer")
    {
        if (event.resources.size() != 1U || event.integers.size() != 4U)
            throw std::logic_error{"BindVertexBuffer native event payload is invalid"};
        const auto bufferHandle = std::get<BufferHandle>(event.resources[0]);
        auto& buffer = RequirePayload(m_owner->Payload(ResourceIdentity(bufferHandle)), PayloadKind::Buffer);
        const auto offset = event.integers[1];
        const auto size = AsUint(event.integers[2], "vertex buffer size");
        const auto stride = AsUint(event.integers[3], "vertex stride");
        if (offset > buffer.nativeSize || event.integers[2] > buffer.nativeSize - offset)
            throw std::out_of_range{"vertex buffer view exceeds native buffer"};
        D3D12_VERTEX_BUFFER_VIEW view{buffer.gpuAddress + offset, size, stride};
        list.IASetVertexBuffers(AsUint(event.integers[0], "vertex slot"), 1U, &view);
        return;
    }
    if (operation == "BindIndexBuffer")
    {
        if (event.resources.size() != 1U || event.integers.size() != 3U)
            throw std::logic_error{"BindIndexBuffer native event payload is invalid"};
        const auto bufferHandle = std::get<BufferHandle>(event.resources[0]);
        auto& buffer = RequirePayload(m_owner->Payload(ResourceIdentity(bufferHandle)), PayloadKind::Buffer);
        const auto offset = event.integers[0];
        const auto size = AsUint(event.integers[1], "index buffer size");
        if (offset > buffer.nativeSize || event.integers[1] > buffer.nativeSize - offset)
            throw std::out_of_range{"index buffer view exceeds native buffer"};
        D3D12_INDEX_BUFFER_VIEW view{buffer.gpuAddress + offset, size,
                                     event.integers[2] == static_cast<std::uint64_t>(IndexType::UInt16)
                                         ? DXGI_FORMAT_R16_UINT
                                         : DXGI_FORMAT_R32_UINT};
        list.IASetIndexBuffer(&view);
        return;
    }
    if (operation == "BindResourceSet")
    {
        if (event.resources.size() != 1U || event.integers.empty())
            throw std::logic_error{"BindResourceSet native event payload is invalid"};
        auto& set = RequirePayload(m_owner->Payload(event.resources[0]), PayloadKind::Set);
        std::vector<std::uint32_t> offsets;
        offsets.reserve(event.integers.size() - 1U);
        for (std::size_t index = 1U; index < event.integers.size(); ++index)
            offsets.push_back(AsUint(event.integers[index], "dynamic resource-set offset"));
        BindResourceSet(AsUint(event.integers[0], "resource-set index"), set, offsets);
        return;
    }
    if (operation == "Draw")
    {
        if (event.integers.size() != 4U)
            throw std::logic_error{"Draw native event payload is invalid"};
        list.DrawInstanced(
            AsUint(event.integers[0], "draw vertex count"), AsUint(event.integers[1], "draw instance count"),
            AsUint(event.integers[2], "draw first vertex"), AsUint(event.integers[3], "draw first instance"));
        return;
    }
    if (operation == "DrawIndexed")
    {
        if (event.integers.size() != 5U)
            throw std::logic_error{"DrawIndexed native event payload is invalid"};
        list.DrawIndexedInstanced(AsUint(event.integers[0], "indexed count"),
                                  AsUint(event.integers[1], "indexed instances"),
                                  AsUint(event.integers[2], "indexed first"), static_cast<INT>(event.integers[3]),
                                  AsUint(event.integers[4], "indexed first instance"));
        return;
    }
    if (operation == "CopyBuffer")
    {
        if (event.resources.size() != 2U || event.integers.size() != 5U)
            throw std::logic_error{"CopyBuffer native event payload is invalid"};
        const auto sourceHandle = std::get<BufferHandle>(event.resources[0]);
        const auto destinationHandle = std::get<BufferHandle>(event.resources[1]);
        auto& source = RequirePayload(m_owner->Payload(ResourceIdentity(sourceHandle)), PayloadKind::Buffer);
        auto& destination = RequirePayload(m_owner->Payload(ResourceIdentity(destinationHandle)), PayloadKind::Buffer);
        Transition(source, ResourceAccess::CopySource);
        Transition(destination, ResourceAccess::CopyDestination);
        m_logicalAccess[sourceHandle] = ResourceAccess::CopySource;
        m_logicalAccess[destinationHandle] = ResourceAccess::CopyDestination;
        FlushBarriers();
        const auto size = event.integers[4];
        if (event.integers[0] > source.nativeSize || size > source.nativeSize - event.integers[0] ||
            event.integers[2] > destination.nativeSize || size > destination.nativeSize - event.integers[2])
            throw std::out_of_range{"CopyBuffer native range exceeds resource"};
        list.CopyBufferRegion(destination.resource.Get(), event.integers[2], source.resource.Get(), event.integers[0],
                              size);
        destination.readbackPending = false;
        destination.readbackConsumed = true;
        return;
    }
    if (operation == "CopyTextureForReadback")
    {
        if (event.resources.size() != 2U || event.integers.size() != 2U)
            throw std::logic_error{"CopyTextureForReadback native event payload is invalid"};
        const auto sourceHandle = std::get<TextureHandle>(event.resources[0]);
        const auto readbackHandle = std::get<BufferHandle>(event.resources[1]);
        auto& source = RequirePayload(m_owner->Payload(ResourceIdentity(sourceHandle)), PayloadKind::Texture);
        auto& readback = RequirePayload(m_owner->Payload(ResourceIdentity(readbackHandle)), PayloadKind::Buffer);
        const auto width = AsUint(event.integers[0], "readback width");
        const auto height = AsUint(event.integers[1], "readback height");
        const auto description = source.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0U;
        UINT64 rowSize = 0U;
        UINT64 totalBytes = 0U;
        auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
        device.GetCopyableFootprints(&description, 0U, 1U, 0U, &footprint, &rowCount, &rowSize, &totalBytes);
        const auto rowBytes = CheckedAdd(static_cast<std::uint64_t>(width) * 4U, 0U, "readback row overflow");
        if (description.Width != width || description.Height != height || rowCount < height || rowSize != rowBytes ||
            footprint.Footprint.RowPitch < rowBytes || totalBytes == 0U)
            throw std::out_of_range{"texture readback footprint is inconsistent with the source texture"};

        // 公共 readback 可以仅有紧凑像素容量；D3D12 placed footprint 的
        // 每行需要补齐到 256 B，因此按需分配额外 staging，
        // 完成后统一去除 padding。
        const bool needsPaddingResource = readback.readbackStaging != nullptr || totalBytes > readback.nativeSize;
        ID3D12Resource* readbackTarget = readback.resource.Get();
        if (needsPaddingResource)
        {
            if (readback.readbackStaging == nullptr || readback.readbackStagingSize < totalBytes)
            {
                if (readback.readbackStaging != nullptr)
                {
                    // 同帧或前帧的 copy 仍可能引用旧 staging；移交到真实 fence 退休。
                    const auto retiredFence = readback.readbackFence ? readback.readbackFence : UINT64_MAX;
                    m_retiredReadbacks.emplace_back(retiredFence, std::move(readback.readbackStaging));
                }
                D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
                D3D12_RESOURCE_DESC readbackDescription{};
                readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                readbackDescription.Width = totalBytes;
                readbackDescription.Height = 1U;
                readbackDescription.DepthOrArraySize = 1U;
                readbackDescription.MipLevels = 1U;
                readbackDescription.Format = DXGI_FORMAT_UNKNOWN;
                readbackDescription.SampleDesc.Count = 1U;
                readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ThrowIfFailed(device.CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&readback.readbackStaging)),
                              "ID3D12Device::CreateCommittedResource(texture readback padding)");
                readback.readbackStagingSize = totalBytes;
                Internal::SetDebugName(readback.readbackStaging.Get(),
                                       ToWide(readback.debugName.empty() ? "M6.D3D12.ReadbackPadding"
                                                                         : readback.debugName + ".ReadbackPadding"));
                ++m_livePayloadResources;
            }
            readbackTarget = readback.readbackStaging.Get();
        }

        Transition(source, ResourceAccess::CopySource);
        if (!needsPaddingResource)
            Transition(readback, ResourceAccess::CopyDestination);
        m_logicalAccess[sourceHandle] = ResourceAccess::CopySource;
        m_logicalAccess[readbackHandle] = ResourceAccess::CopyDestination;
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = source.resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0U;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readbackTarget;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        list.CopyTextureRegion(&dst, 0U, 0U, 0U, &src, nullptr);
        readback.readbackSource = sourceHandle;
        readback.readbackOwnerSerial = m_activeFrame->serial;
        readback.readbackFence = 0U;
        readback.readbackFootprint = footprint;
        readback.readbackTotalBytes = totalBytes;
        readback.readbackRowCount = rowCount;
        readback.readbackRowSize = rowSize;
        readback.readbackExtent = {width, height};
        readback.readbackFormat = m_owner->Describe(sourceHandle).format;
        readback.readbackPending = true;
        readback.readbackConsumed = false;
        if (std::find(m_activeReadbacks.begin(), m_activeReadbacks.end(), &readback) == m_activeReadbacks.end())
            m_activeReadbacks.push_back(&readback);
        return;
    }
    if (operation == "WriteTimestamp")
    {
        if (event.resources.size() != 1U)
            throw std::logic_error{"WriteTimestamp native event payload is invalid"};
        auto& query = RequirePayload(m_owner->Payload(event.resources[0]), PayloadKind::Timestamp);
        if (query.timestampWritten && query.timestampOwnerSerial > m_completedOwner)
            throw std::logic_error{"timestamp query was written twice before completion"};
        list.EndQuery(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query.queryIndex);
        query.timestampWritten = true;
        query.timestampResolved = false;
        query.timestampOwnerSerial = m_activeFrame->serial;
        query.timestampFence = 0U;
        if (std::find(m_activeQueries.begin(), m_activeQueries.end(), &query) == m_activeQueries.end())
            m_activeQueries.push_back(&query);
        return;
    }
    // 内容重置只修改公共账本，不改变 native resource state 或绑定。
    if (operation == "ResetTransientContents")
        return;
    if (operation == "ApplyTransitions")
    {
        if (event.resources.size() * 3U != event.integers.size())
            throw std::logic_error{"ApplyTransitions native event payload is invalid"};
        for (std::size_t index = 0U; index < event.resources.size(); ++index)
        {
            auto& payload = AsPayload(m_owner->Payload(event.resources[index]));
            const auto before = static_cast<ResourceAccess>(event.integers[index * 3U]);
            const auto after = static_cast<ResourceAccess>(event.integers[index * 3U + 1U]);
            const auto stages = static_cast<ShaderStage>(event.integers[index * 3U + 2U]);
            const auto nativeBefore =
                payload.hasStateKey ? m_stateTracker.CurrentState(payload.stateKey) : D3D12_RESOURCE_STATE_GENERIC_READ;
            Transition(payload, after, stages);
            const auto nativeAfter =
                payload.hasStateKey ? m_stateTracker.CurrentState(payload.stateKey) : D3D12_RESOURCE_STATE_GENERIC_READ;
            if (m_capabilities.debugLayerEnabled && m_diagnosticTrace.size() < 262144)
                m_diagnosticTrace += "graph-access frame=" + std::to_string(m_activeFrame->serial) +
                                     " resource=" + m_owner->DebugName(event.resources[index]) +
                                     " logicalBefore=" + std::to_string(static_cast<unsigned>(before)) +
                                     " logicalAfter=" + std::to_string(static_cast<unsigned>(after)) +
                                     " nativeBefore=" + std::to_string(static_cast<unsigned>(nativeBefore)) +
                                     " nativeAfter=" + std::to_string(static_cast<unsigned>(nativeAfter)) + "\n";
            m_logicalAccess[event.resources[index]] = after;
        }
        const auto emitted = FlushBarriers();
        if (m_capabilities.debugLayerEnabled && m_diagnosticTrace.size() < 262144)
            m_diagnosticTrace += "graph-barrier-batch frame=" + std::to_string(m_activeFrame->serial) +
                                 " emitted=" + std::to_string(emitted) + "\n";
        return;
    }
    if (operation == "EndGraphics")
    {
        if (m_rendering)
            throw std::logic_error{"EndGraphics native event while rendering is active"};
        ResolveActiveQueries();
        FlushBarriers();
        m_queue.CloseRecording();
        m_closed = true;
        return;
    }
    throw std::logic_error{"unknown native command event: " + event.operation};
}
} // namespace MiniEngine::Rhi::D3D12

namespace MiniEngine::Rhi::D3D12
{
void D3D12RhiBackend::BindResourceSet(const std::uint32_t setIndex, Payload& setPayload,
                                      std::span<const std::uint32_t> offsets)
{
    if (m_currentPipeline == nullptr)
        throw std::logic_error{"BindResourceSet requires a bound graphics pipeline"};
    const auto& pipelineLayout = RequirePayload(m_owner->Payload(ResourceIdentity(m_currentPipeline->layoutHandle)),
                                                PayloadKind::PipelineLayout);
    const auto& layoutDesc = m_owner->Describe(m_currentPipeline->layoutHandle);
    if (setIndex >= layoutDesc.setCount || setPayload.set != setIndex)
        throw std::logic_error{"resource set index does not match the active pipeline layout"};
    std::size_t dynamicCount = 0U;
    for (const auto& binding : setPayload.bindings)
        if (binding.layout.dynamicOffset)
            ++dynamicCount;
    if (dynamicCount != offsets.size())
        throw std::logic_error{"resource set dynamic offset count is inconsistent with owner validation"};
    auto& list = m_queue.CommandList();
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());

    for (const auto& binding : setPayload.bindings)
    {
        const auto dynamicOffset = binding.dynamicIndex == UINT32_MAX ? 0U : offsets[binding.dynamicIndex];
        if (binding.fixedRootCbv)
        {
            const auto& buffer = AsPayload(m_owner->Payload(ResourceIdentity(binding.value.buffer.buffer)));
            const auto offset = CheckedAdd(binding.value.buffer.offset, dynamicOffset, "dynamic CBV offset overflow");
            const auto alignedSize = AlignUp(binding.value.buffer.size, 256U);
            if (offset > buffer.nativeSize || alignedSize > buffer.nativeSize - offset)
                throw std::out_of_range{"dynamic root CBV exceeds native buffer"};
            list.SetGraphicsRootConstantBufferView(binding.rootParameter, buffer.gpuAddress + offset);
            continue;
        }
        if (binding.fixedStaticSampler)
            continue;
    }

    for (std::uint32_t groupIndex = 0U; groupIndex < setPayload.groups.size(); ++groupIndex)
    {
        const auto& group = setPayload.groups[groupIndex];
        if (group.kind == DescriptorGroupKind::FixedSrv)
        {
            const auto target = m_srvVisible.Allocate(static_cast<std::uint32_t>(group.sourceSlots.size()));
            const auto source = m_srvStaging.Cpu(group.source);
            device.CopyDescriptorsSimple(static_cast<UINT>(group.sourceSlots.size()), m_srvVisible.Cpu(target), source,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            list.SetGraphicsRootDescriptorTable(group.rootParameter, m_srvVisible.Gpu(target));
            m_activeRanges.push_back({&m_srvVisible, target});
            continue;
        }
        if (group.bindingIndices.empty())
            continue;
        const bool samplerGroup = group.kind == DescriptorGroupKind::Sampler;
        const auto target = samplerGroup
                                ? m_samplerVisible.Allocate(static_cast<std::uint32_t>(group.bindingIndices.size()))
                                : m_srvVisible.Allocate(static_cast<std::uint32_t>(group.bindingIndices.size()));
        for (std::uint32_t slot = 0U; slot < group.bindingIndices.size(); ++slot)
        {
            const auto& binding = setPayload.bindings[group.bindingIndices[slot]];
            const auto destination =
                samplerGroup ? m_samplerVisible.Cpu(target.base + slot) : m_srvVisible.Cpu(target.base + slot);
            if (binding.layout.type == BindingType::UniformBuffer && binding.layout.dynamicOffset)
            {
                const auto& buffer = AsPayload(m_owner->Payload(ResourceIdentity(binding.value.buffer.buffer)));
                const auto offset = CheckedAdd(binding.value.buffer.offset, offsets[binding.dynamicIndex],
                                               "dynamic descriptor CBV offset overflow");
                const auto size = AlignUp(binding.value.buffer.size, 256U);
                if (offset > buffer.nativeSize || size > buffer.nativeSize - offset)
                    throw std::out_of_range{"dynamic descriptor CBV exceeds native buffer"};
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                cbv.BufferLocation = buffer.gpuAddress + offset;
                cbv.SizeInBytes = AsUint(size, "dynamic descriptor CBV size");
                device.CreateConstantBufferView(&cbv, destination);
            }
            else
            {
                const auto source = samplerGroup ? m_samplerStaging.Cpu(group.source.base + slot)
                                                 : m_srvStaging.Cpu(group.source.base + slot);
                device.CopyDescriptorsSimple(1U, destination, source,
                                             samplerGroup ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                                                          : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
        list.SetGraphicsRootDescriptorTable(group.rootParameter,
                                            samplerGroup ? m_samplerVisible.Gpu(target) : m_srvVisible.Gpu(target));
        m_activeRanges.push_back({samplerGroup ? &m_samplerVisible : &m_srvVisible, target});
    }

    // 冻结的 M5 root 没有 sampler table，s0/s1/s2 是 static sampler。
    // 该 profile 中的 sampler 已校验固定描述，因此不重复绑定 heap。
    (void)pipelineLayout;
}

void D3D12RhiBackend::ResolveActiveQueries()
{
    if (m_activeQueries.empty())
        return;
    auto& list = m_queue.CommandList();
    for (auto* query : m_activeQueries)
    {
        if (query == nullptr || !query->timestampWritten || query->timestampResolved)
            continue;
        list.ResolveQueryData(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query->queryIndex, 1U,
                              query->queryReadback.Get(), 0U);
        query->timestampResolved = true;
    }
}

void D3D12RhiBackend::SetSubmissionFence(const std::uint64_t ownerSerial, const std::uint64_t actualFence)
{
    if (ownerSerial == 0U || actualFence == 0U)
        throw std::invalid_argument{"owner serial and native fence must be non-zero"};
    if (ownerSerial <= m_lastOwnerSerial)
        throw std::logic_error{"owner serial mapping must be strictly monotonic"};
    if (!m_ownerToFence.empty() && actualFence < m_ownerToFence.rbegin()->second)
        throw std::logic_error{"native fence mapping must be monotonic"};
    m_ownerToFence.emplace(ownerSerial, actualFence);
    m_lastOwnerSerial = ownerSerial;
}

void D3D12RhiBackend::BeginFrame(const FrameToken& frame, ResourcePayload& chainPayload)
{
    RequireAttached("BeginFrame");
    if (m_activeFrame.has_value() || m_closed || m_rendering)
        throw std::logic_error{"D3D12RhiBackend::BeginFrame while another frame is active"};
    auto& chain = RequirePayload(chainPayload, PayloadKind::SwapChain);
    if (chain.swapChain == nullptr)
        throw std::logic_error{"swap-chain payload has no native swap chain"};
    if (frame.serial == 0U)
        throw std::invalid_argument{"cannot begin a zero-size frame"};
    // 公共 recycle lane 与实际 DXGI backbuffer index 是独立身份；
    // M5 FrameContext 按已等待的 recycle lane 复用，backbuffer 从 chain 单独取得。
    auto& context = m_queue.BeginFrame(frame.recycleLane);
    m_stateTracker.BeginRecording();
    ID3D12DescriptorHeap* heaps[] = {&m_srvVisible.Native(), &m_samplerVisible.Native()};
    m_queue.CommandList().SetDescriptorHeaps(2U, heaps);
    m_activeFrame = frame;
    m_activeChain = &chain;
    m_activeContext = &context;
    m_currentPipeline = nullptr;
    m_activeRanges.clear();
    m_activeQueries.clear();
    m_activeReadbacks.clear();
    m_attachments.clear();
    m_rendering = false;
    m_closed = false;
    // 帧级轮转：把上一帧的查询区间入队并 signal Tracy 自己的 payload fence；zone 必须
    // 写进本帧的 command list，因此这里同时设置录制目标。
    if (m_gpuProfiler)
    {
        m_gpuProfiler->SetCommandList(&m_queue.CommandList());
        m_gpuProfiler->NewFrame();
    }
    m_device->RecordDiagnosticEvent("M6.Rhi.BeginFrame");
}

void D3D12RhiBackend::Submit(const FrameToken& frame)
{
    RequireAttached("Submit");
    if (!m_activeFrame.has_value() || !m_activeContext || frame.serial != m_activeFrame->serial || !m_closed ||
        m_rendering)
        throw std::logic_error{"D3D12RhiBackend::Submit requires the matching closed frame"};
    ResolveActiveQueries();
    const auto fence = m_queue.ExecuteClosedAndSignal(*m_activeContext);
    // 提交之后回读已完成的 GPU timestamp（渲染线程上的唯一 collect 点）。
    if (m_gpuProfiler)
        m_gpuProfiler->Collect();
    m_stateTracker.CommitExecuted();
    m_uploadManager.CommitFrame(fence);
    for (const auto& retired : m_activeRanges)
        if (retired.heap != nullptr)
            retired.heap->Retire(retired.range, fence);
    for (auto* query : m_activeQueries)
    {
        if (query != nullptr && query->timestampWritten)
            query->timestampFence = fence;
    }
    for (auto* readback : m_activeReadbacks)
    {
        if (readback != nullptr && readback->readbackPending)
            readback->readbackFence = fence;
    }
    for (auto& retired : m_retiredReadbacks)
        if (retired.first == UINT64_MAX)
            retired.first = fence;
    SetSubmissionFence(frame.serial, fence);
    ++m_submittedBatches;
    m_device->UpdateRemovalContext(m_queue.NextFenceValue(), fence, m_queue.CompletedValue());
    m_activeRanges.clear();
    m_activeQueries.clear();
    m_activeReadbacks.clear();
    m_activeContext = nullptr;
    m_activeChain = nullptr;
    m_activeFrame.reset();
    m_currentPipeline = nullptr;
    m_closed = false;
    m_rendering = false;
}

void D3D12RhiBackend::Present(ResourcePayload& chainPayload)
{
    auto& chain = RequirePayload(chainPayload, PayloadKind::SwapChain);
    if (chain.swapChain == nullptr)
        throw std::logic_error{"Present requires a native swap chain"};
    static_cast<void>(chain.swapChain->Present());
    m_device->RecordDiagnosticEvent("M6.Rhi.Present");
}
} // namespace MiniEngine::Rhi::D3D12

namespace MiniEngine::Rhi::D3D12
{
std::vector<DeviceLifetime::BackBufferCandidate> D3D12RhiBackend::ResizeBackBuffers(ResourcePayload& chainPayload,
                                                                                    const SwapChainDesc& desc)
{
    RequireAttached("ResizeBackBuffers");
    auto& chain = RequirePayload(chainPayload, PayloadKind::SwapChain);
    if (chain.swapChain == nullptr)
        throw std::logic_error{"swap-chain payload has no native swap chain"};
    if (desc.bufferCount != kSwapChainBufferCount || desc.format != Format::Rgba8Unorm)
        throw std::invalid_argument{"D3D12 backbuffer replacement requires the frozen three-buffer RGBA8 profile"};
    if (desc.extent.width == 0U || desc.extent.height == 0U)
        return {};

    // 调用本回调前 owner 已释放全部旧 backbuffer payload；
    // swap chain 仍持有原生 buffer，可在这里安全执行 resize。
    const auto& config = chain.swapChain->Config();
    if (config.width != desc.extent.width || config.height != desc.extent.height)
        chain.swapChain->RequestResize(desc.extent.width, desc.extent.height);
    if (chain.swapChain->HasPendingResize())
        chain.swapChain->ApplyPendingResize(m_queue);
    if (chain.swapChain->IsSuspended() || chain.swapChain->IsFailed())
        throw std::runtime_error{"D3D12 swap-chain resize did not produce a renderable configuration"};
    if (chain.swapChain->Config().width != desc.extent.width || chain.swapChain->Config().height != desc.extent.height)
        throw std::logic_error{"D3D12 swap-chain resize completed at an unexpected extent"};

    if (desc.extent.height != 0U &&
        static_cast<std::uint64_t>(desc.extent.width) >
            std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(desc.extent.height))
        throw std::overflow_error{"backbuffer extent pixel count overflow"};
    const auto pixels = static_cast<std::uint64_t>(desc.extent.width) * desc.extent.height;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / 4U)
        throw std::overflow_error{"backbuffer byte count overflow"};

    std::vector<DeviceLifetime::BackBufferCandidate> result;
    result.reserve(kSwapChainBufferCount);
    for (std::uint32_t index = 0U; index < kSwapChainBufferCount; ++index)
    {
        const auto name = desc.debugName.empty() ? std::format("M6.D3D12.BackBuffer[{}]", index)
                                                 : desc.debugName + std::format(".BackBuffer[{}]", index);
        auto payload = std::make_unique<Payload>(this, PayloadKind::Texture, name);
        payload->resource = &chain.swapChain->BackBuffer(index);
        payload->stateKey = chain.swapChain->BackBufferKey(index);
        payload->hasStateKey = true;
        // swap chain 在 resize 时注销并重新注册这些 key；
        // 候选 payload 的析构不能替 swap chain 再注销一次。
        payload->ownsStateKey = false;
        payload->backBuffer = true;
        payload->initialState = D3D12_RESOURCE_STATE_PRESENT;
        payload->nativeSize = pixels * 4U;
        payload->gpuAddress = 0; // 纹理没有 GPU VA；只有 buffer 才查询地址。
        payload->rtvCpu = chain.swapChain->RtvHandle(index);
        payload->nativeResourceCount = 0U;
        Internal::SetDebugName(payload->resource.Get(), ToWide(name));

        DeviceLifetime::BackBufferCandidate candidate;
        candidate.desc.dimension = TextureDimension::Texture2D;
        candidate.desc.extent = desc.extent;
        candidate.desc.mipLevels = 1U;
        candidate.desc.arrayLayers = 1U;
        candidate.desc.sampleCount = 1U;
        candidate.desc.format = desc.format;
        candidate.desc.usage = TextureUsage::ColorAttachment | TextureUsage::CopySource;
        candidate.desc.debugName = name;
        candidate.payload = std::move(payload);
        result.push_back(std::move(candidate));
    }
    return result;
}

std::uint32_t D3D12RhiBackend::CurrentBackBufferIndex(ResourcePayload& chainPayload)
{
    RequireAttached("CurrentBackBufferIndex");
    auto& chain = RequirePayload(chainPayload, PayloadKind::SwapChain);
    if (chain.swapChain == nullptr)
        throw std::logic_error{"CurrentBackBufferIndex requires a native swap chain"};
    const auto index = chain.swapChain->CurrentBackBufferIndex();
    if (index >= kSwapChainBufferCount)
        throw std::out_of_range{"native swap-chain backbuffer index exceeds the fixed buffer count"};
    return index;
}

std::unique_ptr<ResourcePayload> D3D12RhiBackend::CreateDynamicBuffer(const BufferDesc& desc,
                                                                      std::span<const std::byte> bytes)
{
    RequireAttached("CreateDynamicBuffer");
    if (!m_activeFrame.has_value() || m_activeContext == nullptr || m_closed)
        throw std::logic_error{"CreateDynamicBuffer requires an open frame"};
    if (desc.memory != MemoryDomain::CpuToGpu || bytes.empty())
        throw std::invalid_argument{"dynamic D3D12 buffers require nonempty CPU-to-GPU data"};

    const auto requested = (std::max)(desc.size, static_cast<std::uint64_t>(bytes.size()));
    const auto allocationSize = AlignUp(requested, 256U);
    const auto allocation = m_uploadManager.Allocate(allocationSize, 256U, "dynamic-buffer");
    if (!allocation)
        throw std::runtime_error{"D3D12 dynamic buffer upload allocation returned an empty span"};
    std::memcpy(allocation.cpu, bytes.data(), bytes.size());

    auto payload = std::make_unique<Payload>(this, PayloadKind::Buffer, desc.debugName);
    payload->dynamic = true;
    payload->uploadHeap = true;
    payload->initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    payload->nativeSize = allocation.size;
    payload->gpuAddress = allocation.gpu;
    // ring/dedicated backing 由 D3D12UploadManager 持有至当帧 fence 完成；
    // slice payload 自身不持有 COM 对象。
    payload->nativeResourceCount = 0U;
    return payload;
}

void D3D12RhiBackend::RecordStandaloneUpload(Payload& destination, const std::uint64_t offset,
                                             const std::span<const std::byte> bytes, const std::uint64_t serial)
{
    if (destination.kind != PayloadKind::Buffer || destination.resource == nullptr || bytes.empty())
        throw std::invalid_argument{"standalone buffer upload has an invalid destination or empty data"};
    if (offset > destination.nativeSize || static_cast<std::uint64_t>(bytes.size()) > destination.nativeSize - offset)
        throw std::out_of_range{"standalone buffer upload exceeds the native destination"};

    UploadAllocation staging{};
    if (destination.uploadHeap)
    {
        std::byte* mapped = nullptr;
        const D3D12_RANGE readRange{0U, 0U};
        ThrowIfFailed(destination.resource->Map(0U, &readRange, reinterpret_cast<void**>(&mapped)),
                      "ID3D12Resource::Map(buffer upload)");
        std::memcpy(mapped + offset, bytes.data(), bytes.size());
        const D3D12_RANGE writtenRange{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + bytes.size())};
        destination.resource->Unmap(0U, &writtenRange);
    }
    else
    {
        staging = m_uploadManager.Allocate(static_cast<std::uint64_t>(bytes.size()),
                                           UploadRingAllocator::kBufferCopyAlignment, "buffer-upload");
        if (!staging || staging.source == nullptr)
            throw std::runtime_error{"D3D12 buffer upload staging allocation is empty"};
        std::memcpy(staging.cpu, bytes.data(), bytes.size());
    }

    if (serial == 0U)
        throw std::invalid_argument{"standalone buffer upload requires a nonzero owner serial"};
    const auto lane = static_cast<std::uint32_t>((serial - 1U) % kD3D12FrameContextCount);
    auto& context = m_queue.BeginFrame(lane);
    m_stateTracker.BeginRecording();
    m_activeFrame = FrameToken{serial, 0U, lane, {}};
    m_activeContext = &context;
    m_activeChain = nullptr;
    m_activeRanges.clear();
    m_activeQueries.clear();
    m_activeReadbacks.clear();
    m_attachments.clear();
    m_currentPipeline = nullptr;
    m_rendering = false;
    m_closed = false;

    try
    {
        if (!destination.uploadHeap)
        {
            Transition(destination, ResourceAccess::CopyDestination);
            FlushBarriers();
            m_queue.CommandList().CopyBufferRegion(destination.resource.Get(), offset, staging.source, staging.offset,
                                                   bytes.size());
            // 直接恢复实际创建状态；逻辑 ResourceAccess::None 只是 owner 值，
            // 不能据此省略必要的原生 barrier。
            m_stateTracker.Transition(destination.stateKey, destination.initialState);
            FlushBarriers();
        }
        FinishStandaloneSubmission(serial, context);
    }
    catch (...)
    {
        if (m_stateTracker.IsRecording())
            m_stateTracker.Rollback();
        throw;
    }
}

void D3D12RhiBackend::RecordStandaloneTextureUpload(Payload& destination,
                                                    const std::span<const TextureSubresourceData> data,
                                                    const std::uint64_t serial)
{
    if (destination.kind != PayloadKind::Texture || destination.resource == nullptr || data.empty())
        throw std::invalid_argument{"standalone texture upload has an invalid destination or empty data"};
    if (serial == 0U)
        throw std::invalid_argument{"standalone texture upload requires a nonzero owner serial"};

    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    const auto description = destination.resource->GetDesc();
    const auto probe = PlanTextureUpload(device, description, 0U);
    const auto staging =
        m_uploadManager.Allocate(probe.totalBytes, UploadRingAllocator::kTexturePlacementAlignment, "texture-upload");
    if (!staging || staging.source == nullptr)
        throw std::runtime_error{"D3D12 texture upload staging allocation is empty"};
    const auto plan = PlanTextureUpload(device, description, staging.offset);
    if (data.size() != plan.subresourceCount || plan.totalBytes > staging.size)
        throw std::out_of_range{"texture upload data or footprint does not fit its staging allocation"};
    const auto uploadByteSize = CheckedAdd(staging.offset, staging.size, "texture staging range overflow");
    // PlanTextureUpload 的 offset 相对 upload resource；allocation.cpu
    // 已包含 allocation.offset，填充前须还原到资源基址。
    auto* uploadBase = staging.cpu - static_cast<std::ptrdiff_t>(staging.offset);
    for (std::uint32_t index = 0U; index < plan.subresourceCount; ++index)
    {
        const auto& source = data[index];
        PackTextureRows(
            plan, index, uploadBase, uploadByteSize,
            TextureUploadSource{source.bytes.data(), source.rowPitch, static_cast<std::uint64_t>(source.bytes.size())});
    }

    const auto lane = static_cast<std::uint32_t>((serial - 1U) % kD3D12FrameContextCount);
    auto& context = m_queue.BeginFrame(lane);
    m_stateTracker.BeginRecording();
    m_activeFrame = FrameToken{serial, 0U, lane, {}};
    m_activeContext = &context;
    m_activeChain = nullptr;
    m_activeRanges.clear();
    m_activeQueries.clear();
    m_activeReadbacks.clear();
    m_attachments.clear();
    m_currentPipeline = nullptr;
    m_rendering = false;
    m_closed = false;

    try
    {
        Transition(destination, ResourceAccess::CopyDestination);
        FlushBarriers();
        RecordTextureCopies(m_queue.CommandList(), *destination.resource.Get(), plan, *staging.source);
        // 普通纹理以 COMMON 创建，深度纹理以 DEPTH_WRITE 创建；
        // tracker 始终保留实际 native state。
        m_stateTracker.Transition(destination.stateKey, destination.initialState);
        FlushBarriers();
        FinishStandaloneSubmission(serial, context);
    }
    catch (...)
    {
        if (m_stateTracker.IsRecording())
            m_stateTracker.Rollback();
        throw;
    }
}

void D3D12RhiBackend::FinishStandaloneSubmission(const std::uint64_t serial, D3D12FrameContext& context)
{
    if (!m_activeFrame.has_value() || m_activeContext != &context || m_activeFrame->serial != serial)
        throw std::logic_error{"standalone submission context is not active"};
    ResolveActiveQueries();
    if (!m_closed)
    {
        m_queue.CloseRecording();
        m_closed = true;
    }
    const auto fence = m_queue.ExecuteClosedAndSignal(context);
    m_stateTracker.CommitExecuted();
    m_uploadManager.CommitFrame(fence);
    for (const auto& retired : m_activeRanges)
        if (retired.heap != nullptr)
            retired.heap->Retire(retired.range, fence);
    for (auto* query : m_activeQueries)
        if (query != nullptr && query->timestampWritten)
            query->timestampFence = fence;
    for (auto* readback : m_activeReadbacks)
        if (readback != nullptr && readback->readbackPending)
            readback->readbackFence = fence;
    SetSubmissionFence(serial, fence);
    ++m_submittedBatches;
    m_device->UpdateRemovalContext(m_queue.NextFenceValue(), fence, m_queue.CompletedValue());

    m_activeRanges.clear();
    m_activeQueries.clear();
    m_activeReadbacks.clear();
    m_activeContext = nullptr;
    m_activeChain = nullptr;
    m_activeFrame.reset();
    m_currentPipeline = nullptr;
    m_closed = false;
    m_rendering = false;
}

void D3D12RhiBackend::UploadBuffer(ResourcePayload& destination, const std::uint64_t offset,
                                   const std::span<const std::byte> bytes, const std::uint64_t serial)
{
    RequireAttached("UploadBuffer");
    auto& payload = RequirePayload(destination, PayloadKind::Buffer);
    RecordStandaloneUpload(payload, offset, bytes, serial);
    m_device->RecordDiagnosticEvent("M6.Rhi.UploadBuffer");
}

void D3D12RhiBackend::UploadTexture(ResourcePayload& destination, const std::span<const TextureSubresourceData> data,
                                    const std::uint64_t serial)
{
    RequireAttached("UploadTexture");
    auto& payload = RequirePayload(destination, PayloadKind::Texture);
    RecordStandaloneTextureUpload(payload, data, serial);
    m_device->RecordDiagnosticEvent("M6.Rhi.UploadTexture");
}
} // namespace MiniEngine::Rhi::D3D12
