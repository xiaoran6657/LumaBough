// ============================================================================
// Task.h — M7 任务表示（窄接口：函数指针 + payload owner）
// 里程碑：M7-03（任务系统契约、所有权与生命周期）
// 职责：定义 task 的最小形态。刻意不用 std::function：payload 的分配与释放必须由
//   调用者显式拥有，系统不隐藏分配、不猜测所有权。
// 关联：docs/architecture/README.md（Task 表示 / 禁止项）
//       TaskGroup.h（完成计数）、TaskSystem.h（提交与等待）
// 归属契约：
//   * payload 允许两种来源：明确 owned heap/block（由任务完成路径释放）或 frame arena 中
//     的 immutable context（保证 TaskGroup::Wait() 返回后才 reset）；
//   * 禁止捕获栈引用后让 task 超出当前作用域；禁止 task 抛异常穿过 worker 入口；
//   * 禁止在 task 内持有未 pin 的 asset 指针或可重分配容器元素地址。
// ============================================================================

#pragma once

#include <cstdint>

namespace MiniEngine::Tasks
{
class TaskGroup;

// task 函数必须是 noexcept：异常不得越过 worker 入口（见 TaskSystem.cpp 的 Execute）。
using TaskFunction = void (*)(void*) noexcept;

enum class TaskFlags : std::uint8_t
{
    None = 0,
    // 允许 main thread 在等待期间帮助执行。只有"纯 CPU、无外部阻塞、可被任意线程执行"的
    // task 才能带这个标志（render/I/O 线程永不带此标志取任务）。
    MainHelpAllowed = 1 << 0
};

[[nodiscard]] constexpr bool HasFlag(const TaskFlags value, const TaskFlags flag) noexcept
{
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

// 诊断字段：Submit 会填 id / enqueueTicks / submitterThread，用于统计与失败定位。
// 这些字段不参与调度决策，也不得成为正确性前提。
struct TaskDebugInfo
{
    std::uint64_t id = 0;
    const char* category = "Unknown"; // 必须是静态存储期字符串。
    std::uint32_t submitterThread = 0;
    // 提交时刻（steady_clock ticks）：worker 用它累计 queueLatency（等待时间）。
    std::uint64_t enqueueTicks = 0;
};

struct Task
{
    TaskFunction function = nullptr;
    void* context = nullptr;
    TaskGroup* group = nullptr;
    TaskFlags flags = TaskFlags::None;
    TaskDebugInfo debug{};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return function != nullptr;
    }

    void Execute() const noexcept
    {
        function(context);
    }
};
} // namespace MiniEngine::Tasks
