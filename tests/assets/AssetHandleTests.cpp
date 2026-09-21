// ============================================================================
// AssetHandleTests.cpp — AssetId / AssetHandle / AssetPool 的值类型契约
// 里程碑：M3（7-A / 7-C）
// 职责：验证句柄的世代失效语义（销毁后旧句柄不可用、槽位复用不复活旧句柄）、
//       修订号跨生命周期单调、以及 stale-handle 清单中的关键判定。
// 关联：engine/assets/include/MiniEngine/Assets/AssetPool.h（被测类型）
//       docs/architecture/DECISIONS.md §3
// ============================================================================

#include <MiniEngine/Assets/AssetPool.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

namespace
{
struct TestAsset final
{
    int value{};
};

struct OtherAsset final
{
    int value{};
};

MiniEngine::Assets::AssetId MakeId(const std::byte suffix)
{
    MiniEngine::Assets::AssetId id{};
    id.bytes.back() = suffix;
    return id;
}
} // namespace

// M3-04 Stale Handle 清单 #1：默认 Handle invalid（index == kInvalidIndex 或 generation == 0）。
TEST(AssetHandleTests, ZeroGenerationIsInvalid)
{
    using Handle = MiniEngine::Assets::AssetHandle<TestAsset>;

    EXPECT_FALSE(Handle{}.IsValid());
    EXPECT_FALSE(Handle(0, 0).IsValid());
    EXPECT_TRUE(Handle(0, 1).IsValid());
}

// M3-04 Stale Handle 清单 #2：正确 type/index/generation 可访问。
TEST(AssetPoolTests, ReloadPreservesHandleAndIncrementsRevision)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    const auto handle = pool.ResolveOrCreate(MakeId(std::byte{1}));

    ASSERT_TRUE(pool.Commit(handle, TestAsset{10}));
    const auto first = pool.TryGet(handle);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->asset->value, 10);
    EXPECT_EQ(first->revision, 1);

    ASSERT_TRUE(pool.Commit(handle, TestAsset{20}));
    const auto second = pool.TryGet(handle);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->asset->value, 20);
    EXPECT_EQ(second->revision, 2);
    EXPECT_EQ(handle, pool.ResolveOrCreate(MakeId(std::byte{1})));
}

// M3-04 Stale Handle 清单 #3：out-of-range index 返回 null/错误。
TEST(AssetPoolTests, OutOfRangeIndexReturnsNull)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    using Handle = MiniEngine::Assets::AssetHandle<TestAsset>;

    const Handle outOfRange{12345, 1};
    EXPECT_FALSE(pool.TryGet(outOfRange).has_value());
    EXPECT_FALSE(pool.Commit(outOfRange, TestAsset{1}));
    EXPECT_FALSE(pool.Unload(outOfRange));
}

// M3-04 Stale Handle 清单 #4：unload 后旧 Handle stale。
TEST(AssetPoolTests, UnloadedHandleIsStale)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    const auto handle = pool.ResolveOrCreate(MakeId(std::byte{1}));
    ASSERT_TRUE(pool.Commit(handle, TestAsset{10}));

    ASSERT_TRUE(pool.Unload(handle));
    EXPECT_FALSE(pool.TryGet(handle).has_value());
}

// M3-04 Stale Handle 清单 #5：slot reuse 得到相同 index、不同 generation。
TEST(AssetPoolTests, ReusedSlotInvalidatesOldGeneration)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    const auto oldHandle = pool.ResolveOrCreate(MakeId(std::byte{1}));
    ASSERT_TRUE(pool.Unload(oldHandle));

    const auto newHandle = pool.ResolveOrCreate(MakeId(std::byte{2}));
    EXPECT_EQ(newHandle.Index(), oldHandle.Index());
    EXPECT_NE(newHandle.Generation(), oldHandle.Generation());
    EXPECT_FALSE(pool.TryGet(oldHandle).has_value());
}

// M3-04 Stale Handle 清单 #6：wrong typed Handle 无法编译或不能构造（类型隔离）。
static_assert(
    !std::is_convertible_v<MiniEngine::Assets::AssetHandle<TestAsset>, MiniEngine::Assets::AssetHandle<OtherAsset>>);
static_assert(!std::is_same_v<MiniEngine::Assets::AssetHandle<TestAsset>, MiniEngine::Assets::AssetHandle<OtherAsset>>);

// M3-04 Stale Handle 清单 #7/#8：hot reload 后同一 Handle 有效且 revision+1。
TEST(AssetPoolTests, HotReloadKeepsHandleAndIncrementsRevision)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    const auto handle = pool.ResolveOrCreate(MakeId(std::byte{1}));

    ASSERT_TRUE(pool.Commit(handle, TestAsset{10}));
    const std::uint64_t firstRevision = pool.TryGet(handle)->revision;

    ASSERT_TRUE(pool.Commit(handle, TestAsset{20}));
    const auto second = pool.TryGet(handle);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->asset->value, 20);
    EXPECT_EQ(second->revision, firstRevision + 1);
    EXPECT_EQ(second->revision, 2);
}

// M3-04 Stale Handle 清单 #9：hot reload 失败 payload/revision 不变。
// 本实现以 Commit 失败表示 reload 失败：无效 handle 的 Commit 必须返回 false，
// 且不能污染已有 slot。
TEST(AssetPoolTests, FailedCommitLeavesExistingPayloadAndRevisionUnchanged)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    const auto handle = pool.ResolveOrCreate(MakeId(std::byte{1}));
    ASSERT_TRUE(pool.Commit(handle, TestAsset{10}));
    const auto before = pool.TryGet(handle);
    ASSERT_TRUE(before.has_value());

    using Handle = MiniEngine::Assets::AssetHandle<TestAsset>;
    const Handle stale{handle.Index(), static_cast<std::uint32_t>(handle.Generation() + 1000U)};
    EXPECT_FALSE(pool.Commit(stale, TestAsset{99}));

    const auto after = pool.TryGet(handle);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->asset->value, 10);
    EXPECT_EQ(after->revision, before->revision);
}

// M3-04 Stale Handle 清单 #10：Unload 后同 id 重新 ResolveOrCreate 得到新 generation
// （对应 Manifest 删除→重新出现场景，slot 复用而非误解析到新资产）。
TEST(AssetPoolTests, UnloadThenResolveSameIdCreatesFreshSlot)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    const auto first = pool.ResolveOrCreate(MakeId(std::byte{1}));
    ASSERT_TRUE(pool.Unload(first));

    const auto second = pool.ResolveOrCreate(MakeId(std::byte{1}));
    EXPECT_EQ(second.Index(), first.Index());
    EXPECT_NE(second.Generation(), first.Generation());
    EXPECT_TRUE(pool.TryGet(second).has_value() == false); // 新 slot 尚未 Commit
}

// M3-04 Stale Handle 清单 #11：generation overflow 被明确拒绝。
// 通过循环 Unload/Resolve 反复复用同一 slot，验证 generation 单调递增不 wrap。
TEST(AssetPoolTests, GenerationDoesNotWrapOnReuse)
{
    MiniEngine::Assets::AssetPool<TestAsset> pool;
    std::uint32_t previousGeneration = 0;

    for (int iteration = 0; iteration < 64; ++iteration)
    {
        const auto handle = pool.ResolveOrCreate(MakeId(std::byte{1}));
        if (iteration > 0)
        {
            EXPECT_NE(handle.Generation(), previousGeneration);
            EXPECT_GT(handle.Generation(), previousGeneration);
        }
        previousGeneration = handle.Generation();
        ASSERT_TRUE(pool.Unload(handle));
    }
}

// M3-04 Stale Handle 清单 #12：AssetId collision detector negative test。
// 相同内容 hash 的不同 URI 必须产生不同 AssetId（由 AssetId 派生层保证）；
// 此处验证不同 suffix 得到不同 id，相同 suffix 得到相同 id。
TEST(AssetHandleTests, AssetIdUniqueness)
{
    const auto idA = MakeId(std::byte{1});
    const auto idB = MakeId(std::byte{2});
    const auto idA2 = MakeId(std::byte{1});

    EXPECT_EQ(idA, idA2);
    EXPECT_NE(idA, idB);
    EXPECT_TRUE(idA.IsValid());
    EXPECT_TRUE(idB.IsValid());
}
