// ============================================================================
// DescriptorAllocatorTests.cpp — descriptor range 分配器的纯 CPU 契约测试
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 2 条）
// 职责：穷举分配器规则：material/global 两个表的连续性、相邻空闲块合并、
//       容量边界与溢出、generation 过期、double-free/部分匹配、retire 前不可
//       复用、retire fence 单调。
// 为什么直接测生产实现：分配逻辑（连续/合并/generation/回收）与 GPU 无关，
//       D3D12DescriptorAllocator 是纯 CPU 类型；"模型与实现逐分支等价"的最强
//       形式就是同一份实现——不再另写一份会漂移的模型。
// 句柄换算与 descriptor 拷贝由 D3D12DescriptorHeapDeviceTests 覆盖。
// 关联：docs/architecture/README.md（测试清单）
// ============================================================================
#include "D3D12DescriptorAllocator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

using MiniEngine::Rhi::D3D12::DescriptorRange;
using MiniEngine::Rhi::D3D12::DescriptorRangeAllocator;

namespace
{
// 两个 SRV table 的大小来自 D3D12RootBindings（t0—t4 / t5—t8）。
constexpr std::uint32_t kMaterialTable = 5U;
constexpr std::uint32_t kGlobalTable = 4U;
} // namespace

// 05 篇「本步验证」第 1 条：material table 5 / global table 4 必须**连续**。
TEST(DescriptorAllocatorTests, AllocatesContiguousMaterialAndGlobalTables)
{
    DescriptorRangeAllocator allocator{32U};
    const DescriptorRange material = allocator.Allocate(kMaterialTable);
    const DescriptorRange global = allocator.Allocate(kGlobalTable);

    EXPECT_EQ(material.base, 0U);
    EXPECT_EQ(material.count, kMaterialTable);
    EXPECT_EQ(global.base, kMaterialTable) << "global 表必须紧邻 material 表（两者都是连续区间）";
    EXPECT_EQ(global.count, kGlobalTable);
    EXPECT_NE(material.generation, global.generation) << "每次分配都必须产生新的 generation";
}

// 归还相邻区间后必须合并，否则"整段空闲"会被切成碎片而分配失败。
TEST(DescriptorAllocatorTests, CoalescesAdjacentFreeRanges)
{
    DescriptorRangeAllocator allocator{16U};
    const DescriptorRange first = allocator.Allocate(4U);
    const DescriptorRange second = allocator.Allocate(4U);

    // 容量 16 下分配两段各 4：布局是 [0,4) + [4,8) 已用、[8,16) 空闲。
    // 只归还第一段后，空闲块变为 [0,4) 与 [8,16)：最大空闲块是 **8**（尾块），
    // 两者不相邻因此不合并——这里刻意把算术写清楚，避免"以为合并了"的误判。
    allocator.Free(first);
    EXPECT_EQ(allocator.LargestFreeBlock(), 8U) << "归还第一段后尾块（8）才是最大空闲块";
    EXPECT_EQ(allocator.FreeBlockCount(), 2U) << "不相邻的空闲块不得合并";

    allocator.Free(second);
    EXPECT_EQ(allocator.LargestFreeBlock(), 16U) << "两段相邻归还后必须合并回整段";
    EXPECT_EQ(allocator.FreeBlockCount(), 1U);

    const DescriptorRange whole = allocator.Allocate(16U);
    EXPECT_EQ(whole.base, 0U);
}

// 容量边界：分配刚好放得下时成功，再要一个即失败（bad_alloc），且失败不破坏记账。
TEST(DescriptorAllocatorTests, EnforcesCapacityBoundaryWithoutCorruptingState)
{
    DescriptorRangeAllocator allocator{8U};
    const DescriptorRange whole = allocator.Allocate(8U);
    EXPECT_EQ(whole.base, 0U);

    EXPECT_THROW(static_cast<void>(allocator.Allocate(1U)), std::bad_alloc) << "无连续空间必须失败";
    EXPECT_EQ(allocator.ActiveRangeCount(), 1U) << "失败不得改变记账";
    EXPECT_EQ(allocator.FreeBlockCount(), 0U);

    allocator.Free(whole);
    EXPECT_EQ(allocator.LargestFreeBlock(), 8U);
}

// generation 与重复释放：同一 range 释放两次必须立即失败（第二次已是过期句柄）。
TEST(DescriptorAllocatorTests, RejectsDoubleFreeAndGenerationMismatch)
{
    DescriptorRangeAllocator allocator{8U};
    const DescriptorRange range = allocator.Allocate(4U);
    allocator.Free(range);
    EXPECT_THROW(allocator.Free(range), std::logic_error) << "double free 必须失败";

    // 重新分配同一个 base 会得到新 generation：旧 range 的任何操作都必须失败。
    const DescriptorRange reused = allocator.Allocate(4U);
    EXPECT_EQ(reused.base, range.base);
    EXPECT_NE(reused.generation, range.generation);
    EXPECT_THROW(allocator.Free(range), std::logic_error) << "过期 generation 必须失败";
    EXPECT_THROW(allocator.Retire(range, 3U), std::logic_error);

    // 部分匹配（改了 count）同样必须失败：range 是整体句柄，不允许改写成员。
    const DescriptorRange partial{reused.base, reused.count - 1U, reused.generation};
    EXPECT_THROW(allocator.Free(partial), std::logic_error);
}

// 延迟回收（05 篇核心契约）：retire 之后必须 completed >= retireFence 才可复用。
TEST(DescriptorAllocatorTests, DefersReuseUntilFenceCompletes)
{
    DescriptorRangeAllocator allocator{8U};
    const DescriptorRange range = allocator.Allocate(8U);
    allocator.Retire(range, 5U);

    // 已 retire 但未完成：既不能复用（容量耗尽），也不能再次 retire（已不在 active）。
    EXPECT_THROW(static_cast<void>(allocator.Allocate(1U)), std::bad_alloc);
    EXPECT_THROW(allocator.Retire(range, 6U), std::logic_error);

    allocator.Reclaim(4U);
    EXPECT_THROW(static_cast<void>(allocator.Allocate(1U)), std::bad_alloc) << "completed < retireFence 时不得回收";
    EXPECT_EQ(allocator.RetiredRangeCount(), 1U);

    allocator.Reclaim(5U);
    EXPECT_EQ(allocator.RetiredRangeCount(), 0U);
    EXPECT_EQ(allocator.Allocate(8U).base, 0U) << "completed >= retireFence 后区间必须可复用";
}

// retire fence 的契约：非零、单调；并且 retire 的 range 必须落在容量内。
TEST(DescriptorAllocatorTests, RetireFenceMustBeNonZeroAndMonotonic)
{
    DescriptorRangeAllocator allocator{16U};
    const DescriptorRange first = allocator.Allocate(4U);
    const DescriptorRange second = allocator.Allocate(4U);

    EXPECT_THROW(allocator.Retire(first, 0U), std::invalid_argument) << "fence 0 是「从未提交」的保留值";

    allocator.Retire(first, 5U);
    EXPECT_THROW(allocator.Retire(second, 4U), std::logic_error) << "retire fence 回退必须失败";
    EXPECT_NO_THROW(allocator.Retire(second, 5U)) << "同一提交内 retire 多个 range 合法（同值）";

    // 越界 range：base+count 超出容量必须失败（防止记账被伪造的 range 破坏）。
    // 该校验先于 generation 校验，因此这里的 generation 取值无关紧要。
    const DescriptorRange beyond{12U, 8U, 1U};
    EXPECT_THROW(allocator.Retire(beyond, 6U), std::out_of_range);
}

// 容量为 0 的分配器非法：SAMPLER 容量 0 的语义是"不创建 heap"，不是"创建空 heap"。
TEST(DescriptorAllocatorTests, RejectsZeroCapacityConstruction)
{
    EXPECT_THROW(DescriptorRangeAllocator{0U}, std::invalid_argument);
}

// 零长度分配非法（0 长度的 range 不是合法句柄）。
TEST(DescriptorAllocatorTests, RejectsZeroLengthAllocation)
{
    DescriptorRangeAllocator allocator{4U};
    EXPECT_THROW(static_cast<void>(allocator.Allocate(0U)), std::invalid_argument);
}
