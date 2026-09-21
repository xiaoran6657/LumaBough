// ============================================================================
// TaskSystem.h — M7 任务系统公开契约（提交 / 等待 / 关闭）
// 里程碑：M7-03（任务系统契约、所有权与生命周期）
// 归属与线程规则（与 ADR-0008 一致）：
//   * worker 只执行纯 CPU task：不执行文件 I/O、不等 GPU fence/present、不做 RHI 调用；
//   * 等待 API 必须显式声明角色（MainThread / Worker / NonHelping）：不能用"不是 worker"
//     推断 main，否则 render/I/O/任意外部线程都会被误当成 helper；
//   * render thread 使用 NonHelping，保持 RHI ownership 与帧调度可预测；
//   * 任何允许 help 的 task 都不能依赖"必须由原提交线程继续执行"。
// 生命周期：
//   Running → StopAccepting(Draining|Cancelling) → Joining → Stopped；Shutdown 幂等。
//   析构会兜底 drain（不丢任务），但初始化/关闭顺序要求显式 Shutdown：
//   logging/profiling → allocators → TaskSystem → AssetLoader → RenderSystem（反向关闭）。
// 关联：docs/architecture/README.md
//       engine/tasks/src/TaskSystem.cpp（状态机与 worker 入口）
//       docs/architecture/DECISIONS.md
// ============================================================================

#pragma once

#include <MiniEngine/Tasks/Task.h>
#include <MiniEngine/Tasks/TaskGroup.h>
#include <MiniEngine/Tasks/TaskStatistics.h>

#include <cstdint>
#include <limits>
#include <memory>

namespace MiniEngine::Tasks
{
// worker 线程返回 [0, WorkerCount())；非 worker 线程（main/render/I/O/测试线程）
// 返回该值。worker-local context 的判定只走 CurrentWorkerIndex()。
inline constexpr std::uint32_t kNonWorkerIndex = std::numeric_limits<std::uint32_t>::max();

// 默认 worker 数：max(1, hardware_concurrency() - 2)。保留 main/render 与 I/O 线程后，
// 真实最优值由 1,2,4,… sweep 决定（M7-09 记录）；hardware_concurrency()==0 时 fallback 到 1。
[[nodiscard]] std::uint32_t DefaultWorkerCount() noexcept;

// 调度器版本：M7-03 的全局队列（v0）与 M7-04 的 per-worker deque + 窃取（v1）共存，
// 便于同一二进制内做 A/B（scaling sweep 的 "版本" 维度）。
enum class SchedulerMode : std::uint8_t
{
    GlobalQueue,   // v0：单队列 + 条件变量（M7-03 基线）
    PerWorkerDeque // v1：owner bottom / thief top；外部提交走 inject queue
};

struct TaskSystemConfig
{
    std::uint32_t workerCount = 1;
    // v0 全局队列容量；v1 里同时是 inject queue（外部提交与本地满时的回退）容量。
    std::uint32_t injectQueueCapacity = 4096;
    // v1：每个 worker 的 local deque 容量。递归提交在本地满时必须回退到 inject，
    // 不允许无界本地增长。
    std::uint32_t localQueueCapacity = 256;
    std::uint32_t stealAttemptsBeforeSleep = 4; // v1：睡前最多尝试的 victim 数
    std::uint64_t randomSeed = 6657;            // v1：worker-local PRNG 种子（禁用全局 rand）
    SchedulerMode mode = SchedulerMode::GlobalQueue;
    // 测试缝（默认 0＝关闭，生产不使用）：在"pending 已记账、归宿未定"之间自旋指定微秒，
    // 把 publish 的记账窗口拉长到可确定复现的程度（M7-TASK-RACE-FLAKE 的回归用例）。
    std::uint32_t injectPublishStallMicroseconds = 0;
};

enum class SubmitResult
{
    Accepted,
    InvalidTask,
    QueueFull,
    Stopping
};

// 显式等待角色：决定等待者是否帮助执行、以及能取哪类任务。
enum class TaskWaitRole : std::uint8_t
{
    MainThread, // 非 worker 的外部线程；只取 MainHelpAllowed 任务。
    Worker,     // worker 线程内部等待（嵌套 group）；可取任意任务。
    NonHelping  // render/I/O 线程；不取任务，只等完成通知。
};

// 系统级统计快照：用于测试断言与 M7-05 起的 raw JSON（taskQueueDepth/activeWorkers）。
struct TaskSystemStatistics
{
    std::uint64_t submitted = 0;
    std::uint64_t executed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t pending = 0;         // accepted - (executed + cancelled)
    std::uint64_t queueDepth = 0;      // inject queue 当前深度
    std::uint64_t localQueueDepth = 0; // v1：所有 local deque 深度之和
    std::uint64_t sleepingWorkers = 0;
    std::uint32_t workerCount = 0;
};

// 单 worker 计数快照（relaxed 读；只用于观测）。
struct WorkerCounters
{
    std::uint64_t executed = 0;
    std::uint64_t stealAttempts = 0;
    std::uint64_t stealSuccesses = 0;
    std::uint64_t sleeps = 0;
    std::uint64_t queueLatencyNanoseconds = 0;
    std::uint64_t localPushes = 0;
    std::uint64_t injectedPushes = 0;
    std::uint64_t localQueueHighWater = 0;
    std::uint64_t wakeups = 0;
    std::uint32_t lastVictim = 0xFFFFFFFFU;
};

class TaskSystem final
{
  public:
    explicit TaskSystem(const TaskSystemConfig& config);
    ~TaskSystem();

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

    // Submit 的顺序契约：校验 → group.Add（预留完成位）→ publish → 失败回滚 group。
    // 传 group 的重载负责完成计数；不传 group 的版本要求 payload 由调用者自行回收
    // （例如 shutdown 取消时按 group 回收的语义不适用于它）。
    [[nodiscard]] SubmitResult Submit(Task task) noexcept;
    [[nodiscard]] SubmitResult Submit(Task task, TaskGroup& group) noexcept;

    // Wait 返回后，本组 payload 可以被安全回收。role 必须与调用线程身份一致。
    void Wait(TaskGroup& group, TaskWaitRole role) noexcept;
    // 取一个可执行任务并执行；无可执行任务返回 false。NonHelping 永远返回 false。
    [[nodiscard]] bool TryExecuteOne(TaskWaitRole role) noexcept;

    // drain=true：停止接收 → 排空已接收任务 → 通知并 join（正常退出路径）。
    // drain=false：停止接收 → 取消未开始任务（按 group.Done 记账）→ 通知并 join（fatal 路径）。
    // 幂等：重复调用只做 join 收尾；并发调用由 CAS 决出唯一执行者。
    void Shutdown(bool drain) noexcept;

    [[nodiscard]] std::uint32_t WorkerCount() const noexcept;
    [[nodiscard]] bool IsAccepting() const noexcept;
    [[nodiscard]] TaskSystemStatistics Statistics() const noexcept;
    [[nodiscard]] WorkerCounters WorkerCountersFor(std::uint32_t workerIndex) const noexcept;

    // 当前线程的 worker 索引（kNonWorkerIndex 表示非 worker）。worker-local context 入口。
    [[nodiscard]] static std::uint32_t CurrentWorkerIndex() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Tasks
