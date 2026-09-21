#include <MiniEngine/World/RenderQueueBuilder.h>

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Assets/MeshAsset.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

using MiniEngine::Assets::AssetHandle;
using MiniEngine::Assets::AssetId;
using MiniEngine::Assets::DeriveAssetId;
using MiniEngine::Assets::MeshAsset;
using MiniEngine::World::BuildLightViewProjection;
using MiniEngine::World::CullingOptions;
using MiniEngine::World::DirectionalLight;
using MiniEngine::World::Float3;
using MiniEngine::World::Frustum;
using MiniEngine::World::MaterialInfoLookup;
using MiniEngine::World::Matrix4;
using MiniEngine::World::MeshDrawInfo;
using MiniEngine::World::MeshInfoLookup;
using MiniEngine::World::NormalMatrixFromWorld;
using MiniEngine::World::RenderItem;
using MiniEngine::World::RenderPacket;
using MiniEngine::World::RenderPacketCamera;
using MiniEngine::World::Sphere;
using MiniEngine::World::VolumeRelation;

namespace
{
// 单位平移矩阵（row-major：平移位于最后一行前三个元素）。
Matrix4 Translation(const float x, const float y, const float z)
{
    Matrix4 matrix{};
    matrix.values[12] = x;
    matrix.values[13] = y;
    matrix.values[14] = z;
    return matrix;
}

// 构造带 mesh 句柄与位置的 RenderItem；网格信息由测试 lookup 提供身份与本地包围球。
// castsShadow 可选：06 篇 shadow queue 的过滤条件依赖每物体的投影标记。
RenderItem MakeItem(const AssetHandle<MeshAsset> mesh, const Matrix4& world, const bool castsShadow = true)
{
    RenderItem item{};
    item.mesh = mesh;
    item.world = world;
    item.castsShadow = castsShadow;
    return item;
}

MeshInfoLookup LookupFrom(std::map<std::uint32_t, MeshDrawInfo> byHandleIndex)
{
    return [info = std::move(byHandleIndex)](const AssetHandle<MeshAsset> handle) -> std::optional<MeshDrawInfo>
    {
        const auto found = info.find(handle.Index());
        return found != info.end() ? std::optional{found->second} : std::nullopt;
    };
}

MeshDrawInfo InfoFor(const char* uri, const float radius, const std::uint32_t indexCount = 0U)
{
    return MeshDrawInfo{DeriveAssetId(uri), Sphere{{0.0F, 0.0F, 0.0F}, radius}, indexCount};
}

RenderPacketCamera IdentityCamera()
{
    RenderPacketCamera camera{};
    camera.view = Matrix4{};
    camera.projection = Matrix4{};
    camera.worldPosition = {0.0F, 0.0F, -1.0F};
    return camera;
}
} // namespace

TEST(RenderQueueBuilderTests, MainFrustumCullingCountsAreConserved)
{
    // D3D 单位裁剪体（view=proj=identity）：x/y ∈ [-1,1]，z ∈ [0,1]。
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F)),  // Inside
        MakeItem(AssetHandle<MeshAsset>{2U, 1U}, Translation(-1.0F, 0.0F, 0.5F)), // Intersecting
        MakeItem(AssetHandle<MeshAsset>{3U, 1U}, Translation(0.0F, 0.0F, -0.2F)), // Outside
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/a", 0.1F, 36U)},
        {2U, InfoFor("mesh/b", 0.1F, 36U)},
        {3U, InfoFor("mesh/c", 0.05F, 36U)},
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom(std::move(info)), CullingOptions{});

    // 06 篇守恒不变量：candidateObjects = mainVisible + mainCulled。
    EXPECT_EQ(packet.stats.candidateObjects, 3U);
    EXPECT_EQ(packet.stats.mainVisible, 2U);
    EXPECT_EQ(packet.stats.mainCulled, 1U);
    EXPECT_EQ(packet.stats.mainVisible, packet.stats.mainInside + packet.stats.mainIntersecting);
    EXPECT_EQ(packet.mainOpaque.size(), 2U);
    // trianglesSubmitted 只统计进入主队列的物体（2 × 36/3 = 24 个三角形）。
    EXPECT_EQ(packet.stats.trianglesSubmitted, 24U);
    EXPECT_EQ(packet.stats.opaqueDrawCalls, packet.mainOpaque.size());
    EXPECT_EQ(packet.stats.shadowDrawCalls, packet.shadowCasters.size());
    ASSERT_EQ(packet.mainOpaque.size(), 2U);
    // DeriveAssetId 是哈希派生：a/b 的哈希字节序不可假设，只断言可见集合正确。
    const AssetId idA = DeriveAssetId("mesh/a");
    const AssetId idB = DeriveAssetId("mesh/b");
    EXPECT_TRUE(packet.mainOpaque[0].meshId == idA || packet.mainOpaque[1].meshId == idA);
    EXPECT_TRUE(packet.mainOpaque[0].meshId == idB || packet.mainOpaque[1].meshId == idB);

    // 固定光视锥覆盖原点邻域：主视锥剔除的 C 仍参与阴影（两次 culling 相互独立）。
    EXPECT_EQ(packet.shadowCasters.size(), 3U);
    EXPECT_EQ(packet.stats.shadowCandidates, 3U);
    EXPECT_EQ(packet.stats.shadowCulled, 0U);
}

TEST(RenderQueueBuilderTests, CullingDisabledKeepsEverything)
{
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F)),
        MakeItem(AssetHandle<MeshAsset>{2U, 1U}, Translation(0.0F, 0.0F, -0.2F)),
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/a", 0.1F, 36U)},
        {2U, InfoFor("mesh/c", 0.05F, 12U)},
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(
        items, IdentityCamera(), DirectionalLight{}, LookupFrom(std::move(info)), CullingOptions{false, false});

    // culling off：全部 candidate 进入队列，但 bounds/统计照常构建（06 篇「CLI」——
    // off 侧的 A/B 数据必须与 on 侧同口径，否则对比失去意义）。
    EXPECT_EQ(packet.stats.candidateObjects, 2U);
    EXPECT_EQ(packet.mainOpaque.size(), 2U);
    EXPECT_EQ(packet.stats.mainCulled, 0U);
    EXPECT_EQ(packet.stats.mainVisible, 2U);
    EXPECT_EQ(packet.stats.trianglesSubmitted, 12U + 4U);
    EXPECT_EQ(packet.shadowCasters.size(), 2U);
    EXPECT_EQ(packet.stats.shadowCulled, 0U);
    // 两个计时字段必须被填充（>0），否则 A/B 的 CPU overhead 无从记录。
    EXPECT_GT(packet.stats.cullingCpuMicroseconds, 0.0);
    EXPECT_GT(packet.stats.queueBuildCpuMicroseconds, 0.0);
    EXPECT_GE(packet.stats.queueBuildCpuMicroseconds, packet.stats.cullingCpuMicroseconds);
}

TEST(RenderQueueBuilderTests, ShadowCullingDropsObjectsOutsideLightVolume)
{
    // (1000,0,0) 远超固定光视锥（±20 横向、深度段 0.1..60），主视锥与光视锥都剔除。
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(1000.0F, 0.0F, 0.0F)),
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/far", 1.0F)},
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom(std::move(info)), CullingOptions{});

    EXPECT_EQ(packet.stats.mainCulled, 1U);
    EXPECT_EQ(packet.stats.shadowCandidates, 1U);
    EXPECT_EQ(packet.stats.shadowCulled, 1U);
    EXPECT_EQ(packet.stats.shadowVisible, 0U);
    EXPECT_TRUE(packet.mainOpaque.empty());
    EXPECT_TRUE(packet.shadowCasters.empty());
}

// 06 篇「两次 culling」：main 与 shadow 的开关必须可独立控制——
// 合用一个布尔时，"shadow queue 独立"这条契约无法验证（开关含义模糊）。
TEST(RenderQueueBuilderTests, ShadowCullingIsIndependentOfMainCulling)
{
    // 物体在固定光视锥外（(1000,0,0)），主视锥剔除关闭。
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(1000.0F, 0.0F, 0.0F)),
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/far", 1.0F, 36U)},
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(
        items, IdentityCamera(), DirectionalLight{}, LookupFrom(std::move(info)), CullingOptions{false, true});

    // main 关：物体留在主队列；shadow 开：同一物体被光视锥剔除。
    EXPECT_EQ(packet.mainOpaque.size(), 1U);
    EXPECT_EQ(packet.stats.mainCulled, 0U);
    EXPECT_EQ(packet.shadowCasters.size(), 0U);
    EXPECT_EQ(packet.stats.shadowCulled, 1U);
}

// shadow queue 的过滤条件是 castsShadow 标记（随 RenderItem 从 World 组件提取），
// 而非渲染端常量——false 的物体不进入 shadow 统计口径，也不进 shadow 队列。
TEST(RenderQueueBuilderTests, CastsShadowFalseExcludesObjectFromShadowQueueOnly)
{
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F), false),
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/a", 0.1F, 36U)},
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom(std::move(info)), CullingOptions{});

    // 主队列照常包含（阴影标记只影响 shadow queue）。
    EXPECT_EQ(packet.mainOpaque.size(), 1U);
    EXPECT_EQ(packet.stats.shadowCandidates, 0U);
    EXPECT_EQ(packet.stats.shadowVisible, 0U);
    EXPECT_EQ(packet.stats.shadowCulled, 0U);
    EXPECT_TRUE(packet.shadowCasters.empty());
}

// Intersecting 是"仍绘制"的细分：宁可多画，绝不漏剔除（06 篇「Sphere test」）。
TEST(RenderQueueBuilderTests, IntersectingObjectsAreCountedSeparatelyAndKept)
{
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F)),  // Inside
        MakeItem(AssetHandle<MeshAsset>{2U, 1U}, Translation(-1.0F, 0.0F, 0.5F)), // Intersecting
        MakeItem(AssetHandle<MeshAsset>{3U, 1U}, Translation(0.0F, 0.0F, -0.2F)), // Outside
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/a", 0.1F)},
        {2U, InfoFor("mesh/b", 0.1F)},
        {3U, InfoFor("mesh/c", 0.05F)},
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom(std::move(info)), CullingOptions{});

    EXPECT_EQ(packet.stats.mainInside, 1U);
    EXPECT_EQ(packet.stats.mainIntersecting, 1U);
    EXPECT_EQ(packet.stats.mainVisible, 2U);
    EXPECT_EQ(packet.mainOpaque.size(), 2U);
    // 守恒式的完整形态：visible = inside + intersecting。
    EXPECT_EQ(packet.stats.mainVisible, packet.stats.mainInside + packet.stats.mainIntersecting);
}

TEST(RenderQueueBuilderTests, SortsByMeshIdBytesNotHandleOrder)
{
    // 输入顺序 c/b/a（句柄 3/2/1）；输出必须按 AssetId 字节序，与句柄槽位分配无关。
    // 期望顺序由"输入 id 集合按 bytes 排序"导出，不假设任何具体哈希顺序。
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{3U, 1U}, Translation(0.0F, 0.0F, 0.5F)),
        MakeItem(AssetHandle<MeshAsset>{2U, 1U}, Translation(0.0F, 0.0F, 0.4F)),
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.3F)),
    };

    std::map<std::uint32_t, MeshDrawInfo> info{
        {1U, InfoFor("mesh/a", 0.1F)},
        {2U, InfoFor("mesh/b", 0.1F)},
        {3U, InfoFor("mesh/c", 0.1F)},
    };

    std::vector<AssetId> expected{DeriveAssetId("mesh/a"), DeriveAssetId("mesh/b"), DeriveAssetId("mesh/c")};
    std::sort(expected.begin(), expected.end(),
              [](const AssetId& left, const AssetId& right) { return left.bytes < right.bytes; });

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(
        items, IdentityCamera(), DirectionalLight{}, LookupFrom(std::move(info)), CullingOptions{false, false});

    ASSERT_EQ(packet.mainOpaque.size(), 3U);
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        EXPECT_EQ(packet.mainOpaque[index].meshId, expected[index]);
    }
}

TEST(RenderQueueBuilderTests, CopiesMaterialHandleAndResolvesStableMaterialId)
{
    std::vector<RenderItem> items{MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F))};
    items[0].material = MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MaterialAsset>{7U, 2U};

    const AssetId materialId = DeriveAssetId("material/blue");
    const MaterialInfoLookup materialInfo = [materialId](const auto handle) -> std::optional<AssetId>
    { return handle.Index() == 7U && handle.Generation() == 2U ? std::optional{materialId} : std::nullopt; };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom({{1U, InfoFor("mesh/a", 0.1F)}}),
                                                                     CullingOptions{}, materialInfo);

    ASSERT_EQ(packet.mainOpaque.size(), 1U);
    EXPECT_EQ(packet.mainOpaque[0].material.Index(), 7U);
    EXPECT_EQ(packet.mainOpaque[0].material.Generation(), 2U);
    EXPECT_EQ(packet.mainOpaque[0].materialId, materialId);
}

TEST(RenderQueueBuilderTests, SortsSameMeshByMaterialId)
{
    std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F)),
        MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.1F, 0.0F, 0.5F)),
    };
    items[0].material = MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MaterialAsset>{2U, 1U};
    items[1].material = MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MaterialAsset>{3U, 1U};
    const AssetId firstId = DeriveAssetId("material/first");
    const AssetId secondId = DeriveAssetId("material/second");
    const MaterialInfoLookup materialInfo = [firstId, secondId](const auto handle) -> std::optional<AssetId>
    { return handle.Index() == 2U ? std::optional{firstId} : std::optional{secondId}; };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom({{1U, InfoFor("mesh/a", 0.1F)}}),
                                                                     CullingOptions{}, materialInfo);

    ASSERT_EQ(packet.mainOpaque.size(), 2U);
    // 与生产排序键一致，按 AssetId 的原始无符号字节序判断期望顺序；
    // std::array<std::byte> 的库比较实现可能走不同的三路比较路径。
    const bool firstBeforeSecond = std::memcmp(firstId.bytes.data(), secondId.bytes.data(), firstId.bytes.size()) < 0;
    EXPECT_EQ(packet.mainOpaque[0].materialId, firstBeforeSecond ? firstId : secondId);
    EXPECT_EQ(packet.mainOpaque[1].materialId, firstBeforeSecond ? secondId : firstId);
}

TEST(RenderQueueBuilderTests, MissingMaterialInfoKeepsHandleAndInvalidMaterialId)
{
    std::vector<RenderItem> items{MakeItem(AssetHandle<MeshAsset>{1U, 1U}, Translation(0.0F, 0.0F, 0.5F))};
    items[0].material = MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MaterialAsset>{9U, 1U};
    const MaterialInfoLookup materialInfo = [](const auto) -> std::optional<AssetId> { return std::nullopt; };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom({{1U, InfoFor("mesh/a", 0.1F)}}),
                                                                     CullingOptions{}, materialInfo);

    ASSERT_EQ(packet.mainOpaque.size(), 1U);
    EXPECT_EQ(packet.mainOpaque[0].material.Index(), 9U);
    EXPECT_FALSE(packet.mainOpaque[0].materialId.IsValid());
}

TEST(RenderQueueBuilderTests, MissingMeshInfoStaysVisibleConservatively)
{
    // 句柄查询失败（payload 未就绪）：保守恒可见，不因数据缺失丢物体。
    const std::vector<RenderItem> items{
        MakeItem(AssetHandle<MeshAsset>{9U, 1U}, Translation(1000.0F, 0.0F, 0.0F)),
    };

    const RenderPacket packet = MiniEngine::World::BuildRenderPacket(items, IdentityCamera(), DirectionalLight{},
                                                                     LookupFrom({}), CullingOptions{});

    EXPECT_EQ(packet.mainOpaque.size(), 1U);
    EXPECT_EQ(packet.stats.mainCulled, 0U);
    // 缺失包围球不进 trianglesSubmitted 统计（索引数不可知），但队列照常包含。
    EXPECT_EQ(packet.stats.trianglesSubmitted, 0U);
    EXPECT_EQ(packet.shadowCasters.size(), 1U);
}

TEST(RenderQueueBuilderTests, NormalMatrixForTranslationIsIdentity)
{
    const Matrix4 normal = NormalMatrixFromWorld(Translation(3.0F, -4.0F, 5.0F));

    EXPECT_FLOAT_EQ(normal.values[0], 1.0F);
    EXPECT_FLOAT_EQ(normal.values[5], 1.0F);
    EXPECT_FLOAT_EQ(normal.values[10], 1.0F);
    EXPECT_FLOAT_EQ(normal.values[1], 0.0F);
    EXPECT_FLOAT_EQ(normal.values[4], 0.0F);
    EXPECT_FLOAT_EQ(normal.values[12], 0.0F);
    EXPECT_FLOAT_EQ(normal.values[15], 1.0F);
}

TEST(RenderQueueBuilderTests, NormalMatrixForNonUniformScaleIsInverseTranspose)
{
    Matrix4 world = Matrix4{};
    world.values[0] = 2.0F;  // x 缩放 2
    world.values[5] = 3.0F;  // y 缩放 3
    world.values[10] = 4.0F; // z 缩放 4

    const Matrix4 normal = NormalMatrixFromWorld(world);

    EXPECT_FLOAT_EQ(normal.values[0], 0.5F);
    EXPECT_NEAR(normal.values[5], 1.0F / 3.0F, 1.0e-6F);
    EXPECT_FLOAT_EQ(normal.values[10], 0.25F);
    // 对角缩放的逆 = 转置，非对角分量保持 0。
    EXPECT_FLOAT_EQ(normal.values[1], 0.0F);
    EXPECT_FLOAT_EQ(normal.values[4], 0.0F);
}

TEST(RenderQueueBuilderTests, NormalMatrixForShearIsInverseTranspose)
{
    // 剪切 fixture 是能暴露"忘了转置"的最小用例：对角矩阵的 A⁻¹ 与 (A⁻¹)ᵀ
    // 相同，只测对角缩放结构上无法发现转置缺失（M4-01 审查 P1 的教训）。
    // world row0 = [1,2,0,0]：t=(1,0,0) → t'=(1,2,0)；正确的 n' = (-2,1,0)，
    // n'·t' = 0。若错误地返回 A⁻¹ 本身，n' = (0,1,0)，n'·t' = 2 ≠ 0。
    Matrix4 world = Matrix4{};
    world.values[1] = 2.0F;

    const Matrix4 normal = NormalMatrixFromWorld(world);
    EXPECT_FLOAT_EQ(normal.values[1], 0.0F);  // N[0][1] = A⁻¹[1][0] = 0
    EXPECT_FLOAT_EQ(normal.values[4], -2.0F); // N[1][0] = A⁻¹[0][1] = -2
}

TEST(RenderQueueBuilderTests, RejectsSingularWorldMatrix)
{
    Matrix4 world = Matrix4{};
    world.values[0] = 0.0F; // x 基坍缩 → 3x3 奇异

    // [[nodiscard]] 返回值在 EXPECT_THROW 内显式丢弃（C4834 /W4 /WX）。
    EXPECT_THROW(static_cast<void>(NormalMatrixFromWorld(world)), std::runtime_error);
}

TEST(RenderQueueBuilderTests, LightProjectionContainsOriginRegion)
{
    const Frustum lightFrustum = Frustum::FromRowVectorDirect3D(BuildLightViewProjection(DirectionalLight{}));

    // 原点邻域在固定光视锥内；远超 ±20 横向范围的球在外。
    EXPECT_EQ(lightFrustum.Classify(Sphere{{0.0F, 0.0F, 0.0F}, 5.0F}), VolumeRelation::Inside);
    EXPECT_EQ(lightFrustum.Classify(Sphere{{1000.0F, 0.0F, 0.0F}, 1.0F}), VolumeRelation::Outside);
    EXPECT_EQ(lightFrustum.Classify(Sphere{{0.0F, 1000.0F, 0.0F}, 1.0F}), VolumeRelation::Outside);
}

TEST(RenderQueueBuilderTests, LightProjectionOffsetsForAsymmetricVolume)
{
    // off-center 偏移项的端到端验证。注意不能直接断言 VP 矩阵的 values[12]——
    // BuildLightViewProjection 返回 view×projection 乘积，该位混入 view 平移贡献。
    //
    // 手算 golden（默认光 directionToLight=(1,2,1)/√6）：
    //   xAxis = normalize(cross(up, f)) = (-1/√2, 0, 1/√2)
    //   eye   = toLight·30 = (30/√6, 60/√6, 30/√6)
    // 世界点 p = (-35/√2, 0, 35/√2) 的光空间坐标恰为 (x_v=35, y_v=0, z_v=30)。
    // 非对称盒（left=0, right=40，横向中心 20）：x_clip = 35·2/40 − 1 = 0.75
    // → 小球 Inside；若缺偏移项（审查 P2 的缺陷），x_clip = 1.75 → Outside。
    // 同一点在对称盒（±20）下 x_v=35 超出右界 → Outside（对照）。
    DirectionalLight asymmetric{};
    asymmetric.shadowLeft = 0.0F;
    asymmetric.shadowRight = 40.0F;
    const Frustum asymmetricFrustum = Frustum::FromRowVectorDirect3D(BuildLightViewProjection(asymmetric));
    EXPECT_EQ(asymmetricFrustum.Classify(Sphere{{-24.7487373F, 0.0F, 24.7487373F}, 0.1F}), VolumeRelation::Inside);

    const Frustum symmetricFrustum = Frustum::FromRowVectorDirect3D(BuildLightViewProjection(DirectionalLight{}));
    EXPECT_EQ(symmetricFrustum.Classify(Sphere{{-24.7487373F, 0.0F, 24.7487373F}, 0.1F}), VolumeRelation::Outside);
}

TEST(RenderQueueBuilderTests, RejectsDegenerateLightDirection)
{
    DirectionalLight light{};
    light.directionToLight = {0.0F, 0.0F, 0.0F};

    EXPECT_THROW(static_cast<void>(BuildLightViewProjection(light)), std::runtime_error);
}

// ---------- M4-03：shadow space 坐标映射的 CPU 参考 ----------

namespace
{
// row-vector 变换：p' = p * M（点，含 w=1 平移项）；out[j] = Σ_i p[i]*values[i*4+j]。
Float3 TransformPoint(const Matrix4& matrix, const Float3& point)
{
    const float p[4] = {point.x, point.y, point.z, 1.0F};
    Float3 out{};
    out.x = p[0] * matrix.values[0] + p[1] * matrix.values[4] + p[2] * matrix.values[8] + p[3] * matrix.values[12];
    out.y = p[0] * matrix.values[1] + p[1] * matrix.values[5] + p[2] * matrix.values[9] + p[3] * matrix.values[13];
    out.z = p[0] * matrix.values[2] + p[1] * matrix.values[6] + p[2] * matrix.values[10] + p[3] * matrix.values[14];
    return out;
}

// D3D clip → shadow map UV（与 ShadowSampling.hlsli 逐字对应的 CPU 参考）。
// uv.y = -ndc.y*0.5 + 0.5 是 Y flip 契约；depth 保持 [0,1]，不做 [-1,1] remap。
float ShadowMapU(const float ndcX)
{
    return ndcX * 0.5F + 0.5F;
}

float ShadowMapV(const float ndcY)
{
    return -ndcY * 0.5F + 0.5F;
}
} // namespace

TEST(RenderQueueBuilderTests, LightViewProjectionKeepsSceneCenterInsideDepthRange)
{
    // 原点位于固定 shadow volume 中心区：eye = toLight*30，深度段 [0.1, 60]，
    // 故原点的 clip depth = (30-0.1)/59.9 —— D3D 深度 [0,1]（03 篇硬契约，
    // 错用 OpenGL [-1,1] remap 会让 receiver depth debug 立即暴露）。
    const Matrix4 lightViewProjection = BuildLightViewProjection(DirectionalLight{});
    const Float3 clip = TransformPoint(lightViewProjection, Float3{0.0F, 0.0F, 0.0F});

    ASSERT_GT(clip.z, 0.0F); // w=1（正交投影），clip.z 即 ndc.z
    EXPECT_NEAR(clip.z, 29.9F / 59.9F, 1.0e-4F);
    EXPECT_LE(clip.z, 1.0F);

    // 对称 volume（±20）下光轴上的原点投影到 NDC/UV 中心。
    EXPECT_NEAR(clip.x, 0.0F, 1.0e-4F);
    EXPECT_NEAR(clip.y, 0.0F, 1.0e-4F);
    EXPECT_NEAR(ShadowMapU(clip.x), 0.5F, 1.0e-4F);
    EXPECT_NEAR(ShadowMapV(clip.y), 0.5F, 1.0e-4F);
}

TEST(RenderQueueBuilderTests, ShadowUvMappingFlipsVerticalAxis)
{
    // Y flip 的单调性 golden：NDC y 越大 → 贴图 V 越小（首行对应 NDC 顶端）。
    // 错误写法 uv.y = ndc.y*0.5+0.5 会让 shadow 上下镜像（03 篇必测故障表第 1 行）。
    EXPECT_NEAR(ShadowMapV(0.5F), 0.25F, 1.0e-6F);
    EXPECT_NEAR(ShadowMapV(-0.5F), 0.75F, 1.0e-6F);
    EXPECT_LT(ShadowMapV(0.9F), ShadowMapV(-0.9F));
    // U 轴不翻转（Y flip 只在 V）。
    EXPECT_NEAR(ShadowMapU(0.5F), 0.75F, 1.0e-6F);
}
