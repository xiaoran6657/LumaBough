// ============================================================================
// D3D12SwapChainDeviceTests.cpp — 真 GPU 上的交换链、状态往返与安全点 Resize
// 里程碑：M5（04 篇 Command List、SwapChain 与 Resize）
// 职责：在真实 Device + 真实 HWND 上验证 04 篇的交换链契约：
//       三缓冲 FLIP_DISCARD 创建、current index 取自身、back buffer 每帧
//       PRESENT→RENDER_TARGET→PRESENT 往返、Present 前状态断言、resize 只在
//       flush 后一次性重建、0×0 只挂起不 ResizeBuffers，以及全程 Debug Layer
//       零消息。纯决策逻辑由 SwapChainConfigTests 覆盖。
// 说明：基础用例使用隐藏窗口；G01 受控实验另覆盖可见 HWND、vsync、GBV 和 WARP。
// 关联：docs/architecture/README.md（阶段验收）
// ============================================================================
#include "D3D12Queue.h"
#include "D3D12Renderer.h"
#include "D3D12ResourceStateTracker.h"
#include "D3D12SwapChain.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <Windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12Queue;
using MiniEngine::Rhi::D3D12::D3D12SwapChain;

namespace
{
// 隐藏窗口：仅作交换链宿主，避免测试弹窗抢焦点。
class HiddenWindow final
{
  public:
    HiddenWindow()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"MiniEngineD3D12TestWindow";
        RegisterClassExW(&windowClass); // 已注册时返回 0，可忽略（多用例共享类名）

        m_window = CreateWindowExW(0, windowClass.lpszClassName, L"MiniEngine D3D12 test window", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, instance, nullptr);
        // 保持隐藏：测试不需要可见窗口，避免 ctest 运行时弹窗抢焦点。
    }

    ~HiddenWindow()
    {
        if (m_window != nullptr)
        {
            DestroyWindow(m_window);
        }
    }

    [[nodiscard]] void* Handle() const noexcept
    {
        return m_window;
    }

    void BindResize(MiniEngine::Rhi::D3D12::D3D12Renderer* renderer)
    {
        m_renderer = renderer;
        SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    [[nodiscard]] std::uint32_t ResizeMessages() const noexcept
    {
        return m_resizeMessages;
    }

  private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* self = reinterpret_cast<HiddenWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_SIZE && self != nullptr && self->m_renderer != nullptr)
        {
            ++self->m_resizeMessages;
            self->m_renderer->RequestResize(LOWORD(lparam), HIWORD(lparam));
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    MiniEngine::Rhi::D3D12::D3D12Renderer* m_renderer = nullptr;
    std::uint32_t m_resizeMessages = 0;
    HWND m_window = nullptr;
};

// 必须晚于资源、交换链、队列析构，再枚举调试层；原夹具只检查了释放之前。
struct ValidationAfterRelease final
{
    D3D12Device* device = nullptr;

    ~ValidationAfterRelease()
    {
        if (device != nullptr)
        {
            const auto report = device->DrainInfoQueue();
            for (const auto& message : report.messages)
            {
                EXPECT_GT(message.severity, 2U) << "post-release ID=" << message.id << " " << message.description;
            }
        }
    }
};

// 设备 + 队列 + 交换链的公共前置（debug 模式：让零消息 Gate 有实际意义）。
struct SwapChainHarness final
{
    std::unique_ptr<D3D12Device> device;
    ValidationAfterRelease validation;
    HiddenWindow window;
    D3D12Queue queue;
    // 07 篇：状态真源是 tracker；交换链把 back buffer 注册进来并委托它记账。
    MiniEngine::Rhi::D3D12::D3D12ResourceStateTracker tracker;
    D3D12SwapChain swapChain;

    SwapChainHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = D3D12Device::Create(options);
        validation.device = device.get();
        // 公共头只暴露不透明句柄（边界约定）；类型还原在后端内部/测试处完成。
        auto* const nativeDevice = static_cast<ID3D12Device*>(device->NativeDeviceHandle());
        auto* const nativeFactory = static_cast<IDXGIFactory7*>(device->NativeFactoryHandle());
        // 顺序与生产一致：Queue 必须先创建，CreateSwapChainForHwnd 的 device 参数
        // 就是它（顺序颠倒会以 null queue 调用 → DXGI_ERROR_INVALID_CALL）。
        queue.Initialize(*nativeDevice);
        swapChain.Initialize(*nativeDevice, *nativeFactory, queue.NativeQueue(), tracker, window.Handle(), 320U, 240U,
                             false);
    }

    // fence 完成后立即析构，不靠时间等待掩盖资源寿命错误。
    ~SwapChainHarness()
    {
        queue.FlushGpu("test-teardown");
    }

    // 走一遍"标准帧"：barrier 进/出 + 清屏 + 提交 + 呈现（与 D3D12Renderer 同序，
    // 07 篇起状态经由 tracker 登记 + 分批 flush）。
    bool RenderFrameOnce()
    {
        const std::uint32_t index = swapChain.CurrentBackBufferIndex();
        tracker.BeginRecording();
        MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = queue.BeginFrame(index);
        ID3D12GraphicsCommandList& list = queue.CommandList();

        swapChain.TrackTransition(index, D3D12_RESOURCE_STATE_RENDER_TARGET);
        static_cast<void>(tracker.FlushBarriersTo(list));

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain.RtvHandle(index);
        list.OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0.04F, 0.08F, 0.14F, 1.0F};
        list.ClearRenderTargetView(rtv, clear, 0, nullptr);

        swapChain.TrackTransition(index, D3D12_RESOURCE_STATE_PRESENT);
        static_cast<void>(tracker.FlushBarriersTo(list));

        static_cast<void>(queue.ExecuteAndSignal(frame));
        tracker.CommitExecuted();
        return swapChain.Present();
    }
};
} // namespace

// 创建 Gate：三缓冲、current index 合法、三个 back buffer 状态均为 PRESENT、
// 且 tearing 能力与 vsync=0 的组合被正确记录（04 篇"metadata 记录实际 flag"）。
TEST(D3D12SwapChainDeviceTests, InitializeCreatesThreeBufferPresentState)
{
    SwapChainHarness harness;

    const MiniEngine::Rhi::D3D12::SwapChainConfig& facts = harness.swapChain.Config();
    EXPECT_EQ(facts.width, 320U);
    EXPECT_EQ(facts.height, 240U);
    EXPECT_EQ(facts.bufferCount, 3U) << "baseline 固定三缓冲（与 FrameContext 数一致）";
    EXPECT_FALSE(facts.vsync);
    EXPECT_TRUE(facts.tearingEnabled == facts.tearingSupported)
        << "vsync=0 时 tearingEnabled 必须等于驱动能力（能力为真则标志生效）";
    if (facts.tearingEnabled)
    {
        EXPECT_EQ(facts.flags, static_cast<std::uint32_t>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));
    }
    else
    {
        EXPECT_EQ(facts.flags, 0U);
    }

    const std::uint32_t index = harness.swapChain.CurrentBackBufferIndex();
    EXPECT_LT(index, 3U);
    for (std::uint32_t buffer = 0; buffer < 3U; ++buffer)
    {
        EXPECT_EQ(harness.swapChain.TrackedState(buffer), D3D12_RESOURCE_STATE_PRESENT)
            << "tracker 初始状态固定为 PRESENT（04 篇 Back buffer 与 RTV 节）";
    }

    // 创建期不应产生任何调试层消息（零容忍 Gate 的起点）。
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 连续 12 帧（4 次完整轮转）：每帧状态往返、current index 由 SwapChain 决定、
// 每帧结束三个 back buffer 都回到 PRESENT、fence 单调、退出零消息。
TEST(D3D12SwapChainDeviceTests, FramesCycleBackBuffersAndReturnToPresentState)
{
    SwapChainHarness harness;

    std::uint64_t previousFence = 0;
    for (std::uint32_t frame = 0; frame < 12U; ++frame)
    {
        const std::uint32_t indexBefore = harness.swapChain.CurrentBackBufferIndex();
        const bool presented = harness.RenderFrameOnce();
        if (!presented)
        {
            // 隐藏窗口被遮挡时 Present 返回 false：跳过该帧（不把"未呈现"当失败）。
            continue;
        }

        EXPECT_NE(harness.swapChain.TrackedState(indexBefore), D3D12_RESOURCE_STATE_RENDER_TARGET)
            << "呈现后不得停留在 RENDER_TARGET（Present 会断言，这里再确认一次）";
        for (std::uint32_t buffer = 0; buffer < 3U; ++buffer)
        {
            EXPECT_EQ(harness.swapChain.TrackedState(buffer), D3D12_RESOURCE_STATE_PRESENT)
                << "每帧结束时全部 back buffer 必须回到 PRESENT";
        }

        const std::uint64_t fence = harness.queue.NextFenceValue() - 1U;
        EXPECT_GT(fence, previousFence) << "fence 严格单调";
        previousFence = fence;
    }

    harness.queue.FlushGpu("test-end");
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "状态往返不得产生调试层消息";
}

// Resize：记录 pending 不立即生效；Apply 时 flush → 重建 → 状态复位 PRESENT。
TEST(D3D12SwapChainDeviceTests, ResizeAppliesAfterFlushAndResetsState)
{
    SwapChainHarness harness;
    static_cast<void>(harness.RenderFrameOnce());
    harness.swapChain.RequestResize(640U, 480U);
    EXPECT_TRUE(harness.swapChain.HasPendingResize()) << "WM_SIZE 只记录，不在消息路径动 GPU 资源";

    harness.swapChain.ApplyPendingResize(harness.queue);
    EXPECT_FALSE(harness.swapChain.HasPendingResize());
    EXPECT_FALSE(harness.swapChain.IsSuspended());
    EXPECT_EQ(harness.swapChain.Config().width, 640U);
    EXPECT_EQ(harness.swapChain.Config().height, 480U);
    EXPECT_EQ(harness.swapChain.Config().resizeCount, 1U);

    for (std::uint32_t buffer = 0; buffer < 3U; ++buffer)
    {
        EXPECT_EQ(harness.swapChain.TrackedState(buffer), D3D12_RESOURCE_STATE_PRESENT)
            << "resize 后 tracker 必须重新注册为 PRESENT";
        // 取回的资源必须已经是新尺寸（旧引用已释放、GetBuffer 重新取回）。
        const D3D12_RESOURCE_DESC backBufferDesc = harness.swapChain.BackBuffer(buffer).GetDesc();
        EXPECT_EQ(backBufferDesc.Width, 640U) << "resize 后必须重新取回新尺寸的 buffer";
        EXPECT_EQ(backBufferDesc.Height, 480U);
    }

    // resize 后仍能正常呈现（新尺寸下往返一次）。
    static_cast<void>(harness.RenderFrameOnce());
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "resize 不得产生调试层消息";
}

// 0×0（minimized）：只挂起，绝不调用 ResizeBuffers（resizeCount 不变）。
TEST(D3D12SwapChainDeviceTests, ZeroSizeSuspendsWithoutResizeBuffers)
{
    SwapChainHarness harness;
    static_cast<void>(harness.RenderFrameOnce());

    harness.swapChain.RequestResize(0U, 0U);
    harness.swapChain.ApplyPendingResize(harness.queue);

    EXPECT_TRUE(harness.swapChain.IsSuspended());
    EXPECT_EQ(harness.swapChain.Config().resizeCount, 0U) << "0×0 不得触发 ResizeBuffers";
    EXPECT_EQ(harness.swapChain.Config().width, 320U) << "挂起期间保持旧尺寸";

    // 恢复：以原尺寸重新申请 → 解除挂起（不重建 buffer，因为尺寸未变）。
    harness.swapChain.RequestResize(320U, 240U);
    harness.swapChain.ApplyPendingResize(harness.queue);
    EXPECT_FALSE(harness.swapChain.IsSuspended());
    EXPECT_EQ(harness.swapChain.Config().resizeCount, 0U) << "尺寸未变不需要 ResizeBuffers";

    static_cast<void>(harness.RenderFrameOnce());
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 负向：current back buffer 未回到 PRESENT 时 Present 必须拒绝（04 篇断言契约）。
TEST(D3D12SwapChainDeviceTests, PresentRequiresBackBufferInPresentState)
{
    SwapChainHarness harness;
    const std::uint32_t index = harness.swapChain.CurrentBackBufferIndex();
    harness.tracker.BeginRecording();
    MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(index);
    ID3D12GraphicsCommandList& list = harness.queue.CommandList();

    // 故意只走"进入 RENDER_TARGET"的一半，不回到 PRESENT。
    harness.swapChain.TrackTransition(index, D3D12_RESOURCE_STATE_RENDER_TARGET);
    static_cast<void>(harness.tracker.FlushBarriersTo(list));
    static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    harness.tracker.CommitExecuted();

    EXPECT_THROW(static_cast<void>(harness.swapChain.Present()), std::logic_error);

    // 收尾：补上回程，避免残留状态污染后续用例（每个用例都是独立进程，仍保持整洁）。
    harness.queue.FlushGpu("test-cleanup");
}

// G01：通过真实 HWND 的 WM_SIZE 回调走生产渲染器 Request/Apply 路径。
// 每次 Present 返回后立即 SetWindowPos，中间无 Sleep、额外 flush 或消息泵。
// 分离设备寿命，确保 renderer/queue/back buffer 释放期也处于零消息 Gate 内。
struct ImmediateResizeCase final
{
    bool visible;
    bool vsync;
    bool gbv;
    bool warp;
    const char* name;
};

void PrintTo(const ImmediateResizeCase& value, std::ostream* output)
{
    *output << value.name;
}

class D3D12ImmediateResizeTests : public ::testing::TestWithParam<ImmediateResizeCase>
{
};

TEST_P(D3D12ImmediateResizeTests, PresentThenWmSizeWithoutDelay)
{
    using namespace MiniEngine::Rhi::D3D12;
    const auto settings = GetParam();
    DeviceCreateOptions deviceOptions;
    deviceOptions.debugLayer = true;
    deviceOptions.gpuValidation = settings.gbv;
    deviceOptions.warp = settings.warp;
    auto device = D3D12Device::Create(deviceOptions);
    ValidationAfterRelease validation{device.get()};
    HiddenWindow window;
    const auto hwnd = static_cast<HWND>(window.Handle());
    if (settings.visible)
    {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    constexpr std::uint32_t kCycles = 64U;
    std::uint32_t presented = 0;
    LARGE_INTEGER frequency{};
    ASSERT_TRUE(QueryPerformanceFrequency(&frequency));
    {
        D3D12Renderer renderer;
        D3D12RendererOptions options;
        options.width = 320U;
        options.height = 240U;
        options.vsync = settings.vsync;
        renderer.Initialize(*device, window.Handle(), options);
        window.BindResize(&renderer);
        struct ResizeBinding final
        {
            HiddenWindow& window;
            ~ResizeBinding()
            {
                window.BindResize(nullptr);
            }
        } binding{window};
        for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle)
        {
            SCOPED_TRACE(cycle);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            // 本轮消息泵之后才提交 Present；它和 resize 之间没有消息泵。
            ASSERT_TRUE(renderer.RenderFrame()) << "controlled experiment requires an actual Present";
            ++presented;
            LARGE_INTEGER afterPresent{};
            QueryPerformanceCounter(&afterPresent);
            const auto signal = renderer.Queue().NextFenceValue();
            const auto messageCount = window.ResizeMessages();
            const std::uint32_t width = cycle % 2U == 0U ? 480U : 400U;
            const std::uint32_t height = cycle % 2U == 0U ? 360U : 300U;
            RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            ASSERT_TRUE(AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE));
            ASSERT_TRUE(SetWindowPos(hwnd, nullptr, 0, 0, rectangle.right - rectangle.left,
                                     rectangle.bottom - rectangle.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
            LARGE_INTEGER afterSize{};
            QueryPerformanceCounter(&afterSize);
            ASSERT_EQ(window.ResizeMessages(), messageCount + 1U);
            ASSERT_TRUE(renderer.SwapChain().HasPendingResize());
            ASSERT_EQ(renderer.SwapChainFacts().resizeCount, cycle);
            // Apply 内部发出新的 post-Present signal；无需调用方预先 FlushGpu。
            ASSERT_TRUE(renderer.ApplyPendingResizeIfNeeded());
            ASSERT_EQ(renderer.Queue().NextFenceValue(), signal + 1U);
            const auto completed = renderer.Queue().CompletedValue();
            ASSERT_GE(completed, signal);
            ASSERT_EQ(renderer.SwapChainFacts().resizeCount, cycle + 1U);
            ASSERT_EQ(renderer.SwapChainFacts().width, width);
            ASSERT_EQ(renderer.SwapChainFacts().height, height);
            for (std::uint32_t index = 0; index < 3U; ++index)
            {
                const auto desc = renderer.SwapChain().BackBuffer(index).GetDesc();
                ASSERT_EQ(desc.Width, width);
                ASSERT_EQ(desc.Height, height);
                ASSERT_EQ(renderer.SwapChain().TrackedState(index), D3D12_RESOURCE_STATE_PRESENT);
            }
            const auto report = device->DrainInfoQueue();
            for (const auto& item : report.messages)
            {
                EXPECT_GT(item.severity, 2U) << "ID=" << item.id << " " << item.description;
            }
            std::cout << "G01 case=" << settings.name << " cycle=" << cycle << " presentQpc=" << afterPresent.QuadPart
                      << " wmSizeQpc=" << afterSize.QuadPart << " frequency=" << frequency.QuadPart
                      << " flushSignal=" << signal << " completed=" << completed << " messages=" << report.Size()
                      << '\n';
        }
        window.BindResize(nullptr);
        // 最后一帧的 Present 后直接析构 renderer，另覆盖无 resize 的 shutdown。
        ASSERT_TRUE(renderer.RenderFrame()) << "teardown probe requires an actual Present";
    }
    const auto report = device->DrainInfoQueue();
    for (const auto& item : report.messages)
    {
        EXPECT_GT(item.severity, 2U) << "renderer-release ID=" << item.id << " " << item.description;
    }
    EXPECT_FALSE(device->IsDeviceRemoved());
    RecordProperty("cycles", kCycles);
    RecordProperty("presented", presented);
    RecordProperty("postRendererReleaseMessages", static_cast<int>(report.Size()));
    std::cout << "G01 case=" << settings.name << " cycles=" << kCycles << " presented=" << presented
              << " rendererReleaseMessages=" << report.Size() << '\n';
}

INSTANTIATE_TEST_SUITE_P(Controlled, D3D12ImmediateResizeTests,
                         ::testing::Values(ImmediateResizeCase{false, false, false, false, "HiddenImmediate"},
                                           ImmediateResizeCase{true, false, false, false, "VisibleImmediate"},
                                           ImmediateResizeCase{false, true, false, false, "HiddenVsync"},
                                           ImmediateResizeCase{true, true, false, false, "VisibleVsync"},
                                           ImmediateResizeCase{true, false, true, false, "VisibleGbv"},
                                           ImmediateResizeCase{true, false, false, true, "VisibleWarp"}),
                         [](const ::testing::TestParamInfo<ImmediateResizeCase>& info) { return info.param.name; });
