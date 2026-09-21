// ============================================================================
// D3D12ForwardPassDeviceTests.cpp — forward pass 消费 RenderPacket 的设备级契约
// 里程碑：M5（09 篇迁移顺序第 2 步「baked mesh + texture + depth」）
// 职责：在真实 Device + 真实 DXIL + 真实 HDR/主深度上验证"渲染端只读消费 RenderPacket"：
//   1. packet 里的每个 mainOpaque 条目要么 draw、要么计入 skip（记账不可脱节）；
//   2. **帧内零创建**：PSO 全部来自 init 的 baseline 集（08 篇硬约束的单测版），
//      mesh 资产按 revision 只在首帧上传（第二帧 no-op）；
//   3. 材质句柄失效时按 02 篇失败语义退回默认材质继续 draw（不丢 draw、不崩溃），
//      材质表的 5 个槽位全部绑到真实 fallback（禁止留空残留）。
// 场景构造：手工填一个单 mesh 的 RenderPacket（World → packet 的完整链路由沙盒
//      取证覆盖；这里聚焦渲染端消费侧）。
// 环境：需要 D3D12 硬件或 WARP；需要 MINIENGINE_D3D12_SHADER_DIR 指向编译产物。
// 关联：engine/rhi/d3d12/src/D3D12Renderer.cpp RecordForwardPass（被测路径）
//       tests/rhi/d3d12/D3D12TrianglePassDeviceTests.cpp（harness 同款）
// ============================================================================
#include "D3D12Renderer.h"
#include "D3D12RootSignature.h"

#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <MiniEngine/World/RenderPacket.h>

#include <Windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#ifndef MINIENGINE_D3D12_SHADER_DIR
#error "MINIENGINE_D3D12_SHADER_DIR must be defined by the test target (see tests/rhi/d3d12/CMakeLists.txt)"
#endif

using MiniEngine::Rhi::D3D12::D3D12DescriptorHeap;
using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12Renderer;
using MiniEngine::Rhi::D3D12::D3D12RendererOptions;

namespace
{
// 隐藏窗口：仅作交换链宿主（与 D3D12TrianglePassDeviceTests 同款），避免 ctest 弹窗。
class HiddenWindow final
{
  public:
    HiddenWindow()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"MiniEngineD3D12ForwardTestWindow";
        RegisterClassExW(&windowClass); // 已注册时返回 0，可忽略（多用例共享类名）

        m_window =
            CreateWindowExW(0, windowClass.lpszClassName, L"MiniEngine D3D12 forward test window", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, instance, nullptr);
        // 保持隐藏：测试不需要可见窗口。
    }
    ~HiddenWindow()
    {
        if (m_window != nullptr)
        {
            DestroyWindow(m_window);
        }
    }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;

    [[nodiscard]] void* Handle() const noexcept
    {
        return m_window;
    }

  private:
    HWND m_window = nullptr;
};

// 一个最小可绘制网格（48 字节 PbrVertex stride + uint32 索引，与 .memesh v2 一致）。
MiniEngine::Assets::MeshAsset MakePbrTriangleMesh()
{
    MiniEngine::Assets::MeshAsset mesh;
    mesh.vertexStride = sizeof(MiniEngine::Assets::PbrVertex);
    mesh.vertexCount = 3U;
    mesh.indexStride = 4U;
    mesh.indexCount = 3U;
    for (std::uint32_t vertex = 0U; vertex < 3U; ++vertex)
    {
        MiniEngine::Assets::PbrVertex v{};
        v.position[0] = static_cast<float>(vertex);
        v.position[2] = 0.5F;
        v.normal[2] = 1.0F;
        v.tangent[0] = 1.0F;
        v.tangent[3] = 1.0F;
        const auto* const bytes = reinterpret_cast<const std::byte*>(&v);
        mesh.vertexData.insert(mesh.vertexData.end(), bytes, bytes + sizeof(MiniEngine::Assets::PbrVertex));
    }
    for (std::uint32_t index = 0U; index < 3U; ++index)
    {
        const auto* const bytes = reinterpret_cast<const std::byte*>(&index);
        mesh.indexData.insert(mesh.indexData.end(), bytes, bytes + sizeof(index));
    }
    return mesh;
}

// 设备 + 渲染器 + root signature + baseline PSO + descriptor heap 的公共前置。
// 成员声明顺序即构造顺序、逆序即析构顺序：rootSignature/srvHeap 必须比 renderer
// 活得久（渲染器只持非拥有引用）。
struct ForwardHarness final
{
    std::unique_ptr<D3D12Device> device;
    HiddenWindow window;
    MiniEngine::Assets::AssetManager assets;
    D3D12DescriptorHeap srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    D3D12Renderer renderer;
    MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset> meshHandle;

    ForwardHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions deviceOptions;
        deviceOptions.debugLayer = true; // 零消息断言必须有调试层才有意义
        device = D3D12Device::Create(deviceOptions);

        D3D12RendererOptions rendererOptions;
        rendererOptions.width = 320U;
        rendererOptions.height = 240U;
        rendererOptions.vsync = false; // 测试不做呈现节流
        renderer.Initialize(*device, window.Handle(), rendererOptions);

        MiniEngine::Rhi::D3D12::RootSignatureFacts facts{};
        rootSignature = MiniEngine::Rhi::D3D12::CreateM5RootSignature(
            *static_cast<ID3D12Device*>(device->NativeDeviceHandle()), facts);
        renderer.BindRootSignature(*rootSignature.Get(), 0U);

        // shader-visible heap：材质表（3×5）+ global 表（4）的包含各帧全局表与逐 draw 快照，取 256 留余量。
        srvHeap.Initialize(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()),
                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256U, true, L"M5.Test.ForwardSrvHeap");
        renderer.BindDescriptorHeap(srvHeap);
        renderer.CreateBaselinePsoSet(MINIENGINE_D3D12_SHADER_DIR);

        meshHandle = assets.Meshes().ResolveOrCreate(MiniEngine::Assets::DeriveAssetId("meshes/test/forward"));
        EXPECT_TRUE(assets.Meshes().Commit(meshHandle, MakePbrTriangleMesh()));
    }

    // 收尾：先 flush 再等待，之后成员才按逆序析构（与 TriangleHarness 同款理由）。
    ~ForwardHarness()
    {
        renderer.FlushGpu("forward-test-teardown");
        Sleep(100);
    }

    // 跑 frames 帧（每帧都设置同一个 packet 输入），返回已呈现帧数。
    std::uint32_t RenderFrames(const MiniEngine::World::RenderPacket& packet, const std::uint32_t frames)
    {
        std::uint32_t presented = 0U;
        for (std::uint32_t index = 0U; index < frames; ++index)
        {
            renderer.SetFrameInput(&packet, &assets);
            if (renderer.RenderFrame())
            {
                ++presented;
            }
        }
        return presented;
    }
};

// 构造单 mesh 的 packet：identity 变换 + 默认光；bounds 给大球（渲染端不做剔除）。
MiniEngine::World::RenderPacket MakeSingleDrawPacket(
    const MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset>& mesh,
    const MiniEngine::Assets::AssetId& meshId)
{
    MiniEngine::World::RenderPacket packet{};
    MiniEngine::World::RenderDraw draw{};
    draw.mesh = mesh;
    draw.meshId = meshId;
    for (int row = 0; row < 4; ++row)
    {
        draw.world.values[static_cast<std::size_t>(row) * 4U + row] = 1.0F;
        draw.normal.values[static_cast<std::size_t>(row) * 4U + row] = 1.0F;
    }
    draw.worldBounds = MiniEngine::World::Sphere{MiniEngine::World::Float3{0.0F, 0.0F, 0.0F}, 100.0F};
    packet.mainOpaque.push_back(draw);
    for (int row = 0; row < 4; ++row)
    {
        packet.viewProjection.values[static_cast<std::size_t>(row) * 4U + row] = 1.0F;
    }
    packet.cameraWorldPosition = {0.0F, 3.0F, -10.0F};
    return packet;
}
MiniEngine::World::RenderPacket MakeTwoDrawPacket(
    const MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset>& mesh,
    const MiniEngine::Assets::AssetId& meshId)
{
    MiniEngine::World::RenderPacket packet = MakeSingleDrawPacket(mesh, meshId);
    packet.mainOpaque.push_back(packet.mainOpaque.front());
    return packet;
}

std::uint32_t UsedShaderVisibleDescriptors(const D3D12DescriptorHeap& heap)
{
    return heap.UsedCount();
}
} // namespace

// 正路径：每帧恰好 1 次 draw，三个计数互相吻合；PSO 全部来自 baseline 集（帧内零创建），
// mesh 只在首帧上传（第二帧 no-op）；revision lag 收敛为 0。
TEST(D3D12ForwardPassDeviceTests, DrawsEachPacketEntryOnceWithoutInFramePsoCreation)
{
    ForwardHarness harness;
    const std::size_t createdAfterInit = harness.renderer.PsoStats().created;
    const auto meshId = *harness.assets.Meshes().TryGetAssetId(harness.meshHandle);
    const MiniEngine::World::RenderPacket packet = MakeSingleDrawPacket(harness.meshHandle, meshId);

    constexpr std::uint32_t kFrames = 2U;
    const std::uint32_t presented = harness.RenderFrames(packet, kFrames);
    EXPECT_GT(presented, 0U) << "测试环境应至少呈现一帧（WARP/硬件均可）";

    EXPECT_EQ(harness.renderer.ForwardDrawCount(), kFrames) << "每帧恰好一次 draw（draw 语义按录制计）";
    EXPECT_EQ(harness.renderer.ForwardSkippedDrawCount(), 0U);
    EXPECT_EQ(harness.renderer.ForwardIndexCount(), kFrames * 3U) << "单 mesh 3 索引 × 帧数";
    EXPECT_EQ(harness.renderer.UploadedMeshCount(), 1U) << "第二帧必须是 no-op，不得重复上传";
    EXPECT_EQ(harness.renderer.ToneMapDrawCount(), kFrames);
    EXPECT_EQ(harness.renderer.MeshRevisionLag(), 0U);
    EXPECT_EQ(harness.renderer.MaterialTableCount(), 3U) << "每 FrameContext 一张材质表";
    EXPECT_EQ(harness.renderer.PsoStats().created, createdAfterInit) << "帧循环里不得创建 PSO（08 篇硬约束的单测版）";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 负路径：mesh 句柄失效时该 draw 必须计入 skip（不能既不画也不记账，也不能崩溃）。
TEST(D3D12ForwardPassDeviceTests, InvalidMeshHandleSkipsTheDrawAndCountsIt)
{
    ForwardHarness harness;
    const MiniEngine::World::RenderPacket packet = MakeSingleDrawPacket(
        MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset>{}, MiniEngine::Assets::AssetId{});

    constexpr std::uint32_t kFrames = 2U;
    static_cast<void>(harness.RenderFrames(packet, kFrames));

    EXPECT_EQ(harness.renderer.ForwardDrawCount(), 0U) << "失效句柄不得产生 draw";
    EXPECT_EQ(harness.renderer.ForwardSkippedDrawCount(), kFrames) << "skip 必须被记账（对账判据的另一半）";
    EXPECT_EQ(harness.renderer.UploadedMeshCount(), 0U);
    EXPECT_EQ(harness.renderer.ForwardIndexCount(), 0U);
    EXPECT_EQ(harness.renderer.ToneMapDrawCount(), kFrames) << "tone map 不受单个 draw 失效影响";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 材质句柄失效：按 02 篇失败语义退回默认材质继续 draw；材质表 5 槽全部绑到真实
// fallback（调试层零消息 = 没有留空绑定）。
TEST(D3D12ForwardPassDeviceTests, InvalidMaterialStillDrawsWithDefaultMaterial)
{
    ForwardHarness harness;
    const auto meshId = *harness.assets.Meshes().TryGetAssetId(harness.meshHandle);
    MiniEngine::World::RenderPacket packet = MakeSingleDrawPacket(harness.meshHandle, meshId);
    // 显式把材质句柄清成无效：渲染端必须走 default material 分支。
    packet.mainOpaque[0].material = {};

    constexpr std::uint32_t kFrames = 1U;
    static_cast<void>(harness.RenderFrames(packet, kFrames));

    EXPECT_EQ(harness.renderer.ForwardDrawCount(), kFrames) << "材质缺失不丢 draw（保守可视化契约）";
    EXPECT_EQ(harness.renderer.ForwardSkippedDrawCount(), 0U);
    EXPECT_EQ(harness.renderer.UploadedMeshCount(), 1U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 判别性覆盖：两个 draw 与 ToneMap 必须各拥有独立的 5-slot 材质表快照。
TEST(D3D12ForwardPassDeviceTests, MultipleDrawsAndToneMapUseIndependentFiveSlotSnapshots)
{
    ForwardHarness harness;
    const auto meshId = *harness.assets.Meshes().TryGetAssetId(harness.meshHandle);
    const MiniEngine::World::RenderPacket packet = MakeTwoDrawPacket(harness.meshHandle, meshId);
    const std::uint32_t usedBefore = UsedShaderVisibleDescriptors(harness.srvHeap);
    const std::size_t rangesBefore = harness.srvHeap.ActiveRangeCount();

    static_cast<void>(harness.RenderFrames(packet, 1U));

    EXPECT_EQ(harness.renderer.ForwardDrawCount(), 2U);
    EXPECT_EQ(harness.renderer.ToneMapDrawCount(), 1U);
    EXPECT_EQ(UsedShaderVisibleDescriptors(harness.srvHeap), usedBefore + 15U)
        << "两个 draw + ToneMap 必须各增加一个 5-slot 快照";
    EXPECT_EQ(harness.srvHeap.ActiveRangeCount(), rangesBefore + 3U)
        << "两个 draw 与 ToneMap 必须是三个独立 descriptor 区间";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 三个 FrameContext 首次各建立自己的快照；下一轮复用已完成 context 时不得继续扩张，
// 且帧内不得因为材质表或 pass 查找创建新的 PSO。
TEST(D3D12ForwardPassDeviceTests, CompletedFrameContextsReuseMaterialSnapshotsWithoutPsoGrowth)
{
    ForwardHarness harness;
    const auto meshId = *harness.assets.Meshes().TryGetAssetId(harness.meshHandle);
    const MiniEngine::World::RenderPacket packet = MakeTwoDrawPacket(harness.meshHandle, meshId);
    const std::uint32_t usedBefore = UsedShaderVisibleDescriptors(harness.srvHeap);
    const std::uint64_t createdAfterInit = harness.renderer.PsoStats().created;

    static_cast<void>(harness.RenderFrames(packet, 3U));
    const std::uint32_t usedAfterFirstRotation = UsedShaderVisibleDescriptors(harness.srvHeap);
    EXPECT_EQ(usedAfterFirstRotation, usedBefore + 45U) << "三个 FrameContext 首轮各需两个 draw + ToneMap 的 15 个槽位";

    static_cast<void>(harness.RenderFrames(packet, 3U));
    EXPECT_EQ(UsedShaderVisibleDescriptors(harness.srvHeap), usedAfterFirstRotation)
        << "复用已完成 FrameContext 不得持续分配材质表";
    EXPECT_EQ(harness.renderer.PsoStats().created, createdAfterInit) << "跨帧复用材质表时不得创建 PSO";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}
// M5-12 回归：triangle 与真实 forward/ToneMap 同帧时不能把全 pass 常量总量填进 triangle 证据。
// 同时覆盖 resize 与关停 triangle；保留总量计数能证明其他 pass 仍在正常上传。
TEST(D3D12ForwardPassDeviceTests, MixedTriangleConstantsRemainIndependentAcrossResizeAndDisable)
{
    ForwardHarness harness;
    const auto meshId = *harness.assets.Meshes().TryGetAssetId(harness.meshHandle);
    const MiniEngine::World::RenderPacket packet = MakeSingleDrawPacket(harness.meshHandle, meshId);
    harness.renderer.SetTriangleEnabled(true);

    static_cast<void>(harness.RenderFrames(packet, 3U));
    EXPECT_EQ(harness.renderer.TriangleDrawCount(), 3U);
    EXPECT_EQ(harness.renderer.TriangleConstantUploadBytes(), 3U * (128U + 208U));
    EXPECT_GT(harness.renderer.ConstantUploadBytes(), harness.renderer.TriangleConstantUploadBytes());

    harness.renderer.RequestResize(640U, 480U);
    ASSERT_TRUE(harness.renderer.ApplyPendingResizeIfNeeded());
    static_cast<void>(harness.RenderFrames(packet, 1U));
    EXPECT_EQ(harness.renderer.TriangleDrawCount(), 4U);
    EXPECT_EQ(harness.renderer.TriangleVertexCount(), 12U);
    EXPECT_EQ(harness.renderer.TriangleConstantUploadBytes(), 4U * (128U + 208U));

    const std::uint64_t totalBeforeDisable = harness.renderer.ConstantUploadBytes();
    harness.renderer.SetTriangleEnabled(false);
    static_cast<void>(harness.RenderFrames(packet, 2U));
    EXPECT_EQ(harness.renderer.TriangleDrawCount(), 4U);
    EXPECT_EQ(harness.renderer.TriangleConstantUploadBytes(), 4U * (128U + 208U));
    EXPECT_GT(harness.renderer.ConstantUploadBytes(), totalBeforeDisable);
    EXPECT_EQ(harness.renderer.ForwardDrawCount(), 6U);
    EXPECT_EQ(harness.renderer.ToneMapDrawCount(), 6U);
    EXPECT_EQ(harness.renderer.PsoStats().created, harness.renderer.BaselinePsoCount());
    harness.renderer.FlushGpu("mixed-triangle-test-complete");
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}
