// ============================================================================
// D3D12UploadDeviceTests.cpp — 真 GPU 上的上传环、策略层与资源上传
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期）
// 职责：验证需要设备的部分：持久映射与 CPU/GPU 指针换算、Shutdown 的 idle 前置、
//       策略层三条路径（ring / dedicated / 拒绝）、dedicated staging 在 fence 前
//       不释放、"等最老 fence 一次"的 stall 记账，以及**奇数尺寸多 mip 纹理的
//       足迹打包与逐行复制往返**（读回逐字节比对）。
// 纯记账规则（空/填满/回绕/溢出/批量回收）由 UploadRingTests 在同一份实现上穷举。
// 环境：需要 D3D12 硬件或 WARP。
// 关联：docs/architecture/README.md（测试清单）
// ============================================================================
#include "D3D12Queue.h"
#include "D3D12TextureUpload.h"
#include "D3D12UploadManager.h"
#include "D3D12UploadRing.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using MiniEngine::Rhi::D3D12::D3D12Queue;
using MiniEngine::Rhi::D3D12::D3D12UploadManager;
using MiniEngine::Rhi::D3D12::D3D12UploadRing;
using MiniEngine::Rhi::D3D12::UploadAllocation;
using MiniEngine::Rhi::D3D12::UploadRingAllocator;

namespace
{
// 设备 + 队列的公共前置（debug 模式：让"零消息"有实际意义）。
struct UploadHarness final
{
    std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device;
    D3D12Queue queue;

    UploadHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = MiniEngine::Rhi::D3D12::D3D12Device::Create(options);
        queue.Initialize(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()));
    }

    [[nodiscard]] ID3D12Device* NativeDevice() const
    {
        return static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    }
};

// 创建裸缓冲（DEFAULT/READBACK/UPLOAD 之一），用于静态上传与其读回验证。
Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device& device, std::uint64_t bytes, D3D12_HEAP_TYPE heapType,
                                                    D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heapType;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    EXPECT_TRUE(SUCCEEDED(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                         initialState, nullptr, IID_PPV_ARGS(&resource))));
    return resource;
}
} // namespace

// 持久映射与指针换算：cpu = base + offset、gpu 增量 == offset 增量。
TEST(D3D12UploadDeviceTests, RingMapsOnceAndExposesCpuAndGpuPointers)
{
    UploadHarness harness;
    D3D12UploadRing ring;
    ring.Initialize(*harness.NativeDevice(), 64U * 1024U);

    EXPECT_TRUE(ring.IsMapped()) << "创建时即持久映射（shutdown 才 Unmap）";
    EXPECT_EQ(ring.Capacity(), 64U * 1024U);

    const UploadAllocation first = ring.TryAllocateConstant(256U);
    const UploadAllocation second = ring.TryAllocateConstant(256U);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.offset % UploadRingAllocator::kConstantBufferAlignment, 0U);
    EXPECT_EQ(second.gpu - first.gpu, second.offset - first.offset) << "GPUVA 与偏移同步推进";
    EXPECT_EQ(second.cpu - first.cpu, static_cast<std::ptrdiff_t>(second.offset - first.offset));

    // 写入必须落在映射范围内：写满一段（内容正确性由后面的缓冲/纹理往返验证）。
    std::memset(first.cpu, 0xAB, static_cast<std::size_t>(first.size));
}

// Shutdown 前置：有 pending span 时必须拒绝；回收后才允许 Unmap。
TEST(D3D12UploadDeviceTests, RingShutdownRequiresIdleRing)
{
    UploadHarness harness;
    D3D12UploadRing ring;
    ring.Initialize(*harness.NativeDevice(), 4096U);

    const UploadAllocation allocation = ring.TryAllocateConstant(1024U);
    ASSERT_TRUE(allocation);
    ring.CommitFrame(1U);

    EXPECT_THROW(ring.Shutdown("test"), std::logic_error) << "pending span 未回收时不得 Unmap";
    EXPECT_TRUE(ring.IsMapped());

    ring.Reclaim(1U);
    EXPECT_NO_THROW(ring.Shutdown("test"));
    EXPECT_FALSE(ring.IsMapped());
}

// 策略层路径 1/2：大请求走 dedicated，且**在提交该帧的 fence 完成前不释放**。
TEST(D3D12UploadDeviceTests, ManagerUsesDedicatedForLargeRequestsAndRetiresByFence)
{
    UploadHarness harness;
    D3D12UploadRing ring;
    ring.Initialize(*harness.NativeDevice(), 16U * 1024U); // 阈值 = 4 KiB

    D3D12UploadManager manager;
    manager.Initialize(harness.NativeDevice(), ring, [](std::uint64_t) {}, 1024U * 1024U);

    // 8 KiB > ring/4 → dedicated 路径（不占用 ring）。
    const UploadAllocation large = manager.Allocate(8U * 1024U, 16U, "texture");
    ASSERT_TRUE(large);
    EXPECT_EQ(manager.Stats().dedicatedAllocations, 1U);
    EXPECT_EQ(manager.Stats().ringAllocations, 0U);
    EXPECT_EQ(manager.PendingDedicatedCount(), 1U);
    EXPECT_TRUE(manager.DeferredRelease().Empty()) << "尚未提交：还没有归属的 fence";

    manager.CommitFrame(21U);
    EXPECT_EQ(manager.PendingDedicatedCount(), 0U);
    EXPECT_EQ(manager.DeferredRelease().PendingCount(), 1U);

    static_cast<void>(manager.Reclaim(20U));
    EXPECT_EQ(manager.DeferredRelease().PendingCount(), 1U) << "completed < retireFence 不得释放 staging";
    static_cast<void>(manager.Reclaim(21U));
    EXPECT_EQ(manager.DeferredRelease().PendingCount(), 0U);
    EXPECT_EQ(manager.DeferredRelease().ReclaimedCount(), 1U) << "回调确实执行了（staging 已 Unmap/释放）";
}

// 策略层路径 3：ring 无安全 span 时只等**最老 pending fence 一次**，并记录 stall。
TEST(D3D12UploadDeviceTests, ManagerWaitsOldestFenceOnceAndRecordsStall)
{
    UploadHarness harness;
    D3D12UploadRing ring;
    ring.Initialize(*harness.NativeDevice(), 2048U); // 阈值 = 512 字节

    std::vector<std::uint64_t> waitedValues;
    D3D12UploadManager manager;
    manager.Initialize(
        harness.NativeDevice(), ring, [&waitedValues](const std::uint64_t value) { waitedValues.push_back(value); },
        1024U * 1024U);

    // 用 4 段 512 字节把 ring 精确填满（每段都不超过阈值，因此走 ring）。
    for (int index = 0; index < 4; ++index)
    {
        const UploadAllocation allocation = manager.Allocate(512U, 256U, "constants");
        ASSERT_TRUE(allocation) << "第 " << index << " 段";
    }
    manager.CommitFrame(5U);
    EXPECT_EQ(manager.Stats().ringAllocations, 4U);

    // 第五段：ring 已满 → 等最老 fence（5）→ 回收后重试成功。
    const UploadAllocation fifth = manager.Allocate(512U, 256U, "constants");
    ASSERT_TRUE(fifth) << "等最老 fence 回收后必须能继续分配";
    EXPECT_EQ(manager.Stats().ringNoSpanEvents, 1U) << "该计数是「无安全 span 的次数」，不承诺实际阻塞时长";
    ASSERT_EQ(waitedValues.size(), 1U);
    EXPECT_EQ(waitedValues.front(), 5U) << "必须只等最老的那个 fence";
    manager.CommitFrame(6U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 策略层失败语义：超过 dedicated 预算必须显式失败（不静默降级/不无限增大 ring）。
TEST(D3D12UploadDeviceTests, ManagerRejectsRequestsBeyondDedicatedBudget)
{
    UploadHarness harness;
    D3D12UploadRing ring;
    ring.Initialize(*harness.NativeDevice(), 16U * 1024U);

    D3D12UploadManager manager;
    manager.Initialize(harness.NativeDevice(), ring, [](std::uint64_t) {}, 4096U);

    EXPECT_THROW(static_cast<void>(manager.Allocate(8192U, 512U, "oversized")), std::runtime_error);
    EXPECT_EQ(manager.Stats().rejections, 1U);
    EXPECT_EQ(manager.Stats().dedicatedAllocations, 0U);
}

// 静态缓冲上传（06 篇「Static buffer upload」）：ring → CopyBufferRegion →
// barrier 到 VERTEX_AND_CONSTANT_BUFFER，再经 READBACK 读回逐字节比对。
TEST(D3D12UploadDeviceTests, UploadsStaticBufferThroughRingAndReadsBack)
{
    UploadHarness harness;
    ID3D12Device& device = *harness.NativeDevice();

    constexpr std::uint64_t kBufferBytes = 512U;
    D3D12UploadRing ring;
    ring.Initialize(device, 64U * 1024U);
    const UploadAllocation upload = ring.TryAllocate(kBufferBytes, UploadRingAllocator::kBufferCopyAlignment);
    ASSERT_TRUE(upload);
    for (std::uint64_t index = 0; index < kBufferBytes; ++index)
    {
        upload.cpu[index] = static_cast<std::byte>(index & 0xFFU);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer =
        CreateBuffer(device, kBufferBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback =
        CreateBuffer(device, kBufferBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_NE(vertexBuffer.Get(), nullptr);

    {
        MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
        ID3D12GraphicsCommandList& list = harness.queue.CommandList();
        list.CopyBufferRegion(vertexBuffer.Get(), 0U, &ring.Native(), upload.offset, kBufferBytes);
        MiniEngine::Rhi::D3D12::RecordResourceTransition(list, *vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    }
    harness.queue.FlushGpu("buffer-upload-test");

    {
        MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
        ID3D12GraphicsCommandList& list = harness.queue.CommandList();
        MiniEngine::Rhi::D3D12::RecordResourceTransition(list, *vertexBuffer.Get(),
                                                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        list.CopyBufferRegion(readback.Get(), 0U, vertexBuffer.Get(), 0U, kBufferBytes);
        static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    }
    harness.queue.FlushGpu("buffer-readback-test");

    const D3D12_RANGE readRange{0U, static_cast<SIZE_T>(kBufferBytes)};
    void* mapped = nullptr;
    ASSERT_TRUE(SUCCEEDED(readback->Map(0U, &readRange, &mapped)));
    const auto* bytes = static_cast<const std::byte*>(mapped);
    for (std::uint64_t index = 0; index < kBufferBytes; ++index)
    {
        EXPECT_EQ(bytes[index], static_cast<std::byte>(index & 0xFFU)) << "byte " << index;
    }
    readback->Unmap(0U, nullptr);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 奇数尺寸 + 多 mip 的纹理上传（06 篇「Texture upload」）：
// GetCopyableFootprints 计划 → 逐行打包（源 pitch 与目标 RowPitch 分开）→
// CopyTextureRegion → transition 到 PS_RESOURCE → 读回逐行比对。
// 3×5 + 3 mips（3x5 / 1x2 / 1x1）会同时暴露行距、mip 尺寸与 subresource 顺序错误。
TEST(D3D12UploadDeviceTests, UploadsOddSizeMultiMipTextureAndReadsBackRowByRow)
{
    UploadHarness harness;
    ID3D12Device& device = *harness.NativeDevice();

    D3D12_RESOURCE_DESC textureDescription{};
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = 3U;
    textureDescription.Height = 5U;
    textureDescription.DepthOrArraySize = 1U;
    textureDescription.MipLevels = 3U;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDescription.SampleDesc.Count = 1U;

    // 先探足迹总量（offset 0），据此决定 ring 容量；随后用真实偏移重算计划
    // （footprint 的 Offset 依赖基址，重算比手动加偏移更不易错）。
    const MiniEngine::Rhi::D3D12::TextureUploadPlan probe =
        MiniEngine::Rhi::D3D12::PlanTextureUpload(device, textureDescription, 0U);
    ASSERT_EQ(probe.subresourceCount, 3U);
    EXPECT_GT(probe.totalBytes, 0U);

    D3D12UploadRing ring;
    ring.Initialize(device, MiniEngine::Rhi::D3D12::AlignUp(probe.totalBytes, 1024U));
    const UploadAllocation upload = ring.TryAllocate(probe.totalBytes, UploadRingAllocator::kTexturePlacementAlignment);
    ASSERT_TRUE(upload);
    EXPECT_EQ(upload.offset % UploadRingAllocator::kTexturePlacementAlignment, 0U) << "纹理放置必须 512 对齐";

    const MiniEngine::Rhi::D3D12::TextureUploadPlan plan =
        MiniEngine::Rhi::D3D12::PlanTextureUpload(device, textureDescription, upload.offset);
    ASSERT_EQ(plan.totalBytes, probe.totalBytes);

    // 源数据：每个 subresource 用可辨识的行模式（同 subresource 内每行相同，便于逐行比对）。
    std::vector<std::vector<std::byte>> sources(plan.subresourceCount);
    for (std::uint32_t subresource = 0; subresource < plan.subresourceCount; ++subresource)
    {
        const MiniEngine::Rhi::D3D12::TextureSubresourcePlan& entry = plan.subresources[subresource];
        sources[subresource].assign(static_cast<std::size_t>(entry.rowCount * entry.rowSize),
                                    static_cast<std::byte>(0x11U * (subresource + 1U)));
        MiniEngine::Rhi::D3D12::PackTextureRows(
            plan, subresource, upload.cpu, upload.size,
            {sources[subresource].data(), entry.rowSize, static_cast<std::uint64_t>(sources[subresource].size())});
    }

    // 源侧越界必须被拒绝（审查意见 M5-06 P2-2：只校验目标越界会留下越界读）：
    // 故意声明比"实际需要"更小的 byteSize，PackTextureRows 必须抛出而不是照读。
    {
        const MiniEngine::Rhi::D3D12::TextureSubresourcePlan& entry = plan.subresources[0];
        const std::uint64_t requiredBytes = (entry.rowCount - 1U) * entry.rowSize + entry.rowSize;
        EXPECT_THROW(MiniEngine::Rhi::D3D12::PackTextureRows(plan, 0U, upload.cpu, upload.size,
                                                             {sources[0].data(), entry.rowSize, requiredBytes - 1U}),
                     std::out_of_range)
            << "源缓冲比需要的少 1 字节也必须失败";
    }

    // 目标纹理：DEFAULT heap + 初始 COPY_DEST（上传完成后再转 PS_RESOURCE）。
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_TRUE(
            SUCCEEDED(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &textureDescription,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))));
    }
    // 读回缓冲：与纹理同布局的 footprints（用 probe 的 0 基址版本）。
    Microsoft::WRL::ComPtr<ID3D12Resource> readback =
        CreateBuffer(device, probe.totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_NE(readback.Get(), nullptr);

    {
        MiniEngine::Rhi::D3D12::D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
        ID3D12GraphicsCommandList& list = harness.queue.CommandList();
        MiniEngine::Rhi::D3D12::RecordTextureCopies(list, *texture.Get(), plan, ring.Native());
        // 生产路径到此为止（纹理进入 PS_RESOURCE 供渲染采样）；
        // 下面是**测试专用**的读回腿：再转一次 COPY_SOURCE 才能拷回 READBACK 缓冲。
        MiniEngine::Rhi::D3D12::RecordResourceTransition(list, *texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        MiniEngine::Rhi::D3D12::RecordResourceTransition(
            list, *texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        // 纹理 → 读回缓冲（逐 subresource，使用 probe 的 footprint 作为目标）。
        for (std::uint32_t subresource = 0; subresource < probe.subresourceCount; ++subresource)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = probe.subresources[subresource].placement;

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = MiniEngine::Rhi::D3D12::CalcSubresourceIndex(
                subresource % textureDescription.MipLevels, subresource / textureDescription.MipLevels, 0U,
                textureDescription.MipLevels, textureDescription.DepthOrArraySize);
            list.CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
        }
        static_cast<void>(harness.queue.ExecuteAndSignal(frame));
    }
    harness.queue.FlushGpu("texture-upload-test");

    const D3D12_RANGE readRange{0U, static_cast<SIZE_T>(probe.totalBytes)};
    void* mapped = nullptr;
    ASSERT_TRUE(SUCCEEDED(readback->Map(0U, &readRange, &mapped)));
    const auto* bytes = static_cast<const std::byte*>(mapped);
    for (std::uint32_t subresource = 0; subresource < probe.subresourceCount; ++subresource)
    {
        const MiniEngine::Rhi::D3D12::TextureSubresourcePlan& entry = probe.subresources[subresource];
        for (std::uint32_t row = 0; row < entry.rowCount; ++row)
        {
            const std::uint64_t rowOffset =
                entry.placement.Offset + static_cast<std::uint64_t>(row) * entry.placement.Footprint.RowPitch;
            for (std::uint64_t column = 0; column < entry.rowSize; ++column)
            {
                // 只比对有效行字节：RowPitch 内的 padding 没有定义（不猜它是什么）。
                EXPECT_EQ(bytes[rowOffset + column], static_cast<std::byte>(0x11U * (subresource + 1U)))
                    << "subresource " << subresource << " row " << row << " column " << column;
            }
        }
    }
    readback->Unmap(0U, nullptr);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "纹理上传不得产生调试层消息";
}
