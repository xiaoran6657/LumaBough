// ============================================================================
// D3D12UploadRing.h — 持久映射的 UPLOAD 缓冲（CPU 写入 → GPU 读取的时间差载体）
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 1 条）
// 职责：创建一个 committed 的 UPLOAD heap buffer，**创建后立即 Map 一次、直到
//       shutdown 才 Unmap**（持久映射），保存 CPU base 与 GPUVA base，并把区间记账
//       委托给 UploadRingAllocator（纯 CPU，可单独穷举）。每次分配返回
//       {cpu 指针, gpu 地址, offset, size}，调用方直接把数据写进 cpu 指针。
// 容量：默认 32 MiB，可由启动参数缩小做压力测试（`--upload-ring`）。
// 内部性说明：后端内部类型（src/），直接暴露 ID3D12Resource 与 GPUVA。
// 关联：docs/architecture/README.md（Upload Ring baseline / 对齐）
//       engine/rhi/d3d12/src/D3D12UploadRingAllocator.h（区间与回收规则）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string_view>

#include "D3D12UploadRingAllocator.h"

namespace MiniEngine::Rhi::D3D12
{
// 一次上传分配：cpu 指针用于写入，gpu 地址用于 CopyBufferRegion/CopyTextureRegion。
struct UploadAllocation final
{
    std::byte* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    // 承载这些字节的**资源**：ring 路径 = ring 的 UPLOAD buffer，dedicated 路径 =
    // 该次的 staging 资源（存活到 CommitFrame 的 fence 完成）。
    // 为什么需要它（09 篇第 2 步补）：纹理上传必须用 `CopyTextureRegion`，而 placed
    // footprint 的 Offset 是**缓冲内字节偏移**、不是 GPUVA——调用方必须能拿到资源本身。
    // 缓冲上传用不上（CopyBufferRegion 可以直接用 gpu 地址），故此前没有这个字段。
    ID3D12Resource* source = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return cpu != nullptr && size != 0U;
    }
};

class D3D12UploadRing final
{
  public:
    D3D12UploadRing() = default;
    ~D3D12UploadRing();
    D3D12UploadRing(const D3D12UploadRing&) = delete;
    D3D12UploadRing& operator=(const D3D12UploadRing&) = delete;

    // 创建 UPLOAD buffer 并持久 Map（readRange = {0,0} 表示整段只写）。
    //
    // 失败：capacity == 0 抛 std::invalid_argument；重复 Initialize 抛 std::logic_error；
    //   HRESULT 失败抛 HResultError 并保留未映射状态。
    void Initialize(ID3D12Device& device, std::uint64_t capacity);

    // 分配（失败语义见 UploadRingAllocator：无安全 span 时返回空 allocation）。
    [[nodiscard]] UploadAllocation TryAllocate(std::uint64_t size, std::uint64_t alignment);
    // 便捷入口：常量缓冲的 256 字节对齐档位。
    [[nodiscard]] UploadAllocation TryAllocateConstant(std::uint64_t size);

    void CommitFrame(std::uint64_t fenceValue);
    void Reclaim(std::uint64_t completedFenceValue);

    // 解除映射并释放资源。要求 IsIdle()（无 current 也无 pending）——
    // 否则抛 std::logic_error：shutdown 前必须先 FlushGpu 并 Reclaim。
    void Shutdown(std::string_view reason);

    // 底层上传缓冲资源：CopyTextureRegion/CopyBufferRegion 的源必须以**资源**形式给出
    // （placed footprint 的 Offset 是缓冲内字节偏移，不是 GPUVA）。
    [[nodiscard]] ID3D12Resource& Native() const noexcept;

    [[nodiscard]] const UploadRingAllocator& Allocator() const noexcept;
    [[nodiscard]] std::uint64_t Capacity() const noexcept;
    [[nodiscard]] std::uint64_t HighWaterBytes() const noexcept;
    [[nodiscard]] bool IsIdle() const noexcept;
    [[nodiscard]] bool IsMapped() const noexcept;

  private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    std::optional<UploadRingAllocator> m_allocator;
    std::byte* m_cpuBase = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase = 0;
};
} // namespace MiniEngine::Rhi::D3D12
