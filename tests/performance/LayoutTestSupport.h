// ============================================================================
// LayoutTestSupport.h — M7-08 布局实验夹具（等价测试与 microbenchmark 共用）
// 里程碑：M7-08（数据布局实验）
// 职责：用确定性 PRNG 生成"目标可见率"的规范代理集，再派生三种布局与两个视锥；
//       同时提供**生产输入**（RenderItem 快照 + 两条 lookup）与生产 packet 构建，
//       使"baseline = M6 真实实现"可以在同一个夹具里逐项对照。
// 数据分布：透视相机（fovY=60°、16:9、near=0.1、far=200，与 M7-05 同一口径）；
//       可见组放在视锥内（深度 20..150、横向 ±0.8·d、纵向 ±0.45·d、半径 0.5），
//       不可见组放在相机后方或远侧（|x| > 2.5·d）——目标可见率因此可精确构造，
//       并由用例的覆盖率护栏断言（M7-P09 的教训：空/退化集合会让比对失去证明力）。
// 关联：engine/world/include/MiniEngine/World/RenderProxyLayouts.h
//       docs/architecture/README.md「数据集矩阵」
// ============================================================================

#pragma once

#include <MiniEngine/World/RenderProxyLayouts.h>
#include <MiniEngine/World/RenderQueueBuilder.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace MiniEngine::TestSupport
{
using World::LayoutProxySource;
using World::RenderItem;
using World::RenderProxyAoS;
using World::RenderProxyCold;
using World::RenderProxyHotCold;
using World::RenderProxySoA;

// 逐实体字节账（报告与验收录音用；不参与判定）。
struct LayoutBytesSummary final
{
    std::size_t aosBytesPerEntity = 0;
    std::size_t hotColdHotBytesPerEntity = 0;
    std::size_t hotColdColdBytesPerEntity = 0;
    std::size_t soaScanBytesPerEntity = 0;
    std::size_t soaPayloadBytesPerEntity = 0;
    std::size_t soaCapacityBytes = 0;
    std::size_t soaLogicalBytes = 0;
};

struct LayoutFixture final
{
    std::uint32_t count = 0;
    std::uint32_t targetVisible = 0;
    std::uint64_t seed = 0;
    std::vector<LayoutProxySource> sources;
    std::vector<RenderProxyAoS> aos;
    RenderProxyHotCold hotCold;
    RenderProxySoA soa;
    World::Frustum cameraFrustum;
    World::Frustum lightFrustum;
    World::DirectionalLight light{};
    World::RenderPacketCamera camera{};
};

// xorshift64 → [0,1)：与 M7-05 的夹具同一 PRNG，场景几何固定 seed 复现。
[[nodiscard]] inline float UnitRandom(std::uint64_t& state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<float>(state % 1000001U) / 1000000.0F;
}

// 生产口径透视投影（row-major row-vector、LH、D3D 深度 0..1）。
[[nodiscard]] inline World::Matrix4 PerspectiveProjection()
{
    constexpr float kFovYRadians = 1.0471975511965976F; // 60°
    constexpr float kAspect = 16.0F / 9.0F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 200.0F;
    const float yScale = 1.0F / std::tan(kFovYRadians * 0.5F);
    const float xScale = yScale / kAspect;
    World::Matrix4 projection{};
    projection.values[0] = xScale;
    projection.values[5] = yScale;
    projection.values[10] = kFar / (kFar - kNear);
    projection.values[11] = 1.0F;
    projection.values[14] = -kNear * kFar / (kFar - kNear);
    projection.values[15] = 0.0F;
    return projection;
}

[[nodiscard]] inline World::RenderPacketCamera MakePerspectiveCamera()
{
    World::RenderPacketCamera camera{};
    camera.view = World::Matrix4{};
    camera.projection = PerspectiveProjection();
    camera.worldPosition = {0.0F, 0.0F, 0.0F};
    return camera;
}

// row-vector 约定的矩阵乘法（与 RenderQueueBuilder.cpp 的内部 Multiply 同循环顺序：
// 夹具的视锥必须与生产 BuildRenderPacket 得到的平面逐位一致，否则等价对照无意义）。
[[nodiscard]] inline World::Matrix4 Multiply(const World::Matrix4& left, const World::Matrix4& right)
{
    World::Matrix4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0.0F;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += left.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(inner)] *
                       right.values[static_cast<std::size_t>(inner) * 4U + static_cast<std::size_t>(column)];
            }
            result.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(column)] = sum;
        }
    }
    return result;
}

[[nodiscard]] inline World::Matrix4 TranslationMatrix(const float x, const float y, const float z)
{
    World::Matrix4 matrix{};
    matrix.values[12] = x;
    matrix.values[13] = y;
    matrix.values[14] = z;
    return matrix;
}

[[nodiscard]] inline Assets::AssetId MakeAssetId(const std::uint64_t value)
{
    Assets::AssetId id{};
    for (std::size_t index = 0; index < id.bytes.size(); ++index)
    {
        id.bytes[index] = static_cast<std::byte>((value >> ((index % 8U) * 8U)) & 0xFFU);
    }
    return id;
}

// 构造夹具：count 个规范代理，可见率命中 visibleRatio（由用例断言护栏）。
[[nodiscard]] inline LayoutFixture MakeLayoutFixture(const std::uint32_t count, const double visibleRatio,
                                                     const std::uint64_t seed)
{
    LayoutFixture fixture;
    fixture.count = count;
    fixture.seed = seed;
    fixture.targetVisible = static_cast<std::uint32_t>(std::lround(visibleRatio * static_cast<double>(count)));
    fixture.camera = MakePerspectiveCamera();
    fixture.cameraFrustum =
        World::Frustum::FromRowVectorDirect3D(Multiply(fixture.camera.view, fixture.camera.projection));
    fixture.lightFrustum = World::Frustum::FromRowVectorDirect3D(World::BuildLightViewProjection(fixture.light));

    const Assets::AssetId meshId = MakeAssetId(0x5EED0001ULL);
    const Assets::AssetId materialId = MakeAssetId(0x5EED0002ULL);
    std::uint64_t state = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;

    fixture.sources.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        LayoutProxySource source;
        if (index < fixture.targetVisible)
        {
            // 可见组：视锥内（含半径 0.5 的保守球也仍在体内）。
            const float depth = 20.0F + UnitRandom(state) * 130.0F; // 20..150
            const float lateral = (UnitRandom(state) * 2.0F - 1.0F) * 0.8F * depth;
            const float vertical = (UnitRandom(state) * 2.0F - 1.0F) * 0.45F * depth;
            source.world = TranslationMatrix(lateral, vertical, depth);
        }
        else
        {
            // 不可见组：一半在相机后方，一半在远侧（两种剔除原因都被覆盖）。
            if ((index % 2U) == 0U)
            {
                source.world = TranslationMatrix((UnitRandom(state) * 2.0F - 1.0F) * 40.0F, 0.0F,
                                                 -(5.0F + UnitRandom(state) * 195.0F));
            }
            else
            {
                const float depth = 20.0F + UnitRandom(state) * 130.0F;
                const float side =
                    (UnitRandom(state) < 0.5F ? -1.0F : 1.0F) * (2.5F + UnitRandom(state) * 3.0F) * depth;
                source.world = TranslationMatrix(side, 0.0F, depth);
            }
        }
        source.localBounds = {World::Float3{0.0F, 0.0F, 0.0F}, 0.5F};
        source.meshId = meshId;
        source.materialId = materialId;
        source.entityIndex = index;
        source.indexCount = 36U; // 12 个三角形：trianglesSubmitted 口径与生产一致
        source.castsShadow = (index % 7U) != 0U;
        source.receivesShadow = true;
        source.hasBounds = true;
        // 冷字段：每次分配独立字符串（真实冷数据不是 interned 常量，避免掩盖访问代价）。
        source.cold.debugName = "proxy/" + std::to_string(index) + "/segment" + std::to_string(index % 17U);
        source.cold.sourceAsset = MakeAssetId(0xC01D0000ULL + index);
        source.cold.authoringIndex = index;
        source.cold.diagnosticsFlags = index % 4U;
        fixture.sources.push_back(std::move(source));
    }

    fixture.aos.reserve(count);
    for (const LayoutProxySource& source : fixture.sources)
    {
        fixture.aos.push_back(World::ToAoS(source));
    }
    fixture.hotCold = World::ToHotCold(fixture.sources);
    fixture.soa = World::ToSoA(fixture.sources);
    return fixture;
}

// 生产输入：同一份夹具 → RenderItem 快照 + 两条 lookup（与运行器的用法一致）。
[[nodiscard]] inline std::vector<RenderItem> ToProductionItems(const LayoutFixture& fixture)
{
    std::vector<RenderItem> items;
    items.reserve(fixture.sources.size());
    for (const LayoutProxySource& source : fixture.sources)
    {
        RenderItem item;
        item.world = source.world;
        item.mirrored = source.mirrored;
        item.castsShadow = source.castsShadow;
        item.receivesShadow = source.receivesShadow;
        items.push_back(item);
    }
    return items;
}

[[nodiscard]] inline World::MeshInfoLookup MakeMeshLookup(const LayoutFixture& fixture)
{
    return [&fixture](Assets::AssetHandle<Assets::MeshAsset>) -> std::optional<World::MeshDrawInfo>
    {
        if (fixture.sources.empty())
        {
            return std::nullopt;
        }
        return World::MeshDrawInfo{fixture.sources.front().meshId, fixture.sources.front().localBounds,
                                   fixture.sources.front().indexCount};
    };
}

[[nodiscard]] inline World::MaterialInfoLookup MakeMaterialLookup(const LayoutFixture& fixture)
{
    return [&fixture](Assets::AssetHandle<Assets::MaterialAsset>) -> std::optional<Assets::AssetId>
    {
        if (fixture.sources.empty())
        {
            return std::nullopt;
        }
        return fixture.sources.front().materialId;
    };
}

// 生产 packet（M6 真实实现）：等价测试与运行器 Debug 校验的参照物。
[[nodiscard]] inline World::RenderPacket BuildProductionPacket(const LayoutFixture& fixture)
{
    const std::vector<RenderItem> items = ToProductionItems(fixture);
    return World::BuildRenderPacket(items, fixture.camera, fixture.light, MakeMeshLookup(fixture), {},
                                    MakeMaterialLookup(fixture));
}

[[nodiscard]] inline LayoutBytesSummary SummarizeLayoutBytes(const LayoutFixture& fixture)
{
    LayoutBytesSummary summary;
    summary.aosBytesPerEntity = fixture.aos.empty() ? 0 : fixture.aos.front().Bytes();
    summary.hotColdHotBytesPerEntity =
        fixture.hotCold.Size() == 0 ? 0 : fixture.hotCold.HotBytes() / fixture.hotCold.Size();
    summary.hotColdColdBytesPerEntity =
        fixture.hotCold.Size() == 0 ? 0 : fixture.hotCold.ColdBytes() / fixture.hotCold.Size();
    summary.soaScanBytesPerEntity = fixture.soa.ScanBytesPerEntity();
    summary.soaPayloadBytesPerEntity = fixture.soa.PayloadBytesPerEntity();
    summary.soaCapacityBytes = fixture.soa.CapacityBytes();
    summary.soaLogicalBytes = fixture.soa.HotBytes();
    return summary;
}
} // namespace MiniEngine::TestSupport
