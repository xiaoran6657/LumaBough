#include "D3D11Capabilities.h"
#include "D3D12Capabilities.h"
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <MiniEngine/Rhi/RhiValidation.h>
#include <Windows.h>
#include <array>
#include <cstdlib>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <wrl/client.h>

using namespace MiniEngine::Rhi;
using Microsoft::WRL::ComPtr;
namespace
{
struct Run final
{
    const char* name;
    bool warp;
    bool debug;
    bool gbv = false;
};
void PrintTo(const Run& value, std::ostream* stream)
{
    *stream << value.name;
}
void Record(const RhiCapabilities& c, const char* name)
{
    std::ostringstream json;
    json << "{\"backend\":" << std::quoted(std::string(ToString(c.backend))) << ",\"run\":" << std::quoted(name)
         << ",\"adapterName\":" << std::quoted(c.adapterName) << ",\"adapterLuid\":" << c.adapterLuid
         << ",\"driverVersion\":" << std::quoted(c.driverVersion)
         << ",\"maxColorAttachments\":" << c.maxColorAttachments
         << ",\"maxTextureDimension2D\":" << c.maxTextureDimension2D << ",\"maxAnisotropy\":" << c.maxAnisotropy
         << ",\"uniformBufferOffsetAlignment\":" << c.uniformBufferOffsetAlignment
         << ",\"gpuTimestamps\":" << std::boolalpha << c.gpuTimestamps
         << ",\"timestampFrequency\":" << c.timestampFrequency
         << ",\"depthComparisonSampling\":" << c.depthComparisonSampling
         << ",\"debugLayerEnabled\":" << c.debugLayerEnabled << ",\"gpuValidationEnabled\":" << c.gpuValidationEnabled
         << ",\"formatSupport\":[";
    for (std::size_t i = 0; i < c.kFormatCount; ++i)
    {
        if (i)
        {
            json << ',';
        }
        json << static_cast<unsigned>(c.formatSupport[i]);
    }
    json << "],\"cubeFormatSupport\":[";
    for (std::size_t i = 0; i < c.kFormatCount; ++i)
    {
        if (i)
        {
            json << ',';
        }
        json << static_cast<unsigned>(c.cubeFormatSupport[i]);
    }
    json << "]}";
    std::cout << "M6_CAPABILITIES " << json.str() << '\n';
    const DWORD required = GetEnvironmentVariableW(L"M602_EVIDENCE_DIR", nullptr, 0);
    if (required != 0)
    {
        std::wstring output(required, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"M602_EVIDENCE_DIR", output.data(), required);
        ASSERT_GT(length, 0U);
        ASSERT_LT(length, required);
        output.resize(length);
        std::filesystem::create_directories(output);
        std::ofstream file(std::filesystem::path(output) / (std::string(ToString(c.backend)) + '-' + name + ".json"));
        ASSERT_TRUE(file.good());
        file << json.str() << '\n';
        ASSERT_TRUE(file.good());
    }
}
void CheckFixedProfile(const RhiCapabilities& c)
{
    EXPECT_FALSE(c.adapterName.empty());
    EXPECT_NE(c.adapterLuid, 0U);
    EXPECT_GT(c.timestampFrequency, 0U);
    EXPECT_TRUE(c.gpuTimestamps);
    const auto assessment = AssessM6Capabilities(c);
    EXPECT_EQ(assessment.status, RhiCapabilityStatus::Ready) << (assessment.error ? assessment.error->message : "");
    EXPECT_FALSE(c.SupportsSampled(Format::Unknown));
    EXPECT_FALSE(c.SupportsSampled(Format::Count));
    TextureDesc hdr;
    hdr.extent = {1280, 720};
    hdr.format = Format::Rgba16Float;
    hdr.usage = TextureUsage::Sampled | TextureUsage::ColorAttachment;
    EXPECT_NO_THROW(ValidateTextureDesc(hdr, c));
    TextureDesc shadow;
    shadow.extent = {2048, 2048};
    shadow.format = Format::D32Float;
    shadow.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
    EXPECT_NO_THROW(ValidateTextureDesc(shadow, c));
    auto cube = hdr;
    cube.extent = {128, 128};
    cube.dimension = TextureDimension::TextureCube;
    cube.arrayLayers = 6;
    cube.mipLevels = 8;
    EXPECT_NO_THROW(ValidateTextureDesc(cube, c));
    auto unavailable = c;
    unavailable.formatSupport[static_cast<std::size_t>(Format::Rgba16Float)] = 0;
    const auto blocked = AssessM6Capabilities(unavailable);
    EXPECT_EQ(blocked.status, RhiCapabilityStatus::Blocked);
    ASSERT_TRUE(blocked.error);
    EXPECT_EQ(blocked.error->code, RhiErrorCode::Unsupported);
}
void D11Clean(ID3D11Device& device)
{
    ComPtr<ID3D11InfoQueue> queue;
    ASSERT_HRESULT_SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(&queue)));
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
    {
        SIZE_T length = 0;
        ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, nullptr, &length));
        std::vector<std::byte> bytes(length);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(i, message, &length));
        EXPECT_GT(message->Severity, D3D11_MESSAGE_SEVERITY_WARNING) << message->pDescription;
    }
}
class D11CapabilityQuery : public testing::TestWithParam<Run>
{
};
TEST_P(D11CapabilityQuery, MatchesDeviceFlagsAndNativeFormats)
{
    const auto run = GetParam();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, run.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                               nullptr, run.debug ? D3D11_CREATE_DEVICE_DEBUG : 0, levels, 2,
                                               D3D11_SDK_VERSION, &device, &level, &context));
    const auto c = D3D11::QueryCapabilities(*device.Get(), *context.Get());
    Record(c, run.name);
    EXPECT_EQ(c.backend, RhiBackend::D3D11);
    EXPECT_EQ(c.debugLayerEnabled, run.debug);
    EXPECT_FALSE(c.gpuValidationEnabled);
    CheckFixedProfile(c);
    constexpr std::array native{
        DXGI_FORMAT_UNKNOWN,      DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_D32_FLOAT,    DXGI_FORMAT_D24_UNORM_S8_UINT};
    for (std::size_t i = 1; i < native.size(); ++i)
    {
        UINT typed = 0, sampled = 0;
        ASSERT_HRESULT_SUCCEEDED(device->CheckFormatSupport(native[i], &typed));
        ASSERT_HRESULT_SUCCEEDED(device->CheckFormatSupport(i == 6   ? DXGI_FORMAT_R32_FLOAT
                                                            : i == 7 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                                                     : native[i],
                                                            &sampled));
        const auto format = static_cast<Format>(i);
        EXPECT_EQ(c.SupportsSampled(format),
                  (sampled & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) ==
                      (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE));
        EXPECT_EQ(c.SupportsColorAttachment(format), (typed & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0);
        EXPECT_EQ(c.SupportsDepthAttachment(format), (typed & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0);
        EXPECT_EQ(c.SupportsSampled(format, TextureDimension::TextureCube),
                  (sampled & (D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) ==
                      (D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE));
    }
    if (run.debug)
    {
        D11Clean(*device.Get());
    }
}
INSTANTIATE_TEST_SUITE_P(M6, D11CapabilityQuery,
                         testing::Values(Run{"HardwareRelease", false, false}, Run{"HardwareDebug", false, true},
                                         Run{"WarpDebug", true, true}),
                         [](const testing::TestParamInfo<Run>& info) { return info.param.name; });
class D12CapabilityQuery : public testing::TestWithParam<Run>
{
};
TEST_P(D12CapabilityQuery, MatchesCreatedDeviceAndNativeFormats)
{
    const auto run = GetParam();
    D3D12::DeviceCreateOptions options;
    options.debugLayer = run.debug;
    options.gpuValidation = run.gbv;
    options.warp = run.warp;
    auto wrapper = D3D12::D3D12Device::Create(options);
    auto& device = *static_cast<ID3D12Device*>(wrapper->NativeDeviceHandle());
    const auto c = D3D12::QueryCapabilities(*wrapper);
    Record(c, run.name);
    EXPECT_EQ(c.backend, RhiBackend::D3D12);
    EXPECT_EQ(c.debugLayerEnabled, run.debug);
    EXPECT_EQ(c.gpuValidationEnabled, run.gbv);
    EXPECT_EQ(wrapper->Metadata().adapter.isWarp, run.warp);
    CheckFixedProfile(c);
    const auto luid = device.GetAdapterLuid();
    EXPECT_EQ(c.adapterLuid,
              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart);
    constexpr std::array native{
        DXGI_FORMAT_UNKNOWN,      DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_D32_FLOAT,    DXGI_FORMAT_D24_UNORM_S8_UINT};
    for (std::size_t i = 1; i < native.size(); ++i)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT typed{native[i], D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT sampled{i == 6   ? DXGI_FORMAT_R32_FLOAT
                                                  : i == 7 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                                           : native[i],
                                                  D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        ASSERT_HRESULT_SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &typed, sizeof(typed)));
        ASSERT_HRESULT_SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &sampled, sizeof(sampled)));
        const auto format = static_cast<Format>(i);
        EXPECT_EQ(c.SupportsSampled(format), (sampled.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0);
        EXPECT_EQ(c.SupportsColorAttachment(format), (typed.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0);
        EXPECT_EQ(c.SupportsDepthAttachment(format), (typed.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0);
        EXPECT_EQ(c.SupportsSampled(format, TextureDimension::TextureCube),
                  (sampled.Support1 & (D3D12_FORMAT_SUPPORT1_TEXTURECUBE | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) ==
                      (D3D12_FORMAT_SUPPORT1_TEXTURECUBE | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE));
    }
    if (run.debug)
    {
        const auto messages = wrapper->DrainInfoQueue();
        EXPECT_FALSE(messages.HasFailure()) << messages.messages.size();
    }
}
INSTANTIATE_TEST_SUITE_P(M6, D12CapabilityQuery,
                         testing::Values(Run{"HardwareRelease", false, false}, Run{"HardwareDebug", false, true},
                                         Run{"HardwareGBV", false, true, true}, Run{"WarpDebug", true, true}),
                         [](const testing::TestParamInfo<Run>& info) { return info.param.name; });
} // namespace
