#include "D3D12Capabilities.h"
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <array>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include <wrl/client.h>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
using Microsoft::WRL::ComPtr;
UINT FormatSupport(ID3D12Device& device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    return SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
               ? static_cast<UINT>(support.Support1)
               : 0;
}
std::uint8_t LowerFormat(UINT typed, UINT sampled, UINT resource, UINT dimension)
{
    if ((resource & dimension) == 0)
    {
        return 0;
    }
    // 合法同型资源的 copy 由 CopyResource 契约保证；无独立 copy feature bit。
    std::uint8_t result = kFormatSupportCopySource | kFormatSupportCopyDestination;
    if ((sampled & dimension) != 0 && (sampled & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0)
    {
        result |= kFormatSupportSampled;
    }
    if ((typed & dimension) != 0 && (typed & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0)
    {
        result |= kFormatSupportColor;
    }
    if ((typed & dimension) != 0 && (typed & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0)
    {
        result |= kFormatSupportDepth;
    }
    return result;
}
} // namespace
RhiCapabilities QueryCapabilities(const D3D12Device& wrapper)
{
    auto& device = *static_cast<ID3D12Device*>(wrapper.NativeDeviceHandle());
    const auto& metadata = wrapper.Metadata();
    RhiCapabilities c;
    c.backend = RhiBackend::D3D12;
    c.adapterName = metadata.adapter.description;
    c.adapterLuid = (static_cast<std::uint64_t>(metadata.adapter.luidHigh) << 32) | metadata.adapter.luidLow;
    c.debugLayerEnabled = metadata.debugLayerActive;
    c.gpuValidationEnabled = metadata.gpuValidationActive;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(device.GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
    {
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
        {
            const auto bits = static_cast<std::uint64_t>(version.QuadPart);
            c.driverVersion = std::to_string((bits >> 48) & 65535) + '.' + std::to_string((bits >> 32) & 65535) + '.' +
                              std::to_string((bits >> 16) & 65535) + '.' + std::to_string(bits & 65535);
        }
    }
    constexpr std::array levels{D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels{static_cast<UINT>(levels.size()), levels.data(),
                                                    D3D_FEATURE_LEVEL_11_0};
    if (SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels))) &&
        featureLevels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
    {
        c.maxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
        c.maxTextureDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        c.maxAnisotropy = D3D12_REQ_MAXANISOTROPY;
    }
    c.uniformBufferOffsetAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    constexpr std::array formats{
        DXGI_FORMAT_UNKNOWN,      DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_D32_FLOAT,    DXGI_FORMAT_D24_UNORM_S8_UINT};
    for (std::size_t i = 1; i < formats.size(); ++i)
    {
        const bool d24 = formats[i] == DXGI_FORMAT_D24_UNORM_S8_UINT;
        const bool depth = formats[i] == DXGI_FORMAT_D32_FLOAT || d24;
        const UINT typed = FormatSupport(device, formats[i]);
        const UINT sampled =
            depth ? FormatSupport(device, d24 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT) : typed;
        const UINT resource =
            depth ? FormatSupport(device, d24 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS) : typed;
        c.cubeFormatSupport[i] = LowerFormat(typed, sampled, resource, D3D12_FORMAT_SUPPORT1_TEXTURECUBE);
        c.formatSupport[i] = LowerFormat(typed, sampled, resource, D3D12_FORMAT_SUPPORT1_TEXTURE2D);
        if (formats[i] == DXGI_FORMAT_D32_FLOAT)
        {
            c.depthComparisonSampling = (sampled & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON) != 0;
        }
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(device.CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
    {
        UINT64 frequency = 0;
        if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency != 0)
        {
            c.gpuTimestamps = true;
            c.timestampFrequency = frequency;
        }
    }
    return c;
}
} // namespace MiniEngine::Rhi::D3D12
