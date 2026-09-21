#include "NativeDevice.h"
#include "RenderGraphTestFixtures.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <gtest/gtest.h>

using namespace MiniEngine;
using namespace MiniEngine::Rhi;
namespace RG = MiniEngine::RenderGraph;
namespace
{
void RunGraphClearReadback(RhiBackend backend, bool warp)
{
    constexpr Extent2D extent{32, 24};
    InputState input;
    WindowDesc windowDesc;
    windowDesc.title = L"M6-07 graph adapter verification";
    windowDesc.width = extent.width;
    windowDesc.height = extent.height;
    windowDesc.visible = false;
    WindowsWindow window(windowDesc, input);
    RhiDeviceCreateInfo info;
    info.nativeWindow = window.NativeHandle();
    info.useWarp = warp;
    info.enableDebugLayer = true;
    info.enableGpuValidation = backend == RhiBackend::D3D12;
    auto device = CreateRhiDevice(backend, info);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    SwapChainDesc chainDesc;
    chainDesc.extent = extent;
    chainDesc.vsync = false;
    const auto chain = device->CreateSwapChain(chainDesc);
    BufferDesc readbackDesc{extent.width * extent.height * 4, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu,
                            "M607.readback"};
    const auto readback = device->CreateBuffer(readbackDesc, {});
    const auto frame = device->BeginFrame(chain);
    const auto backState = device->QueryTextureState(frame, frame.backBuffer);
    EXPECT_FALSE(backState.fullyDefined);
    EXPECT_EQ(backState.access, ResourceAccess::Present);
    EXPECT_FALSE(device->QueryBufferState(frame, readback).fullyDefined);
    auto badFrame = frame;
    ++badFrame.owner;
    EXPECT_THROW((void)device->QueryTextureState(badFrame, frame.backBuffer), RhiException);

    RG::RenderGraph graph;
    auto back = graph.ImportTexture("BackBuffer", {frame.backBuffer,
                                                   backState.descriptor,
                                                   backState.access,
                                                   ResourceAccess::Present,
                                                   RG::ContentState::Undefined,
                                                   "swapchain",
                                                   {}});
    auto cpu = graph.ImportBuffer("Readback", {readback,
                                               readbackDesc,
                                               ResourceAccess::None,
                                               ResourceAccess::CopyDestination,
                                               RG::ContentState::Undefined,
                                               "test owner",
                                               {}});
    auto transient = graph.CreateTexture("Transient", RG::Tests::MakeColorDesc());
    RG::Tests::AddAttachmentPass(graph, "TransientClear", transient);
    graph.AddPass<RG::Tests::TexturePassData>(
        "Clear",
        [&](RG::RgBuilder& builder, RG::Tests::TexturePassData& data)
        {
            back = data.texture = builder.Write(back, ResourceAccess::ColorWrite);
            builder.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store, {0.25F, 0.5F, 0.75F, 1.0F});
        },
        [](const auto& data, const RG::RgResources& resources, IRhiCommandList&)
        { EXPECT_TRUE(resources.Get(data.texture)); });
    struct CopyData
    {
        RG::RgTexture source;
        RG::RgBuffer destination;
    };
    graph.AddPass<CopyData>(
        "Readback",
        [&](RG::RgBuilder& builder, CopyData& data)
        {
            data.source = builder.Read(back, ResourceAccess::CopySource);
            cpu = data.destination = builder.Write(cpu, ResourceAccess::CopyDestination, RG::WriteCoverage::Full);
        },
        [](const CopyData& data, const RG::RgResources& resources, IRhiCommandList& commands)
        { commands.CopyTextureForReadback(resources.Get(data.source), resources.Get(data.destination), extent); });
    graph.Export(cpu);
    graph.Present(back);
    auto plan = graph.Compile();
    const auto aliveBefore = device->Diagnostics().aliveObjects;
    plan.Execute(*device, frame);
    EXPECT_EQ(graph.Phase(), RG::GraphPhase::Executed);
    EXPECT_EQ(device->Diagnostics().aliveObjects, aliveBefore);
    EXPECT_EQ(device->QueryTextureState(frame, frame.backBuffer).access, ResourceAccess::Present);
    EXPECT_TRUE(device->QueryTextureState(frame, frame.backBuffer).fullyDefined);
    EXPECT_TRUE(device->QueryBufferState(frame, readback).fullyDefined);
    device->EndFrame(frame, chain);
    EXPECT_THROW((void)device->QueryBufferState(frame, readback), RhiException);
    device->WaitIdle();
    const auto result = device->TryReadTextureReadback(readback);
    ASSERT_TRUE(result);
    ASSERT_FALSE(result->unavailable);
    ASSERT_EQ(result->extent, extent);
    ASSERT_GE(result->bytes.size(), result->rowPitch * extent.height);
    // 验证每个像素，防止空回读或错误 source 被当作成功 clear。
    for (std::uint32_t y = 0; y < extent.height; ++y)
    {
        for (std::uint32_t x = 0; x < extent.width; ++x)
        {
            const auto offset = y * result->rowPitch + x * 4;
            EXPECT_NEAR(std::to_integer<int>(result->bytes[offset]), 64, 1);
            EXPECT_NEAR(std::to_integer<int>(result->bytes[offset + 1]), 128, 1);
            EXPECT_NEAR(std::to_integer<int>(result->bytes[offset + 2]), 191, 1);
            EXPECT_EQ(std::to_integer<int>(result->bytes[offset + 3]), 255);
        }
    }
    device->Destroy(readback);
    device->Destroy(chain);
    native.Shutdown();
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U);
    EXPECT_EQ(report.liveResources, 0U);
    EXPECT_EQ(device->Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device->Diagnostics().retiringObjects, 0U);
}
TEST(GraphNative, D3D11HardwareClearReadbackAndRetirement)
{
    RunGraphClearReadback(RhiBackend::D3D11, false);
}
TEST(GraphNative, D3D12HardwareClearReadbackAndRetirement)
{
    RunGraphClearReadback(RhiBackend::D3D12, false);
}
TEST(GraphNative, D3D11WarpClearReadbackAndRetirement)
{
    RunGraphClearReadback(RhiBackend::D3D11, true);
}
TEST(GraphNative, D3D12WarpClearReadbackAndRetirement)
{
    RunGraphClearReadback(RhiBackend::D3D12, true);
}
} // namespace
