// ============================================================================
// D3D12AssetCacheDeviceTests.cpp — 资产 → GPU 资源缓存的设备级契约
// 里程碑：M5（09 篇迁移顺序第 2 步「baked mesh + texture + depth」）
// 职责：在真实 Device + 真实上传链（ring → Copy*Region → tracker transition）上验证：
//   1. revision 驱动上传与 no-op（D3D11AssetCache 同款语义的 D3D12 镜像）；
//   2. **替换路径的资源生命周期**：旧资源不得在帧内立即析构（可能仍被在飞命令列表
//      引用），必须先注销 tracker、本体挂 retired 队列、在 ReleaseAll 安全点释放——
//      M5-08 的 PSO 覆盖路径修过同一型问题，这里用 RetiredResourceCount 钉死；
//   3. 占位 payload（0 顶点/0×0）不建 0 字节资源、stale handle 不碰缓存；
//   4. 纹理完整 mip 链上传 + SRV 可用（sRGB/线性格式选择由实现保证，这里验证
//      "SRV 真的建出来且 revision 驱动可重传"）；
//   5. 全程 Debug Layer 零消息（与生产同口径的零容忍）。
// 语义对照：tests/rhi/d3d11/D3D11AssetCacheTests.cpp（不可共享断言的部分是
//      D3D12 特有的 tracker 注册与 retired 队列）。
// 环境：需要 D3D12 硬件或 WARP。
// 关联：engine/rhi/d3d12/src/D3D12AssetCache.cpp（被测实现）
//       docs/architecture/README.md（迁移顺序第 2 步）
// ============================================================================
#include "D3D12AssetCache.h"
#include "D3D12DescriptorHeap.h"
#include "D3D12Queue.h"
#include "D3D12ResourceStateTracker.h"
#include "D3D12UploadManager.h"
#include "D3D12UploadRing.h"

#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Assets/TextureAsset.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace
{
using namespace MiniEngine::Assets;
using MiniEngine::Rhi::D3D12::AssetUploadContext;
using MiniEngine::Rhi::D3D12::D3D12AssetCache;
using MiniEngine::Rhi::D3D12::D3D12DescriptorHeap;
using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12Queue;
using MiniEngine::Rhi::D3D12::D3D12ResourceStateTracker;
using MiniEngine::Rhi::D3D12::D3D12UploadManager;
using MiniEngine::Rhi::D3D12::D3D12UploadRing;

// 上传缓存需要的全部设备侧前置。成员声明顺序即构造顺序、逆序即析构顺序：
// ring/manager 依赖 device，cache 依赖 staging heap 与 tracker；收尾必须先
// FlushGpu 再 Reclaim，否则 ring 带 pending span 无法 Shutdown。
struct AssetHarness final
{
    std::unique_ptr<D3D12Device> device;
    D3D12Queue queue;
    D3D12UploadRing ring;
    D3D12UploadManager uploadManager;
    D3D12ResourceStateTracker tracker;
    D3D12DescriptorHeap stagingHeap;

    AssetHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true; // 零消息断言必须有调试层才有意义
        device = D3D12Device::Create(options);
        auto* const nativeDevice = static_cast<ID3D12Device*>(device->NativeDeviceHandle());

        queue.Initialize(*nativeDevice);
        ring.Initialize(*nativeDevice, 1024U * 1024U);
        // 等待回调：测试里经 FlushGpu 兜底，不需要定向等待。
        uploadManager.Initialize(nativeDevice, ring, [](std::uint64_t) {}, 4U * 1024U * 1024U);
        // 非 shader-visible 的 staging heap：纹理 SRV 的落脚点（M5-05 约束）。
        stagingHeap.Initialize(*nativeDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256U, false,
                               L"M5.Test.AssetStagingHeap");
    }

    ~AssetHarness()
    {
        queue.FlushGpu("asset-cache-test-teardown");
        uploadManager.Reclaim(queue.CompletedValue());
    }

    [[nodiscard]] ID3D12Device& NativeDevice() const
    {
        return *static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    }

    // 一段已 Begin 的帧：构造时 BeginFrame，析构不动（提交由 RecordUpload 负责）。
    // 注意它是**嵌套类型**且必须声明在使用它的成员模板之前（MSVC 不为成员模板
    // 扩展 complete-class context）。
    struct FrameContextGuard final
    {
        D3D12Queue& queue;
        MiniEngine::Rhi::D3D12::D3D12FrameContext& frame;

        explicit FrameContextGuard(AssetHarness& harness) : queue(harness.queue), frame(queue.BeginFrame(0U))
        {
        }
        FrameContextGuard(const FrameContextGuard&) = delete;
        FrameContextGuard& operator=(const FrameContextGuard&) = delete;
        ~FrameContextGuard() = default;
    };

    // 在一段已 Begin 的帧内执行 upload：caller 回调里完成 Ensure*，随后统一提交。
    // tracker 的生命周期与生产一致：BeginRecording → 录制（Register/Transition/Flush）
    // → Execute → CommitExecuted（M5-07：Transition 只允许发生在录制内）。
    template <typename Fn> void RecordUpload(Fn&& record)
    {
        FrameContextGuard guard{*this};
        tracker.BeginRecording();
        const AssetUploadContext context{NativeDevice(), queue.CommandList(), uploadManager, tracker};
        record(context);
        static_cast<void>(queue.ExecuteAndSignal(guard.frame));
        tracker.CommitExecuted();
        queue.FlushGpu("asset-cache-test-upload");
        uploadManager.Reclaim(queue.CompletedValue());
    }
};

// 一个最小可绘制网格：3 个 PbrVertex 顶点（48B stride，与 .memesh v2 一致）+ 3 个
// uint32 索引。payloadTag 让两次提交的内容可区分（revision bump 用）。
MeshAsset MakePbrTriangleMesh(const std::uint8_t payloadTag)
{
    MeshAsset mesh;
    mesh.vertexStride = sizeof(PbrVertex);
    mesh.vertexCount = 3U;
    mesh.indexStride = 4U;
    mesh.indexCount = 3U;
    for (std::uint32_t vertex = 0U; vertex < 3U; ++vertex)
    {
        PbrVertex v{};
        v.position[0] = static_cast<float>(vertex);
        v.position[1] = static_cast<float>(payloadTag);
        v.position[2] = 0.0F;
        v.normal[0] = 0.0F;
        v.normal[1] = 0.0F;
        v.normal[2] = 1.0F;
        v.tangent[0] = 1.0F;
        v.tangent[1] = 0.0F;
        v.tangent[2] = 0.0F;
        v.tangent[3] = 1.0F;
        v.uv0[0] = 0.0F;
        v.uv0[1] = 0.0F;
        const auto* const bytes = reinterpret_cast<const std::byte*>(&v);
        mesh.vertexData.insert(mesh.vertexData.end(), bytes, bytes + sizeof(PbrVertex));
    }
    for (std::uint32_t index = 0U; index < 3U; ++index)
    {
        const std::uint32_t value = index;
        const auto* const bytes = reinterpret_cast<const std::byte*>(&value);
        mesh.indexData.insert(mesh.indexData.end(), bytes, bytes + sizeof(value));
    }
    return mesh;
}

AssetHandle<MeshAsset> CommitMesh(AssetManager& manager, const std::string& uri, MeshAsset mesh)
{
    const auto handle = manager.Meshes().ResolveOrCreate(DeriveAssetId(uri));
    EXPECT_TRUE(manager.Meshes().Commit(handle, std::move(mesh)));
    return handle;
}

// width×width 的 RGBA8 sRGB 纹理，mipCount 级完整链（内容按 mip 编号填充以便区分）。
TextureAsset MakeTexture(const std::uint32_t width, const std::uint32_t mipCount)
{
    TextureAsset texture;
    texture.width = width;
    texture.height = width;
    texture.usage = TextureUsage::BaseColor;
    texture.pixelFormat = TexturePixelFormat::Rgba8Unorm;
    texture.colorSpace = TextureColorSpace::Srgb;
    texture.mipCount = mipCount;

    std::uint64_t offset = 0U;
    for (std::uint32_t level = 0U; level < mipCount; ++level)
    {
        const std::uint32_t mipWidth = width >> level;
        const std::uint32_t rowPitch = mipWidth * 4U;
        const std::uint32_t byteSize = rowPitch * mipWidth;
        texture.pixels.resize(texture.pixels.size() + byteSize, std::byte{static_cast<unsigned char>(0x10U + level)});
        texture.mips.push_back(TextureMipInfo{offset, rowPitch, byteSize});
        offset += byteSize;
    }
    return texture;
}

AssetHandle<TextureAsset> CommitTexture(AssetManager& manager, const std::string& uri, TextureAsset texture)
{
    const auto handle = manager.Textures().ResolveOrCreate(DeriveAssetId(uri));
    EXPECT_TRUE(manager.Textures().Commit(handle, std::move(texture)));
    return handle;
}
} // namespace

// revision 驱动上传与 no-op：第二次 Ensure 不得重复上传（字节计数不变）。
TEST(D3D12AssetCacheDeviceTests, UploadsMeshOnceAndNoOpsOnSameRevision)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/triangle", MakePbrTriangleMesh(1U));

    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    const auto view = cache.TryGetMeshView(handle);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->indexCount, 3U);
    EXPECT_EQ(view->vertexBuffer.StrideInBytes, sizeof(PbrVertex)) << "stride 必须是 .memesh v2 的 48 字节";
    EXPECT_EQ(view->indexBuffer.Format, DXGI_FORMAT_R32_UINT);
    EXPECT_EQ(cache.UploadedMeshCount(), 1U);
    EXPECT_GT(cache.UploadedVertexBytes(), 0U);
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);

    const std::uint64_t vertexBytes = cache.UploadedVertexBytes();
    const std::uint64_t indexBytes = cache.UploadedIndexBytes();
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    EXPECT_EQ(cache.UploadedVertexBytes(), vertexBytes) << "同 revision 不得重复上传";
    EXPECT_EQ(cache.UploadedIndexBytes(), indexBytes);
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 替换路径的资源生命周期：revision bump 后旧资源**不得立即析构**——先注销 tracker
// （计数回落），本体挂 retired 队列，ReleaseAll（GPU idle 安全点）才真正释放。
TEST(D3D12AssetCacheDeviceTests, RevisionBumpRetiresOldResourcesUntilSafePoint)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);
    const std::size_t baselineTracked = harness.tracker.TrackedResourceCount();

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/bump", MakePbrTriangleMesh(1U));
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    const auto original = cache.TryGetMeshView(handle);
    ASSERT_TRUE(original.has_value());

    // CPU payload 变化 → Ensure 上传"全新"资源并替换旧 entry。
    EXPECT_TRUE(manager.Meshes().Commit(handle, MakePbrTriangleMesh(2U)));
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    const auto reloaded = cache.TryGetMeshView(handle);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_NE(reloaded->vertexBuffer.BufferLocation, original->vertexBuffer.BufferLocation)
        << "revision bump 必须给出新资源，而不是原地复用";
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);

    // 旧 VB/IB：tracker 已注销（计数不增长），本体在 retired 队列里等安全点。
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked + 2U) << "旧 key 必须已注销，新 key 恰好补位";
    EXPECT_EQ(cache.RetiredResourceCount(), 2U) << "旧 VB/IB 不得在帧内析构";

    cache.ReleaseAll();
    EXPECT_EQ(cache.RetiredResourceCount(), 0U) << "安全点必须清空 retired 队列";
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked);
    EXPECT_FALSE(cache.TryGetMeshView(handle).has_value());
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// pending 资源必须先绑定提交 fence；completed=0 或未达到 fence 时不得提前释放。
TEST(D3D12AssetCacheDeviceTests, PendingRetiredResourcesWaitForCommittedFence)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);
    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/pending-fence", MakePbrTriangleMesh(1U));

    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    EXPECT_TRUE(manager.Meshes().Commit(handle, MakePbrTriangleMesh(2U)));
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    ASSERT_EQ(cache.RetiredResourceCount(), 2U);

    cache.BeginFrame(0U);
    EXPECT_EQ(cache.RetiredResourceCount(), 2U);
    cache.CommitFrame(10U);
    cache.BeginFrame(9U);
    EXPECT_EQ(cache.RetiredResourceCount(), 2U);
    cache.BeginFrame(10U);
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 纹理反复替换时，旧 staging descriptor 立即归还，活跃槽位保持有界；资源仍按 fence 延迟回收。
TEST(D3D12AssetCacheDeviceTests, RepeatedTextureReplacementKeepsStagingDescriptorBounded)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);
    AssetManager manager;
    const auto handle = CommitTexture(manager, "textures/test/bounded-staging", MakeTexture(4U, 2U));

    for (std::uint8_t tag = 0U; tag < 6U; ++tag)
    {
        if (tag != 0U)
        {
            TextureAsset replacement = MakeTexture(4U, 2U);
            replacement.pixels[0] = std::byte{static_cast<unsigned char>(0x40U + tag)};
            EXPECT_TRUE(manager.Textures().Commit(handle, std::move(replacement)));
        }
        harness.RecordUpload([&](const AssetUploadContext& context)
                             { EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, handle)); });
        EXPECT_EQ(harness.stagingHeap.ActiveRangeCount(), 1U);
        cache.CommitFrame(static_cast<std::uint64_t>(tag + 1U));
        cache.BeginFrame(static_cast<std::uint64_t>(tag + 1U));
    }

    EXPECT_EQ(harness.stagingHeap.ActiveRangeCount(), 1U);
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 占位 payload（0 顶点）：记录 revision 但不建 0 字节资源，也没有可绘制视图。
TEST(D3D12AssetCacheDeviceTests, EmptyPayloadRecordsRevisionWithoutDrawableView)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/placeholder", MeshAsset{});

    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    EXPECT_FALSE(cache.TryGetMeshView(handle).has_value()) << "占位没有可绘制内容";
    EXPECT_EQ(cache.UploadedMeshCount(), 0U);
    EXPECT_EQ(cache.PlaceholderCount(), 1U);
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U) << "revision 已记录，因此不算 lag";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// stale/非法 handle：返回 false 且不触碰缓存（不建资源、不计 rejection——那是
// "上传抛异常"的计数，不是"句柄无效"的）。
TEST(D3D12AssetCacheDeviceTests, StaleHandleFailsWithoutTouchingCache)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);

    AssetManager manager;
    const AssetHandle<MeshAsset> stale{};

    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_FALSE(cache.EnsureMeshUploaded(context, manager, stale)); });
    EXPECT_EQ(cache.UploadedMeshCount(), 0U);
    EXPECT_EQ(cache.PlaceholderCount(), 0U);
    EXPECT_EQ(cache.RejectedUploadCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 资产被卸载后 PruneStale 淘汰 entry：先注销 tracker、本体挂 retired（同替换路径）。
TEST(D3D12AssetCacheDeviceTests, PruneStaleRemovesUnloadedEntriesAndRetiresResources)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);
    const std::size_t baselineTracked = harness.tracker.TrackedResourceCount();

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/prune", MakePbrTriangleMesh(1U));
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, handle)); });
    EXPECT_TRUE(cache.TryGetMeshView(handle).has_value());

    EXPECT_TRUE(manager.Meshes().Unload(handle));
    cache.PruneStale(manager);
    EXPECT_FALSE(cache.TryGetMeshView(handle).has_value());
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked) << "淘汰必须同时注销 tracker";
    EXPECT_EQ(cache.RetiredResourceCount(), 2U);

    cache.ReleaseAll();
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 纹理：完整 mip 链上传 + SRV 落在 staging heap；同 revision 的第二次 Ensure 是 no-op。
TEST(D3D12AssetCacheDeviceTests, UploadsTextureMipChainAndExposesStableSrv)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);

    AssetManager manager;
    const auto handle = CommitTexture(manager, "textures/test/albedo", MakeTexture(4U, 3U));

    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, handle)); });
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    ASSERT_TRUE(cache.TryGetTextureSrv(handle, srv));
    EXPECT_NE(srv.ptr, 0U);
    EXPECT_EQ(cache.UploadedTextureCount(), 1U);
    EXPECT_EQ(cache.UploadedTextureBytes(), (4U * 4U + 2U * 2U + 1U) * 4U) << "3 级 mip 的级联字节数";

    const std::uint64_t textureBytes = cache.UploadedTextureBytes();
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, handle)); });
    EXPECT_EQ(cache.UploadedTextureBytes(), textureBytes) << "同 revision 不得重复上传";
    EXPECT_EQ(cache.TextureRevisionLagCount(manager), 0U);
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 纹理替换：revision bump 换新资源 + 新 SRV；旧本体挂 retired、tracker 计数不增长。
TEST(D3D12AssetCacheDeviceTests, TextureRevisionBumpRetiresOldResource)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);
    const std::size_t baselineTracked = harness.tracker.TrackedResourceCount();

    AssetManager manager;
    const auto handle = CommitTexture(manager, "textures/test/bump", MakeTexture(4U, 2U));
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, handle)); });
    D3D12_CPU_DESCRIPTOR_HANDLE firstSrv{};
    ASSERT_TRUE(cache.TryGetTextureSrv(handle, firstSrv));

    EXPECT_TRUE(manager.Textures().Commit(handle, MakeTexture(8U, 3U)));
    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, handle)); });
    D3D12_CPU_DESCRIPTOR_HANDLE secondSrv{};
    ASSERT_TRUE(cache.TryGetTextureSrv(handle, secondSrv));
    EXPECT_EQ(cache.UploadedTextureCount(), 1U) << "替换后仍是一个可用 entry";
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked + 1U);
    EXPECT_EQ(cache.RetiredResourceCount(), 1U) << "旧纹理本体不得立即析构";

    cache.ReleaseAll();
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 0×0 纹理是占位：不分配描述符、没有 SRV，但 revision 被记录（下次热重载会重试）。
TEST(D3D12AssetCacheDeviceTests, EmptyTextureIsPlaceholderWithoutDescriptor)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);

    AssetManager manager;
    TextureAsset empty;
    empty.pixelFormat = TexturePixelFormat::Rgba8Unorm;
    empty.colorSpace = TextureColorSpace::Srgb;
    const auto handle = CommitTexture(manager, "textures/test/empty", empty);

    harness.RecordUpload([&](const AssetUploadContext& context)
                         { EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, handle)); });
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    EXPECT_FALSE(cache.TryGetTextureSrv(handle, srv));
    EXPECT_EQ(cache.UploadedTextureCount(), 0U);
    EXPECT_EQ(cache.UploadedTextureBytes(), 0U);
    EXPECT_EQ(cache.PlaceholderCount(), 1U);
    EXPECT_EQ(cache.TextureRevisionLagCount(manager), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// ReleaseAll 是唯一释放点：当前 entry 与 retired 队列一起清空，tracker 完全回落。
TEST(D3D12AssetCacheDeviceTests, ReleaseAllUnregistersEverything)
{
    AssetHarness harness;
    D3D12AssetCache cache;
    cache.Initialize(harness.stagingHeap, harness.tracker);
    const std::size_t baselineTracked = harness.tracker.TrackedResourceCount();

    AssetManager manager;
    const auto mesh = CommitMesh(manager, "meshes/test/release", MakePbrTriangleMesh(1U));
    const auto texture = CommitTexture(manager, "textures/test/release", MakeTexture(2U, 1U));
    harness.RecordUpload(
        [&](const AssetUploadContext& context)
        {
            EXPECT_TRUE(cache.EnsureMeshUploaded(context, manager, mesh));
            EXPECT_TRUE(cache.EnsureTextureUploaded(context, manager, texture));
        });
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked + 3U) << "VB + IB + 纹理";

    cache.ReleaseAll();
    EXPECT_FALSE(cache.TryGetMeshView(mesh).has_value());
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    EXPECT_FALSE(cache.TryGetTextureSrv(texture, srv));
    EXPECT_EQ(harness.tracker.TrackedResourceCount(), baselineTracked);
    EXPECT_EQ(cache.RetiredResourceCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}
