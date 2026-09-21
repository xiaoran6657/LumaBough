// ============================================================================
// D3D12Queue.h — 单 DIRECT Queue + 全局 Fence + 三 FrameContext 的提交时间线
// 里程碑：M5（03 篇 Queue、Fence 与 FrameContext；手抄清单第 1/2 条）
// 职责：封装 M5 的提交所有权链：BeginFrame（只在复用未完成 context 时等待 →
//       allocator.Reset → commandList.Reset）→ 录制 → ExecuteAndSignal（Close +
//       Execute + Signal + 写回 context 的单一调用点）→ Present（由 04 篇的
//       SwapChain 完成，本类之后）。fence 值从 1 严格单调递增，接近 UINT64_MAX
//       直接 fatal，不允许回绕或复用。
// 内部性说明：与 D3D11 的 ShadowMap/Screenshot 同型，本类属于后端内部
//       （src/），由未来的 D3D12Renderer 与设备级测试消费；模板的 include/
//       位置按仓库惯例移入 src/（见 D3D12FrameContext.h 的说明）。
// 等待语义：只有复用当前 back-buffer 对应 context 且其 fence 未完成时才阻塞；
//       绝不提供每帧 WaitForGpu（那会把三帧并行退化成串行）。FlushGpu 仅允许
//       四类场景（见 .cpp 的 FlushGpu 注释），调用方必须带 reason 供取证。
// 关联：docs/architecture/README.md（BeginFrame/EndFrame/Reset 规则）
//       docs/architecture/DECISIONS.md（决策 1/2/4）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "D3D12FrameContext.h"

namespace MiniEngine::Rhi::D3D12
{
// FrameContext 数量与 swap-chain buffer count 一致（M5 冻结为 3）。
inline constexpr std::size_t kD3D12FrameContextCount = 3U;

// fence 时间线的哨兵：Signal 到 UINT64_MAX 表示时间线耗尽；GetCompletedValue()
// 返回 UINT64_MAX 表示 device removed。两者都必须显式失败。
inline constexpr std::uint64_t kD3D12FenceSentinel = UINT64_MAX;

// 事件等待的"非预期时长"阈值（毫秒）：三帧并行下 allocator 复用等待只应在
// GPU 严重落后时出现；超过阈值按潜在性能问题记录（诊断用，不判定失败）。
inline constexpr std::uint32_t kFenceWaitWarnThresholdMs = 250U;

class D3D12Queue final
{
  public:
    D3D12Queue() = default;
    ~D3D12Queue();
    D3D12Queue(const D3D12Queue&) = delete;
    D3D12Queue& operator=(const D3D12Queue&) = delete;

    // 创建 Queue（DIRECT / NORMAL）、Fence（初值 0）、RAII event、三个
    // FrameContext 各自的 allocator 与唯一的可复用 command list（创建后立即
    // Close，保证首次 Reset 前状态一致），并设置 M5.D3D12.* 稳定名称。
    //
    // 失败：任一 HRESULT 失败抛 HResultError；重复调用抛 std::logic_error。
    void Initialize(ID3D12Device& device);

    // BeginFrame：取 backBufferIndex 对应的 context；只有其 fence 未完成时
    // 等待（SetEventOnCompletion + WaitForSingleObject + 唤醒通知），然后
    // allocator.Reset 与 commandList.Reset（空 PSO，04 篇起传入真实 PSO）。
    //
    // 失败：index 越界抛 std::out_of_range；上一帧尚未 Execute 就再次
    // BeginFrame 抛 std::logic_error（一个 allocator 同期只服务一个录制 list）；
    // Reset/等待的 HRESULT 失败抛 HResultError。
    D3D12FrameContext& BeginFrame(std::uint32_t backBufferIndex);

    // EndFrame 的提交半边（Present 由调用方在之后执行）：Close 录制中的
    // command list → ExecuteCommandLists → Signal(nextFenceValue)。Signal
    // 成功后才在同一调用点写回 frame.submittedFenceValue 并返回该值；
    // Signal 失败即 fatal（本 queue 不可继续复用 allocator，由上层走
    // device-lost 路径）。
    //
    // 失败：未处于录制状态抛 std::logic_error；Close/Execute/Signal 的
    // HRESULT 失败或 fence 时间线耗尽抛 HResultError/std::overflow_error。
    std::uint64_t ExecuteAndSignal(D3D12FrameContext& frame);

    // 适配公共 NativeRhiBackend：EndGraphics 只负责一次 Close，Submit 只执行
    // 已关闭的 command list 并 Signal。旧 ExecuteAndSignal 仍组合调用这两个入口。
    void CloseRecording();
    std::uint64_t ExecuteClosedAndSignal(D3D12FrameContext& frame);

    // 全量 flush（signal 唯一 fence → 等待完成）。只允许四类场景：
    // ResizeBuffers 前、正常 shutdown 前、销毁整个 Device lifetime 资源集、
    // 明确的一次性初始化提交边界。reason 必须写明场景（进 debug trace 取证）。
    // 普通帧末尾、上传紧张、截图、热重载禁止调用——那些由 ring/retirement 策略处理。
    void FlushGpu(std::string_view reason);

    // 定向等待某个**已提交**的 fence 值完成（不产生新的 Signal、不是全量 flush）。
    //
    // 用途（06 篇）：上传环容量策略在"没有安全 span"时只等**最老可释放 fence 一次**，
    // 而不是 flush 整个队列（04 篇明确禁止在上传紧张时随手 flush）。等待耗时写进日志
    // 供 stall 取证；已完成则立即返回、不记 stall。
    //
    // 参数：
    //   value  —— 必须 > 0 且不大于最后一次提交值（即确实已提交）
    //   reason —— 场景标识（进日志，例如 "upload-ring-pressure"）
    // 失败：value == 0 或尚未提交 → std::invalid_argument / std::logic_error；
    //   等待过程中的 HRESULT 失败抛 HResultError。
    void WaitForSubmittedFence(std::uint64_t value, std::string_view reason);

    // 当前已完成 fence 值。返回 UINT64_MAX（device removed 哨兵）时抛
    // std::runtime_error，由上层走 device-lost 诊断路径。
    [[nodiscard]] std::uint64_t CompletedValue() const;

    // 下一次提交将使用的 fence 值（诊断/trace 用；不推进时间线）。
    [[nodiscard]] std::uint64_t NextFenceValue() const noexcept;

    // debug trace 开关（02 篇"仅 debug/诊断模式输出"）：开启后每个 BeginFrame/
    // ExecuteAndSignal/等待/Flush 经 WriteLog 输出一行 trace。默认关闭。
    void SetTraceEnabled(bool enabled) noexcept;

    [[nodiscard]] ID3D12CommandQueue& NativeQueue() const noexcept;
    [[nodiscard]] ID3D12Fence& Fence() const noexcept;
    [[nodiscard]] ID3D12GraphicsCommandList& CommandList() const noexcept;

  private:
    // 等待 fence 达到 value：SetEventOnCompletion + WaitForSingleObject +
    // PIXNotifyWakeFromFenceSignal（WinPixEventRuntime vendoring 后生效，
    // 当前编译期开关下为 no-op）。完成值已达标时直接返回。
    void WaitFor(std::uint64_t value);

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    std::array<D3D12FrameContext, kD3D12FrameContextCount> m_frames{};
    void* m_event = nullptr; // RAII：析构 CloseHandle（模板 TODO 已按仓库惯例落地）
    std::uint64_t m_nextFenceValue = 1;
    bool m_recording = false;  // 单一可复用 list：同一时刻只允许一段录制
    bool m_listClosed = false; // CloseRecording 后等待 Submit；Submit 成功后清除
    bool m_initialized = false;
    bool m_traceEnabled = false;
    std::uint64_t m_debugCpuFrame = 0;
};
} // namespace MiniEngine::Rhi::D3D12
