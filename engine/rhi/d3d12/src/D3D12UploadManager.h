// ============================================================================
// D3D12UploadManager.h — 上传策略层：ring 复用 + 定向等待 + dedicated staging
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期）
// 职责：把 06 篇冻结的 M5 上传策略落成一处可测的实现：
//   1) 小请求（<= ring/4）：走 Upload Ring；无安全 span 时**只等最老 pending fence
//      一次**后重试，并把这次等待记入 stall 统计（不 busy-wait、不 flush 整个队列）；
//   2) 大请求（> ring/4）：创建 dedicated UPLOAD staging（committed），不阻塞 ring；
//      其释放挂到**提交该帧的 fence** 上（fence 前绝不释放）；
//   3) 单请求超过预算或不可表示：显式失败（std::runtime_error），不做静默降级。
// 与 queue 的关系：等待通过注入的 FenceWaiter 完成（生产传 D3D12Queue 的定向等待，
//       测试注入假实现），因此整层策略可在无 GPU 的情况下被穷举。
// 内部性说明：后端内部类型（src/）。
// 关联：docs/architecture/README.md（M5 policy / 测试清单）
//       engine/rhi/d3d12/src/D3D12DeferredRelease.h（dedicated 的延迟释放）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "D3D12DeferredRelease.h"
#include "D3D12UploadRing.h"

namespace MiniEngine::Rhi::D3D12
{
// 策略层统计（进 metadata/benchmark：ring 高水位与 stall 都是容量决策的证据）。
struct UploadManagerStats final
{
    std::uint64_t ringAllocations = 0;
    // ring 无安全 span 的次数（每次都会"等最老 pending fence 一次"）。
    // 命名刻意不用 "stalls"：最老 fence 可能已完成、等待耗时可以是 0 µs，
    // 因此这是"事件次数"而不是"实际阻塞次数"（审查意见 M5-06 次要项）。
    std::uint64_t ringNoSpanEvents = 0;
    std::uint64_t stallMicroseconds = 0; // 等待总耗时（诊断；不参与判定）
    std::uint64_t dedicatedAllocations = 0;
    std::uint64_t dedicatedBytes = 0;
    std::uint64_t rejections = 0;
};

class D3D12UploadManager final
{
  public:
    // 等待某个**已提交**的 fence 值完成（生产：D3D12Queue 的定向等待；测试：假实现）。
    using FenceWaiter = std::function<void(std::uint64_t)>;

    D3D12UploadManager() = default;
    ~D3D12UploadManager();
    D3D12UploadManager(const D3D12UploadManager&) = delete;
    D3D12UploadManager& operator=(const D3D12UploadManager&) = delete;

    // 绑定 ring 与 dedicated 创建所需的设备。
    //
    // 参数：
    //   device            —— 创建 dedicated staging（可为空：此时大请求直接失败）
    //   ring              —— 已初始化的上传环
    //   waiter            —— 定向等待实现（不可为空）
    //   dedicatedBudget   —— dedicated 单次上限（字节）；0 表示不限制（仍受 size 可表示性约束）
    // 失败：waiter 为空 → std::invalid_argument。
    void Initialize(ID3D12Device* device, D3D12UploadRing& ring, FenceWaiter waiter, std::uint64_t dedicatedBudget);

    // 按策略分配上传空间。tag 只用于日志定位（例如 "object-constants" / "mesh"）。
    //
    // 失败：
    //   - 请求 0 字节或对齐非法 → std::invalid_argument；
    //   - 超过 dedicatedBudget（或无法创建 dedicated）→ std::runtime_error；
    //   - 等过最老 fence 后 ring 仍无空间且不满足 dedicated 条件 → std::runtime_error。
    [[nodiscard]] UploadAllocation Allocate(std::uint64_t size, std::uint64_t alignment, std::string_view tag);

    // 提交当前帧：ring 的 current span 归入 fence；本帧创建的 dedicated staging
    // 也归入同一 fence（fence 完成后才真正释放）。
    void CommitFrame(std::uint64_t fenceValue);

    // 回收 ring pending span 与 dedicated staging（判据都是 completed >= retireFence）。
    void Reclaim(std::uint64_t completedFenceValue);

    [[nodiscard]] const UploadManagerStats& Stats() const noexcept;
    [[nodiscard]] std::size_t PendingDedicatedCount() const noexcept;
    [[nodiscard]] D3D12DeferredRelease& DeferredRelease() noexcept;
    [[nodiscard]] std::size_t RetiredDedicatedCount() const noexcept
    {
        return m_deferred.PendingCount();
    }

  private:
    // dedicated 条件：06 篇规定"大于 ring 1/4 的 mesh/texture upload"走 dedicated。
    [[nodiscard]] bool NeedsDedicated(std::uint64_t size) const noexcept;
    [[nodiscard]] UploadAllocation CreateDedicated(std::uint64_t size, std::uint64_t alignment, std::string_view tag);

    ID3D12Device* m_device = nullptr;
    D3D12UploadRing* m_ring = nullptr;
    FenceWaiter m_waiter;
    std::uint64_t m_dedicatedBudget = 0;
    // 本帧创建的 dedicated staging：CommitFrame 时挂到该帧 fence 上。
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_frameDedicated;
    D3D12DeferredRelease m_deferred;
    UploadManagerStats m_stats;
};
} // namespace MiniEngine::Rhi::D3D12
