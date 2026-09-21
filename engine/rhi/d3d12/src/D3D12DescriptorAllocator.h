// ============================================================================
// D3D12DescriptorAllocator.h — descriptor 槽位区间分配器（纯 CPU，无 D3D 类型）
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 1/2 条）
// 职责：管理一段 [0, capacity) 的槽位空间：首次适配分配连续区间、按 generation
//       识别过期句柄、相邻空闲块合并、按 fence 延迟回收。它**不接触 GPU**——
//       因此 CPU 测试直接消费生产实现（05 篇要求"模型与实现逐分支等价"，
//       同一份实现是最强形式），而 D3D12DescriptorHeap 只负责 heap 与句柄换算。
// 为什么拆两层：分配规则（连续/合并/generation/回收）是可穷举的纯逻辑，混进
//       ID3D12DescriptorHeap 会让每个边界用例都必须先创建设备；拆开后 GPU 与
//       CPU 两条路径的失败语义都能被独立锁定。
// 失败语义：
//   Allocate：count == 0 → std::invalid_argument；generation 耗尽 → std::overflow_error；
//             无足够连续空间 → std::bad_alloc（容量边界）。
//   Free/Retire：空 range、部分匹配、generation 不匹配、重复释放 → std::logic_error；
//             Retire 的 fence == 0 → std::invalid_argument，fence 回退 → std::logic_error。
// 关联：docs/architecture/README.md（分配与热重载）
//       engine/rhi/d3d12/src/D3D12DescriptorHeap.h（heap 与句柄换算）
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <queue>

namespace MiniEngine::Rhi::D3D12
{
// 一次分配得到的连续区间。generation 用于识别"过期句柄"：同一 base 被回收后
// 再分配会得到新的 generation，旧 range 的后续操作立即失败（绝不悄悄操作别人的槽位）。
struct DescriptorRange final
{
    std::uint32_t base = 0;
    std::uint32_t count = 0;
    std::uint32_t generation = 0;

    // 有效区间判据（count != 0）：默认构造的 range 不是合法句柄。
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return count != 0U;
    }
};

class DescriptorRangeAllocator final
{
  public:
    // 建立容量为 capacity 的分配器（初始整段空闲）。
    // 失败：capacity == 0 抛 std::invalid_argument。
    explicit DescriptorRangeAllocator(std::uint32_t capacity);

    DescriptorRangeAllocator(const DescriptorRangeAllocator&) = delete;
    DescriptorRangeAllocator& operator=(const DescriptorRangeAllocator&) = delete;

    // 首次适配分配 count 个连续槽位。
    // 失败：见文件头的失败语义（invalid_argument / overflow_error / bad_alloc）。
    [[nodiscard]] DescriptorRange Allocate(std::uint32_t count);

    // 立即归还（仅用于从未提交给 GPU 的分配，例如初始化失败回滚）。
    // 失败：range 无效/过期/部分匹配/已释放抛 std::logic_error。
    void Free(DescriptorRange range);

    // 延迟归还：进入 retired 队列，completedFence >= fenceValue 才真正可复用。
    // 失败：见文件头的失败语义（含 fence == 0 与 fence 回退）。
    void Retire(DescriptorRange range, std::uint64_t fenceValue);

    // 回收所有 retireFence <= completedFenceValue 的区间并合并相邻空闲块。
    void Reclaim(std::uint64_t completedFenceValue);

    [[nodiscard]] std::uint32_t Capacity() const noexcept;
    // 包括仍在等待 fence 的 retired 槽位，碎片不影响计数。
    [[nodiscard]] std::uint32_t UsedCount() const noexcept;
    [[nodiscard]] std::size_t ActiveRangeCount() const noexcept;
    [[nodiscard]] std::size_t FreeBlockCount() const noexcept;
    [[nodiscard]] std::size_t RetiredRangeCount() const noexcept;
    // 当前连续空闲空间的最大值（诊断：容量是否够下一个表）。
    [[nodiscard]] std::uint32_t LargestFreeBlock() const noexcept;

  private:
    struct Retired final
    {
        std::uint64_t fenceValue;
        DescriptorRange range;
    };

    // 归还并合并相邻空闲块（前后都判 overlap：overlap 说明记账已坏，立即失败）。
    void FreeAndCoalesce(DescriptorRange range);
    // 校验 range 是当前活跃分配且 generation 匹配；返回其迭代器。
    [[nodiscard]] std::map<std::uint32_t, DescriptorRange>::iterator FindActive(const DescriptorRange& range);

    std::map<std::uint32_t, std::uint32_t> m_freeByBase;     // base → 空闲块长度
    std::map<std::uint32_t, DescriptorRange> m_activeByBase; // base → 活跃 range
    std::queue<Retired> m_retired;                           // 按 retireFence 单调入队
    std::uint32_t m_capacity = 0;
    std::uint32_t m_nextGeneration = 1;
    std::uint64_t m_lastRetireFence = 0;
};
} // namespace MiniEngine::Rhi::D3D12
