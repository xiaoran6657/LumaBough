// ============================================================================
// TaskStatistics.h — M7 任务统计（每 worker 独立计数，cache line 隔离）
// 里程碑：M7-03（任务系统契约、所有权与生命周期）
// 说明：这些计数全部是 relaxed 原子：它们只用于观测与实验台账，不发布 payload、
//   也不参与完成判定（完成判定只由 TaskGroup 的 acquire/release 链负责）。
// 关联：docs/architecture/README.md（统计与配置）
//       docs/architecture/README.md（steal* 字段的用途）
// ============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace MiniEngine::Tasks
{
// C++20 才提供 hardware_destructive_interference_size；缺失时用 64 兜底（x64 cache line）。
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kDestructiveInterferenceSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kDestructiveInterferenceSize = 64;
#endif

// 每 worker 一个实例并以 cache line 对齐：相邻 worker 的写入不共享 cache line。
// C4324 是"因对齐说明符而填充"的提示——这正是本结构的目的（false sharing 是性能缺陷，
// 不是编译缺陷），因此在结构范围里显式关闭它，而不是抬高全目标警告等级。
#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(kDestructiveInterferenceSize) WorkerStatistics
{
    std::atomic<std::uint64_t> executed{0};
    // steal 尝试按"被尝试的 victim"计数（与 victim 选择次数一致），成功数单独记。
    std::atomic<std::uint64_t> stealAttempts{0};
    std::atomic<std::uint64_t> stealSuccesses{0};
    std::atomic<std::uint64_t> sleeps{0};
    // Σ(执行时刻 - 提交时刻)：单条计数不含样本，均值由 executed 求出（M7-09 报告口径）。
    std::atomic<std::uint64_t> queueLatencyNanoseconds{0};
    // ---- M7-04：per-worker deque 与窃取观测 ----
    std::atomic<std::uint64_t> localPushes{0};    // 进入本 worker local deque 的任务数
    std::atomic<std::uint64_t> injectedPushes{0}; // 本地满/外部提交导致的 inject 路由数
    // local deque 深度高水位由 WorkDeque 自己在锁内维护（WorkDeque::HighWater）。
    std::atomic<std::uint64_t> wakeups{0}; // 因"有工作"被唤醒次数（spin 的反面证据）
    std::atomic<std::uint64_t> stolenTasks{0};          // 本 worker 偷到的任务数（= stealSuccesses）
    std::atomic<std::uint32_t> lastVictim{0xFFFFFFFFU}; // 最近一次成功偷取的 victim 索引
};
#pragma warning(pop)

static_assert(alignof(WorkerStatistics) >= kDestructiveInterferenceSize);
} // namespace MiniEngine::Tasks

// These counters use relaxed operations: they do not publish task payload or completion.
