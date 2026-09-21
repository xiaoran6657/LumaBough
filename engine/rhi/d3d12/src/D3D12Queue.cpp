// ============================================================================
// D3D12Queue.cpp — 提交时间线实现：等待语义、单调 fence 与单一 Signal 写点
// 里程碑：M5（03 篇 Queue、Fence 与 FrameContext；手抄清单第 2 条）
// 职责：实现 D3D12Queue.h 的所有权链。三个关键契约：
//   1) BeginFrame 只在复用未完成 context 时等待（其余路径零等待）；
//   2) ExecuteAndSignal 把 Signal 成功与写回 context 合并为单一调用点，
//      Signal 失败即 fatal（上层走 device-lost，不再复用 allocator）；
//   3) fence 时间线严格单调，接近 UINT64_MAX 直接 fatal，绝不回绕。
// 关联：docs/architecture/README.md
//       Microsoft Learn：Executing and Synchronizing Command Lists
// ============================================================================
#include "D3D12Queue.h"

#include "D3D12Diagnostics.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <MiniEngine/Core/Log.h>

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <format>
#include <pix3.h>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// debug trace 行（03 篇「可观察性」格式；upload/retired 字段随 06 篇补齐）。
// 仅 trace 开启时输出，benchmark 不逐帧写日志。
void TraceFrame(const std::uint64_t cpuFrame, const std::uint32_t backBufferIndex, const bool waited,
                const std::uint64_t waitFence, const std::uint64_t completed, const std::uint64_t submitFence,
                const std::string_view stage)
{
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "cpuFrame=%llu bb=%u waited=%d waitFence=%llu completed=%llu submitFence=%llu allocator=Frame%u %s",
                  static_cast<unsigned long long>(cpuFrame), backBufferIndex, waited ? 1 : 0,
                  static_cast<unsigned long long>(waitFence), static_cast<unsigned long long>(completed),
                  static_cast<unsigned long long>(submitFence), backBufferIndex, stage.data());
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, line);
}
} // namespace

D3D12Queue::~D3D12Queue()
{
    // 析构期可发现化（审查意见 P2-4）：若仍有 context 的提交未被确认完成，说明
    // 调用方忘了在允许场景里 FlushGpu（03 篇只允许 resize/shutdown/资源集销毁/
    // 一次性初始化边界）。这里只告警不抛（析构不得抛出）：把"忘记 flush"从静默
    // 变成日志可观测，具体处置仍由调用方决定（device removed 场景单独说明）。
    //
    // m_recording == true（耗尽 / Signal 失败等 fatal 路径）时跳过检查：那种情况下
    // queue 已不可继续使用，调用方已收到异常，再报"未 flush"只是噪音。
    if (m_initialized && m_fence != nullptr && !m_recording)
    {
        const std::uint64_t completed = m_fence->GetCompletedValue();
        if (completed == kD3D12FenceSentinel)
        {
            MiniEngine::WriteLog(MiniEngine::LogLevel::Warning,
                                 "d3d12 queue destroyed after device removal (completed value is the UINT64_MAX "
                                 "sentinel); completion of submitted frames is unknown");
        }
        else
        {
            for (std::size_t index = 0; index < kD3D12FrameContextCount; ++index)
            {
                const std::uint64_t submittedValue = m_frames[index].submittedFenceValue;
                if (submittedValue != 0U && completed < submittedValue)
                {
                    MiniEngine::WriteLog(MiniEngine::LogLevel::Warning,
                                         "d3d12 queue destroyed with incomplete fence: frame" + std::to_string(index) +
                                             " submitted=" + std::to_string(submittedValue) +
                                             " completed=" + std::to_string(completed) +
                                             "; a FlushGpu was expected before destruction");
                }
            }
        }
    }

    // RAII event：自动重置事件，专门服务 SetEventOnCompletion；析构统一关闭。
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
        m_event = nullptr;
    }
}

void D3D12Queue::Initialize(ID3D12Device& device)
{
    if (m_initialized)
    {
        throw std::logic_error{"D3D12Queue::Initialize called twice"};
    }

    // 1) 唯一的 DIRECT 队列（ADR-0006 决策 1：无 copy/compute 队列、无跨队列同步）。
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ThrowIfFailed(device.CreateCommandQueue(&description, IID_PPV_ARGS(&m_queue)), "ID3D12Device::CreateCommandQueue");

    // 2) 全局 Fence：初值 0，整条时间线共享（ADR-0006 决策 2/4 的判定来源）。
    ThrowIfFailed(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "ID3D12Device::CreateFence");

    // 3) RAII event：自动重置、初始无信号；一次 SetEventOnCompletion 服务一次等待。
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_event == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
    }

    // 4) 三 FrameContext 各自的 allocator：互不共享（Reset 只作用于本 context）。
    for (std::size_t index = 0; index < kD3D12FrameContextCount; ++index)
    {
        m_frames[index] = D3D12FrameContext{};
        ThrowIfFailed(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&m_frames[index].directAllocator)),
                      "ID3D12Device::CreateCommandAllocator");
    }

    // 5) 唯一可复用 command list：创建后立即 Close（03 篇 Reset 规则：避免首次
    //    Reset 前状态不一致）；PSO 在 Reset 时传入（04 篇起为真实初始 PSO）。
    ThrowIfFailed(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].directAllocator.Get(),
                                           nullptr, IID_PPV_ARGS(&m_commandList)),
                  "ID3D12Device::CreateCommandList");
    ThrowIfFailed(m_commandList->Close(), "ID3D12GraphicsCommandList::Close(initial)");

    // 6) 稳定命名（02 篇契约：核心对象必须可被 PIX/DRED 读出）。
    Internal::SetDebugName(m_queue.Get(), L"M5.D3D12.Queue");
    Internal::SetDebugName(m_fence.Get(), L"M5.D3D12.Fence");
    Internal::SetDebugName(m_commandList.Get(), L"M5.D3D12.Frame.List");
    for (std::size_t index = 0; index < kD3D12FrameContextCount; ++index)
    {
        const std::wstring name = std::format(L"M5.D3D12.Frame{}.Allocator", index);
        Internal::SetDebugName(m_frames[index].directAllocator.Get(), name);
    }

    m_initialized = true;
}

D3D12FrameContext& D3D12Queue::BeginFrame(const std::uint32_t backBufferIndex)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12Queue::BeginFrame before Initialize"};
    }
    if (backBufferIndex >= kD3D12FrameContextCount)
    {
        throw std::out_of_range{"back-buffer index"};
    }
    if (m_recording || m_listClosed)
    {
        // 一个 allocator 同时只能被一个录制 list 使用：全局单 list 下，上一段
        // 录制未 Execute 就开启新帧会让 allocator 在 GPU 仍在使用时被 Reset。
        throw std::logic_error{"BeginFrame while a previous recording has not been executed"};
    }

    ++m_debugCpuFrame;
    D3D12FrameContext& frame = m_frames[backBufferIndex];

    // 等待判定：只有复用当前 context 且其 fence 未完成才阻塞（其余路径零等待）。
    bool waited = false;
    const std::uint64_t completedBefore = CompletedValue();
    if (completedBefore < frame.submittedFenceValue)
    {
        const auto waitStart = std::chrono::steady_clock::now();
        WaitFor(frame.submittedFenceValue);
        waited = true;

        // 非预期长等待是三帧并行被破坏或 GPU 严重落后的信号：只记录不失败。
        const auto waitMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
        if (waitMs > static_cast<std::int64_t>(kFenceWaitWarnThresholdMs))
        {
            MiniEngine::WriteLog(MiniEngine::LogLevel::Warning,
                                 "d3d12 fence wait exceeded threshold: " + std::to_string(waitMs) + "ms for value " +
                                     std::to_string(frame.submittedFenceValue));
        }
    }

    // 防御性断言：等待之后 allocator 的 fence 必须已完成——若不成立说明时间线
    // 被外部改坏，Reset 会踩到 GPU 仍在使用的命令数据（03 篇 Reset 规则）。
    const std::uint64_t completed = CompletedValue();
    if (completed < frame.submittedFenceValue)
    {
        throw std::logic_error{"allocator Reset attempted before its context fence completed"};
    }

    ThrowIfFailed(frame.directAllocator->Reset(), "ID3D12CommandAllocator::Reset");
    ThrowIfFailed(m_commandList->Reset(frame.directAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    m_recording = true;

    if (m_traceEnabled)
    {
        TraceFrame(m_debugCpuFrame, backBufferIndex, waited, frame.submittedFenceValue, completed, m_nextFenceValue,
                   "begin");
    }
    return frame;
}

std::uint64_t D3D12Queue::ExecuteAndSignal(D3D12FrameContext& frame)
{
    CloseRecording();
    return ExecuteClosedAndSignal(frame);
}

void D3D12Queue::CloseRecording()
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12Queue::CloseRecording before Initialize"};
    }
    if (!m_recording)
    {
        throw std::logic_error{"CloseRecording without an open recording"};
    }

    // EndGraphics 的唯一 Close 写点；成功后 list 进入 Submit-only 阶段。
    ThrowIfFailed(m_commandList->Close(), "ID3D12GraphicsCommandList::Close");
    m_recording = false;
    m_listClosed = true;
}

std::uint64_t D3D12Queue::ExecuteClosedAndSignal(D3D12FrameContext& frame)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12Queue::ExecuteClosedAndSignal before Initialize"};
    }
    if (!m_listClosed)
    {
        throw std::logic_error{"ExecuteClosedAndSignal requires a previously closed recording"};
    }

    // fence 时间线耗尽即 fatal，且检查必须在提交之前：若先把命令交给 GPU 再发现
    // 没有 fence 值可 Signal，会留下永不被 fence 覆盖的 GPU 工作。
    if (m_nextFenceValue == kD3D12FenceSentinel)
    {
        throw std::overflow_error{"D3D12 fence timeline exhausted (UINT64_MAX)"};
    }

    PIXScopedEvent(m_queue.Get(), PIX_COLOR_DEFAULT, "M5.Frame %llu",
                   static_cast<unsigned long long>(m_nextFenceValue));
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    const std::uint64_t submitted = m_nextFenceValue++;
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), submitted), "ID3D12CommandQueue::Signal");

    // Signal 成功后才发布 context 与 list 状态；Signal 失败时 queue 保持 fatal closed 状态。
    frame.submittedFenceValue = submitted;
    ++frame.debugFrameCount;
    m_listClosed = false;

    if (m_traceEnabled)
    {
        TraceFrame(m_debugCpuFrame, static_cast<std::uint32_t>(&frame - m_frames.data()), false, submitted,
                   CompletedValue(), m_nextFenceValue, "submit");
    }
    return submitted;
}

void D3D12Queue::FlushGpu(const std::string_view reason)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12Queue::FlushGpu before Initialize"};
    }
    // Flush 期间不得有未完成录制（否则 list 状态悬空、allocator 无法安全 Reset）。
    if (m_recording || m_listClosed)
    {
        throw std::logic_error{"FlushGpu while a recording is open or awaiting submission"};
    }

    // 唯一 flush 口径：signal 唯一 fence → SetEventOnCompletion → 等待。
    if (m_nextFenceValue == kD3D12FenceSentinel)
    {
        throw std::overflow_error{"D3D12 fence timeline exhausted (UINT64_MAX)"};
    }
    const std::uint64_t value = m_nextFenceValue++;
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), value), "FlushGpu Signal");
    WaitFor(value);

    // flush 语义即"所有 context 的提交都已完成"：显式核验而不是假设。
    for (std::size_t index = 0; index < kD3D12FrameContextCount; ++index)
    {
        const std::uint64_t submitted = m_frames[index].submittedFenceValue;
        if (submitted != 0U && CompletedValue() < submitted)
        {
            throw std::logic_error{"FlushGpu returned before all context fences completed"};
        }
    }

    if (m_traceEnabled)
    {
        MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 flush: reason=" + std::string{reason} +
                                                             " completed=" + std::to_string(CompletedValue()));
    }
}

void D3D12Queue::WaitFor(const std::uint64_t value)
{
    if (CompletedValue() >= value)
    {
        return;
    }
    PIXScopedEvent(PIX_COLOR_DEFAULT, "FenceWait:%llu", static_cast<unsigned long long>(value));
    ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_event), "ID3D12Fence::SetEventOnCompletion");
    const DWORD result = WaitForSingleObject(m_event, INFINITE);
    if (result != WAIT_OBJECT_0)
    {
        const HRESULT error = result == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_UNEXPECTED;
        ThrowIfFailed(error, "WaitForSingleObject");
    }
    // 唤醒原因进 PIX 时间线（M5-11 验收"fence wait 可解释"的来源）。
    // WinPixEventRuntime 尚未 vendored（M5-01/02 已挂决策），先以编译期开关
    // 接线；引入依赖后定义 MINIENGINE_ENABLE_PIX_RUNTIME 即生效，语义不变。
#if defined(MINIENGINE_ENABLE_PIX_RUNTIME)
    PIXNotifyWakeFromFenceSignal(m_event);
#endif
}

void D3D12Queue::WaitForSubmittedFence(const std::uint64_t value, const std::string_view reason)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12Queue::WaitForSubmittedFence before Initialize"};
    }
    if (value == 0U)
    {
        throw std::invalid_argument{"WaitForSubmittedFence requires a submitted fence value"};
    }
    if (value >= m_nextFenceValue)
    {
        // 尚未提交的值不可能完成：等待它等于死等。
        throw std::logic_error{"WaitForSubmittedFence requires an already submitted fence value"};
    }
    if (CompletedValue() >= value)
    {
        return; // 已完成：不是 stall，无需记录
    }

    const auto waitStart = std::chrono::steady_clock::now();
    WaitFor(value);
    const auto waitMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 targeted fence wait: reason=" + std::string{reason} +
                                                         " value=" + std::to_string(value) +
                                                         " waitedMs=" + std::to_string(waitMs));
}

void D3D12Queue::SetTraceEnabled(const bool enabled) noexcept
{
    m_traceEnabled = enabled;
}

std::uint64_t D3D12Queue::CompletedValue() const
{
    // UINT64_MAX 是 device removed 的哨兵值：必须显式失败，绝不能当作"全部完成"。
    const std::uint64_t value = m_fence->GetCompletedValue();
    if (value == kD3D12FenceSentinel)
    {
        throw std::runtime_error{"D3D12 fence completed value is the device-removed sentinel (UINT64_MAX)"};
    }
    return value;
}

std::uint64_t D3D12Queue::NextFenceValue() const noexcept
{
    return m_nextFenceValue;
}

ID3D12CommandQueue& D3D12Queue::NativeQueue() const noexcept
{
    return *m_queue.Get();
}

ID3D12Fence& D3D12Queue::Fence() const noexcept
{
    return *m_fence.Get();
}

ID3D12GraphicsCommandList& D3D12Queue::CommandList() const noexcept
{
    return *m_commandList.Get();
}
} // namespace MiniEngine::Rhi::D3D12
