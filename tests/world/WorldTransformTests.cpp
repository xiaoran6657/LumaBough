// ============================================================================
// WorldTransformTests.cpp — World 实体/层级/变换与组件的契约
// 里程碑：M3（06 篇）
// 职责：验证世代失效、subtree 销毁、parent-before-child 的 dirty 传播、环与断链
//       拒绝、kMaxHierarchyDepth 预算、TRS golden 手算对照（row-vector/LH）、
//       mirrored 标记与组件校验、RenderItem 确定性顺序。
// 关联：engine/world/src/World.cpp（被测实现）
//       docs/architecture/README.md
// ============================================================================

#include <MiniEngine/World/World.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace
{
MiniEngine::World::Matrix4 Translation(const float x, const float y, const float z)
{
    MiniEngine::World::Matrix4 result{};
    result.values[12] = x;
    result.values[13] = y;
    result.values[14] = z;
    return result;
}

MiniEngine::World::Quaternion RotationY90() // 绕 +Y 旋转 +90°（左手系：+Z 转向 +X）
{
    const float half = std::sqrt(2.0F) / 2.0F;
    return MiniEngine::World::Quaternion{0.0F, half, 0.0F, half};
}

void ExpectMatrixNear(const MiniEngine::World::Matrix4& expected, const MiniEngine::World::Matrix4& actual)
{
    for (std::size_t i = 0; i < expected.values.size(); ++i)
    {
        // TRS compose 会留下 ~1e-7 的 float 舍入残差，必须用 NEAR 而不是 FLOAT_EQ。
        EXPECT_NEAR(expected.values[i], actual.values[i], 1.0e-5F) << "component " << i;
    }
}

MiniEngine::World::MeshRendererComponent Renderer(const std::uint32_t meshIndex, const std::uint32_t materialIndex,
                                                  const bool visible = true)
{
    MiniEngine::World::MeshRendererComponent result{};
    result.mesh = MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset>{meshIndex, 1};
    // M4-02 v2：材质语义由 `.memat` 承载，组件只持有 Material Handle。
    result.material = MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MaterialAsset>{materialIndex, 1};
    result.visible = visible;
    return result;
}
} // namespace

// ---------- Entity：generation 与 slot 复用 ----------

TEST(WorldEntityTests, ZeroGenerationIsInvalid)
{
    EXPECT_FALSE((MiniEngine::World::Entity{0, 0}.IsValid()));
    EXPECT_TRUE((MiniEngine::World::Entity{0, 1}.IsValid()));
}

TEST(WorldEntityTests, DestroyedEntityStaysStaleAfterSlotReuse)
{
    MiniEngine::World::World world;
    const auto first = world.CreateEntity();
    ASSERT_TRUE(world.DestroyEntity(first));

    const auto second = world.CreateEntity();
    EXPECT_EQ(second.index, first.index);               // 复用同一 slot
    EXPECT_EQ(second.generation, first.generation + 1); // generation 递增
    EXPECT_FALSE(world.IsAlive(first));                 // 旧 handle 保持 stale
    EXPECT_TRUE(world.IsAlive(second));
}

TEST(WorldEntityTests, DestroyRemovesAllComponentsForReusedSlot)
{
    MiniEngine::World::World world;
    const auto original = world.CreateEntity();
    ASSERT_TRUE(world.SetName(original, "Cube"));
    ASSERT_TRUE(world.SetMeshRenderer(original, Renderer(1, 2)));
    ASSERT_TRUE(world.DestroyEntity(original));

    const auto reused = world.CreateEntity();
    EXPECT_EQ(reused.index, original.index);
    EXPECT_EQ(world.TryGetName(reused), nullptr);
    EXPECT_EQ(world.TryGetMeshRenderer(reused), nullptr);
    EXPECT_NE(world.TryGetTransform(reused), nullptr); // 每个 Entity 必有 Transform
}

// ---------- Transform：row-vector 组合与 dirty 传播 ----------

TEST(WorldTransformTests, ChildWorldUsesLocalTimesParentWorld)
{
    MiniEngine::World::World world;
    const auto parent = world.CreateEntity();
    const auto child = world.CreateEntity();
    ASSERT_TRUE(world.SetLocalMatrix(parent, Translation(10.0F, 0.0F, 0.0F)));
    ASSERT_TRUE(world.SetLocalMatrix(child, Translation(0.0F, 2.0F, 0.0F)));
    ASSERT_TRUE(world.SetParent(child, parent));

    world.UpdateTransforms();

    const auto* transform = world.TryGetTransform(child);
    ASSERT_NE(transform, nullptr);
    EXPECT_FLOAT_EQ(transform->world.values[12], 10.0F);
    EXPECT_FLOAT_EQ(transform->world.values[13], 2.0F);
    EXPECT_FLOAT_EQ(transform->world.values[14], 0.0F);
}

TEST(WorldTransformTests, LocalChangePropagatesToAllDescendants)
{
    MiniEngine::World::World world;
    const auto parent = world.CreateEntity();
    const auto child = world.CreateEntity();
    const auto grandchild = world.CreateEntity();
    ASSERT_TRUE(world.SetLocalMatrix(parent, Translation(10.0F, 0.0F, 0.0F)));
    ASSERT_TRUE(world.SetLocalMatrix(child, Translation(0.0F, 2.0F, 0.0F)));
    ASSERT_TRUE(world.SetLocalMatrix(grandchild, Translation(0.0F, 0.0F, 3.0F)));
    ASSERT_TRUE(world.SetParent(child, parent));
    ASSERT_TRUE(world.SetParent(grandchild, child));
    world.UpdateTransforms();

    auto* grandTransform = world.TryGetTransform(grandchild);
    ASSERT_NE(grandTransform, nullptr);
    EXPECT_FLOAT_EQ(grandTransform->world.values[12], 10.0F);
    EXPECT_FLOAT_EQ(grandTransform->world.values[13], 2.0F);
    EXPECT_FLOAT_EQ(grandTransform->world.values[14], 3.0F);

    // 修改 root local 后，descendants 全部标记 dirty，下次 update 跟随。
    ASSERT_TRUE(world.SetLocalMatrix(parent, Translation(20.0F, 0.0F, 0.0F)));
    world.UpdateTransforms();
    EXPECT_FLOAT_EQ(grandTransform->world.values[12], 20.0F);
    EXPECT_FLOAT_EQ(grandTransform->world.values[13], 2.0F);
    EXPECT_FLOAT_EQ(grandTransform->world.values[14], 3.0F);
}

TEST(WorldTransformTests, ReparentChangesWorldComposition)
{
    MiniEngine::World::World world;
    const auto first = world.CreateEntity();
    const auto second = world.CreateEntity();
    const auto child = world.CreateEntity();
    ASSERT_TRUE(world.SetLocalMatrix(first, Translation(10.0F, 0.0F, 0.0F)));
    ASSERT_TRUE(world.SetLocalMatrix(second, Translation(5.0F, 0.0F, 0.0F)));
    ASSERT_TRUE(world.SetLocalMatrix(child, Translation(1.0F, 0.0F, 0.0F)));
    ASSERT_TRUE(world.SetParent(child, first));
    world.UpdateTransforms();

    ASSERT_TRUE(world.SetParent(child, second)); // reparent 到另一 root
    world.UpdateTransforms();

    const auto* transform = world.TryGetTransform(child);
    ASSERT_NE(transform, nullptr);
    EXPECT_FLOAT_EQ(transform->world.values[12], 6.0F);
}

TEST(WorldTransformTests, RejectsTransformHierarchyCycle)
{
    MiniEngine::World::World world;
    const auto first = world.CreateEntity();
    const auto second = world.CreateEntity();
    ASSERT_TRUE(world.SetParent(second, first));
    EXPECT_FALSE(world.SetParent(first, second)); // 直接环
}

TEST(WorldTransformTests, RejectsCycleThroughLongerChain)
{
    MiniEngine::World::World world;
    const auto a = world.CreateEntity();
    const auto b = world.CreateEntity();
    const auto c = world.CreateEntity();
    ASSERT_TRUE(world.SetParent(b, a));
    ASSERT_TRUE(world.SetParent(c, b));
    EXPECT_FALSE(world.SetParent(a, c)); // a 不能 parent 给自己的 descendant
}

TEST(WorldTransformTests, KeepWorldYesIsExplicitlyRejected)
{
    MiniEngine::World::World world;
    const auto parent = world.CreateEntity();
    const auto child = world.CreateEntity();
    ASSERT_TRUE(world.SetParent(child, parent));

    const auto other = world.CreateEntity();
    EXPECT_FALSE(world.SetParent(child, other, MiniEngine::World::KeepWorld::Yes));
    // 失败不改状态：child 仍是原 parent。
    const auto* transform = world.TryGetTransform(child);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->parent, parent);
}

TEST(WorldTransformTests, DeepHierarchyUpdatesWithoutRecursionAndRespectsBudget)
{
    MiniEngine::World::World world;
    std::vector<MiniEngine::World::Entity> chain;
    chain.push_back(world.CreateEntity());
    ASSERT_TRUE(world.SetLocalMatrix(chain.front(), Translation(1.0F, 0.0F, 0.0F)));
    for (std::size_t depth = 1; depth < MiniEngine::World::kMaxHierarchyDepth; ++depth)
    {
        auto entity = world.CreateEntity();
        ASSERT_TRUE(world.SetLocalMatrix(entity, Translation(1.0F, 0.0F, 0.0F)));
        ASSERT_TRUE(world.SetParent(entity, chain.back())) << "depth " << depth;
        chain.push_back(entity);
    }

    world.UpdateTransforms();
    const auto* leaf = world.TryGetTransform(chain.back());
    ASSERT_NE(leaf, nullptr);
    EXPECT_FLOAT_EQ(leaf->world.values[12], static_cast<float>(MiniEngine::World::kMaxHierarchyDepth));

    // 超出 kMaxHierarchyDepth 预算被拒绝。
    auto beyond = world.CreateEntity();
    EXPECT_FALSE(world.SetParent(beyond, chain.back()));
}

TEST(WorldTransformTests, RejectsNonFiniteAndSingularLocalTransforms)
{
    MiniEngine::World::World world;
    const auto entity = world.CreateEntity();

    auto nonFinite = Translation(0.0F, 0.0F, 0.0F);
    nonFinite.values[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(world.SetLocalMatrix(entity, nonFinite));

    auto singular = Translation(0.0F, 0.0F, 0.0F);
    singular.values[0] = 0.0F;
    EXPECT_FALSE(world.SetLocalMatrix(entity, singular));
}

// ---------- TRS：row-vector / 左手系 golden 与 normalize ----------

TEST(WorldTransformTests, LocalTrsMatchesRowVectorLeftHandedGolden)
{
    MiniEngine::World::World world;
    const auto entity = world.CreateEntity();

    // T(1,2,3) * R_y(+90°) * S(2,1,1)，行向量约定下 M = S * R * T：
    // row0 = 2*(0,0,-1)，row2 = (1,0,0)，translation 在最后一行。
    ASSERT_TRUE(world.SetLocalTrs(entity, MiniEngine::World::Float3{1.0F, 2.0F, 3.0F}, RotationY90(),
                                  MiniEngine::World::Float3{2.0F, 1.0F, 1.0F}));
    world.UpdateTransforms();

    // Matrix4{} 默认是单位矩阵；golden 期望必须显式清零后逐项给出，
    // 否则 values[0]/values[10] 会残留单位矩阵的 1。
    MiniEngine::World::Matrix4 expected{};
    expected.values.fill(0.0F);
    expected.values[2] = -2.0F; // row0 = (0, 0, -2)
    expected.values[5] = 1.0F;  // row1 = (0, 1, 0)
    expected.values[8] = 1.0F;  // row2 = (1, 0, 0)
    expected.values[12] = 1.0F;
    expected.values[13] = 2.0F;
    expected.values[14] = 3.0F;
    expected.values[15] = 1.0F; // affine 的 m33
    ExpectMatrixNear(expected, world.TryGetTransform(entity)->world);
}

TEST(WorldTransformTests, LocalTrsNormalizesQuaternionInput)
{
    MiniEngine::World::World world;
    const auto normalized = world.CreateEntity();
    const auto unnormalized = world.CreateEntity();

    const auto rotation = RotationY90();
    const MiniEngine::World::Quaternion doubled{rotation.x * 2.0F, rotation.y * 2.0F, rotation.z * 2.0F,
                                                rotation.w * 2.0F};
    ASSERT_TRUE(world.SetLocalTrs(normalized, {}, rotation, {1.0F, 1.0F, 1.0F}));
    ASSERT_TRUE(world.SetLocalTrs(unnormalized, {}, doubled, {1.0F, 1.0F, 1.0F}));
    world.UpdateTransforms();

    ExpectMatrixNear(world.TryGetTransform(normalized)->world, world.TryGetTransform(unnormalized)->world);
}

TEST(WorldTransformTests, LocalTrsRejectsZeroRotationAndZeroDeterminantScale)
{
    MiniEngine::World::World world;
    const auto entity = world.CreateEntity();

    EXPECT_FALSE(
        world.SetLocalTrs(entity, {}, MiniEngine::World::Quaternion{0.0F, 0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}));
    EXPECT_FALSE(world.SetLocalTrs(entity, {}, RotationY90(), {0.0F, 1.0F, 1.0F})); // 零 determinant
}

// ---------- mirrored determinant ----------

TEST(WorldTransformTests, NegativeDeterminantMarksMirroredRenderItem)
{
    MiniEngine::World::World world;
    const auto normal = world.CreateEntity();
    const auto mirrored = world.CreateEntity();
    ASSERT_TRUE(world.SetLocalTrs(normal, {}, RotationY90(), {1.0F, 1.0F, 1.0F}));
    ASSERT_TRUE(world.SetLocalTrs(mirrored, {}, RotationY90(), {-1.0F, 1.0F, 1.0F})); // 负缩放
    ASSERT_TRUE(world.SetMeshRenderer(normal, Renderer(1, 2)));
    ASSERT_TRUE(world.SetMeshRenderer(mirrored, Renderer(3, 4)));
    world.UpdateTransforms();

    EXPECT_FALSE(world.TryGetTransform(normal)->mirrored);
    EXPECT_TRUE(world.TryGetTransform(mirrored)->mirrored);

    const auto items = world.BuildRenderItems();
    ASSERT_EQ(items.size(), 2);
    EXPECT_FALSE(items[0].mirrored);
    EXPECT_TRUE(items[1].mirrored);
}

// ---------- Components ----------

TEST(WorldComponentTests, NameAndMeshRendererCanBeAddedQueriedAndRemoved)
{
    MiniEngine::World::World world;
    const auto entity = world.CreateEntity();
    const auto renderer = Renderer(3, 4);

    ASSERT_TRUE(world.SetName(entity, "Demo Cube"));
    ASSERT_TRUE(world.SetMeshRenderer(entity, renderer));
    ASSERT_NE(world.TryGetName(entity), nullptr);
    EXPECT_EQ(world.TryGetName(entity)->value, "Demo Cube");
    ASSERT_NE(world.TryGetMeshRenderer(entity), nullptr);
    EXPECT_EQ(world.TryGetMeshRenderer(entity)->mesh, renderer.mesh);

    EXPECT_TRUE(world.RemoveName(entity));
    EXPECT_TRUE(world.RemoveMeshRenderer(entity));
    EXPECT_EQ(world.TryGetName(entity), nullptr);
    EXPECT_EQ(world.TryGetMeshRenderer(entity), nullptr);
}

TEST(WorldComponentTests, RejectsInvalidRendererInputs)
{
    MiniEngine::World::World world;
    const auto entity = world.CreateEntity();

    // M4-02 v2：SetMeshRenderer 要求 mesh 与 material 两个 Handle 都有效。
    // 材质语义（因子/贴图）全部由 `.memat` 承载，组件不再内联 baseColorFactor
    // （无材质 primitive 由 Cooker 合成 default 材质兜底，句柄恒有产物可指）。
    auto missingMaterial = Renderer(1, 2);
    missingMaterial.material = {};
    EXPECT_FALSE(world.SetMeshRenderer(entity, missingMaterial));

    auto missingMesh = Renderer(1, 2);
    missingMesh.mesh = {};
    EXPECT_FALSE(world.SetMeshRenderer(entity, missingMesh));

    // 失败不留半写入组件。
    EXPECT_EQ(world.TryGetMeshRenderer(entity), nullptr);
}

// ---------- RenderItem extraction ----------

TEST(WorldComponentTests, RenderItemsUseEntityOrderAndSkipInvisibleComponents)
{
    MiniEngine::World::World world;
    const auto first = world.CreateEntity();
    const auto second = world.CreateEntity();
    const auto third = world.CreateEntity();

    ASSERT_TRUE(world.SetMeshRenderer(first, Renderer(10, 20)));
    ASSERT_TRUE(world.SetMeshRenderer(second, Renderer(11, 21, false)));
    ASSERT_TRUE(world.SetMeshRenderer(third, Renderer(12, 22)));
    world.UpdateTransforms();

    const auto items = world.BuildRenderItems();
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].mesh, Renderer(10, 20).mesh);
    EXPECT_EQ(items[1].mesh, Renderer(12, 22).mesh);
}
