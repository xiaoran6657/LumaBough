// ============================================================================
// StatisticsTests.cpp — M7 统计原语与机读契约回归
// 里程碑：M7-01
// 职责：锁定 M7-09 定义的分位数语义（线性插值）、MAD、hitch 阈值与噪声下限；
//       这些量是后续全部 A/B 结论的判定基础，改动必须让本文件先失败。
// 关联：docs/architecture/README.md
// ============================================================================

#include <MiniEngine/Benchmark/BenchmarkSchema.h>
#include <MiniEngine/Benchmark/Statistics.h>

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
using namespace MiniEngine::Benchmark;

TEST(M7Statistics, QuantileUsesLinearInterpolation)
{
    const std::vector<double> sorted{1.0, 2.0, 3.0, 4.0};
    EXPECT_DOUBLE_EQ(Quantile(sorted, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(Quantile(sorted, 0.5), 2.5);
    EXPECT_DOUBLE_EQ(Quantile(sorted, 1.0), 4.0);
    // index = 0.95 * 3 = 2.85 → 3 * 0.15 + 4 * 0.85 = 3.85
    EXPECT_NEAR(Quantile(sorted, 0.95), 3.85, 1e-12);
    EXPECT_NEAR(Quantile(sorted, 0.99), 3.97, 1e-12);
}

TEST(M7Statistics, DescribeReportsMedianMadAndTails)
{
    const std::vector<double> samples{5.0, 1.0, 3.0, 2.0, 4.0};
    const Distribution result = Describe(samples);
    EXPECT_DOUBLE_EQ(result.median, 3.0);
    // |x - 3| = {2,1,0,1,2} → median = 1
    EXPECT_DOUBLE_EQ(result.mad, 1.0);
    EXPECT_NEAR(result.p95, 4.8, 1e-12);
    EXPECT_NEAR(result.p99, 4.96, 1e-12);
    EXPECT_DOUBLE_EQ(result.maximum, 5.0);
}

TEST(M7Statistics, EmptySampleSetIsRejected)
{
    const std::vector<double> empty;
    EXPECT_THROW(static_cast<void>(Describe(empty)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(CountHitches(empty)), std::invalid_argument);
}

TEST(M7Statistics, NonFiniteSamplesAreRejected)
{
    const std::vector<double> notANumber{1.0, std::numeric_limits<double>::quiet_NaN()};
    EXPECT_THROW(static_cast<void>(Describe(notANumber)), std::invalid_argument);
    const std::vector<double> infinite{std::numeric_limits<double>::infinity()};
    EXPECT_THROW(static_cast<void>(CountHitches(infinite)), std::invalid_argument);
}

TEST(M7Statistics, HitchCountsUseStrictThresholds)
{
    // 恰好等于阈值不计入：">16.67 / >33.33 / >50.0" 是严格大于。
    const std::vector<double> samples{16.67, 16.68, 33.33, 33.34, 50.0, 50.01};
    const HitchCounts counts = CountHitches(samples);
    EXPECT_EQ(counts.over16_67, 5U);
    EXPECT_EQ(counts.over33_33, 3U);
    EXPECT_EQ(counts.over50, 1U);
}

TEST(M7Statistics, NoiseFloorUsesMaxOfThreePercentAndTwiceRelativeMad)
{
    Distribution noisy;
    noisy.median = 10.0;
    noisy.mad = 1.0;
    EXPECT_DOUBLE_EQ(RelativeMad(noisy), 0.1);
    EXPECT_DOUBLE_EQ(MinimumUsefulChange(noisy), 0.2);

    Distribution calm;
    calm.median = 100.0;
    calm.mad = 0.5;
    EXPECT_DOUBLE_EQ(RelativeMad(calm), 0.005);
    // 2 × 0.005 = 1% 低于 3% 下限。
    EXPECT_DOUBLE_EQ(MinimumUsefulChange(calm), 0.03);
}

TEST(M7Statistics, RelativeMadRejectsNonPositiveMedian)
{
    Distribution invalid;
    invalid.median = 0.0;
    invalid.mad = 1.0;
    EXPECT_THROW(static_cast<void>(RelativeMad(invalid)), std::invalid_argument);
}

TEST(M7Statistics, ComputeStatisticsAggregatesFrameFields)
{
    std::vector<FrameSample> samples(3);
    for (std::uint32_t index = 0; index < samples.size(); ++index)
    {
        samples[index].frameSerial = index + 1;
        samples[index].cpuFrameMs = 8.0 + static_cast<double>(index);
        samples[index].worldExtractMs = 1.0;
        samples[index].cullMs = 2.0;
        samples[index].visibleCount = 30;
        samples[index].drawCount = 30;
        samples[index].residentBytes = 83'509'248;
        samples[index].allocationCount = 40 + index;
        samples[index].uploadsCommitted = 2;
        samples[index].activeWorkers = 1;
    }
    samples[1].cpuFrameMs = 40.0; // 制造一个 >33.33ms 的 hitch

    const RunStatistics statistics = ComputeStatistics(samples);
    EXPECT_EQ(statistics.sampleCount, 3U);
    // {8, 40, 10} 排序后为 {8, 10, 40}：中位数是 10，最大值是 40。
    EXPECT_DOUBLE_EQ(statistics.cpuFrameMs.median, 10.0);
    EXPECT_DOUBLE_EQ(statistics.cpuFrameMs.maximum, 40.0);
    EXPECT_EQ(statistics.hitches.over16_67, 1U);
    EXPECT_EQ(statistics.hitches.over33_33, 1U);
    EXPECT_EQ(statistics.hitches.over50, 0U);
    EXPECT_EQ(statistics.activeWorkers, 1U);
    EXPECT_EQ(statistics.totalUploadsCommitted, 6U);
    EXPECT_DOUBLE_EQ(statistics.visibleCount.median, 30.0);
    EXPECT_DOUBLE_EQ(statistics.residentBytes.maximum, 83'509'248.0);
}

TEST(M7Statistics, ComputeStatisticsRejectsEmptyInput)
{
    const std::vector<FrameSample> empty;
    EXPECT_THROW(static_cast<void>(ComputeStatistics(empty)), std::invalid_argument);
}
} // namespace
