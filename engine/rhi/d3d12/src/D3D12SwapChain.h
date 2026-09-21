// ============================================================================
// D3D12SwapChain.h — 三缓冲 FLIP_DISCARD 交换链、RTV 与安全点 Resize
// 里程碑：M5（04 篇 Command List、SwapChain 与 Resize；手抄清单第 1/2 条）
// 职责：按 04 篇 baseline 创建交换链（R8G8B8A8_UNORM / BufferCount=3 /
//       FLIP_DISCARD / SampleCount=1 / RENDER_TARGET_OUTPUT），持 CPU-only RTV
//       heap 的三个连续 descriptor，并保证：current index 只来自
//       IDXGISwapChain3::GetCurrentBackBufferIndex（不自行推算）、back buffer 每帧
//       PRESENT↔RENDER_TARGET 往返、Present 前状态必为 PRESENT、窗口 0×0
//       （minimized）时既不渲染也不 ResizeBuffers、Resize 只在一次 FlushGpu
//       之后一次性重建全部 buffer 引用。
// 内部性说明：后端内部类型（src/），由 D3D12Renderer 与设备级测试消费。
// 状态跟踪范围：本篇只跟踪三个 back buffer 的 PRESENT↔RENDER_TARGET（07 篇的
//       通用 ResourceStateTracker 接管后本类改为委托，语义不变）。
// 关联：docs/architecture/README.md
//       docs/architecture/DECISIONS.md（决策 1/2/4）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

#include "D3D12ResourceStateRegistry.h"

namespace MiniEngine::Rhi::D3D12
{
class D3D12Queue;
class D3D12ResourceStateTracker;

// 交换链的冻结 baseline（04 篇「SwapChain baseline」）。
inline constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr std::uint32_t kSwapChainBufferCount = 3U;

// tearing 决策的纯函数（04 篇「可选 tearing」四步的机读版）：
//   1) 只有驱动支持（CheckFeatureSupport == TRUE）才允许加 ALLOW_TEARING 标志；
//   2) 允许存在标志 ≠ 每次都用：只有 vsync=0 且窗口化时才传 PRESENT_ALLOW_TEARING；
//   3) 任一前提不满足即回落到普通 Present，不静默降级成别的 swap effect。
// 返回：Present 调用应传的 flag（0 或 DXGI_PRESENT_ALLOW_TEARING）。
[[nodiscard]] std::uint32_t ResolvePresentFlags(bool vsync, bool tearingEnabled, bool isWindowed);

// 窗口尺寸是否为"挂起"（0×0 = minimized）：挂起时既不渲染也不 ResizeBuffers。
[[nodiscard]] bool IsSuspendedSize(std::uint32_t width, std::uint32_t height);

// tearing 的"实际启用"判定（04 篇四步中的第 2/3 步）：必须同时满足
//   驱动支持（CheckFeatureSupport）、vsync=0、以及**交换链创建时真的带上了**
//   ALLOW_TEARING 标志。
// 第三条容易被漏掉：交换链一旦以 vsync=1 创建（无该标志），运行期再切到 vsync=0
// 也不能传 DXGI_PRESENT_ALLOW_TEARING——DXGI 会直接拒绝。运行期切换 vsync 时
// 必须用它重算 tearingEnabled，而不是只看 supported && !vsync。
[[nodiscard]] bool ResolveTearingEnabled(bool tearingSupported, bool vsync, std::uint32_t swapChainFlags);

// 交换链创建/运行期事实（进 metadata 与验收记录；tearing 的实际生效口径在此）。
struct SwapChainConfig final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bufferCount = kSwapChainBufferCount;
    bool vsync = true;
    bool windowed = true;          // 本篇只支持窗口化；独占全屏会改变 tearing 前提
    bool tearingSupported = false; // 驱动能力（CheckFeatureSupport）
    bool tearingEnabled = false;   // 见 ResolveTearingEnabled
    std::uint32_t flags = 0;       // 创建时实际使用的 DXGI_SWAP_CHAIN_FLAG_*
    std::uint32_t resizeCount = 0; // 成功完成的 ResizeBuffers 次数（诊断）
};

class D3D12SwapChain final
{
  public:
    D3D12SwapChain() = default;
    ~D3D12SwapChain() = default;
    D3D12SwapChain(const D3D12SwapChain&) = delete;
    D3D12SwapChain& operator=(const D3D12SwapChain&) = delete;

    // 创建交换链与 RTV（device 参数传 Direct Queue——04 篇明确规定），
    // 命名 `M5.SwapChain` / `M5.BackBuffer[i]`，并查询 tearing 能力；
    // 同时把三个 back buffer 以实际初始状态（PRESENT）注册进 07 篇的状态 tracker
    // （tracker 是状态的唯一真源，本类不再自持三态数组）。
    //
    // 失败：创建/查询/GetBuffer/CreateRenderTargetView/注册 任一失败抛
    //   HResultError / std::logic_error；窗口句柄为空抛 std::invalid_argument。
    void Initialize(ID3D12Device& device, IDXGIFactory7& factory, ID3D12CommandQueue& queue,
                    D3D12ResourceStateTracker& tracker, void* nativeWindow, std::uint32_t width, std::uint32_t height,
                    bool vsync);

    // current index 只来自 SwapChain：绝不用 (index+1)%N 推测（04 篇契约）。
    [[nodiscard]] std::uint32_t CurrentBackBufferIndex() const;

    [[nodiscard]] ID3D12Resource& BackBuffer(std::uint32_t index) const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(std::uint32_t index) const;

    // 状态跟踪（委托 07 篇 tracker）：TrackTransition 只登记状态请求并把 barrier
    // 放进批，由调用方在依赖新状态的绘制之前 FlushBarriersTo；
    // TrackedState 读 tracker 的 pending 视角；Present 前断言当前 back buffer 为 PRESENT。
    [[nodiscard]] D3D12_RESOURCE_STATES TrackedState(std::uint32_t index) const;
    void TrackTransition(std::uint32_t index, D3D12_RESOURCE_STATES after);

    // back buffer 在 tracker 里的键（resize 后会带新 generation）。
    [[nodiscard]] ResourceKey BackBufferKey(std::uint32_t index) const;

    // Present：SyncInterval = vsync ? 1 : 0；flag 由 ResolvePresentFlags 决定。
    // 返回 false 表示被遮挡（DXGI_STATUS_OCCLUDED，调用方进入低频探测待机）。
    // 失败：device removed/reset/hung 时抛 HResultError（上层走 DRED fatal 路径）。
    [[nodiscard]] bool Present();

    // 记录 pending 尺寸（WM_SIZE 路径只记录，不在消息处理里动 GPU 资源）。
    // 0×0 记为挂起请求：ApplyPendingResize 会进入 suspended 而不调用 ResizeBuffers。
    void RequestResize(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] bool HasPendingResize() const noexcept;

    // 安全点执行 resize：FlushGpu → 释放全部 back buffer 引用 → ResizeBuffers →
    // 重新 GetBuffer/建 RTV → 状态复位 PRESENT → 更新 config；失败即进入不可渲染
    // 状态并抛出（不混用新旧资源、不留半初始化）。
    void ApplyPendingResize(D3D12Queue& queue);

    [[nodiscard]] bool IsSuspended() const noexcept;
    [[nodiscard]] bool IsFailed() const noexcept;
    [[nodiscard]] const SwapChainConfig& Config() const noexcept;
    void SetVsync(bool vsync) noexcept;

  private:
    void CreateBackBufferViews(ID3D12Device& device);
    // 以当前 m_generation 把三个 back buffer 注册进 tracker（Initialize 与 resize 后调用）。
    void RegisterBackBuffers();

    // 非拥有指针：Device 的生命周期由调用方（D3D12Device）持有且必然长于交换链；
    // Resize 重建 RTV 时需要它，且不宜把 device 复制进本类（会延长设备寿命）。
    ID3D12Device* m_device = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount> m_backBuffers;
    // 状态真源在 07 篇 tracker：本类只保存键（含 generation，resize 后递增）。
    std::array<ResourceKey, kSwapChainBufferCount> m_backBufferKeys{};
    D3D12ResourceStateTracker* m_tracker = nullptr;
    std::uint32_t m_generation = 0;
    std::uint32_t m_rtvDescriptorSize = 0;
    SwapChainConfig m_config;
    std::uint32_t m_pendingWidth = 0;
    std::uint32_t m_pendingHeight = 0;
    bool m_hasPendingResize = false;
    bool m_suspended = false;
    bool m_failed = false;
};
} // namespace MiniEngine::Rhi::D3D12
