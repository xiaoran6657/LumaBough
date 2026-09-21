#include "M604ShaderFixtures.h"
#include "NativeDevice.h"
#include "SmokeScene.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <MiniEngine/Rhi/RhiValidation.h>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
using namespace MiniEngine;
using namespace MiniEngine::Rhi;
using namespace MiniEngine::Sandbox;
namespace
{
SmokeShaders LoadShaders(std::span<const ShaderPackage> packages, RhiBackend backend)
{
    SmokeShaders shaders;
    for (const auto& package : packages)
    {
        if (package.assetId == "ToneMapVSMain")
            shaders.toneVertex = SelectShader(package, backend, ShaderStage::Vertex);
        if (package.assetId == "ToneMapPSMain")
            shaders.tonePixel = SelectShader(package, backend, ShaderStage::Pixel);
        if (package.assetId == "ShadowDepthVSMain")
            shaders.depthVertex = SelectShader(package, backend, ShaderStage::Vertex);
    }
    return shaders;
}
void PartialUpload(RhiBackend backend)
{
    InputState input;
    WindowDesc windowDesc;
    windowDesc.visible = false;
    windowDesc.width = 32;
    windowDesc.height = 24;
    WindowsWindow window(windowDesc, input);
    RhiDeviceCreateInfo info;
    info.nativeWindow = window.NativeHandle();
    info.enableDebugLayer = true;
    info.enableGpuValidation = backend == RhiBackend::D3D12;
    auto device = CreateRhiDevice(backend, info);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    SwapChainDesc chainDesc;
    chainDesc.extent = {32, 24};
    chainDesc.vsync = false;
    const auto chain = device->CreateSwapChain(chainDesc);
    {
        const auto packages = M604::LoadM604Packages();
        SmokeScene scene(*device, LoadShaders(packages, backend), {32, 24}, 2);
        // EV2 位于未覆盖的前16B；只改 offset16 后，实际 shader 必须仍消费 EV2。
        std::array<float, 16> original{};
        original[0] = 2.0F;
        const auto constants =
            device->CreateBuffer({64, BufferUsage::Uniform, MemoryDomain::CpuToGpu, "partial upload preserve"},
                                 std::as_bytes(std::span(original)));
        device->WaitIdle();
        const std::array<float, 4> replacement{9, 8, 7, 6};
        device->UploadBuffer(constants, 16, std::as_bytes(std::span(replacement)));
        device->WaitIdle();
        auto setDesc = native.Lifetime().Describe(scene.ToneSet());
        for (auto& binding : setDesc.bindings)
            if (binding.binding == 9)
                binding.buffer = {constants, 0, 16};
        const auto set = device->CreateResourceSet(setDesc);
        const auto readback = device->CreateBuffer(
            {32 * 24 * 4, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "partial upload pixels"}, {});
        const auto frame = device->BeginFrame(chain);
        auto& commands = device->BeginGraphics(frame);
        auto& sink = device->GraphCommandSink(frame);
        const std::array<GraphResourceImport, 4> imports{
            {{frame.backBuffer, {}}, {scene.Source(), {}}, {{}, constants}, {{}, readback}}};
        sink.ImportResources(imports);
        const std::array<AccessTransition, 4> initial{
            {{frame.backBuffer, {}, ResourceAccess::Present, ResourceAccess::ColorWrite},
             {scene.Source(), {}, ResourceAccess::None, ResourceAccess::SampledRead},
             {{}, constants, ResourceAccess::None, ResourceAccess::UniformRead},
             {{}, readback, ResourceAccess::None, ResourceAccess::CopyDestination}}};
        sink.ApplyTransitions(initial);
        Render::SmokeBindings bindings;
        bindings.pipeline = scene.TonePipeline();
        bindings.sets[0] = set;
        bindings.setCount = 1;
        Render::FullscreenPass(commands, frame.backBuffer, {32, 24}, bindings);
        const AccessTransition copy{frame.backBuffer, {}, ResourceAccess::ColorWrite, ResourceAccess::CopySource};
        sink.ApplyTransitions(std::span(&copy, 1));
        commands.CopyTextureForReadback(frame.backBuffer, readback, {32, 24});
        // 同帧重复截图覆盖同一结果槽；旧 staging 必须活到真实完成。
        commands.CopyTextureForReadback(frame.backBuffer, readback, {32, 24});
        const AccessTransition present{frame.backBuffer, {}, ResourceAccess::CopySource, ResourceAccess::Present};
        sink.ApplyTransitions(std::span(&present, 1));
        device->EndGraphics(frame, commands);
        device->EndFrame(frame, chain);
        device->WaitIdle();
        const auto image = device->TryReadTextureReadback(readback);
        ASSERT_TRUE(image);
        const auto tone = [](float hdr)
        {
            const float linear = hdr / (hdr + 1);
            const float srgb = linear <= .0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1 / 2.4F) - .055F;
            return static_cast<int>(std::round(srgb * 255));
        };
        const std::array<int, 4> expected{tone(16), tone(8), tone(4), 255};
        for (std::size_t i = 0; i < image->bytes.size(); ++i)
            ASSERT_LE(std::abs(static_cast<int>(std::to_integer<unsigned char>(image->bytes[i])) - expected[i % 4]), 1);
        device->Destroy(set);
        device->Destroy(constants);
        device->Destroy(readback);
    }
    device->Destroy(chain);
    native.Shutdown();
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U) << report.trace;
    EXPECT_EQ(report.liveResources, 0U) << report.trace;
}
TEST(M606Regression, D3D11PartialUploadPreservesShaderVisiblePrefix)
{
    PartialUpload(RhiBackend::D3D11);
}
TEST(M606Regression, D3D12PartialUploadPreservesShaderVisiblePrefix)
{
    PartialUpload(RhiBackend::D3D12);
}
TEST(M606Regression, D3D11CubeAttachmentViewsCreateWithoutWarning)
{
    RhiDeviceCreateInfo info;
    info.enableDebugLayer = true;
    auto device = CreateRhiDevice(RhiBackend::D3D11, info);
    TextureDesc desc{TextureDimension::TextureCube,
                     {16, 16},
                     1,
                     6,
                     1,
                     Format::Rgba16Float,
                     TextureUsage::Sampled | TextureUsage::ColorAttachment,
                     "cube attachment"};
    const auto texture = device->CreateTexture(desc);
    device->Destroy(texture);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    native.Shutdown();
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U) << report.trace;
    EXPECT_EQ(report.liveResources, 0U) << report.trace;
}
TEST(M606Regression, D3D11NonDebugTraceDoesNotAccumulateFrames)
{
    InputState input;
    WindowDesc windowDesc;
    windowDesc.visible = false;
    windowDesc.width = 16;
    windowDesc.height = 16;
    WindowsWindow window(windowDesc, input);
    RhiDeviceCreateInfo info;
    info.nativeWindow = window.NativeHandle();
    auto device = CreateRhiDevice(RhiBackend::D3D11, info);
    SwapChainDesc desc;
    desc.extent = {16, 16};
    desc.vsync = false;
    const auto chain = device->CreateSwapChain(desc);
    {
        SmokeScene scene(*device, {}, desc.extent, 1);
        for (int i = 0; i < 128; ++i)
            scene.Render(chain, false);
    }
    device->Destroy(chain);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    native.Shutdown();
    EXPECT_LT(native.NativeReport().trace.size(), 1024U);
    EXPECT_EQ(native.SemanticTrace(), "miniengine.native-rhi-semantic.v1\n");
}
TEST(M606Regression, InvalidClearHintsFailBeforeNativeCreation)
{
    RhiCapabilities caps;
    caps.maxTextureDimension2D = 16384;
    caps.formatSupport.fill(31);
    TextureDesc desc{TextureDimension::Texture2D,   {16, 16},    1, 1, 1, Format::Rgba8Unorm,
                     TextureUsage::ColorAttachment, "clear hint"};
    desc.clearColorHint[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(ValidateTextureDesc(desc, caps), RhiException);
    desc.clearColorHint[0] = 0;
    desc.clearDepthHint = 2;
    EXPECT_THROW(ValidateTextureDesc(desc, caps), RhiException);
}
} // namespace

TEST(M606Regression, D3D12RetiredTimestampSlotsAreReusedBeyondHeapCapacity)
{
    RhiDeviceCreateInfo info;
    info.enableDebugLayer = true;
    auto device = CreateRhiDevice(RhiBackend::D3D12, info);
    for (int i = 0; i < 4200; ++i)
    {
        const auto query = device->CreateTimestampQuery("recycled slot");
        device->Destroy(query);
        if (i % 32 == 31)
            device->WaitIdle();
    }
    auto& native = dynamic_cast<NativeDevice&>(*device);
    native.Shutdown();
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U) << report.trace;
    EXPECT_EQ(report.liveResources, 0U) << report.trace;
}
