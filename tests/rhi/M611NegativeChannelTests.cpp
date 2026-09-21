// ============================================================================
// M611NegativeChannelTests.cpp — M6-11 后端负向通道（公共 RHI 抽象层）
// 职责：通过公共 RHI + 测试专用开关，故意跳过一次 D3D11 hazard unbind 或一次
//       D3D12 state transition，证明 Debug Layer/GBV 能按预期 message ID 精确捕获；
//       同一场景的正常帧零消息作为正向对照。
// 边界：注入开关只在 backend 内部接口；生产 sample/pass 不调用，未知名称直接失败。
// 关联：docs/architecture/README.md
// ============================================================================
#include "M604ShaderFixtures.h"
#include "NativeDevice.h"
#include "SmokeScene.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <d3d11.h>
#include <d3d12.h>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
using namespace MiniEngine;
using namespace MiniEngine::Rhi;
using namespace MiniEngine::Sandbox;

SmokeShaders Shaders(std::span<const ShaderPackage> packages, RhiBackend backend)
{
    SmokeShaders result;
    for (const auto& package : packages)
    {
        if (package.assetId == "ToneMapVSMain")
            result.toneVertex = SelectShader(package, backend, ShaderStage::Vertex);
        if (package.assetId == "ToneMapPSMain")
            result.tonePixel = SelectShader(package, backend, ShaderStage::Pixel);
        if (package.assetId == "ShadowDepthVSMain")
            result.depthVertex = SelectShader(package, backend, ShaderStage::Vertex);
    }
    return result;
}

std::vector<unsigned> MessageIds(const std::string& trace)
{
    std::vector<unsigned> ids;
    const std::string marker = "native-message=";
    for (auto at = trace.find(marker); at != std::string::npos; at = trace.find(marker, at + marker.size()))
    {
        const auto begin = at + marker.size();
        const auto end = trace.find(':', begin);
        if (end == std::string::npos)
            break;
        ids.push_back(static_cast<unsigned>(std::stoul(trace.substr(begin, end - begin))));
    }
    return ids;
}

std::string DescribeIds(const std::vector<unsigned>& ids)
{
    std::string text;
    for (const auto id : ids)
        text += std::to_string(id) + " ";
    return text;
}

bool ContainsId(const std::vector<unsigned>& ids, std::uint32_t expected)
{
    for (const auto id : ids)
        if (id == expected)
            return true;
    return false;
}

// 隐藏窗口 + 真实 factory device + swapchain；负向与正向对照共用同一构造。
struct NegativeHarness final
{
    explicit NegativeHarness(RhiBackend backend)
    {
        desc.title = L"M6-11 negative channel";
        desc.width = 64;
        desc.height = 48;
        desc.visible = false;
        window = std::make_unique<WindowsWindow>(desc, input);
        info.nativeWindow = window->NativeHandle();
        info.enableDebugLayer = true;
        info.enableGpuValidation = backend == RhiBackend::D3D12;
        device = CreateRhiDevice(backend, info);
        native = &dynamic_cast<NativeDevice&>(*device);
        chainDesc.extent = {64, 48};
        chainDesc.vsync = false;
        chainDesc.debugName = "M6-11 negative chain";
        chain = device->CreateSwapChain(chainDesc);
    }
    ~NegativeHarness()
    {
        try
        {
            device->Destroy(chain);
            device->WaitIdle();
            native->Shutdown();
        }
        catch (...)
        {
        }
    }
    InputState input;
    WindowDesc desc;
    std::unique_ptr<WindowsWindow> window;
    RhiDeviceCreateInfo info;
    std::unique_ptr<IRhiDevice> device;
    NativeDevice* native = nullptr;
    SwapChainDesc chainDesc;
    SwapChainHandle chain{};
};

// 负向帧预期由帧诊断门禁抛出带 native message 的 RhiException。
struct NegativeOutcome final
{
    bool threw = false;
    std::string message;
};

NegativeOutcome RunNegativeFrame(const std::function<SmokeFrameResult()>& render)
{
    NegativeOutcome outcome;
    try
    {
        (void)render();
    }
    catch (const RhiException& error)
    {
        outcome.threw = true;
        outcome.message = error.what();
    }
    return outcome;
}

TEST(M611NegativeChannel, D3D11HazardUnbindSkipIsCapturedByDebugLayer)
{
    NegativeHarness harness(RhiBackend::D3D11);
    auto packages = M604::LoadM604Packages();
    const auto shaders = Shaders(packages, RhiBackend::D3D11);
    SmokeScene scene(*harness.device, shaders, {64, 48}, 2);
    const auto render = [&] { return scene.Render(harness.chain, true); };
    // 正向对照：同一场景正常帧必须零 native 消息，且没有注入记录。
    (void)render();
    harness.device->WaitIdle();
    const auto clean = harness.native->NativeReport();
    EXPECT_EQ(clean.warningErrors, 0U) << clean.trace;
    EXPECT_EQ(clean.injectedFaults, 0U);
    // 负向：跳过下一次冲突 output 的主动 unbind，资源仍是 RTV 时绑定 SRV。
    harness.native->InjectBackendFaultForTesting("skip-next-hazard-unbind");
    const auto outcome = RunNegativeFrame(render);
    const auto report = harness.native->NativeReport();
    EXPECT_EQ(report.injectedFaults, 1U) << "fault injection must actually be consumed";
    const auto ids = MessageIds(report.trace);
    for (const auto id : ids)
        std::cout << "captured native-message id=" << id << " injected=" << report.injectedFaults << std::endl;
    EXPECT_TRUE(outcome.threw) << "injected hazard must be reported by the frame diagnostics gate\n" << report.trace;
    EXPECT_GT(report.warningErrors, 0U);
    EXPECT_TRUE(ContainsId(ids, D3D11_MESSAGE_ID_DEVICE_PSSETSHADERRESOURCES_HAZARD))
        << "observed ids: " << DescribeIds(ids) << "\n"
        << report.trace;
}

TEST(M611NegativeChannel, D3D12TransitionSkipIsCapturedByGpuValidation)
{
    NegativeHarness harness(RhiBackend::D3D12);
    auto packages = M604::LoadM604Packages();
    const auto shaders = Shaders(packages, RhiBackend::D3D12);
    // 级别 3 使用真实 depth 源：DEPTH_WRITE → shader-read 的漏 barrier 正是 GBV 942 的覆盖方向。
    SmokeScene scene(*harness.device, shaders, {64, 48}, 3);
    const auto render = [&] { return scene.Render(harness.chain, true); };
    (void)render();
    harness.device->WaitIdle();
    const auto clean = harness.native->NativeReport();
    EXPECT_EQ(clean.warningErrors, 0U) << clean.trace;
    EXPECT_EQ(clean.injectedFaults, 0U);
    // 负向：第二次渲染漏掉下一次 read 方向 state transition，GBV 必须精确捕获。
    // GBV 的资源状态校验在执行后落地，因此显式 WaitIdle 再排空报告，而不是只依赖
    // EndFrame 的即时门禁。
    harness.native->InjectBackendFaultForTesting("skip-next-read-transition");
    (void)RunNegativeFrame(render);
    harness.device->WaitIdle();
    const auto report = harness.native->NativeReport();
    EXPECT_EQ(report.injectedFaults, 1U) << "fault injection must actually be consumed";
    const auto ids = MessageIds(report.trace);
    for (const auto id : ids)
        std::cout << "captured native-message id=" << id << " injected=" << report.injectedFaults << std::endl;
    EXPECT_GT(report.warningErrors, 0U);
    EXPECT_TRUE(ContainsId(ids, D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_RESOURCE_STATE))
        << "observed ids: " << DescribeIds(ids) << "\n"
        << report.trace;
}
} // namespace
