// ============================================================================
// D3D12UploadRingAllocator.h — 环形上传缓冲的区间记账（纯 CPU，无 D3D 类型）
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 1/2 条）
// 职责：管理 [0, capacity) 的环形空间：按对齐向上取整后分配连续 span、必要时回绕、
//       保证**既不覆盖 pending span（已被提交引用）也不覆盖本帧 current span**、
//       按 fence 延迟回收、并统计 high-water（进 benchmark/metadata）。
//       它不接触 GPU——因此 CPU 测试直接消费生产实现（与 05 篇分配器同一策略）。
// 回绕语义（06 篇明确要求）：一帧内若发生回绕，尾部与头部会形成**两个非回绕 span**，
//       CommitFrame 给二者标记**同一个 fence**；禁止用 begin > end 的单一区间表示回绕。
// 失败语义：构造容量 0 → std::invalid_argument；对齐非法 → std::invalid_argument；
//       CommitFrame 的 fence == 0 或回退 → std::invalid_argument / std::logic_error。
//       无安全 span 时**不抛异常**，返回 std::nullopt 由策略层决定（等最老 fence 或 dedicated）。
// 关联：docs/architecture/README.md（Ring 分配 / 测试清单）
//       engine/rhi/d3d12/src/D3D12UploadRing.h（映射后的资源与 CPU/GPU 指针）
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
// ring 内的一段空间（不含 CPU/GPU 指针：那是 D3D12UploadRing 的职责）。
struct UploadRingSpan final
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

class UploadRingAllocator final
{
  public:
    // 对齐档位（06 篇「对齐」表）：CBV/root CBV 256、buffer copy 至少 16、
    // texture placement 512、texture row pitch 256。
    static constexpr std::uint64_t kConstantBufferAlignment = 256U;
    static constexpr std::uint64_t kBufferCopyAlignment = 16U;
    static constexpr std::uint64_t kTexturePlacementAlignment = 512U;
    static constexpr std::uint64_t kTextureRowPitchAlignment = 256U;

    explicit UploadRingAllocator(std::uint64_t capacity);

    UploadRingAllocator(const UploadRingAllocator&) = delete;
    UploadRingAllocator& operator=(const UploadRingAllocator&) = delete;

    // 分配一段对齐后的连续空间。
    //
    // 参数：
    //   size      —— 请求字节数（0 或 > capacity 直接返回 nullopt，不抛）
    //   alignment —— 2 的幂；非 2 的幂抛 std::invalid_argument
    // 返回：有安全 span 时为偏移与大小；否则 nullopt（调用方按策略处理：
    //   等最老 fence 一次重试，或改用 dedicated staging）。
    [[nodiscard]] std::optional<UploadRingSpan> TryAllocate(std::uint64_t size, std::uint64_t alignment);

    // 把本帧 current span 全部转入 pending 并标记同一 fence（回绕帧的两个 span 同 fence）。
    // 失败：fence == 0 抛 std::invalid_argument；fence 回退抛 std::logic_error。
    void CommitFrame(std::uint64_t fenceValue);

    // 回收所有 retireFence <= completedFenceValue 的 pending span。
    void Reclaim(std::uint64_t completedFenceValue);

    [[nodiscard]] std::uint64_t Capacity() const noexcept;
    [[nodiscard]] std::uint64_t Head() const noexcept;
    // 峰值占用（pending + current 的字节数之和的最大值）：进 benchmark/metadata。
    [[nodiscard]] std::uint64_t HighWaterBytes() const noexcept;
    [[nodiscard]] std::uint64_t OccupiedBytes() const noexcept;
    [[nodiscard]] std::size_t PendingSpanCount() const noexcept;
    [[nodiscard]] std::size_t CurrentSpanCount() const noexcept;
    // 最老的 pending fence（0 表示没有 pending）：策略层"等最老 fence 一次"用它。
    [[nodiscard]] std::uint64_t OldestPendingFence() const noexcept;
    // 当前最大的可分配连续空间（诊断：判断是否需要 dedicated/等 fence）。
    [[nodiscard]] std::uint64_t LargestFreeSpan() const noexcept;
    // shutdown 前置条件：没有 current、也没有 pending。
    [[nodiscard]] bool IsIdle() const noexcept;

  private:
    struct Span final
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };

    struct PendingSpan final
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t fenceValue = 0;
    };

    // 与 pending + current 是否重叠（区间按"非回绕"存储，因此可用普通区间判交）。
    [[nodiscard]] bool Overlaps(std::uint64_t begin, std::uint64_t end) const;
    // 记录 current span（相邻则合并）并更新 high-water。
    void Record(std::uint64_t begin, std::uint64_t end);

    std::deque<PendingSpan> m_pending; // 按 fence 单调入队
    std::vector<Span> m_current;       // 本帧已分配（尚未 Commit）
    std::uint64_t m_capacity = 0;
    std::uint64_t m_head = 0;
    std::uint64_t m_highWater = 0;
    std::uint64_t m_lastCommittedFence = 0;
};
} // namespace MiniEngine::Rhi::D3D12
