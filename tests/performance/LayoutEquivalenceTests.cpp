// ============================================================================
// LayoutEquivalenceTests.cpp — M7-08 数据布局等价性（A22/A23/A24 的前置证据）
// 里程碑：M7-08（数据布局实验）
// 职责：
//   * 三布局（AoS / hot-cold / SoA）在同一夹具下产出**相同的可见序列与语义 hash**
//     （10k/50k/100k × 可见率 0.1/0.5/0.9，对应文档的数据集矩阵）；
//   * AoS 内核与**生产 BuildRenderPacket** 逐项一致（baseline 是 M6 真实实现）；
//   * 覆盖率护栏：可见数必须命中目标可见率、shadow 组非退化（M7-P09 的教训）；
//   * 冷数据在 split 之后内容不变（冷访问路径的代价由 micro/端到端测量，不由本文件断言）；
//   * 逐实体字节账与 cache-line 隔离（false sharing 实验的前置条件）。
// 时间不在本文件断言：只验证"等价"，性能判定见 E-M7-LAYOUT-001/002 的 sweep。
// 关联：engine/world/include/MiniEngine/World/RenderProxyLayouts.h
//       tests/performance/LayoutTestSupport.h
//       docs/architecture/README.md
// ============================================================================

#include "LayoutTestSupport.h"

#include <MiniEngine/Tasks/TaskStatistics.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
using namespace MiniEngine;
using namespace MiniEngine::World;

// 三种布局的可见序列与语义 hash 必须逐位一致（计数由 CompareLayoutResults 逐项对照）。
TEST(LayoutEquivalenceTests, AosHotColdAndSoaProduceSameVisibleSequence)
{
    for (const std::uint32_t count : {10000U, 50000U, 100000U})
    {
        for (const double visibleRatio : {0.1, 0.5, 0.9})
        {
            const TestSupport::LayoutFixture fixture = TestSupport::MakeLayoutFixture(count, visibleRatio, 6657);

            const LayoutCullResult aos = CullAoS(fixture.aos, fixture.cameraFrustum, fixture.lightFrustum);
            const LayoutCullResult hotCold = CullHotCold(fixture.hotCold, fixture.cameraFrustum, fixture.lightFrustum);
            const LayoutCullResult soa = CullSoA(fixture.soa, fixture.cameraFrustum, fixture.lightFrustum);

            const std::string hotColdDiff = CompareLayoutResults(aos, hotCold);
            const std::string soaDiff = CompareLayoutResults(aos, soa);
            EXPECT_TRUE(hotColdDiff.empty())
                << "count=" << count << " ratio=" << visibleRatio << " diff=" << hotColdDiff;
            EXPECT_TRUE(soaDiff.empty()) << "count=" << count << " ratio=" << visibleRatio << " diff=" << soaDiff;

            // 覆盖率护栏：可见数命中目标，且 shadow 组既不空也不全可见。
            EXPECT_GE(aos.mainVisible, fixture.targetVisible) << "count=" << count << " ratio=" << visibleRatio;
            EXPECT_LE(aos.mainVisible, fixture.targetVisible + count / 50U)
                << "count=" << count << " ratio=" << visibleRatio;
            EXPECT_EQ(aos.mainVisible + aos.mainCulled, count);
            EXPECT_GT(aos.shadowVisible, 0U) << "count=" << count;
            EXPECT_GT(aos.shadowCulled, 0U) << "count=" << count;
            EXPECT_EQ(aos.shadowVisible + aos.shadowCulled, aos.shadowCandidates);
            EXPECT_GT(aos.trianglesSubmitted, 0U);

            // 可见序列必须是输入顺序的子序列（内核不得重排或去重）。
            EXPECT_TRUE(std::is_sorted(aos.mainVisibleEntityIndices.begin(), aos.mainVisibleEntityIndices.end()));
            EXPECT_EQ(aos.mainVisibleEntityIndices.size(), aos.mainVisible);
        }
    }
}

// M7-LAYOUT-SIMD：批量化 SoA 变体（块级 early-out）必须与逐实体 SoA 内核**逐字节相同**——
// 块级结论只是"跳过重复判定"的加速，不得改变任何可见性/计数/顺序/hash。
// 覆盖块大小边界：1（退化为逐实体）、7（不整除）、64（默认）、4096（单块覆盖全场）。
TEST(LayoutEquivalenceTests, BatchedSoaMatchesScalarSoaForEveryBlockSize)
{
    for (const std::uint32_t count : {10000U, 50000U})
    {
        for (const double visibleRatio : {0.1, 0.5, 0.9})
        {
            const TestSupport::LayoutFixture fixture = TestSupport::MakeLayoutFixture(count, visibleRatio, 6657);
            const LayoutCullResult soa = CullSoA(fixture.soa, fixture.cameraFrustum, fixture.lightFrustum);
            for (const std::uint32_t blockSize : {1U, 7U, 64U, 4096U})
            {
                const LayoutCullResult batched =
                    CullSoABatched(fixture.soa, fixture.cameraFrustum, fixture.lightFrustum, {}, blockSize);
                const std::string diff = CompareLayoutResults(soa, batched);
                EXPECT_TRUE(diff.empty()) << "count=" << count << " ratio=" << visibleRatio
                                          << " block=" << blockSize << " diff=" << diff;
                EXPECT_EQ(batched.semanticHash, soa.semanticHash)
                    << "count=" << count << " ratio=" << visibleRatio << " block=" << blockSize;
                EXPECT_EQ(batched.mainVisibleEntityIndices, soa.mainVisibleEntityIndices);
                EXPECT_EQ(batched.shadowVisibleEntityIndices, soa.shadowVisibleEntityIndices);
            }
        }
    }
}

// baseline 必须是 M6 真实实现：AoS 内核与生产 BuildRenderPacket 逐项一致。
TEST(LayoutEquivalenceTests, LayoutKernelsMatchProductionRenderPacket)
{
    for (const double visibleRatio : {0.1, 0.5, 0.9})
    {
        const TestSupport::LayoutFixture fixture = TestSupport::MakeLayoutFixture(50000U, visibleRatio, 4242);
        const RenderPacket packet = TestSupport::BuildProductionPacket(fixture);

        // 生产路径必须真的做出判定（不是全可见/全剔除的退化数据）。
        ASSERT_EQ(packet.stats.candidateObjects, 50000U);
        ASSERT_GT(packet.stats.mainCulled, 0U);
        ASSERT_GT(packet.stats.mainVisible, 0U);

        const LayoutCullResult aos = CullAoS(fixture.aos, fixture.cameraFrustum, fixture.lightFrustum);
        const LayoutCullResult hotCold = CullHotCold(fixture.hotCold, fixture.cameraFrustum, fixture.lightFrustum);
        const LayoutCullResult soa = CullSoA(fixture.soa, fixture.cameraFrustum, fixture.lightFrustum);

        const std::string aosDiff = CompareLayoutResultsAgainstPacket(aos, packet);
        const std::string hotColdDiff = CompareLayoutResultsAgainstPacket(hotCold, packet);
        const std::string soaDiff = CompareLayoutResultsAgainstPacket(soa, packet);
        EXPECT_TRUE(aosDiff.empty()) << "ratio=" << visibleRatio << " diff=" << aosDiff;
        EXPECT_TRUE(hotColdDiff.empty()) << "ratio=" << visibleRatio << " diff=" << hotColdDiff;
        EXPECT_TRUE(soaDiff.empty()) << "ratio=" << visibleRatio << " diff=" << soaDiff;
    }
}

// 固定 seed 的夹具必须自证可复现：同参数两次构建得到相同语义 hash（防夹具自身漂移）。
TEST(LayoutEquivalenceTests, FixtureIsDeterministicAcrossRebuilds)
{
    const TestSupport::LayoutFixture first = TestSupport::MakeLayoutFixture(10000U, 0.5, 99);
    const TestSupport::LayoutFixture second = TestSupport::MakeLayoutFixture(10000U, 0.5, 99);
    const LayoutCullResult firstResult = CullAoS(first.aos, first.cameraFrustum, first.lightFrustum);
    const LayoutCullResult secondResult = CullAoS(second.aos, second.cameraFrustum, second.lightFrustum);
    EXPECT_EQ(firstResult.semanticHash, secondResult.semanticHash);
    EXPECT_EQ(firstResult.mainVisibleEntityIndices, secondResult.mainVisibleEntityIndices);

    // 不同 seed 必须产生不同数据（否则"固定 seed 复现"没有意义）。
    const TestSupport::LayoutFixture different = TestSupport::MakeLayoutFixture(10000U, 0.5, 100);
    const LayoutCullResult differentResult = CullAoS(different.aos, different.cameraFrustum, different.lightFrustum);
    EXPECT_NE(firstResult.semanticHash, differentResult.semanticHash);
}

// hot/cold split 的语义检查 + 逐实体字节账：split 只搬走冷数据，热数据一字不改。
TEST(LayoutEquivalenceTests, HotColdSplitKeepsColdDataSeparateAndIntact)
{
    const TestSupport::LayoutFixture fixture = TestSupport::MakeLayoutFixture(10000U, 0.5, 7);
    ASSERT_EQ(fixture.hotCold.Size(), fixture.sources.size());
    ASSERT_EQ(fixture.hotCold.cold.size(), fixture.sources.size());

    std::size_t originalColdCharacters = 0;
    std::size_t splitColdCharacters = 0;
    for (std::size_t index = 0; index < fixture.sources.size(); ++index)
    {
        EXPECT_EQ(fixture.hotCold.cold[index].debugName, fixture.sources[index].cold.debugName);
        EXPECT_EQ(fixture.hotCold.cold[index].authoringIndex, fixture.sources[index].cold.authoringIndex);
        originalColdCharacters += fixture.sources[index].cold.debugName.size();
        splitColdCharacters += fixture.hotCold.cold[index].debugName.size();
    }
    EXPECT_EQ(originalColdCharacters, splitColdCharacters);

    const TestSupport::LayoutBytesSummary bytes = TestSupport::SummarizeLayoutBytes(fixture);
    // hot 部分与 AoS 完全相同（本实验只做 split）。
    EXPECT_EQ(bytes.hotColdHotBytesPerEntity, bytes.aosBytesPerEntity);
    EXPECT_GT(bytes.hotColdColdBytesPerEntity, 0U);
    // SoA 的扫描字段比 AoS 记录更紧凑（矩阵 + 两个包围球 + 两个 AssetId + 计数 + flags）。
    EXPECT_LT(bytes.soaScanBytesPerEntity, bytes.aosBytesPerEntity);
    // 固定尺寸构造：capacity 与 size 相等（"allocations" 口径可复算）。
    // HotBytes() 只算热字段与句柄载荷，cold 是单独数组——这里把两者相加后与真实分配比对。
    EXPECT_EQ(bytes.soaCapacityBytes, fixture.soa.HotBytes() + fixture.soa.cold.size() * sizeof(RenderProxyCold));
}

// false sharing 实验的前置条件：相邻 worker 的统计实例不得共享 cache line。
TEST(LayoutEquivalenceTests, PerWorkerStatisticsDoNotOverlapCacheLines)
{
    Tasks::WorkerStatistics statistics[2]{};
    const auto first = reinterpret_cast<std::uintptr_t>(&statistics[0]);
    const auto second = reinterpret_cast<std::uintptr_t>(&statistics[1]);
    EXPECT_GE(second - first, Tasks::kDestructiveInterferenceSize);
    EXPECT_GE(alignof(Tasks::WorkerStatistics), Tasks::kDestructiveInterferenceSize);
}
} // namespace
