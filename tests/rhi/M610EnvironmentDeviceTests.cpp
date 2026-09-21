// M6-10：生产 factory 的 IBL 完整生成、公开所有权与原生诊断回归。
#include "NativeDevice.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Rhi/RhiFactory.h>
#include <array>
#include <gtest/gtest.h>

using namespace MiniEngine;
using namespace MiniEngine::Rhi;
namespace
{
void EnvironmentRoundTrip(RhiBackend backend)
{
    InputState input;
    WindowDesc windowDesc;
    windowDesc.width = 64;
    windowDesc.height = 48;
    windowDesc.visible = false;
    windowDesc.title = L"M6 IBL owner validation";
    WindowsWindow window(windowDesc, input);
    RhiDeviceCreateInfo info;
    info.nativeWindow = window.NativeHandle();
    info.enableDebugLayer = true;
    auto device = CreateRhiDevice(backend, info);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    TextureDesc panoramaDesc;
    panoramaDesc.extent = {4, 2};
    panoramaDesc.format = Format::Rgba16Float;
    panoramaDesc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
    panoramaDesc.debugName = "M610.ConstantPanorama";
    const auto panorama = device->CreateTexture(panoramaDesc);
    std::array<std::uint16_t, 32> pixels{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        pixels[i * 4] = 0x4000;
        pixels[i * 4 + 1] = 0x3C00;
        pixels[i * 4 + 2] = 0x3800;
        pixels[i * 4 + 3] = 0x3C00;
    }
    const TextureSubresourceData upload{std::as_bytes(std::span(pixels)), 32, 64};
    device->UploadTexture(panorama, std::span(&upload, 1));
    const auto prepared = PrepareRhiEnvironment(*device, panorama, M610_TEST_SHADER_ROOT, 1);
    EXPECT_EQ(prepared.sourceRevision, 1);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(prepared.textures[i]);
        EXPECT_EQ(prepared.descriptors[i].usage, TextureUsage::Sampled);
    }
    EXPECT_EQ(prepared.descriptors[0].extent.width, 512);
    EXPECT_EQ(prepared.descriptors[0].mipLevels, 10);
    EXPECT_EQ(prepared.descriptors[1].extent.width, 32);
    EXPECT_EQ(prepared.descriptors[2].extent.width, 128);
    EXPECT_EQ(prepared.descriptors[3].format, Format::Rg16Float);
    EXPECT_EQ(native.NativeReport().warningErrors, 0);
    for (auto texture : prepared.textures)
        device->Destroy(texture);
    device->Destroy(panorama);
    device->WaitIdle();
    EXPECT_EQ(device->Diagnostics().aliveObjects, 0);
    EXPECT_EQ(device->Diagnostics().retiringObjects, 0);
    native.Shutdown();
    const auto final = native.NativeReport(true);
    EXPECT_EQ(final.warningErrors, 0);
    EXPECT_EQ(final.liveResources, 0);
}
TEST(M610Environment, D3D11PublishesOnlyValidatedCompleteTextureOwners)
{
    EnvironmentRoundTrip(RhiBackend::D3D11);
}
TEST(M610Environment, D3D12PublishesOnlyValidatedCompleteTextureOwners)
{
    EnvironmentRoundTrip(RhiBackend::D3D12);
}
} // namespace
