// ============================================================================
// CullingSceneTests.cpp — M4-06 固定 seed 1000 实例 culling baseline
// 里程碑：M4（06 篇「Fixed culling scene」；手抄清单第 1、3 条）
// 职责：用固定 seed 的确定性生成器构造 06 篇要求的六组实例（视锥内 / 跨平面 /
//       视锥外 / 含相机大球 / 负与非均匀缩放 / 屏幕外但在光视锥内的 caster），
//       把**期望分类数量写死在测试里**（不依赖 runtime 随机），并验证：
//         1. 分组计数与 golden 一致（candidate = visible + culled 等守恒式）；
//         2. culling off 时全部 candidate 进队列、统计口径不变；
//         3. 同一输入连续 100 帧 queue 顺序 hash 一致（截图可复现的根据）；
//         4. Handle 槽位改变但 AssetId 不变时，顺序不变（排序键不能用 Handle）。
// 场景相机用 identity view/projection（= D3D 单位裁剪体），使每个分组的
// 内/外判定可以**解析推导**，而不是靠观察输出反推期望值。
// 关联：docs/architecture/README.md「Fixed culling scene」
//       engine/world/src/RenderQueueBuilder.cpp（被测实现）
//       tests/world/FrustumTests.cpp（平面提取与球分类的数学 golden）
// ============================================================================

#include <MiniEngine/World/RenderQueueBuilder.h>

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Assets/MeshAsset.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using MiniEngine::Assets::AssetHandle;
using MiniEngine::Assets::AssetId;
using MiniEngine::Assets::DeriveAssetId;
using MiniEngine::Assets::MaterialAsset;
using MiniEngine::Assets::MeshAsset;
using MiniEngine::World::BuildRenderPacket;
using MiniEngine::World::CullingOptions;
using MiniEngine::World::DirectionalLight;
using MiniEngine::World::Float3;
using MiniEngine::World::Matrix4;
using MiniEngine::World::MeshDrawInfo;
using MiniEngine::World::MeshInfoLookup;
using MiniEngine::World::RenderItem;
using MiniEngine::World::RenderPacket;
using MiniEngine::World::RenderPacketCamera;
using MiniEngine::World::Sphere;

namespace
{
// 分组规模（合计 1000）。分组边界的选择让每组的分类结果都可以解析推导：
//   A 全内（400）         ：|x|,|y| ≤ 0.8、z ∈ [0.15, 0.85]、r = 0.02 → Inside
//   B 跨侧平面（200）     ：|x| = 1.0、r = 0.05 → 与 x=±1 平面相交 → Intersecting
//   C1 相机外/光内（150） ：|x| = 1.5 → 相机 Outside；|p| < 18 → 光视锥 Inside
//   C2 双外（150）        ：(±60,±60,±60) 角点 → 相机与光视锥都 Outside
//   D 含相机大球（1）     ：center 单位体中心、r = 10 → 恒可见
//   E 负/非均匀缩放（50） ：放在 A 的深处，保守球半径放大但仍完全在体内
//   F 屏幕外 caster（49） ：(±10,±10,z_k)，相机 Outside、光视锥 Inside
constexpr std::uint32_t kGroupInside = 400U;
constexpr std::uint32_t kGroupCrossing = 200U;
constexpr std::uint32_t kGroupCameraOutsideLightInside = 150U;
constexpr std::uint32_t kGroupOutsideBoth = 150U;
constexpr std::uint32_t kGroupCameraSphere = 1U;
constexpr std::uint32_t kGroupScaled = 50U;
constexpr std::uint32_t kGroupOffscreenCaster = 49U;
constexpr std::uint32_t kTotalInstances = kGroupInside + kGroupCrossing + kGroupCameraOutsideLightInside +
                                          kGroupOutsideBoth + kGroupCameraSphere + kGroupScaled + kGroupOffscreenCaster;
static_assert(kTotalInstances == 1000U, "06 篇固定场景必须恰好 1000 个实例");

// 单位平移矩阵（row-major：平移在最后一行前三个分量）。
Matrix4 Translation(const float x, const float y, const float z)
{
    Matrix4 matrix{};
    matrix.values[12] = x;
    matrix.values[13] = y;
    matrix.values[14] = z;
    return matrix;
}

// 对角缩放矩阵（允许负值——镜像 fixture）。
Matrix4 Scale(const float x, const float y, const float z)
{
    Matrix4 matrix{};
    matrix.values[0] = x;
    matrix.values[5] = y;
    matrix.values[10] = z;
    matrix.values[15] = 1.0F;
    return matrix;
}

Matrix4 MultiplyRows(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result{};
    for (std::uint32_t row = 0; row < 4; ++row)
    {
        for (std::uint32_t column = 0; column < 4; ++column)
        {
            float sum = 0.0F;
            for (std::uint32_t k = 0; k < 4; ++k)
            {
                sum += left.values[static_cast<std::size_t>(row) * 4U + k] *
                       right.values[static_cast<std::size_t>(k) * 4U + column];
            }
            result.values[static_cast<std::size_t>(row) * 4U + column] = sum;
        }
    }
    return result;
}

// identity view/projection 相机：世界坐标即 clip 坐标（D3D 单位裁剪体），
// 使"某个位置是否可见"可以按 |x|,|y| ≤ 1、z ∈ [0,1] 直接推导。
RenderPacketCamera IdentityCamera()
{
    RenderPacketCamera camera{};
    camera.view = Matrix4{};
    camera.projection = Matrix4{};
    camera.worldPosition = {0.0F, 0.0F, -1.0F};
    return camera;
}

// 固定 seed 线性同余生成器（确定性：跨平台/跨运行逐位一致，绝不依赖 rand()）。
class DeterministicRandom final
{
  public:
    explicit DeterministicRandom(const std::uint32_t seed) : m_state{seed}
    {
    }

    // [0, 1) 均匀分布。
    [[nodiscard]] float NextUnit()
    {
        m_state = m_state * 1664525U + 1013904223U; // Numerical Recipes 常数
        return static_cast<float>(m_state >> 8U) / static_cast<float>(1U << 24U);
    }

    // [low, high] 均匀分布。
    [[nodiscard]] float NextRange(const float low, const float high)
    {
        return low + NextUnit() * (high - low);
    }

  private:
    std::uint32_t m_state;
};

// 构造 06 篇固定场景的 1000 个 RenderItem（顺序即 entityIndex，确定性）。
std::vector<RenderItem> MakeCullingScene()
{
    std::vector<RenderItem> items;
    items.reserve(kTotalInstances);
    DeterministicRandom random{20260908U}; // 固定 seed：日期 + 里程碑，便于溯源

    // A：全内 400。
    for (std::uint32_t index = 0; index < kGroupInside; ++index)
    {
        items.push_back(RenderItem{
            Translation(random.NextRange(-0.8F, 0.8F), random.NextRange(-0.8F, 0.8F), random.NextRange(0.15F, 0.85F)),
            AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});
    }

    // B：跨侧平面 200（|x| = 1 与左右平面相交，z/y 在体内深处）。
    for (std::uint32_t index = 0; index < kGroupCrossing; ++index)
    {
        const float x = index % 2U == 0U ? 1.0F : -1.0F;
        items.push_back(RenderItem{Translation(x, random.NextRange(-0.5F, 0.5F), random.NextRange(0.15F, 0.85F)),
                                   AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});
    }

    // C1：相机外 / 光视锥内 150（|x| = 1.5 已在单位体外；|p| ≈ 1.6 仍在光视锥内）。
    for (std::uint32_t index = 0; index < kGroupCameraOutsideLightInside; ++index)
    {
        const float x = index % 2U == 0U ? 1.5F : -1.5F;
        items.push_back(RenderItem{Translation(x, random.NextRange(-0.5F, 0.5F), random.NextRange(0.15F, 0.85F)),
                                   AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});
    }

    // C2：双外 150（±60 角点：|p| ≥ 60 或横向远超光视锥 ±20）。
    for (std::uint32_t index = 0; index < kGroupOutsideBoth; ++index)
    {
        const float x = index % 2U == 0U ? 60.0F : -60.0F;
        const float y = (index / 2U) % 2U == 0U ? 60.0F : -60.0F;
        const float z = (index / 4U) % 2U == 0U ? 60.0F : -60.0F;
        items.push_back(RenderItem{Translation(x, y, z), AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});
    }

    // D：含相机大球 1（单位体裁剪体整体落在球内 → 恒可见）。
    items.push_back(
        RenderItem{Translation(0.0F, 0.0F, 0.5F), AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});

    // E：负 / 非均匀缩放 50（放在 A 的深处；保守球半径放大但仍在体内）。
    for (std::uint32_t index = 0; index < kGroupScaled; ++index)
    {
        const float scaleX = index % 2U == 0U ? -2.0F : 2.5F; // 一半镜像
        const float scaleY = 0.5F + random.NextUnit() * 1.5F;
        // row-vector 组合顺序：p * (S*T) = (p*S)*T —— 先缩放再平移（T*S 会把平移
        // 行乘进缩放，实例会落回原点附近，分类结果随之改变——06 篇固定场景的
        // 期望计数依赖正确的组合顺序）。
        const Matrix4 world = MultiplyRows(
            Scale(scaleX, scaleY, 1.5F),
            Translation(random.NextRange(-0.3F, 0.3F), random.NextRange(-0.3F, 0.3F), random.NextRange(0.3F, 0.7F)));
        items.push_back(RenderItem{world, AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});
    }

    // F：屏幕外 caster 49（|x|=|y|=10 必在单位体外；光视锥横向 ±20、深度 0.1..60 覆盖）。
    for (std::uint32_t index = 0; index < kGroupOffscreenCaster; ++index)
    {
        const float x = index % 2U == 0U ? 10.0F : -10.0F;
        const float y = (index / 2U) % 2U == 0U ? 10.0F : -10.0F;
        const float z = -10.0F + static_cast<float>(index % 21U); // z ∈ [-10, 10]
        items.push_back(RenderItem{Translation(x, y, z), AssetHandle<MeshAsset>{1U, 1U}, AssetHandle<MaterialAsset>{}});
    }

    return items;
}

// 所有实例共用同一 mesh（同 AssetId）与 36 索引（12 三角形）。
MeshInfoLookup MakeLookup()
{
    const MeshDrawInfo info{DeriveAssetId("mesh/culling-instance"), Sphere{{0.0F, 0.0F, 0.0F}, 0.05F}, 36U};
    return [info](const AssetHandle<MeshAsset>&) -> std::optional<MeshDrawInfo> { return info; };
}

// meshId 字节序列的 FNV-1a hash：queue 顺序稳定性的紧凑指纹。
std::uint64_t QueueOrderHash(const RenderPacket& packet)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& draw : packet.mainOpaque)
    {
        for (const std::byte byte : draw.meshId.bytes)
        {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 1099511628211ULL;
        }
        hash ^= draw.entityIndex;
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace

// golden 计数：每个数字都由分组定义解析推导（推导式见文件头注释），
// 不是"跑一次记一次"的快照——生成器是固定 seed，任何一侧漂移都会失败。
TEST(CullingSceneTests, ClassifiesFixedThousandInstanceSceneWithExpectedCounts)
{
    const std::vector<RenderItem> items = MakeCullingScene();
    ASSERT_EQ(items.size(), kTotalInstances);

    const RenderPacket packet =
        BuildRenderPacket(items, IdentityCamera(), DirectionalLight{}, MakeLookup(), CullingOptions{});

    // 守恒不变量（06 篇「统计」硬性要求）。
    EXPECT_EQ(packet.stats.candidateObjects, kTotalInstances);
    EXPECT_EQ(packet.stats.candidateObjects, packet.stats.mainVisible + packet.stats.mainCulled);
    EXPECT_EQ(packet.stats.shadowCandidates, packet.stats.shadowVisible + packet.stats.shadowCulled);
    EXPECT_EQ(packet.stats.mainVisible, packet.stats.mainInside + packet.stats.mainIntersecting);

    // 分组 golden：
    //   可见 = A(400) + B(200) + D(1) + E(50) = 651；剔除 = C1(150) + C2(150) + F(49) = 349。
    EXPECT_EQ(packet.stats.mainVisible, 651U);
    EXPECT_EQ(packet.stats.mainCulled, 349U);
    //   Inside = A(400) + D(1) + E(50) = 451（E 的保守球仍在单位体内）；
    //   Intersecting = B(200)。
    EXPECT_EQ(packet.stats.mainInside, 451U);
    EXPECT_EQ(packet.stats.mainIntersecting, 200U);
    //   shadow：全部默认投影；C2(150) 在光视锥外，其余 850 可见（含屏幕外 caster F）。
    EXPECT_EQ(packet.stats.shadowCandidates, 1000U);
    EXPECT_EQ(packet.stats.shadowVisible, 850U);
    EXPECT_EQ(packet.stats.shadowCulled, 150U);
    //   队列尺寸与三角形数：可见 651 × 12 三角形 = 7812。
    EXPECT_EQ(packet.stats.opaqueDrawCalls, 651U);
    EXPECT_EQ(packet.stats.shadowDrawCalls, 850U);
    EXPECT_EQ(packet.stats.trianglesSubmitted, 651U * 12U);

    // 屏幕外 caster 必须保留在 shadow queue（06 篇：不能复用 main visible list）。
    EXPECT_EQ(packet.mainOpaque.size(), 651U);
    EXPECT_EQ(packet.shadowCasters.size(), 850U);
}

// culling off：全部 candidate 进队列（A/B 的 off 侧口径），统计与计时照常产出。
TEST(CullingSceneTests, CullingOffSubmitsEverythingWithSameStatsContract)
{
    const std::vector<RenderItem> items = MakeCullingScene();

    const RenderPacket packet =
        BuildRenderPacket(items, IdentityCamera(), DirectionalLight{}, MakeLookup(), CullingOptions{false, false});

    EXPECT_EQ(packet.stats.candidateObjects, 1000U);
    EXPECT_EQ(packet.stats.mainVisible, 1000U);
    EXPECT_EQ(packet.stats.mainCulled, 0U);
    EXPECT_EQ(packet.stats.shadowVisible, 1000U);
    EXPECT_EQ(packet.stats.shadowCulled, 0U);
    EXPECT_EQ(packet.stats.opaqueDrawCalls, 1000U);
    // off 侧三角形 = 1000 × 12（on 侧 7812 —— A/B 的 GPU work 差异来源）。
    EXPECT_EQ(packet.stats.trianglesSubmitted, 12000U);
    EXPECT_GT(packet.stats.cullingCpuMicroseconds, 0.0);
    EXPECT_GT(packet.stats.queueBuildCpuMicroseconds, 0.0);
}

// 同一输入连续 100 帧：queue 顺序 hash 必须逐位一致（截图可复现的根据）。
TEST(CullingSceneTests, QueueOrderIsStableAcrossOneHundredBuilds)
{
    const std::vector<RenderItem> items = MakeCullingScene();
    const MeshInfoLookup lookup = MakeLookup();

    const std::uint64_t golden =
        QueueOrderHash(BuildRenderPacket(items, IdentityCamera(), DirectionalLight{}, lookup, CullingOptions{}));
    for (int frame = 0; frame < 100; ++frame)
    {
        const std::uint64_t hash =
            QueueOrderHash(BuildRenderPacket(items, IdentityCamera(), DirectionalLight{}, lookup, CullingOptions{}));
        ASSERT_EQ(hash, golden) << "frame " << frame << " 的 Draw 顺序与第 0 帧不一致";
    }
}

// Handle 槽位（进程内 index）改变但 AssetId 不变：顺序必须不变——
// 这正是排序键禁止使用 Handle index 的原因（06 篇「Render Queue」）。
//
// 用例设计（M4-06 审查修正）：**每个实例的 AssetId 互不相同**，否则排序结果由
// entityIndex 决定、与排序键是否消费 Handle 无关，测试成为同义反复。两套 lookup
// 给同一批 item 分配**不同**的 Handle 槽位（slot 平移 13 取模），但都解析到与
// item 位置绑定的同一 AssetId——若实现错误地把 Handle.Index() 混入排序键，
// 两套分配的顺序必然不同，hash 对比立刻暴露。
TEST(CullingSceneTests, QueueOrderIgnoresHandleSlotAssignment)
{
    constexpr std::uint32_t kInstanceCount = 24U;
    // 两套 Handle 槽位分配：A = 恒等（slot = itemIndex + 1），B = +13 的循环置换。
    const auto slotOf = [kInstanceCount](const std::uint32_t itemIndex, const bool shifted)
    { return shifted ? ((itemIndex + 13U) % kInstanceCount) + 1U : itemIndex + 1U; };

    // item 位置（= entityIndex）与 AssetId 绑定；两套分配只有 Handle 槽位不同。
    const auto buildItems = [slotOf](const bool shifted)
    {
        std::vector<RenderItem> items;
        items.reserve(kInstanceCount);
        for (std::uint32_t index = 0; index < kInstanceCount; ++index)
        {
            items.push_back(RenderItem{Translation(static_cast<float>(index), 0.0F, 0.5F),
                                       AssetHandle<MeshAsset>{slotOf(index, shifted), 1U},
                                       AssetHandle<MaterialAsset>{}});
        }
        return items;
    };

    // lookup 按 Handle 槽位反查 item 位置（与 buildItems 的分配互逆）。
    const auto makeLookup = [slotOf, kInstanceCount](const bool shifted)
    {
        return [slotOf, kInstanceCount, shifted](const AssetHandle<MeshAsset>& handle) -> std::optional<MeshDrawInfo>
        {
            std::uint32_t itemIndex = 0;
            const bool found = [&]()
            {
                for (std::uint32_t index = 0; index < kInstanceCount; ++index)
                {
                    if (slotOf(index, shifted) == handle.Index())
                    {
                        itemIndex = index;
                        return true;
                    }
                }
                return false;
            }();
            if (!found)
            {
                return std::nullopt;
            }
            return MeshDrawInfo{DeriveAssetId("mesh/slot-perm-" + std::to_string(itemIndex)),
                                Sphere{{0.0F, 0.0F, 0.0F}, 0.05F}, 36U};
        };
    };

    const std::uint64_t hashA = QueueOrderHash(BuildRenderPacket(
        buildItems(false), IdentityCamera(), DirectionalLight{}, makeLookup(false), CullingOptions{false, false}));
    const std::uint64_t hashB = QueueOrderHash(BuildRenderPacket(buildItems(true), IdentityCamera(), DirectionalLight{},
                                                                 makeLookup(true), CullingOptions{false, false}));
    EXPECT_EQ(hashA, hashB) << "排序键混入 Handle.Index() 会随槽位分配改变 Draw 顺序";

    // 正向锚点：顺序必须是 AssetId 字节序（而不是输入顺序的恒等——否则即使实现
    // 消费 Handle，恒等分配也可能碰巧掩盖缺陷）。
    std::vector<AssetId> expected;
    expected.reserve(kInstanceCount);
    for (std::uint32_t index = 0; index < kInstanceCount; ++index)
    {
        expected.push_back(DeriveAssetId("mesh/slot-perm-" + std::to_string(index)));
    }
    std::sort(expected.begin(), expected.end(),
              [](const AssetId& left, const AssetId& right) { return left.bytes < right.bytes; });

    const RenderPacket packet = BuildRenderPacket(buildItems(true), IdentityCamera(), DirectionalLight{},
                                                  makeLookup(true), CullingOptions{false, false});
    ASSERT_EQ(packet.mainOpaque.size(), kInstanceCount);
    for (std::uint32_t index = 0; index < kInstanceCount; ++index)
    {
        EXPECT_EQ(packet.mainOpaque[index].meshId, expected[index]) << "position " << index;
    }
}
