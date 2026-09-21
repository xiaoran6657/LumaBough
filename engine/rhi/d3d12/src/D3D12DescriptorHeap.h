// ============================================================================
// D3D12DescriptorHeap.h — GPU descriptor heap + 句柄换算（分配逻辑委托给分配器）
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 1 条）
// 职责：创建四类 heap 之一并记录实际 increment，按 05 篇冻结的 profile 记录容量；
//       把槽位分配/回收委托给 D3D12DescriptorAllocator（纯 CPU 记账），自身只做
//       CPU/GPU 句柄换算与边界校验。所有 index 走 typed handle，业务代码不手算字节。
// 四类 heap profile（05 篇「四类 Heap」）：CBV_SRV_UAV 16384（shader visible）、
//       SAMPLER 0（M5 全部 static sampler，不创建该 heap）、RTV 256、DSV 32。
// 内部性说明：本类型直接暴露 ID3D12DescriptorHeap/D3D12_CPU_DESCRIPTOR_HANDLE，
//       按仓库边界约定留在 src/（后端内部），不进公共头。
// 关联：docs/architecture/README.md
//       engine/rhi/d3d12/src/D3D12DescriptorAllocator.h（分配规则与失败语义）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

#include "D3D12DescriptorAllocator.h"

namespace MiniEngine::Rhi::D3D12
{
// 05 篇四类 heap 的初始 profile（容量是初始档位，不是无限保证）。
inline constexpr std::uint32_t kCbvSrvUavHeapCapacity = 16384U;
inline constexpr std::uint32_t kSamplerHeapCapacity = 0U; // M5 只用 static sampler
inline constexpr std::uint32_t kRtvHeapCapacity = 256U;
inline constexpr std::uint32_t kDsvHeapCapacity = 32U;

class D3D12DescriptorHeap final
{
  public:
    D3D12DescriptorHeap() = default;
    ~D3D12DescriptorHeap() = default;
    D3D12DescriptorHeap(const D3D12DescriptorHeap&) = delete;
    D3D12DescriptorHeap& operator=(const D3D12DescriptorHeap&) = delete;

    // 创建 heap、记录 increment 并建立容量为 capacity 的分配器。
    //
    // 参数：
    //   device        —— 创建 heap 与查询 increment
    //   type          —— 四类之一；容量 0 表示"不创建"（SAMPLER 的用法）
    //   capacity      —— 槽位数；0 抛 std::invalid_argument
    //   shaderVisible —— M5 只有 CBV_SRV_UAV 为 true
    //   debugName     —— 稳定名称（PIX/live-object 可读）
    // 失败：capacity 为 0 → std::invalid_argument；重复 Initialize → std::logic_error；
    //   HRESULT 失败 → HResultError。
    void Initialize(ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity, bool shaderVisible,
                    const wchar_t* debugName);

    // 以下四个入口直接委托分配器（失败语义见 D3D12DescriptorAllocator.h）。
    [[nodiscard]] DescriptorRange Allocate(std::uint32_t count);
    void Free(DescriptorRange range);
    void Retire(DescriptorRange range, std::uint64_t fenceValue);
    void Reclaim(std::uint64_t completedFenceValue);

    // CPU/GPU 句柄：index 必须 < capacity；GPU 句柄还要求 shaderVisible
    // （非 shader-visible heap 没有 GPU 句柄，抛 std::out_of_range）。
    // 异常类型说明：传错 heap 类型与索引越界同属"调用方参数错误"，统一用 out_of_range，
    // 调用方一条 catch 即可覆盖；若将来出现"按类型分流"的真实需求，再引入独立错误类型
    // （裁定见 docs/architecture/DECISIONS.md §7）。
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t index) const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t index) const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Cpu(const DescriptorRange& range) const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Gpu(const DescriptorRange& range) const;

    [[nodiscard]] ID3D12DescriptorHeap& Native() const noexcept;

    // 取证字段：容量、increment、是否 shader-visible 与分配器统计。
    [[nodiscard]] std::uint32_t Capacity() const noexcept;
    [[nodiscard]] std::uint32_t UsedCount() const noexcept;
    [[nodiscard]] std::uint32_t HighWaterCount() const noexcept
    {
        return m_highWaterCount;
    }
    [[nodiscard]] std::uint64_t AllocationCount() const noexcept
    {
        return m_allocationCount;
    }
    [[nodiscard]] std::uint32_t Increment() const noexcept;
    [[nodiscard]] bool ShaderVisible() const noexcept;
    [[nodiscard]] std::size_t ActiveRangeCount() const noexcept;
    [[nodiscard]] std::size_t FreeBlockCount() const noexcept;
    [[nodiscard]] std::size_t RetiredRangeCount() const noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;

  private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    std::unique_ptr<DescriptorRangeAllocator> m_allocator;
    std::uint32_t m_increment = 0;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_highWaterCount = 0;
    std::uint64_t m_allocationCount = 0;
    bool m_shaderVisible = false;
};
} // namespace MiniEngine::Rhi::D3D12
