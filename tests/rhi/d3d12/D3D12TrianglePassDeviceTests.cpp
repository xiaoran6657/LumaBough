// ============================================================================
// D3D12TrianglePassDeviceTests.cpp — 迁移顺序第 1 步（固定 triangle）与主深度的设备级契约
// 里程碑：M5（09 篇「迁移顺序」第 1 步「固定 triangle：Root Signature/PSO/descriptor/constant」）
// 职责：在真实 Device + 真实 HWND + 真实 DXIL 产物上验证这一步的三条硬约束：
//   1. 固定 triangle 真的发出了 draw（drawCount / vertexCount / constantUploadBytes 互相吻合）；
//   2. **帧内不创建 PSO**：triangle 的 PSO 必须在 init 的 baseline 集里（08 篇硬约束）——
//      这条一旦破，沙盒的 `unexpectedCreations` 判据会把整轮判为失败；
//   3. 主深度真的存在、按交换链尺寸创建、resize 时"先 Unregister 再释放再重建"
//      （generation 递增且 tracker 里的资源数不变——变了说明旧的没注销）。
// 并要求全程 Debug Layer **零消息**（零容忍 Gate 与生产一致）。
// 环境：需要 D3D12 硬件或 WARP；需要 MINIENGINE_D3D12_SHADER_DIR 指向编译产物。
// 关联：docs/architecture/README.md（迁移顺序 / M4 resource profile 保持）
//       engine/rhi/d3d12/src/D3D12DepthBuffer.h（被验证的资源）
// ============================================================================
#include "D3D12DepthBuffer.h"
#include "D3D12Renderer.h"
#include "D3D12RootSignature.h"
#include "D3D12SwapChain.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <Windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#ifndef MINIENGINE_D3D12_SHADER_DIR
#error "MINIENGINE_D3D12_SHADER_DIR must be defined by the test target (see tests/rhi/d3d12/CMakeLists.txt)"
#endif

using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12Renderer;
using MiniEngine::Rhi::D3D12::D3D12RendererOptions;

namespace
{
// 隐藏窗口：仅作交换链宿主（与 D3D12SwapChainDeviceTests 同款），避免 ctest 弹窗抢焦点。
class HiddenWindow final
{
  public:
    HiddenWindow()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"MiniEngineD3D12TriangleTestWindow";
        RegisterClassExW(&windowClass); // 已注册时返回 0，可忽略（多用例共享类名）

        m_window =
            CreateWindowExW(0, windowClass.lpszClassName, L"MiniEngine D3D12 triangle test window", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, instance, nullptr);
        // 保持隐藏：测试不需要可见窗口。
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

  private:
    HWND m_window = nullptr;
};

// 设备 + 渲染器 + root signature + baseline PSO 集的公共前置。
//
// 成员声明顺序即构造顺序、逆序即析构顺序：renderer 先于 device 释放（它持有 device
// 的非拥有指针），rootSignature 必须比 renderer 活得久（PSO 由它创建，渲染器只存裸引用）。
struct TriangleHarness final
{
    std::unique_ptr<D3D12Device> device;
    HiddenWindow window;
    D3D12Renderer renderer;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;

    explicit TriangleHarness(const bool triangleEnabled)
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true; // 零消息断言必须有调试层才有意义
        device = D3D12Device::Create(options);

        D3D12RendererOptions rendererOptions;
        rendererOptions.width = 320U;
        rendererOptions.height = 240U;
        rendererOptions.vsync = false; // 测试不做呈现节流
        renderer.Initialize(*device, window.Handle(), rendererOptions);

        MiniEngine::Rhi::D3D12::RootSignatureFacts facts{};
        rootSignature = MiniEngine::Rhi::D3D12::CreateM5RootSignature(
            *static_cast<ID3D12Device*>(device->NativeDeviceHandle()), facts);
        renderer.BindRootSignature(*rootSignature.Get(), 0U);
        renderer.CreateBaselinePsoSet(MINIENGINE_D3D12_SHADER_DIR);
        renderer.SetTriangleEnabled(triangleEnabled);
    }

    // 收尾：先 flush 再等待，之后成员才按逆序析构。理由与 D3D12SwapChainDeviceTests 相同：
    // 紧跟 Present 释放 back buffer 会触发调试层 OBJECT_DELETED_WHILE_STILL_IN_USE
    // （M5-04 记录 P3 的已知观察，触发条件未完全定位，测试按"实测可行"处理）。
    ~TriangleHarness()
    {
        renderer.FlushGpu("triangle-test-teardown");
        Sleep(100);
    }

    // 跑 frames 帧，返回真正返回 true（已呈现）的帧数。
    // 注意：被遮挡时 Present 返回 false 但**帧仍然被录制**（draw 已发出），
    // 因此 draw 计数不能用"呈现帧数"当上界——这里用调用次数做口径。
    std::uint32_t RenderFrames(const std::uint32_t frames)
    {
        std::uint32_t recorded = 0U;
        for (std::uint32_t index = 0U; index < frames; ++index)
        {
            static_cast<void>(renderer.RenderFrame());
            ++recorded;
        }
        return recorded;
    }
};
} // namespace

// 默认关闭：帧循环与 04 篇的 clear/present 一致（不录 draw、不写常量），但主深度与
// baseline PSO 集**已经就位**——开关只是"是否录这一段"，不是"是否准备好"。
TEST(D3D12TrianglePassDeviceTests, DisabledByDefaultRecordsNoDraw)
{
    TriangleHarness harness{false};

    EXPECT_FALSE(harness.renderer.TriangleEnabled());
    const std::uint32_t recorded = harness.RenderFrames(12U);

    EXPECT_EQ(recorded, 12U);
    EXPECT_EQ(harness.renderer.TriangleDrawCount(), 0U) << "关闭时不得发出 draw";
    EXPECT_EQ(harness.renderer.TriangleVertexCount(), 0U);
    EXPECT_EQ(harness.renderer.TriangleConstantUploadBytes(), 0U);
    EXPECT_EQ(harness.renderer.ConstantUploadBytes(), 0U) << "关闭时不得占用常量上传环";
    EXPECT_EQ(harness.renderer.PsoStats().created, harness.renderer.BaselinePsoCount()) << "关闭时也不得创建 PSO";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 打开后每帧一次 draw：draw / vertex / 常量三者必须互相吻合（计数与录制脱节时，
// metadata 里任何"看起来对"的数字都不可信）。
TEST(D3D12TrianglePassDeviceTests, EnabledRecordsExactlyOneDrawPerFrame)
{
    TriangleHarness harness{true};

    EXPECT_TRUE(harness.renderer.TriangleEnabled());
    const std::uint32_t recorded = harness.RenderFrames(12U);

    EXPECT_EQ(recorded, 12U);
    EXPECT_EQ(harness.renderer.TriangleDrawCount(), recorded) << "每帧恰好一次 DrawInstanced";
    EXPECT_EQ(harness.renderer.TriangleVertexCount(), static_cast<std::uint64_t>(recorded) * 3U);
    // b0 = 128B、b1 = 208B，各按 256B 对齐后仍各占 256B 的 span；这里断言的是
    // "写入的结构体大小"（环的对齐开销由 06 篇的 allocator 单测负责）。
    EXPECT_EQ(harness.renderer.TriangleConstantUploadBytes(), static_cast<std::uint64_t>(recorded) * (128U + 208U));
    EXPECT_EQ(harness.renderer.ConstantUploadBytes(), harness.renderer.TriangleConstantUploadBytes());
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "triangle pass 不得产生调试层消息";
}

// 08 篇硬约束的回归：triangle 的 PSO 必须在 init 建好。若它落在帧内创建，
// 帧数增加会带动 created 增长——这条断言把"帧内零创建"从沙盒判据前移到单测。
TEST(D3D12TrianglePassDeviceTests, TrianglePsoIsCreatedAtInitNotDuringFrames)
{
    TriangleHarness harness{true};

    const std::uint64_t createdAfterInit = harness.renderer.PsoStats().created;
    EXPECT_EQ(createdAfterInit, harness.renderer.BaselinePsoCount());
    EXPECT_EQ(harness.renderer.BaselinePsoCount(), 13U) << "baseline 集必须包含全部 pass 及其镜像变体（共 13 个）";

    static_cast<void>(harness.RenderFrames(16U));

    EXPECT_EQ(harness.renderer.PsoStats().created, createdAfterInit) << "帧内不得创建 PSO";
    EXPECT_EQ(harness.renderer.PendingPsoCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 主深度：按交换链尺寸创建并注册进 tracker（trackedResources = 3 back buffer + 1 depth）。
TEST(D3D12TrianglePassDeviceTests, DepthBufferMatchesSwapChainAndIsTracked)
{
    TriangleHarness harness{true};

    const MiniEngine::Rhi::D3D12::D3D12DepthBuffer& depth = harness.renderer.DepthBuffer();
    ASSERT_TRUE(depth.IsInitialized());
    EXPECT_EQ(depth.Width(), 320U);
    EXPECT_EQ(depth.Height(), 240U);
    EXPECT_EQ(depth.Width(), harness.renderer.SwapChainFacts().width) << "主深度必须与后备缓冲同尺寸";
    EXPECT_EQ(depth.Height(), harness.renderer.SwapChainFacts().height);
    EXPECT_EQ(depth.Generation(), 0U);
    EXPECT_EQ(harness.renderer.StateTracker().TrackedResourceCount(), 4U) << "三个 back buffer + 主深度";

    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// resize：先 Unregister 再释放再重建。判据是"资源数不变"——若忘了 Unregister，
// 旧深度的槽位仍然占着（资源数变成 5），而 generation 递增本身并不能发现这一点。
TEST(D3D12TrianglePassDeviceTests, ResizeRebuildsDepthWithoutLeakingTrackerSlots)
{
    TriangleHarness harness{true};

    harness.renderer.RequestResize(640U, 480U);
    ASSERT_TRUE(harness.renderer.ApplyPendingResizeIfNeeded());

    const MiniEngine::Rhi::D3D12::D3D12DepthBuffer& depth = harness.renderer.DepthBuffer();
    EXPECT_EQ(depth.Width(), 640U);
    EXPECT_EQ(depth.Height(), 480U);
    EXPECT_EQ(depth.Generation(), 1U) << "重建次数 +1（证明真的换了资源，而不是改了字段）";
    EXPECT_EQ(harness.renderer.StateTracker().TrackedResourceCount(), 4U)
        << "旧深度必须先 Unregister，否则会留下一个永不复用的槽位";

    static_cast<void>(harness.RenderFrames(4U));
    EXPECT_EQ(harness.renderer.TriangleDrawCount(), 4U) << "resize 后仍能继续录制";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 负向：没有 baseline PSO 集就打开 triangle 必须在**第一次录制时**显式失败，
// 而不是画出一个"什么都没发生"的空帧（那样现象是黑屏，不是错误）。
TEST(D3D12TrianglePassDeviceTests, TriangleWithoutPsoSetFailsExplicitly)
{
    MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
    options.debugLayer = true;
    const std::unique_ptr<D3D12Device> device = D3D12Device::Create(options);
    HiddenWindow window;

    D3D12Renderer renderer;
    D3D12RendererOptions rendererOptions;
    rendererOptions.width = 320U;
    rendererOptions.height = 240U;
    rendererOptions.vsync = false;
    renderer.Initialize(*device, window.Handle(), rendererOptions);
    renderer.SetTriangleEnabled(true);

    std::string message;
    try
    {
        static_cast<void>(renderer.RenderFrame());
    }
    catch (const std::exception& exception)
    {
        message = exception.what();
    }
    EXPECT_FALSE(message.empty()) << "缺少 baseline PSO 集时必须抛异常";
    EXPECT_NE(message.find("triangle pass requires"), std::string::npos) << message;

    // 失败发生在**录制中**：本帧既不 Close 也不 Execute，渲染器因此停在不可恢复状态，
    // 与生产一致（沙盒捕获后以退出码 3 结束，不继续渲染）。队列自身的契约在这里被
    // 顺带断言——录制未关闭时 flush 必须被拒绝，而不是把半段命令提交出去。
    EXPECT_THROW(renderer.FlushGpu("triangle-negative-teardown"), std::logic_error);
}
