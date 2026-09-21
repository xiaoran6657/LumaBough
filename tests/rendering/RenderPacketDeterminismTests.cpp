// ============================================================================
// RenderPacketDeterminismTests.cpp - M7-05 / M7-A13+A14 evidence.
// 并行 RenderPacket 构建必须与串行 oracle 在各边界数据集上逐字段一致：
//   - 0/1/chunk 边界/大尺寸（含 100k）实体数；
//   - 1/2/4/8 worker × chunk 32..1024 全 sweep；
//   - 全可见、全裁剪与重复排序键；
//   - 随机完成顺序（chunk 完成时间被 lookup 抖动打散）。
// 计时字段（culling/sort/queueBuild microseconds）是测量值，明确排除在比对之外。
// 覆盖护栏：每个比对用例都断言实际可见数量高于下限，防止夹具退化（相机看空场景
// 时"空 == 空"会让测试静默失去证明力——初版单位相机就出现过 mainVisible=0）。
// ============================================================================

#include <MiniEngine/Render/RenderPacketBuilder.h>
#include <MiniEngine/World/RenderQueueBuilder.h>

#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace
{
using namespace MiniEngine;
using MiniEngine::Assets::AssetHandle;
using MiniEngine::Assets::AssetId;
namespace Wld = MiniEngine::World;

[[nodiscard]] AssetId MakeId(const std::uint64_t value)
{
    AssetId id;
    for (std::size_t index = 0; index < id.bytes.size(); ++index)
        id.bytes[index] = static_cast<std::byte>((value >> ((index % 8U) * 8U)) & 0xFFU);
    return id;
}

// Row-major row-vector 平移（含显式缩放；3x3 单位缩放保证 normal matrix 非奇异）。
[[nodiscard]] Wld::Matrix4 TranslationMatrix(const float x, const float y, const float z)
{
    Wld::Matrix4 matrix{};
    matrix.values[12] = x;
    matrix.values[13] = y;
    matrix.values[14] = z;
    return matrix;
}

// xorshift64 -> [0,1)：场景几何固定 seed 复现，不依赖 STL 引擎实现。
[[nodiscard]] float UnitRandom(std::uint64_t& state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<float>(state % 1000001U) / 1000000.0F;
}

// 生产口径透视相机（row-vector LH、D3D 深度 0..1）：fovY=60°、16:9、near=0.1、far=200。
// 必须用透视相机而不是单位矩阵：单位裁剪体（±1）会把所有分布在几十单位的代理全部
// 剔除，使"逐字段比对"退化为空集合比对。
[[nodiscard]] Wld::Matrix4 PerspectiveProjection()
{
    constexpr float kFovYRadians = 1.0471975511965976F; // 60°
    constexpr float kAspect = 16.0F / 9.0F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 200.0F;

    const float yScale = 1.0F / std::tan(kFovYRadians * 0.5F);
    const float xScale = yScale / kAspect;
    Wld::Matrix4 projection{};
    projection.values[0] = xScale;
    projection.values[5] = yScale;
    projection.values[10] = kFar / (kFar - kNear);
    projection.values[11] = 1.0F;
    projection.values[14] = -kNear * kFar / (kFar - kNear);
    projection.values[15] = 0.0F;
    return projection;
}

// 主验证相机：identity view + 透视投影。深度 10..110 处可见窗口约为 ±(0.57..1.03)·d，
// 与代理分布（±40 × ±24 × 10..110）相交出约 60% 的可见率（由用例断言护栏保证）。
[[nodiscard]] Wld::RenderPacketCamera MakePerspectiveCamera()
{
    Wld::RenderPacketCamera camera{};
    camera.view = Wld::Matrix4{};
    camera.projection = PerspectiveProjection();
    camera.worldPosition = {0.0F, 0.0F, 0.0F};
    return camera;
}

// 边界用例相机：单位裁剪体（x/y ∈ [-1,1]，z ∈ [0,1]），配合 MakeTightItems/MakeFarItems
// 构造全可见与全裁剪数据集。
[[nodiscard]] Wld::RenderPacketCamera MakeUnitCamera()
{
    Wld::RenderPacketCamera camera{};
    camera.view = Wld::Matrix4{};
    camera.projection = Wld::Matrix4{};
    camera.worldPosition = {0.0F, 0.0F, -1.0F};
    return camera;
}

[[nodiscard]] std::vector<Wld::RenderItem> MakeItems(const std::uint32_t count, const std::uint64_t seed)
{
    std::vector<Wld::RenderItem> items(count);
    std::uint64_t state = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;

    for (std::uint32_t index = 0; index < count; ++index)
    {
        Wld::RenderItem& item = items[index];
        item.world = TranslationMatrix(UnitRandom(state) * 80.0F - 40.0F, UnitRandom(state) * 48.0F - 24.0F,
                                       10.0F + UnitRandom(state) * 100.0F);
        item.mirrored = index % 5U == 0U;
        item.castsShadow = index % 7U != 0U;
        item.receivesShadow = index % 11U != 0U;
        item.mesh = AssetHandle<Assets::MeshAsset>(index % 64U + 1U, 1U);
        item.material = AssetHandle<Assets::MaterialAsset>(index % 16U + 1U, 1U);
    }
    return items;
}

// 全可见数据集：半径 ≤0.2 的代理落在单位裁剪体内部（配合小半径 mesh lookup）。
[[nodiscard]] std::vector<Wld::RenderItem> MakeTightItems(const std::uint32_t count, const std::uint64_t seed)
{
    std::vector<Wld::RenderItem> items(count);
    std::uint64_t state = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;

    for (std::uint32_t index = 0; index < count; ++index)
    {
        Wld::RenderItem& item = items[index];
        item.world =
            TranslationMatrix(UnitRandom(state) - 0.5F, UnitRandom(state) - 0.5F, 0.25F + UnitRandom(state) * 0.5F);
        item.mirrored = index % 3U == 0U;
        item.castsShadow = index % 4U != 0U;
        item.receivesShadow = index % 9U != 0U;
        item.mesh = AssetHandle<Assets::MeshAsset>(index % 64U + 1U, 1U);
        item.material = AssetHandle<Assets::MaterialAsset>(index % 16U + 1U, 1U);
    }
    return items;
}

// 全裁剪数据集：代理远在单位裁剪体与固定光视锥之外。
[[nodiscard]] std::vector<Wld::RenderItem> MakeFarItems(const std::uint32_t count)
{
    std::vector<Wld::RenderItem> items(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        Wld::RenderItem& item = items[index];
        item.world = TranslationMatrix(-1000.0F - static_cast<float>(index) * 4.0F, 0.0F, -1000.0F);
        item.castsShadow = true;
        item.receivesShadow = true;
        item.mesh = AssetHandle<Assets::MeshAsset>(index % 64U + 1U, 1U);
        item.material = AssetHandle<Assets::MaterialAsset>(index % 16U + 1U, 1U);
    }
    return items;
}

[[nodiscard]] Wld::MeshInfoLookup MakeMeshLookup()
{
    return [](const AssetHandle<Assets::MeshAsset> mesh) -> std::optional<Wld::MeshDrawInfo>
    {
        if (mesh.Index() == 0U || mesh.Index() == AssetHandle<Assets::MeshAsset>::kInvalidIndex)
            return std::nullopt;
        Wld::MeshDrawInfo info;
        info.id = MakeId(mesh.Index());
        info.localBounds = Wld::Sphere{{0.0F, 0.0F, 0.0F}, 1.0F + static_cast<float>(mesh.Index() % 3U)};
        info.indexCount = 36U * (mesh.Index() % 5U + 1U);
        return info;
    };
}

// 全可见用例专用：包围球半径 <0.2，保证代理完整落在单位裁剪体内部。
[[nodiscard]] Wld::MeshInfoLookup MakeSmallMeshLookup()
{
    return [](const AssetHandle<Assets::MeshAsset> mesh) -> std::optional<Wld::MeshDrawInfo>
    {
        if (mesh.Index() == 0U || mesh.Index() == AssetHandle<Assets::MeshAsset>::kInvalidIndex)
            return std::nullopt;
        Wld::MeshDrawInfo info;
        info.id = MakeId(mesh.Index());
        info.localBounds = Wld::Sphere{{0.0F, 0.0F, 0.0F}, 0.1F + static_cast<float>(mesh.Index() % 3U) * 0.05F};
        info.indexCount = 36U * (mesh.Index() % 5U + 1U);
        return info;
    };
}

[[nodiscard]] Wld::MaterialInfoLookup MakeMaterialLookup()
{
    return [](const AssetHandle<Assets::MaterialAsset> material) -> std::optional<Assets::AssetId>
    {
        if (material.Index() == 0U || material.Index() == AssetHandle<Assets::MaterialAsset>::kInvalidIndex)
            return std::nullopt;
        return MakeId(1000U + material.Index());
    };
}

[[nodiscard]] Wld::DirectionalLight MakeLight()
{
    // 默认方向光（固定 shadow volume）：光照数值不参与并行/串行差异。
    return Wld::DirectionalLight{};
}

// 可被编译器保留的自旋延时（volatile 计数阻止优化消除）：用于打散 chunk 完成顺序。
// 不用 sleep_for：Windows 未提升定时器精度时任何亚毫秒 sleep 实际睡满 ~15.6ms 节拍，
// 会让随机完成顺序用例从亚秒级膨胀到数分钟（M7-04 已记录同一系统行为）。
void SpinDelay(const std::uint32_t iterations) noexcept
{
    for (volatile std::uint32_t spin = 0; spin < iterations; ++spin)
    {
    }
}

[[nodiscard]] std::uint64_t Mix(std::uint64_t hash, const std::uint64_t value)
{
    return (hash ^ value) * 1099511628211ULL;
}

[[nodiscard]] std::uint64_t MixId(std::uint64_t hash, const AssetId& id)
{
    for (const std::byte byte : id.bytes)
        hash = Mix(hash, static_cast<std::uint64_t>(byte));
    return hash;
}

// 语义 hash：位精确（bit_cast）+ 排序序，逐 draw 覆盖全部语义字段；计时 microsecond 排除。
// 位精确而不是量化比较：串行与并行对同一输入走相同代码路径，结果必须逐位一致；
// 量化会掩盖真实的浮点分歧。
[[nodiscard]] std::uint64_t SemanticHash(const Wld::RenderPacket& packet)
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mixFloat = [&hash](const float value)
    { hash = Mix(hash, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value))); };
    const auto mixDraw = [&](const Wld::RenderDraw& draw)
    {
        hash = Mix(hash, draw.mesh.Index());
        hash = Mix(hash, draw.material.Index());
        hash = MixId(hash, draw.materialId);
        hash = MixId(hash, draw.meshId);
        for (const float element : draw.world.values)
            mixFloat(element);
        for (const float element : draw.normal.values)
            mixFloat(element);
        mixFloat(draw.worldBounds.center.x);
        mixFloat(draw.worldBounds.center.y);
        mixFloat(draw.worldBounds.center.z);
        mixFloat(draw.worldBounds.radius);
        hash = Mix(hash, draw.entityIndex);
        hash = Mix(hash, draw.mirrored ? 1U : 0U);
        hash = Mix(hash, draw.castsShadow ? 1U : 0U);
        hash = Mix(hash, draw.receivesShadow ? 1U : 0U);
    };
    for (const Wld::RenderDraw& draw : packet.mainOpaque)
        mixDraw(draw);
    for (const Wld::RenderDraw& draw : packet.shadowCasters)
        mixDraw(draw);

    const Wld::CullingStats& stats = packet.stats;
    hash = Mix(hash, stats.candidateObjects);
    hash = Mix(hash, stats.mainVisible);
    hash = Mix(hash, stats.mainCulled);
    hash = Mix(hash, stats.mainInside);
    hash = Mix(hash, stats.mainIntersecting);
    hash = Mix(hash, stats.shadowCandidates);
    hash = Mix(hash, stats.shadowVisible);
    hash = Mix(hash, stats.shadowCulled);
    hash = Mix(hash, stats.opaqueDrawCalls);
    hash = Mix(hash, stats.shadowDrawCalls);
    hash = Mix(hash, stats.trianglesSubmitted);
    return hash;
}

// 守恒与覆盖护栏：逐条不变量在串行与并行上都必须成立。
void ExpectStatsConserved(const Wld::RenderPacket& packet, const std::uint32_t expectedCandidates)
{
    EXPECT_EQ(packet.stats.candidateObjects, expectedCandidates);
    EXPECT_EQ(packet.stats.mainVisible + packet.stats.mainCulled, expectedCandidates);
    EXPECT_EQ(packet.stats.mainInside + packet.stats.mainIntersecting, packet.stats.mainVisible);
    EXPECT_EQ(packet.stats.shadowVisible + packet.stats.shadowCulled, packet.stats.shadowCandidates);
    EXPECT_EQ(static_cast<std::size_t>(packet.stats.opaqueDrawCalls), packet.mainOpaque.size());
    EXPECT_EQ(static_cast<std::size_t>(packet.stats.shadowDrawCalls), packet.shadowCasters.size());
}

struct BuilderFixture
{
    Wld::DirectionalLight light = MakeLight();
    Wld::MaterialInfoLookup materialInfo = MakeMaterialLookup();
    Wld::CullingOptions options{};
};

[[nodiscard]] MiniEngine::Tasks::TaskSystemConfig PerWorkerConfig(const std::uint32_t workers)
{
    MiniEngine::Tasks::TaskSystemConfig config;
    config.workerCount = workers;
    config.mode = MiniEngine::Tasks::SchedulerMode::PerWorkerDeque;
    return config;
}
} // namespace

// 边界尺寸 × workers × chunk 全 sweep：开启 validateAgainstSerial 后每个配置都会在
// Builder 内部用串行 oracle 做逐字段比对（第一个差异立即报告 entityIndex）。
TEST(RenderPacketDeterminismTests, ParallelMatchesSerialForBoundarySizes)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera camera = MakePerspectiveCamera();
    const Wld::MeshInfoLookup meshInfo = MakeMeshLookup();

    for (const std::uint32_t count : {0U, 1U, 31U, 32U, 33U, 255U, 256U, 257U, 5000U})
    {
        const std::vector<Wld::RenderItem> items = MakeItems(count, 6657);
        MiniEngine::Tasks::TaskSystem serialTasks(MiniEngine::Tasks::TaskSystemConfig{});
        MiniEngine::Render::RenderPacketBuilder serialOnly(serialTasks);
        const Wld::RenderPacket serial =
            serialOnly.BuildSerial(items, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo);
        const std::uint64_t expected = SemanticHash(serial);
        ExpectStatsConserved(serial, count);

        for (const std::uint32_t workers : {1U, 2U, 4U, 8U})
        {
            for (const std::uint32_t chunk : {32U, 64U, 128U, 256U, 512U, 1024U})
            {
                MiniEngine::Tasks::TaskSystem tasks(PerWorkerConfig(workers));
                MiniEngine::Render::RenderPacketBuilder builder(tasks);

                MiniEngine::Render::RenderPacketBuildConfig buildConfig;
                buildConfig.chunkSize = chunk;
                buildConfig.validateAgainstSerial = true;
                const Wld::RenderPacket parallel = builder.BuildParallel(
                    items, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo, buildConfig);

                EXPECT_EQ(SemanticHash(parallel), expected)
                    << "count=" << count << " workers=" << workers << " chunk=" << chunk;
                ExpectStatsConserved(parallel, count);

                // 覆盖护栏（A14）：可见集合不能退化为空，否则比对失去证明力。
                if (count >= 1000U)
                {
                    EXPECT_GE(static_cast<std::uint64_t>(parallel.stats.mainVisible) * 4U, count)
                        << "main visible coverage too low";
                    EXPECT_GT(parallel.stats.shadowVisible, 0U) << "shadow coverage empty";
                }
            }
        }
    }
}

// M7-RP-ORDER：k 路归并的两条路径都必须与串行 oracle 逐位一致。
// ① 键分布集中（全部 item 共用同一 mesh/material）→ 主键并列、排序由 entityIndex 决定，
//    归并的"整段移动"快路生效；
// ② 键分布混合（mesh %64 / material %16）→ 通用小根堆路径，逐元素比较。
// 两者都要在 chunk=32（~125 run）到 1024（~4 run）下与串行结果同 hash 且统计守恒。
TEST(RenderPacketDeterminismTests, MergePathsMatchSerialOracle)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera camera = MakePerspectiveCamera();
    const Wld::MeshInfoLookup meshInfo = MakeMeshLookup();

    // ① 集中键：mesh/material/mirrored 全同 → 排序键退化为 entityIndex，而每个 chunk 覆盖
    //    连续的 entityIndex 区间 → 各 run 键区间互不相交，"整段移动"快路必然生效。
    std::vector<Wld::RenderItem> uniform = MakeItems(4000, 6657);
    for (Wld::RenderItem& item : uniform)
    {
        item.mesh = AssetHandle<Assets::MeshAsset>(1U, 1U);
        item.material = AssetHandle<Assets::MaterialAsset>(1U, 1U);
        item.mirrored = false;
    }
    // ② 混合键：主键（mesh/material）在 chunk 之间交错 → 快路不成立，走通用小根堆路径。
    const std::vector<Wld::RenderItem> mixed = MakeItems(4000, 6657);

    // 夹具护栏：两个数据集必须真的处在"键集中/键混合"两端，否则用例只覆盖一条路径。
    const auto distinctMaterials = [](const std::vector<Wld::RenderItem>& items)
    {
        std::set<std::uint32_t> seen;
        for (const Wld::RenderItem& item : items)
            seen.insert(item.material.Index());
        return seen.size();
    };
    ASSERT_EQ(distinctMaterials(uniform), 1U);
    ASSERT_GT(distinctMaterials(mixed), 1U);

    struct Dataset final
    {
        const char* name;
        const std::vector<Wld::RenderItem>* items;
    };
    const Dataset datasets[]{{"uniform-keys", &uniform}, {"mixed-keys", &mixed}};

    for (const Dataset& dataset : datasets)
    {
        MiniEngine::Tasks::TaskSystem serialTasks(MiniEngine::Tasks::TaskSystemConfig{});
        MiniEngine::Render::RenderPacketBuilder serialOnly(serialTasks);
        const Wld::RenderPacket serial = serialOnly.BuildSerial(*dataset.items, camera, fixture.light, meshInfo,
                                                                fixture.options, fixture.materialInfo);
        const std::uint64_t expected = SemanticHash(serial);
        ExpectStatsConserved(serial, static_cast<std::uint32_t>(dataset.items->size()));

        for (const std::uint32_t chunk : {32U, 256U, 1024U})
        {
            MiniEngine::Tasks::TaskSystem tasks(PerWorkerConfig(8));
            MiniEngine::Render::RenderPacketBuilder builder(tasks);
            MiniEngine::Render::RenderPacketBuildConfig buildConfig;
            buildConfig.chunkSize = chunk;
            buildConfig.validateAgainstSerial = true;
            const Wld::RenderPacket parallel = builder.BuildParallel(
                *dataset.items, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo, buildConfig);

            EXPECT_EQ(SemanticHash(parallel), expected) << dataset.name << " chunk=" << chunk;
            ExpectStatsConserved(parallel, static_cast<std::uint32_t>(dataset.items->size()));
            EXPECT_GT(parallel.stats.mainVisible, 0U) << dataset.name << " chunk=" << chunk;
        }
    }
}

// 全可见 / 全裁剪：两条极端路径的逐字段一致性（0 可见时队列为空，也必须统计守恒）。
TEST(RenderPacketDeterminismTests, AllVisibleAndAllCulledMatchSerial)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera unitCamera = MakeUnitCamera();
    const Wld::MeshInfoLookup smallMeshes = MakeSmallMeshLookup();
    const Wld::MeshInfoLookup meshes = MakeMeshLookup();

    MiniEngine::Tasks::TaskSystem tasks(PerWorkerConfig(8));
    MiniEngine::Render::RenderPacketBuilder builder(tasks);

    {
        const std::vector<Wld::RenderItem> items = MakeTightItems(1500, 42);
        const Wld::RenderPacket serial =
            builder.BuildSerial(items, unitCamera, fixture.light, smallMeshes, fixture.options, fixture.materialInfo);
        ASSERT_EQ(serial.stats.mainVisible, 1500U);
        ASSERT_EQ(serial.stats.mainCulled, 0U);

        MiniEngine::Render::RenderPacketBuildConfig buildConfig;
        buildConfig.chunkSize = 128;
        buildConfig.validateAgainstSerial = true;
        const Wld::RenderPacket parallel = builder.BuildParallel(items, unitCamera, fixture.light, smallMeshes,
                                                                 fixture.options, fixture.materialInfo, buildConfig);
        EXPECT_EQ(SemanticHash(parallel), SemanticHash(serial));
        EXPECT_EQ(parallel.stats.mainVisible, 1500U);
    }

    {
        const std::vector<Wld::RenderItem> items = MakeFarItems(1500);
        const Wld::RenderPacket serial =
            builder.BuildSerial(items, unitCamera, fixture.light, meshes, fixture.options, fixture.materialInfo);
        ASSERT_EQ(serial.stats.mainVisible, 0U);
        ASSERT_EQ(serial.stats.mainCulled, 1500U);

        MiniEngine::Render::RenderPacketBuildConfig buildConfig;
        buildConfig.chunkSize = 128;
        buildConfig.validateAgainstSerial = true;
        const Wld::RenderPacket parallel = builder.BuildParallel(items, unitCamera, fixture.light, meshes,
                                                                 fixture.options, fixture.materialInfo, buildConfig);
        EXPECT_EQ(SemanticHash(parallel), SemanticHash(serial));
        EXPECT_EQ(parallel.stats.mainCulled, 1500U);
    }
}

// 随机完成顺序：lookup 的抖动让 chunk 完成时间随机（共享游标跨线程分配 slot），
// 合并必须只按 chunkIndex——输出 hash 与串行 oracle 相等且在 100 次运行中恒定。
TEST(RenderPacketDeterminismTests, CompletionOrderDoesNotChangePacket)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera camera = MakePerspectiveCamera();
    const std::vector<Wld::RenderItem> items = MakeItems(4000, 99);
    const std::vector<Wld::RenderItem> itemsSnapshot = items; // 串行 oracle 用无抖动输入

    std::vector<std::uint32_t> delays(4096);
    std::mt19937 random(6657);
    for (std::uint32_t& delay : delays)
        delay = random() % 6000U;

    std::atomic<std::uint32_t> queryCursor{0};
    const Wld::MeshInfoLookup baseLookup = MakeMeshLookup();
    const Wld::MeshInfoLookup jitteredLookup =
        [&queryCursor, &delays,
         &baseLookup](const AssetHandle<Assets::MeshAsset> mesh) -> std::optional<Wld::MeshDrawInfo>
    {
        const std::uint32_t slot = queryCursor.fetch_add(1, std::memory_order_relaxed) % delays.size();
        SpinDelay(delays[slot]);
        return baseLookup(mesh);
    };

    MiniEngine::Tasks::TaskSystem tasks(PerWorkerConfig(8));
    MiniEngine::Render::RenderPacketBuilder builder(tasks);
    const Wld::RenderPacket reference =
        builder.BuildSerial(itemsSnapshot, camera, fixture.light, baseLookup, fixture.options, fixture.materialInfo);
    const std::uint64_t expected = SemanticHash(reference);
    EXPECT_GE(static_cast<std::uint64_t>(reference.stats.mainVisible) * 4U, 4000U);

    for (std::uint32_t run = 0; run < 100; ++run)
    {
        queryCursor.store(0, std::memory_order_relaxed);
        MiniEngine::Render::RenderPacketBuildConfig buildConfig;
        buildConfig.chunkSize = 256;
        const Wld::RenderPacket packet = builder.BuildParallel(items, camera, fixture.light, jitteredLookup,
                                                               fixture.options, fixture.materialInfo, buildConfig);
        ASSERT_EQ(SemanticHash(packet), expected) << "run=" << run;
    }
}

// 100k 实体（M7-05 第 6 步的规模项）：全量可见集合在代表配置下与串行逐字段一致。
TEST(RenderPacketDeterminismTests, HundredThousandEntitiesMatchSerial)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera camera = MakePerspectiveCamera();
    const Wld::MeshInfoLookup meshInfo = MakeMeshLookup();
    const std::vector<Wld::RenderItem> items = MakeItems(100000, 20260917);

    MiniEngine::Tasks::TaskSystem serialTasks(MiniEngine::Tasks::TaskSystemConfig{});
    MiniEngine::Render::RenderPacketBuilder serialOnly(serialTasks);
    const Wld::RenderPacket serial =
        serialOnly.BuildSerial(items, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo);
    const std::uint64_t expected = SemanticHash(serial);
    ExpectStatsConserved(serial, 100000);
    ASSERT_GE(static_cast<std::uint64_t>(serial.stats.mainVisible) * 4U, 100000U);

    for (const std::uint32_t workers : {1U, 8U})
    {
        for (const std::uint32_t chunk : {256U, 1024U})
        {
            MiniEngine::Tasks::TaskSystem tasks(PerWorkerConfig(workers));
            MiniEngine::Render::RenderPacketBuilder builder(tasks);
            MiniEngine::Render::RenderPacketBuildConfig buildConfig;
            buildConfig.chunkSize = chunk;
            buildConfig.validateAgainstSerial = true;
            const Wld::RenderPacket parallel = builder.BuildParallel(
                items, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo, buildConfig);
            EXPECT_EQ(SemanticHash(parallel), expected) << "workers=" << workers << " chunk=" << chunk;
            ExpectStatsConserved(parallel, 100000);
        }
    }
}

// 有界队列背压：inject/全局队列容量被压到 1 时，超出的 chunk 必须走"提交失败 →
// 主线程同步执行"回退路径，结果与串行 oracle 完全一致（chunk slot 顺序不依赖提交方式）。
TEST(RenderPacketDeterminismTests, QueueFullBackpressureStillProducesSerialIdenticalPacket)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera camera = MakePerspectiveCamera();
    const Wld::MeshInfoLookup meshInfo = MakeMeshLookup();
    const std::vector<Wld::RenderItem> items = MakeItems(1500, 321);

    MiniEngine::Tasks::TaskSystem serialTasks(MiniEngine::Tasks::TaskSystemConfig{});
    MiniEngine::Render::RenderPacketBuilder serialOnly(serialTasks);
    const Wld::RenderPacket serial =
        serialOnly.BuildSerial(items, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo);
    const std::uint64_t expected = SemanticHash(serial);

    MiniEngine::Tasks::TaskSystemConfig config;
    config.workerCount = 2;
    config.injectQueueCapacity = 1; // 12 个 chunk 中绝大多数必须走同步回退
    MiniEngine::Tasks::TaskSystem tasks(config);
    MiniEngine::Render::RenderPacketBuilder builder(tasks);

    MiniEngine::Render::RenderPacketBuildConfig buildConfig;
    buildConfig.chunkSize = 128;
    buildConfig.validateAgainstSerial = true;
    MiniEngine::Render::RenderPacketBuildStats buildStats;
    const Wld::RenderPacket packet = builder.BuildParallel(items, camera, fixture.light, meshInfo, fixture.options,
                                                           fixture.materialInfo, buildConfig, &buildStats);
    EXPECT_EQ(SemanticHash(packet), expected);
    ExpectStatsConserved(packet, 1500);
    // 证明回退路径真的被执行（否则本用例会退化成"全部提交成功"的普通用例）。
    EXPECT_EQ(buildStats.chunkCount, 12U);
    EXPECT_GT(buildStats.syncChunks, 0U);
    EXPECT_EQ(buildStats.taskChunks + buildStats.syncChunks, buildStats.chunkCount);
    EXPECT_GT(buildStats.mergedDraws, 0U);
}

// Wait 返回后 items 快照即可释放：并行构建是同步的，Wait 返回即没有任务再访问输入。
TEST(RenderPacketDeterminismTests, ItemsMayBeReleasedAfterWaitReturns)
{
    BuilderFixture fixture;
    const Wld::RenderPacketCamera camera = MakePerspectiveCamera();
    const Wld::MeshInfoLookup meshInfo = MakeMeshLookup();
    MiniEngine::Tasks::TaskSystem tasks(PerWorkerConfig(4));
    MiniEngine::Render::RenderPacketBuilder builder(tasks);

    auto items = std::make_unique<std::vector<Wld::RenderItem>>(MakeItems(2000, 77));
    const std::uint64_t hashWhileAlive = [&]
    {
        MiniEngine::Render::RenderPacketBuildConfig buildConfig;
        buildConfig.chunkSize = 256;
        const Wld::RenderPacket packet = builder.BuildParallel(*items, camera, fixture.light, meshInfo, fixture.options,
                                                               fixture.materialInfo, buildConfig);
        return SemanticHash(packet);
    }();

    items.reset(); // Wait 已返回：释放快照必须安全（无任务仍在访问）

    // 再构建一次同内容的快照，哈希必须与第一次一致（确定性 + 生命周期双重证据）。
    const std::vector<Wld::RenderItem> again = MakeItems(2000, 77);
    MiniEngine::Render::RenderPacketBuildConfig buildConfig;
    buildConfig.chunkSize = 256;
    const Wld::RenderPacket packet = builder.BuildParallel(again, camera, fixture.light, meshInfo, fixture.options,
                                                           fixture.materialInfo, buildConfig);
    EXPECT_EQ(SemanticHash(packet), hashWhileAlive);

    const Wld::RenderPacket reference =
        builder.BuildSerial(again, camera, fixture.light, meshInfo, fixture.options, fixture.materialInfo);
    EXPECT_EQ(SemanticHash(packet), SemanticHash(reference));
}
