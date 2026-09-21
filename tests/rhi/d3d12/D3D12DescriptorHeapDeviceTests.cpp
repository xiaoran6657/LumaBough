// ============================================================================
// D3D12DescriptorHeapDeviceTests.cpp — 真 GPU 上的 heap 创建、句柄换算与写入
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature）
// 职责：验证需要真实 heap 的部分：四类 profile 的容量/increment、CPU/GPU 句柄
//       偏移（typed handle 换算正确）、非 shader-visible heap 没有 GPU 句柄、
//       把真实 SRV 描述符写入分配的连续区间、CopyDescriptorsSimple 的 slot 语义、
//       retire 后区间在 Reclaim 前不可复用。纯分配规则由 DescriptorAllocatorTests
//       在同一份生产实现上穷举。
// 环境：需要 D3D12 硬件或 WARP（与仓库既有设备级测试同口径）。
// 关联：docs/architecture/README.md（阶段验收）
// ============================================================================
#include "D3D12DescriptorHeap.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <MiniEngine/Rhi/D3D12/D3D12RootBindings.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>

using MiniEngine::Rhi::D3D12::D3D12DescriptorHeap;
using MiniEngine::Rhi::D3D12::DescriptorRange;

namespace
{
struct HeapHarness final
{
    std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device;

    HeapHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = MiniEngine::Rhi::D3D12::D3D12Device::Create(options);
    }

    // 创建一个 1×1 的 committed 纹理，仅作为"合法 SRV 描述符来源"（本篇不做
    // 上传与绘制：05 篇只验证 descriptor 拥有权，纹理内容属 06/09 篇）。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateTinyTexture(ID3D12Device& nativeDevice) const
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = 1U;
        description.Height = 1U;
        description.DepthOrArraySize = 1U;
        description.MipLevels = 1U;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1U;

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        const HRESULT result =
            nativeDevice.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture));
        EXPECT_TRUE(SUCCEEDED(result)) << "create committed texture as descriptor source";
        return texture;
    }

    [[nodiscard]] ID3D12Device* NativeDevice() const
    {
        return static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    }
};
} // namespace

// 四类 heap profile：RTV/DSV/CBV_SRV_UAV 可创建并记录 increment；SAMPLER 容量 0。
TEST(D3D12DescriptorHeapDeviceTests, CreatesM5HeapProfilesAndRecordsIncrements)
{
    HeapHarness harness;
    auto* const device = static_cast<ID3D12Device*>(harness.device->NativeDeviceHandle());

    D3D12DescriptorHeap shaderVisible;
    shaderVisible.Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                             MiniEngine::Rhi::D3D12::kCbvSrvUavHeapCapacity, true, L"M5.Test.CbvSrvUavHeap");
    EXPECT_EQ(shaderVisible.Capacity(), 16384U);
    EXPECT_TRUE(shaderVisible.ShaderVisible());
    EXPECT_GT(shaderVisible.Increment(), 0U) << "创建时必须记录 descriptor increment";

    D3D12DescriptorHeap rtvHeap;
    rtvHeap.Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MiniEngine::Rhi::D3D12::kRtvHeapCapacity, false,
                       L"M5.Test.RtvHeap");
    EXPECT_EQ(rtvHeap.Capacity(), 256U);
    EXPECT_FALSE(rtvHeap.ShaderVisible());

    D3D12DescriptorHeap dsvHeap;
    dsvHeap.Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MiniEngine::Rhi::D3D12::kDsvHeapCapacity, false,
                       L"M5.Test.DsvHeap");
    EXPECT_EQ(dsvHeap.Capacity(), 32U);

    EXPECT_EQ(MiniEngine::Rhi::D3D12::kSamplerHeapCapacity, 0U)
        << "SAMPLER 容量 0 = M5 不创建 sampler heap（全部 static sampler）";

    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "创建 heap 不得产生调试层消息";
}

// CPU/GPU 句柄必须按记录的 increment 线性偏移——业务代码不手算字节全靠它。
TEST(D3D12DescriptorHeapDeviceTests, HandlesUseRecordedIncrement)
{
    HeapHarness harness;
    auto* const device = static_cast<ID3D12Device*>(harness.device->NativeDeviceHandle());

    D3D12DescriptorHeap heap;
    heap.Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64U, true, L"M5.Test.Handles");

    const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = heap.Cpu(0U);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = heap.Gpu(0U);
    for (std::uint32_t index = 0; index < 8U; ++index)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap.Cpu(index);
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap.Gpu(index);
        EXPECT_EQ(cpu.ptr, cpu0.ptr + static_cast<SIZE_T>(index) * heap.Increment());
        EXPECT_EQ(gpu.ptr, gpu0.ptr + static_cast<UINT64>(index) * heap.Increment());
    }

    // 区间起点句柄 == 该 base 的单槽句柄（root table 绑定用的就是它）。
    const DescriptorRange range{5U, 5U, 1U};
    EXPECT_EQ(heap.Cpu(range).ptr, heap.Cpu(5U).ptr);
    EXPECT_EQ(heap.Gpu(range).ptr, heap.Gpu(5U).ptr);

    // 越界索引必须显式失败。
    EXPECT_THROW(static_cast<void>(heap.Cpu(64U)), std::out_of_range);
    EXPECT_THROW(static_cast<void>(heap.Gpu(64U)), std::out_of_range);
}

// 非 shader-visible heap 没有 GPU 句柄：必须显式失败而不是返回无意义的值。
TEST(D3D12DescriptorHeapDeviceTests, NonShaderVisibleHeapRejectsGpuHandles)
{
    HeapHarness harness;
    auto* const device = static_cast<ID3D12Device*>(harness.device->NativeDeviceHandle());

    D3D12DescriptorHeap rtvHeap;
    rtvHeap.Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8U, false, L"M5.Test.RtvHandles");

    EXPECT_THROW(static_cast<void>(rtvHeap.Gpu(0U)), std::out_of_range);
    EXPECT_NO_THROW(static_cast<void>(rtvHeap.Cpu(0U)));
}

// 把真实 SRV 描述符写进连续区间，并用 CopyDescriptorsSimple 从 staging heap 复制到
// shader-visible heap：这是"material table 5 个连续槽位 + 热重载 copy-on-write"的最小
// 可执行证明（05 篇「descriptor 创建」/「分配与热重载」）。
//
// 一条真实约束（本条用例第一次运行时被调试层拦下，故记录在此）：**复制源不能是
// shader-visible heap**——调试层报 "SrcDescriptorRangeStart points to a descriptor
// heap type that is CPU write only, so reading it (in this case a copy source) is
// invalid"。因此热重载路径必须是"在非 shader-visible 的 staging heap 里生成描述符 →
// 整段复制进 shader-visible heap"，不能反过来。
TEST(D3D12DescriptorHeapDeviceTests, CopiesDescriptorsFromStagingIntoShaderVisibleHeap)
{
    HeapHarness harness;
    ID3D12Device& device = *harness.NativeDevice();

    D3D12DescriptorHeap shaderVisible;
    shaderVisible.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16U, true, L"M5.Test.SrvTable");
    D3D12DescriptorHeap staging;
    staging.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16U, false, L"M5.Test.SrvStaging");

    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = harness.CreateTinyTexture(device);
    ASSERT_NE(texture.Get(), nullptr);

    const DescriptorRange materialTable = shaderVisible.Allocate(MiniEngine::Rhi::D3D12::kMaterialSrvCount);
    const DescriptorRange copyTarget = shaderVisible.Allocate(MiniEngine::Rhi::D3D12::kMaterialSrvCount);
    ASSERT_EQ(materialTable.count, 5U);
    ASSERT_EQ(copyTarget.base, 5U) << "第二次 5 槽分配必须紧接第一段（连续）";
    const DescriptorRange stagingRange = staging.Allocate(MiniEngine::Rhi::D3D12::kMaterialSrvCount);

    // 每个槽位写一个合法 SRV：Cpu(index) 必须落在本段内的连续位置。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDescription.Texture2D.MipLevels = 1U;
    for (std::uint32_t slot = 0; slot < stagingRange.count; ++slot)
    {
        device.CreateShaderResourceView(texture.Get(), &srvDescription, staging.Cpu(stagingRange.base + slot));
    }

    // staging（源，非 shader-visible）→ shader-visible（目标）：两个字面量都是 typed range 起点。
    device.CopyDescriptorsSimple(stagingRange.count, shaderVisible.Cpu(copyTarget), staging.Cpu(stagingRange),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    EXPECT_EQ(shaderVisible.ActiveRangeCount(), 2U);
    EXPECT_EQ(staging.ActiveRangeCount(), 1U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "写/复制描述符不得产生调试层消息";
}

// retire 后区间在 Reclaim 之前不可复用；Reclaim(completed) 之后可再次分配。
TEST(D3D12DescriptorHeapDeviceTests, RetiredRangeIsReusableOnlyAfterReclaim)
{
    HeapHarness harness;

    D3D12DescriptorHeap heap;
    heap.Initialize(*harness.NativeDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8U, true, L"M5.Test.Retire");

    const DescriptorRange range = heap.Allocate(8U);
    heap.Retire(range, 12U);
    EXPECT_THROW(static_cast<void>(heap.Allocate(1U)), std::bad_alloc) << "retire 后不得立即复用";

    heap.Reclaim(11U);
    EXPECT_THROW(static_cast<void>(heap.Allocate(1U)), std::bad_alloc) << "completed < retireFence 仍不得复用";

    heap.Reclaim(12U);
    EXPECT_EQ(heap.Allocate(8U).base, 0U) << "completed >= retireFence 后必须可复用";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}
