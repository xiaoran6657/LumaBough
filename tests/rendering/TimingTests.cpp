// ============================================================================
// TimingTests.cpp — M4-08 性能统计数学的单元测试（nearest-rank 分位数与一致性）
// 里程碑：M4（08 篇「统计方法」「报告 schema」）
// 职责：在进入 benchmark 流程前锁定报告统计的数学与拒绝语义：
//   - nearest-rank：p = sorted[ceil(q*N)-1]（median/p95/p99 与手算一致）；
//   - 一致性校验：NaN/负值拒绝、p95>=median、p99>=p95（写出前验证，08 篇）。
// 计时本体（staging/GPU query）需要真实设备，不在单元测试范围；
// 其行为由 benchmark 运行的 valid/missing/disjoint 计数与 Debug Layer 覆盖。
// 关联：docs/architecture/README.md「统计方法」「必做负向测试」
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11GpuTimer.h（被测实现）
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11GpuTimer.h>

#include <gtest/gtest.h>

#include <limits>
#include <vector>

using MiniEngine::Rhi::D3D11::PerfQuantilesAreConsistent;
using MiniEngine::Rhi::D3D11::SummarizeNearestRank;

namespace
{
// 1..N 的递增样本：median/p95/p99 的 nearest-rank 结果可解析推导。
std::vector<double> Range(const std::size_t count)
{
    std::vector<double> samples(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        samples[index] = static_cast<double>(index + 1U);
    }
    return samples;
}
} // namespace

// 奇数 N：nearest-rank q=0.5 → ceil(0.5*5)=3 → 第 3 个样本；q=0.95 → ceil(4.75)=5。
TEST(TimingTests, NearestRankPercentilesForOddCount)
{
    const auto quantiles = SummarizeNearestRank(Range(5));
    EXPECT_DOUBLE_EQ(quantiles.median, 3.0);
    EXPECT_DOUBLE_EQ(quantiles.p95, 5.0);
    EXPECT_DOUBLE_EQ(quantiles.p99, 5.0);
}

// 偶数 N：q=0.5 → ceil(0.5*4)=2 → 第 2 个样本（上中位，nearest-rank 定义）。
TEST(TimingTests, NearestRankPercentilesForEvenCount)
{
    const auto quantiles = SummarizeNearestRank(Range(4));
    EXPECT_DOUBLE_EQ(quantiles.median, 2.0);
    EXPECT_DOUBLE_EQ(quantiles.p95, 4.0);
    EXPECT_DOUBLE_EQ(quantiles.p99, 4.0);
}

// 乱序输入也要得到相同分位数（Summarize 内部排序，不假设输入有序）。
TEST(TimingTests, SummarizeSortsUnorderedInput)
{
    const std::vector<double> unordered{9.0, 1.0, 5.0, 3.0, 7.0};
    const auto quantiles = SummarizeNearestRank(unordered);
    EXPECT_DOUBLE_EQ(quantiles.median, 5.0);
    EXPECT_DOUBLE_EQ(quantiles.p95, 9.0);
}

// 单样本：三个分位数都等于该样本。
TEST(TimingTests, SingleSampleMapsToAllQuantiles)
{
    const auto quantiles = SummarizeNearestRank(std::vector<double>{42.0});
    EXPECT_DOUBLE_EQ(quantiles.median, 42.0);
    EXPECT_DOUBLE_EQ(quantiles.p95, 42.0);
    EXPECT_DOUBLE_EQ(quantiles.p99, 42.0);
}

// 空样本：全 0（调用方在帧数不足时走 BLOCKED 路径，不依赖此函数报错）。
TEST(TimingTests, EmptySamplesYieldZeroQuantiles)
{
    const auto quantiles = SummarizeNearestRank({});
    EXPECT_DOUBLE_EQ(quantiles.median, 0.0);
    EXPECT_DOUBLE_EQ(quantiles.p95, 0.0);
    EXPECT_DOUBLE_EQ(quantiles.p99, 0.0);
}

// 一致性校验：正常值通过；NaN / 负值 / 分位顺序颠倒全部拒绝（08 篇负向测试）。
TEST(TimingTests, QuantileConsistencyRejectsNonFiniteAndDisordered)
{
    EXPECT_TRUE(PerfQuantilesAreConsistent({1.0, 1.5, 2.0}));
    EXPECT_TRUE(PerfQuantilesAreConsistent({0.0, 0.0, 0.0}));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(PerfQuantilesAreConsistent({nan, 1.0, 2.0})) << "NaN timing 必须让 writer 失败";
    EXPECT_FALSE(PerfQuantilesAreConsistent({-1.0, 0.0, 0.0})) << "负 timing 必须让 writer 失败";

    EXPECT_FALSE(PerfQuantilesAreConsistent({2.0, 1.0, 3.0})) << "p95 < median 属于计算错误";
    EXPECT_FALSE(PerfQuantilesAreConsistent({1.0, 3.0, 2.0})) << "p99 < p95 属于计算错误";
    EXPECT_FALSE(PerfQuantilesAreConsistent({nan, nan, nan}));
}
