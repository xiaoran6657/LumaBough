// ============================================================================
// TaskSystemDecodeDispatcher.h — decode 段的生产执行器（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：把 decode 任务提交到 M7-03/04 的任务系统，并提供一个"等待已提交 decode
//       全部完成"的工具/测试入口。decode 是纯 CPU 工作：
//         * 不读文件、不碰 RHI、不创建 GPU 句柄（ADR-0008 线程边界）；
//         * 任务带 MainHelpAllowed：等待线程（主线程/工具线程）可以帮着执行，
//           这既缩短工具路径延迟，也让"decode 在 compute 侧执行"可被观测。
// 关联：docs/architecture/README.md「第 3 步」
//       engine/tasks/include/MiniEngine/Tasks/TaskSystem.h（等待角色契约）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AsyncAssetLoader.h>
#include <MiniEngine/Tasks/TaskGroup.h>
#include <MiniEngine/Tasks/TaskSystem.h>

namespace MiniEngine::Assets
{
class TaskSystemDecodeDispatcher final : public DecodeDispatcher
{
  public:
    explicit TaskSystemDecodeDispatcher(Tasks::TaskSystem& tasks) noexcept : m_tasks(tasks)
    {
    }

    [[nodiscard]] bool SubmitDecode(Tasks::TaskFunction entry, void* context) noexcept override
    {
        Tasks::Task task{entry, context, nullptr, Tasks::TaskFlags::MainHelpAllowed, {0, "AssetDecode", 0}};
        return m_tasks.Submit(task, m_group) == Tasks::SubmitResult::Accepted;
    }

    // 只允许主线程/工具线程调用（会帮助执行 decode）；禁止 render thread 与 I/O 线程调用。
    void WaitDecodes() noexcept override
    {
        m_tasks.Wait(m_group, Tasks::TaskWaitRole::MainThread);
    }

  private:
    Tasks::TaskSystem& m_tasks;
    // 常驻 group：Add/Done 与 Wait 的角色契约由 TaskSystem 保证；组随 dispatcher 存活。
    Tasks::TaskGroup m_group;
};
} // namespace MiniEngine::Assets
