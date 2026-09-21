#include "D3D11Capabilities.h"
#include <MiniEngine/Rhi/RhiError.h>
#include <Windows.h>
#include <array>
#include <chrono>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <thread>
#include <wrl/client.h>

namespace MiniEngine::Rhi::D3D11
{
namespace
{
using Microsoft::WRL::ComPtr;
std::string Utf8(const wchar_t* value)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, nullptr, nullptr);
    text.pop_back();
    return text;
}
std::string DriverVersion(IDXGIAdapter& adapter)
{
    LARGE_INTEGER version{};
    if (FAILED(adapter.CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
    {
        return {};
    }
    const auto bits = static_cast<std::uint64_t>(version.QuadPart);
    return std::to_string((bits >> 48) & 65535) + '.' + std::to_string((bits >> 32) & 65535) + '.' +
           std::to_string((bits >> 16) & 65535) + '.' + std::to_string(bits & 65535);
}
UINT FormatSupport(ID3D11Device& device, DXGI_FORMAT format)
{
    UINT result = 0;
    return SUCCEEDED(device.CheckFormatSupport(format, &result)) ? result : 0;
}
std::uint8_t LowerFormat(UINT typed, UINT sampled, UINT resource, UINT dimension)
{
    if ((resource & dimension) == 0)
    {
        return 0;
    }
    // API 没有 COPY_SUPPORT 位：合法同型、单采样资源可 CopyResource。
    // 此处基于真实资源类型支持 + API copy 契约推导，不伪称独立原生 copy 查询。
    std::uint8_t result = kFormatSupportCopySource | kFormatSupportCopyDestination;
    if ((sampled & dimension) != 0 && (sampled & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0)
    {
        result |= kFormatSupportSampled;
    }
    if ((typed & dimension) != 0 && (typed & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0)
    {
        result |= kFormatSupportColor;
    }
    if ((typed & dimension) != 0 && (typed & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0)
    {
        result |= kFormatSupportDepth;
    }
    return result;
}
void QueryTimestamps(ID3D11Device& device, ID3D11DeviceContext& context, RhiCapabilities& c, std::uint32_t timeout)
{
    ComPtr<ID3D11Query> disjoint, timestamp;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(device.CreateQuery(&desc, &disjoint)))
    {
        return;
    }
    desc.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(device.CreateQuery(&desc, &timestamp)))
    {
        return;
    }
    context.Begin(disjoint.Get());
    context.End(timestamp.Get());
    context.End(disjoint.Get());
    context.Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};
        const HRESULT status = context.GetData(disjoint.Get(), &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(status))
        {
            return;
        }
        if (status == S_OK)
        {
            UINT64 ticks = 0;
            const HRESULT timestampStatus =
                context.GetData(timestamp.Get(), &ticks, sizeof(ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(timestampStatus) || result.Disjoint || result.Frequency == 0)
            {
                return;
            }
            if (timestampStatus == S_OK)
            {
                c.gpuTimestamps = true;
                c.timestampFrequency = result.Frequency;
                return;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
}
} // namespace
RhiCapabilities QueryCapabilities(ID3D11Device& device, ID3D11DeviceContext& context, std::uint32_t timeoutMilliseconds)
{
    ComPtr<ID3D11Device> contextDevice;
    context.GetDevice(&contextDevice);
    if (contextDevice.Get() != &device || context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        timeoutMilliseconds > 10000)
    {
        throw RhiValidationError(RhiError{RhiErrorCode::InvalidArgument, "QueryCapabilities", "Device", "", "d3d11",
                                          "requires matching immediate context and bounded timeout"});
    }
    if (device.GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
    {
        throw RhiValidationError(RhiError{RhiErrorCode::Unsupported, "QueryCapabilities", "Device", "", "d3d11",
                                          "M6 capability mapping requires feature level 11_0"});
    }
    RhiCapabilities c;
    c.backend = RhiBackend::D3D11;
    c.debugLayerEnabled = (device.GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG) != 0;
    c.gpuValidationEnabled = false; // D3D11 不存在 GBV 开关。
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
    {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
        {
            c.adapterName = Utf8(desc.Description);
            c.adapterLuid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                            desc.AdapterLuid.LowPart;
            c.driverVersion = DriverVersion(*adapter.Get());
        }
    }
    // limits 由实际 feature level 对照 API 保证得出，非任意乐观默认值。
    if (device.GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0)
    {
        c.maxColorAttachments = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
        c.maxTextureDimension2D = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        c.maxAnisotropy = D3D11_REQ_MAXANISOTROPY;
    }
    // M6 offset 契约按两个后端共同可实现的 256 B；D3D11 后续用独立 CB / 11.1 range lowering。
    c.uniformBufferOffsetAlignment = 256;
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
        c.formatSupport[i] = LowerFormat(typed, sampled, resource, D3D11_FORMAT_SUPPORT_TEXTURE2D);
        c.cubeFormatSupport[i] = LowerFormat(typed, sampled, resource, D3D11_FORMAT_SUPPORT_TEXTURECUBE);
        if (formats[i] == DXGI_FORMAT_D32_FLOAT)
        {
            c.depthComparisonSampling = (sampled & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON) != 0;
        }
    }
    QueryTimestamps(device, context, c, timeoutMilliseconds);
    return c;
}
} // namespace MiniEngine::Rhi::D3D11
