// ============================================================================
// D3D12ResourceStateRegistry.h — 资源状态记账（纯 CPU，可穷举；无 GPU 对象）
// 里程碑：M5（07 篇 Resource Barrier 与状态跟踪；手抄清单第 1/2 条）
// 职责：以 `(资源 id, generation) → 每个 subresource 的状态` 维护两份状态：
//   - **committed**：最后一次成功 Execute 的状态（全局真值）；
//   - **pending**：当前 recording 内的状态（在 command list 内立即更新）。
//   `Transition` 只在不同状态时产生 barrier 请求；`FlushBarriers` 由设备侧 tracker
//   一次性发出；`CommitExecuted` 把 pending 提升为 committed；`Rollback` 丢弃 pending。
// 为什么与设备侧拆开：状态机（whole/subresource、pending/commit、generation 失效、
//   trace hash）是纯逻辑，可被 CPU 测试穷举；把它混进 ID3D12Resource 记账会让每个
//   边界用例都必须先建资源。设备侧 tracker 只负责"把请求变成真 barrier 并发出去"。
// generation 的用途：资源被重建/换尺寸后 generation 递增，旧句柄的任何操作立即失败
//   ——这正是 04 篇 resize 后旧 back buffer 引用不能继续使用的判据。
// 关联：docs/architecture/README.md（状态表 / Tracker 边界）
//       engine/rhi/d3d12/src/D3D12ResourceStateTracker.h（设备侧）
// ============================================================================
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
// 资源标识：id 在设备侧 tracker 里就是 ID3D12Resource* 的位模式；
// generation 由注册方递增（重建/换尺寸后必须 +1）。
struct ResourceKey final
{
    std::uint64_t id = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool operator==(const ResourceKey& other) const noexcept
    {
        return id == other.id && generation == other.generation;
    }
    [[nodiscard]] bool operator!=(const ResourceKey& other) const noexcept
    {
        return !(*this == other);
    }
    [[nodiscard]] bool operator<(const ResourceKey& other) const noexcept
    {
        return id != other.id ? id < other.id : generation < other.generation;
    }
};

// 一次状态转换请求（barrier 的内容，但不含 ID3D12Resource*）。
struct BarrierRequest final
{
    ResourceKey key;
    std::uint32_t subresource = 0;
    D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_COMMON;
};

class ResourceStateRegistry final
{
  public:
    // 注册一个资源及其每个 subresource 的**实际初始状态**（07 篇：创建时注册，不允许猜）。
    // 失败：subresourceCount == 0、重复注册（同 key）抛 std::invalid_argument / std::logic_error。
    void Register(const ResourceKey key, std::uint32_t subresourceCount, D3D12_RESOURCE_STATES initialState);

    // 注销（通常发生在资源释放/重建前）。未注册的 key 视为 no-op。
    void Unregister(const ResourceKey key);

    // 开始一段 recording：pending 重置为 committed，barrier 批与 trace 清零。
    // 失败：已经在 recording 中 → std::logic_error。
    void BeginRecording();

    // 请求转换：只在不同状态时生成 barrier 请求，并立即更新 pending。
    // 返回本次新增的请求（已追加到内部批；调用方通常忽略返回值，测试用它做断言）。
    //
    // 失败：未 BeginRecording、key 未注册、subresource 越界 → std::logic_error / std::out_of_range。
    std::vector<BarrierRequest> Transition(ResourceKey key, D3D12_RESOURCE_STATES desired,
                                           std::uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    // 取走当前待发 barrier 批（设备侧 tracker 用它在依赖新状态的第一个 draw 之前发出）。
    [[nodiscard]] const std::vector<BarrierRequest>& PendingBarriers() const noexcept;
    void ClearPendingBarriers();

    // recording 成功 Execute 后：pending → committed，结束 recording。
    // 失败：未在 recording 中 → std::logic_error。
    void CommitExecuted();

    // recording 失败（Close/Execute 出错）：丢弃 pending（committed 不变），结束 recording。
    void Rollback();

    [[nodiscard]] bool IsRecording() const noexcept;
    // pending 视角的当前状态（未在 recording 时即 committed）。
    // 失败：key 未注册 → std::out_of_range；subresource 越界 → std::out_of_range。
    [[nodiscard]] D3D12_RESOURCE_STATES CurrentState(ResourceKey key, std::uint32_t subresource) const;
    // 同一资源所有 subresource 是否处于同一状态（"整资源转换"的前置校验，07 篇要求）。
    [[nodiscard]] bool AllSubresourcesMatch(ResourceKey key, D3D12_RESOURCE_STATES state) const;
    // 一个资源被整资源表达（ALL_SUBRESOURCES）时的前置校验用：返回第一个不一致的 subresource。
    [[nodiscard]] std::optional<std::uint32_t> FirstMismatchingSubresource(ResourceKey key,
                                                                           D3D12_RESOURCE_STATES state) const;

    [[nodiscard]] std::size_t TrackedResourceCount() const noexcept;
    [[nodiscard]] std::uint32_t SubresourceCount(ResourceKey key) const;

    // 本帧已生成的 barrier 序列的滚动哈希（每帧 BeginRecording 重置）：
    // "每帧 barrier trace hash 稳定"就是比较它（07 篇正向验证第 1 条）。
    [[nodiscard]] std::uint64_t TraceHash() const noexcept;
    [[nodiscard]] std::uint64_t BarrierCount() const noexcept;
    // committed 状态集合的哈希：跨 run 比较"最终状态是否一致"。
    [[nodiscard]] std::uint64_t CommittedStateHash() const noexcept;

  private:
    struct State final
    {
        std::vector<D3D12_RESOURCE_STATES> committed;
        std::vector<D3D12_RESOURCE_STATES> pending;
        // 注册顺序槽位：trace/state hash 用它而不是 key.id——**地址每次运行都不同**，
        // 直接哈希指针会让"每帧 barrier trace hash 稳定"永远失败（本篇实测发现：
        // 两次相同运行的 chain hash 不一致）。槽位与资源逻辑次序一一对应，跨 run 稳定。
        std::uint32_t traceSlot = 0;
    };

    [[nodiscard]] State& At(ResourceKey key);
    [[nodiscard]] const State& At(ResourceKey key) const;

    std::map<ResourceKey, State> m_states;
    std::vector<BarrierRequest> m_pendingBarriers;
    std::uint64_t m_traceHash = 0;
    std::uint64_t m_barrierCount = 0;
    std::uint32_t m_nextTraceSlot = 0;
    bool m_recording = false;
};
} // namespace MiniEngine::Rhi::D3D12
