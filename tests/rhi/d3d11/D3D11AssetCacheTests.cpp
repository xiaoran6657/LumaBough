// ============================================================================
// D3D11AssetCacheTests.cpp — GPU 上传缓存"新建后替换"语义的契约
// 里程碑：M3（7-D）
// 职责：验证 revision 驱动上传与 no-op、新资源非原地修改（指针证据）、
//       失败保留旧 entry 且 lag 可见、占位 payload 不建 0 字节资源。
// 关联：engine/rhi/d3d11/src/D3D11AssetCache.cpp（被测实现）
//       docs/architecture/DECISIONS.md §5
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11AssetCache.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MeshAsset.h>

#include <gtest/gtest.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
using namespace MiniEngine::Assets;
using MiniEngine::Rhi::D3D11::D3D11AssetCache;

// 调试层优先；SDK 未安装时回退 retail 设备（本测试覆盖缓存逻辑，
// debug-layer Gate 属于 M2 Renderer 验收范围）。
Microsoft::WRL::ComPtr<ID3D11Device> CreateTestDevice()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    constexpr UINT kDebugFlags = D3D11_CREATE_DEVICE_DEBUG;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kDebugFlags, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(result))
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &featureLevel, &context);
    }
    return device;
}

MeshAsset MakeTriangleMesh(const std::byte payloadTag)
{
    // 3 顶点（每顶点 3 float）+ 3 个 uint32 索引；payloadTag 让两次内容可区分。
    MeshAsset mesh;
    mesh.vertexStride = 12;
    mesh.vertexCount = 3;
    mesh.indexStride = 4;
    mesh.indexCount = 3;
    for (std::uint32_t vertex = 0; vertex < mesh.vertexCount; ++vertex)
    {
        mesh.vertexData.push_back(static_cast<std::byte>(vertex));
        mesh.vertexData.push_back(payloadTag);
        mesh.vertexData.push_back(static_cast<std::byte>(0x00U));
        mesh.vertexData.push_back(static_cast<std::byte>(0x00U));
    }
    for (std::uint32_t index = 0; index < mesh.indexCount; ++index)
    {
        mesh.indexData.push_back(static_cast<std::byte>(index));
        mesh.indexData.push_back(static_cast<std::byte>(0x00U));
        mesh.indexData.push_back(static_cast<std::byte>(0x00U));
        mesh.indexData.push_back(static_cast<std::byte>(0x00U));
    }
    return mesh;
}

AssetHandle<MeshAsset> CommitMesh(AssetManager& manager, const std::string& uri, MeshAsset mesh)
{
    const auto handle = manager.Meshes().ResolveOrCreate(DeriveAssetId(uri));
    EXPECT_TRUE(manager.Meshes().Commit(handle, std::move(mesh)));
    return handle;
}
} // namespace

TEST(D3D11AssetCacheTests, UploadsOnceAndNoOpOnSameRevision)
{
    const auto device = CreateTestDevice();
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/triangle", MakeTriangleMesh(std::byte{1U}));

    D3D11AssetCache cache;
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    const auto first = cache.TryGetUploadedView(handle);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->indexCount, 3U);

    // 同 revision 再次 EnsureUploaded：no-op，不替换资源。
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    const auto second = cache.TryGetUploadedView(handle);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->vertexBuffer, first->vertexBuffer);
    EXPECT_EQ(second->indexBuffer, first->indexBuffer);
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);
}

TEST(D3D11AssetCacheTests, CpuRevisionBumpReplacesBuffersWithNewResources)
{
    const auto device = CreateTestDevice();
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/triangle", MakeTriangleMesh(std::byte{1U}));

    D3D11AssetCache cache;
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    const auto original = cache.TryGetUploadedView(handle);
    ASSERT_TRUE(original.has_value());

    // CPU payload 变化（revision+1）→ EnsureUploaded 创建"全新"资源替换。
    EXPECT_TRUE(manager.Meshes().Commit(handle, MakeTriangleMesh(std::byte{2U})));
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    const auto reloaded = cache.TryGetUploadedView(handle);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_NE(reloaded->vertexBuffer, original->vertexBuffer); // 新资源，非原地修改
    EXPECT_NE(reloaded->indexBuffer, original->indexBuffer);
    EXPECT_EQ(reloaded->indexCount, 3U);
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);
}

TEST(D3D11AssetCacheTests, EmptyPayloadRecordsRevisionWithoutDrawableView)
{
    const auto device = CreateTestDevice();
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/placeholder", MeshAsset{}); // 7-A 占位 payload

    D3D11AssetCache cache;
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle)); // 记录 revision，不创建 0 字节缓冲
    EXPECT_FALSE(cache.TryGetUploadedView(handle).has_value());        // 无可绘制内容
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);                    // 但也没有 lag
}

TEST(D3D11AssetCacheTests, StaleHandleFailsEnsureUploaded)
{
    const auto device = CreateTestDevice();
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    AssetManager manager;
    D3D11AssetCache cache;
    // using namespace MiniEngine::Assets 已生效，这里不能再写 Assets::（全局无该名字）。
    const AssetHandle<MeshAsset> stale{};
    EXPECT_FALSE(cache.EnsureUploaded(*device.Get(), manager, stale));
}

// P1-2：资产 Removed/Unload 后 GPU entry 由 PruneStale 淘汰（map 不得只增不减）。
TEST(D3D11AssetCacheTests, PruneStaleRemovesUnloadedEntries)
{
    const auto device = CreateTestDevice();
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/prune", MakeTriangleMesh(std::byte{1U}));

    D3D11AssetCache cache;
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    EXPECT_TRUE(cache.TryGetUploadedView(handle).has_value());

    // 资产被卸载（模拟 Manifest Removed 后的 ApplyPendingRemovals）：CPU 不可再解析。
    EXPECT_TRUE(manager.Meshes().Unload(handle));

    // 淘汰前 stale GPU entry 仍在（旧画面依赖它直到帧边界替换完成）。
    EXPECT_TRUE(cache.TryGetUploadedView(handle).has_value());

    cache.PruneStale(manager);
    EXPECT_FALSE(cache.TryGetUploadedView(handle).has_value());
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);
}

TEST(D3D11AssetCacheTests, RevisionLagTracksUnuploadedCpuChange)
{
    const auto device = CreateTestDevice();
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    AssetManager manager;
    const auto handle = CommitMesh(manager, "meshes/test/triangle", MakeTriangleMesh(std::byte{1U}));

    D3D11AssetCache cache;
    ASSERT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);

    // CPU 变化但尚未 EnsureUploaded → gpuRevisionLag 可见；上传后归零（07 篇）。
    EXPECT_TRUE(manager.Meshes().Commit(handle, MakeTriangleMesh(std::byte{2U})));
    EXPECT_EQ(cache.RevisionLagCount(manager), 1U);
    EXPECT_TRUE(cache.EnsureUploaded(*device.Get(), manager, handle));
    EXPECT_EQ(cache.RevisionLagCount(manager), 0U);
}
