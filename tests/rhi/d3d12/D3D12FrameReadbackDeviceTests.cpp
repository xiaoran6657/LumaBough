// ============================================================================
// D3D12FrameReadbackDeviceTests.cpp — 真 GPU 上的 footprint、fence 与 timestamp 回读契约
// 里程碑：M5（09 篇输出一致性；08 篇 GPU timestamp）
// 职责：用真实 D3D12 设备验证非 256 字节有效行、截图在途 fence、逐行 RGBA8
//       打包，以及只 resolve 成对 timestamp、Poll 单次消费和 frameNumber 传递。
//       测试只新增本文件，不改变生产接线或 CMake 目标声明。
// 关联：engine/rhi/d3d12/src/D3D12FrameReadback.h
//       docs/architecture/README.md
//       docs/architecture/DECISIONS.md
// ============================================================================
#include "D3D12FrameReadback.h"
#include "D3D12Queue.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12FrameContext;
using MiniEngine::Rhi::D3D12::D3D12FrameReadback;
using MiniEngine::Rhi::D3D12::D3D12Queue;

namespace
{
struct ReadbackHarness final
{
    std::unique_ptr<D3D12Device> device;
    D3D12Queue queue;
    D3D12FrameReadback readback;

    ReadbackHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = D3D12Device::Create(options);
        auto* const nativeDevice = static_cast<ID3D12Device*>(device->NativeDeviceHandle());
        queue.Initialize(*nativeDevice);
        readback.Initialize(*nativeDevice, queue.NativeQueue());
    }

    ~ReadbackHarness()
    {
        queue.FlushGpu("frame-readback-test-teardown");
    }

    [[nodiscard]] ID3D12Device* NativeDevice() const noexcept
    {
        return static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    }
};

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateRgbaTexture(ID3D12Device& device, const UINT width,
                                                                       const UINT height)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    const HRESULT result =
        device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    EXPECT_TRUE(SUCCEEDED(result));
    return texture;
}
} // namespace

// width=17 产生 68 字节有效行，而 D3D12 footprint 行距必须独立保存为硬件返回值；
// 测试同时验证 fence 未完成时不 Map，完成后逐行结果与上传的 RGBA8 像素一致。
TEST(D3D12FrameReadbackDeviceTests, ScreenshotUsesFootprintAndMapsOnlyAfterCompletedFence)
{
    ReadbackHarness harness;
    constexpr UINT kWidth = 17U;
    constexpr UINT kHeight = 3U;
    constexpr std::size_t kRowBytes = kWidth * 4U;

    const Microsoft::WRL::ComPtr<ID3D12Resource> texture = CreateRgbaTexture(*harness.NativeDevice(), kWidth, kHeight);
    ASSERT_NE(texture.Get(), nullptr);

    const D3D12_RESOURCE_DESC textureDescription = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    harness.NativeDevice()->GetCopyableFootprints(&textureDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize,
                                                  &totalBytes);
    ASSERT_EQ(rowCount, kHeight);
    ASSERT_EQ(rowSize, kRowBytes);
    ASSERT_GT(footprint.Footprint.RowPitch, rowSize);
    ASSERT_EQ(footprint.Footprint.RowPitch, 256U) << "有效行不是 256 的整倍数，必须依赖 footprint 行距";

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDescription{};
    uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDescription.Width = totalBytes;
    uploadDescription.Height = 1U;
    uploadDescription.DepthOrArraySize = 1U;
    uploadDescription.MipLevels = 1U;
    uploadDescription.SampleDesc.Count = 1U;
    uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    ASSERT_TRUE(SUCCEEDED(harness.NativeDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&upload))));

    std::vector<std::uint8_t> expected(static_cast<std::size_t>(kWidth) * kHeight * 4U);
    std::uint8_t* mappedUpload = nullptr;
    ASSERT_TRUE(SUCCEEDED(upload->Map(0U, nullptr, reinterpret_cast<void**>(&mappedUpload))));
    for (UINT row = 0U; row < kHeight; ++row)
    {
        std::uint8_t* const sourceRow = mappedUpload + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
        for (UINT column = 0U; column < kWidth; ++column)
        {
            const std::size_t pixelOffset = (static_cast<std::size_t>(row) * kWidth + column) * 4U;
            expected[pixelOffset + 0U] = static_cast<std::uint8_t>(10U + column);
            expected[pixelOffset + 1U] = static_cast<std::uint8_t>(20U + row);
            expected[pixelOffset + 2U] = static_cast<std::uint8_t>(30U + row + column);
            expected[pixelOffset + 3U] = 255U;
            std::copy_n(expected.data() + pixelOffset, 4U, sourceRow + column * 4U);
        }
    }
    upload->Unmap(0U, nullptr);

    D3D12FrameReadback& readback = harness.readback;
    readback.BeginFrame(0U, 0U, 300U);
    D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
    ID3D12GraphicsCommandList& list = harness.queue.CommandList();

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0U;
    list.CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list.ResourceBarrier(1U, &barrier);

    readback.RecordScreenshot(*harness.NativeDevice(), list, *texture.Get(), kWidth, kHeight);
    EXPECT_FALSE(readback.TryReadScreenshot(0U).has_value()) << "fence=0 时不得 Map";
    const std::uint64_t fence = harness.queue.ExecuteAndSignal(frame);
    readback.CommitScreenshot(fence);
    EXPECT_FALSE(readback.TryReadScreenshot(0U).has_value()) << "completedFence 未达到提交 fence 时不得 Map";

    // 三次真实提交后仍不消费：第三张占满 ring，第四张必须被拒绝。
    for (std::uint32_t slot = 1U; slot < 3U; ++slot)
    {
        auto& nextFrame = harness.queue.BeginFrame(slot);
        readback.RecordScreenshot(*harness.NativeDevice(), harness.queue.CommandList(), *texture.Get(), kWidth,
                                  kHeight);
        const auto nextFence = harness.queue.ExecuteAndSignal(nextFrame);
        readback.CommitScreenshot(nextFence);
    }
    EXPECT_FALSE(readback.CanRecordScreenshot());
    EXPECT_THROW(readback.RecordScreenshot(*harness.NativeDevice(), list, *texture.Get(), kWidth, kHeight),
                 std::logic_error);
    EXPECT_FALSE(readback.TryReadScreenshot(0U).has_value());
    harness.queue.FlushGpu("frame-readback-screenshot");
    for (std::uint32_t slot = 0U; slot < 3U; ++slot)
    {
        const auto image = readback.TryReadScreenshot(harness.queue.CompletedValue());
        ASSERT_TRUE(image.has_value());
        EXPECT_EQ(image->width, kWidth);
        EXPECT_EQ(image->height, kHeight);
        EXPECT_EQ(image->pixels, expected);
    }
    EXPECT_FALSE(readback.IsScreenshotPending());
    EXPECT_TRUE(readback.CanRecordScreenshot());
    // 回绕后复用首槽，验证旧 fence/footprint 不会阻止新截图。
    auto& wrappedFrame = harness.queue.BeginFrame(0U);
    readback.RecordScreenshot(*harness.NativeDevice(), harness.queue.CommandList(), *texture.Get(), kWidth, kHeight);
    readback.CommitScreenshot(harness.queue.ExecuteAndSignal(wrappedFrame));
    harness.queue.FlushGpu("frame-readback-screenshot-wrap");
    const auto wrappedImage = readback.TryReadScreenshot(harness.queue.CompletedValue());
    ASSERT_TRUE(wrappedImage.has_value());
    EXPECT_EQ(wrappedImage->pixels, expected);
    EXPECT_FALSE(readback.IsScreenshotPending());
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 只执行 pass 0 的成对 query；Resolve 不应覆盖未执行的四个槽位。
// Poll 在 fence 未完成时保留槽位，完成后消费一次，并把提交时的 frameNumber 原样带出。
TEST(D3D12FrameReadbackDeviceTests, ResolvesOnlyPairedQueriesAndPollsOnceWithFrameNumber)
{
    ReadbackHarness harness;
    harness.readback.BeginFrame(0U, 0U, 777U);
    D3D12FrameContext& frame = harness.queue.BeginFrame(0U);
    ID3D12GraphicsCommandList& list = harness.queue.CommandList();

    harness.readback.BeginPass(list, 0U, 0U);
    harness.readback.EndPass(list, 0U, 0U);
    harness.readback.Resolve(list, 0U);

    const std::uint64_t fence = harness.queue.ExecuteAndSignal(frame);
    harness.readback.Commit(0U, fence);

    harness.readback.Poll(0U);
    EXPECT_TRUE(harness.readback.TakeTimings().empty()) << "completedFence 未达到时不得消费 timestamp";

    harness.queue.FlushGpu("frame-readback-timestamp");
    const std::uint64_t completed = harness.queue.CompletedValue();
    harness.readback.Poll(completed);

    const std::vector<MiniEngine::Rhi::D3D12::D3D12FrameTiming> timings = harness.readback.TakeTimings();
    ASSERT_EQ(timings.size(), 1U);
    EXPECT_EQ(timings[0].frameIndex, 0U);
    EXPECT_EQ(timings[0].frameNumber, 777U);
    EXPECT_TRUE(timings[0].valid[0]);
    for (std::size_t pass = 1U; pass < timings[0].valid.size(); ++pass)
    {
        EXPECT_FALSE(timings[0].valid[pass]) << "未执行的 pass 不应产生 timestamp";
    }

    harness.readback.Poll(completed);
    EXPECT_TRUE(harness.readback.TakeTimings().empty()) << "同一槽位被 Poll 消费后不得重复返回";
}
