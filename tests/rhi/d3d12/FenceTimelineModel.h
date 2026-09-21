// ============================================================================
// FenceTimelineModel.h — 纯 CPU 的 fence 时间线模型（03 篇「测试」的 mock 层）
// 里程碑：M5（03 篇 Queue、Fence 与 FrameContext；手抄清单第 3 条）
// 职责：以值语义复刻 D3D12Queue 的时间线规则——严格单调的 next fence、
//       "只有复用未完成 context 才等待"、Reset-before-complete 的负向捕获、
//       按 completed fence 回收 retired 项、UINT64_MAX 视为 fatal。
//       生产实现（D3D12Queue）与其逐条等价；模型可在无 GPU 的 CI 上穷举
//       时间线边界（10,000 帧轮转、时间线耗尽、乱序 retire）。
// 契约对应：NeedsWait == D3D12Queue::BeginFrame 的等待条件；
//       Submit == ExecuteAndSignal 的写回语义；Retire/Reclaim == 06 篇
//       fence 驱动回收的唯一判据（completed >= retireFence）。
// 关联：docs/architecture/README.md（测试清单）
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

namespace MiniEngine::Rhi::D3D12::Tests
{
// 时间线耗尽哨兵：与生产 kD3D12FenceSentinel 同值（模型不 include d3d12.h）。
inline constexpr std::uint64_t kModelFenceSentinel = UINT64_MAX;

class FenceTimelineModel final
{
  public:
    explicit FenceTimelineModel(const std::size_t frameCount) : m_submitted(frameCount, 0U)
    {
        if (frameCount == 0U)
        {
            throw std::invalid_argument{"frame count must be non-zero"};
        }
    }

    // GPU 完成进度的测试驱动（真实 GPU 由 Signal 推进）。
    void SetCompleted(const std::uint64_t value)
    {
        if (value == kModelFenceSentinel)
        {
            throw std::runtime_error{"device-removed sentinel reached"};
        }
        if (value < m_completed)
        {
            throw std::logic_error{"completed fence went backwards"};
        }
        m_completed = value;
    }

    [[nodiscard]] std::uint64_t Completed() const noexcept
    {
        return m_completed;
    }

    [[nodiscard]] std::uint64_t NextFenceValue() const noexcept
    {
        return m_next;
    }

    // BeginFrame 的等待条件：completed < 该 context 的提交值才需要等待。
    [[nodiscard]] bool NeedsWait(const std::size_t frameIndex) const
    {
        return m_completed < m_submitted.at(frameIndex);
    }

    // BeginFrame 的模型版：等待条件成立时直接抛出（模型没有 event 可阻塞，
    // 这正是"Reset-before-complete 由 mock 捕获"的负向路径）。
    void BeginFrame(const std::size_t frameIndex)
    {
        if (NeedsWait(frameIndex))
        {
            throw std::logic_error{"Reset before context fence completion"};
        }
    }

    // ExecuteAndSignal 的模型版：fence 严格单调、耗尽即 fatal，成功后写回 context。
    std::uint64_t Submit(const std::size_t frameIndex)
    {
        if (m_next == kModelFenceSentinel)
        {
            throw std::overflow_error{"fence timeline exhausted"};
        }
        const std::uint64_t submitted = m_next++;
        m_submitted.at(frameIndex) = submitted;
        return submitted;
    }

    // 测试专用：把时间线直接推进到指定值（只允许向前）。
    // 为什么需要它：逐次 Submit 循环到 UINT64_MAX 是 1.8e19 次迭代（等效死循环，
    // 2026-09-10 实测挂死 ctest）；时间线耗尽的"最后一格"必须用跳进构造。
    void SetNextFenceValueForTest(const std::uint64_t value)
    {
        if (value < m_next)
        {
            throw std::logic_error{"fence next value went backwards"};
        }
        m_next = value;
    }

    // 入队回收项：retire fence 必须非零且不减（乱序 retire 会让"按 completed
    // 一次性回收"漏项，是时间线逻辑错误的信号）。
    void Retire(const std::uint64_t fence, const int payload)
    {
        if (fence == 0U)
        {
            throw std::invalid_argument{"retire fence must be non-zero"};
        }
        if (fence < m_lastRetireFence)
        {
            throw std::logic_error{"retire fence went backwards"};
        }
        m_retired.emplace_back(fence, payload);
        m_lastRetireFence = fence;
    }

    // 回收唯一判据：completed >= retireFence。CPU 帧号/交换链索引/睡眠时间都不能替代。
    std::vector<int> Reclaim(const std::uint64_t completed)
    {
        std::vector<int> reclaimed;
        while (!m_retired.empty() && m_retired.front().first <= completed)
        {
            reclaimed.push_back(m_retired.front().second);
            m_retired.pop_front();
        }
        return reclaimed;
    }

    [[nodiscard]] std::size_t RetiredCount() const noexcept
    {
        return m_retired.size();
    }

  private:
    std::uint64_t m_next = 1;
    std::uint64_t m_completed = 0;
    std::uint64_t m_lastRetireFence = 0;
    std::vector<std::uint64_t> m_submitted;
    std::deque<std::pair<std::uint64_t, int>> m_retired;
};
} // namespace MiniEngine::Rhi::D3D12::Tests
