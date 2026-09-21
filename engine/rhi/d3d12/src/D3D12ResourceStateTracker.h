// ============================================================================
// D3D12ResourceStateTracker.h — 设备侧 tracker：把状态请求变成真 barrier
// 里程碑：M5（07 篇 Resource Barrier 与状态跟踪；手抄清单第 1 条）
// 职责：持有 `ResourceKey → ID3D12Resource*` 的映射，把
//       ResourceStateRegistry（纯 CPU 状态机）产出的 BarrierRequest 转成
//       D3D12_RESOURCE_BARRIER，并在依赖新状态的第一个 draw/copy/clear **之前**
//       由调用方 FlushBarriersTo(list) 一次性发出（07 篇 Barrier batching）。
// M5 的单 list 限制（写进类型与 ADR）：只有一个 recording command list，因此不需要
//       跨 list/queue 的合并算法；tracker 不做跨队列状态协商。
// 与 04 篇的关系：tracker 接管 back buffer 的 PRESENT↔RENDER_TARGET 记账后，
//       D3D12SwapChain 的内部三态数组改为委托本类（generation 在 resize 时递增，
//       旧引用立即失效）。
// 内部性说明：后端内部类型（src/），直接暴露 ID3D12Resource 与 command list。
// 关联：docs/architecture/README.md（状态表 / Tracker 边界）
//       engine/rhi/d3d12/src/D3D12ResourceStateRegistry.h（纯 CPU 状态机）
// ============================================================================
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <map>
#include <string>

#include "D3D12ResourceStateRegistry.h"

namespace MiniEngine::Rhi::D3D12
{
class D3D12ResourceStateTracker final
{
  public:
    D3D12ResourceStateTracker() = default;
    ~D3D12ResourceStateTracker() = default;
    D3D12ResourceStateTracker(const D3D12ResourceStateTracker&) = delete;
    D3D12ResourceStateTracker& operator=(const D3D12ResourceStateTracker&) = delete;

    // 注册资源及其实际初始状态（07 篇：创建时注册实际初始 state，未知 state 不允许猜）。
    //
    // 参数：
    //   resource         —— 待跟踪资源（tracker 不持有引用，调用方保证其存活至 Unregister）
    //   subresourceCount —— 子资源数（1 表示无 mip/array）
    //   initialState     —— 资源此刻的真实状态
    //   debugName        —— 诊断名（进日志与 PIX；同时用于把资源在调试层命名）
    //   generation       —— 重建/换尺寸后递增；同一 id 的旧 generation 立即失效
    // 返回：资源键（后续所有操作都用它）。
    // 失败：resource 为空、subresourceCount == 0、同 (id, generation) 重复注册 → 抛异常。
    ResourceKey Register(ID3D12Resource& resource, std::uint32_t subresourceCount, D3D12_RESOURCE_STATES initialState,
                         const wchar_t* debugName, std::uint32_t generation = 0);

    // 注销（释放资源前必须调用，否则 tracker 会持有悬垂指针）。
    void Unregister(ResourceKey key);

    // 开始 recording：pending = committed，barrier 批与 trace 清零。
    void BeginRecording();

    // 请求转换（立即更新 pending）。batched 模式下 barrier 进入待发批；
    // 调用方必须在依赖新状态的第一个绘制/拷贝/清屏**之前** FlushBarriersTo。
    void Transition(ResourceKey key, D3D12_RESOURCE_STATES desired,
                    std::uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    // 把待发 barrier 一次性提交给 command list（07 篇：每个 pass 入口一小批）。
    // 返回本次发出的 barrier 数量（0 表示没有需要发的）。
    std::uint32_t FlushBarriersTo(ID3D12GraphicsCommandList& commandList);

    // list 成功 Execute 之后调用：pending → committed。
    void CommitExecuted();

    // recording 失败（Close/Execute 出错）：丢弃 pending，不污染 committed。
    void Rollback();

    // 断言辅助：确认某资源（全部或指定 subresource）当前处于期望状态。
    // 失败：不匹配 → std::logic_error（带资源名与实际状态，便于定位）。
    void VerifyState(ResourceKey key, D3D12_RESOURCE_STATES expected,
                     std::uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) const;

    [[nodiscard]] bool IsRecording() const noexcept;
    [[nodiscard]] D3D12_RESOURCE_STATES CurrentState(ResourceKey key, std::uint32_t subresource = 0U) const;
    [[nodiscard]] std::size_t TrackedResourceCount() const noexcept;
    [[nodiscard]] std::uint64_t TraceHash() const noexcept;
    [[nodiscard]] std::uint64_t BarrierCount() const noexcept;
    [[nodiscard]] std::uint64_t CommittedStateHash() const noexcept;
    [[nodiscard]] std::uint32_t PendingBarrierCount() const noexcept;

  private:
    [[nodiscard]] static std::uint64_t IdOf(ID3D12Resource& resource) noexcept;

    ResourceStateRegistry m_registry;
    // id → **非拥有**资源指针（tracker 不延长资源寿命：resize/热重载都依赖"先 Unregister
    // 再释放"的顺序，持有引用会让旧资源一直活着）。名字用于诊断消息。
    std::map<std::uint64_t, ID3D12Resource*> m_resources;
    // 名字按 UTF-8 保存（注册时的宽字符现场转换），诊断消息直接拼接不再转换。
    std::map<std::uint64_t, std::string> m_names;
};
} // namespace MiniEngine::Rhi::D3D12
