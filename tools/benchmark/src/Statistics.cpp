// ============================================================================
// Statistics.cpp — M7 统计原语与 FrameSample 聚合实现
// 里程碑：M7-01
// 关联：tools/benchmark/include/MiniEngine/Benchmark/Statistics.h
// ============================================================================

#include <MiniEngine/Benchmark/BenchmarkSchema.h>
#include <MiniEngine/Benchmark/Statistics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace MiniEngine::Benchmark
{
namespace
{
void RequireNonEmpty(std::span<const double> samples)
{
    if (samples.empty())
    {
        throw std::invalid_argument("cannot describe an empty sample set");
    }
}

void RequireFinite(std::span<const double> samples)
{
    for (const double value : samples)
    {
        if (!std::isfinite(value))
        {
            throw std::invalid_argument("sample set contains a non-finite value");
        }
    }
}

template <typename Member> std::vector<double> Extract(const std::vector<FrameSample>& samples, Member member)
{
    std::vector<double> values;
    values.reserve(samples.size());
    for (const FrameSample& sample : samples)
    {
        values.push_back(static_cast<double>(sample.*member));
    }
    return values;
}
} // namespace

double Quantile(std::span<const double> sorted, double q)
{
    if (sorted.empty())
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!(q >= 0.0 && q <= 1.0))
    {
        throw std::invalid_argument("quantile requires 0 <= q <= 1");
    }

    const double index = q * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(index));
    const auto upper = static_cast<std::size_t>(std::ceil(index));
    const double fraction = index - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

Distribution Describe(std::span<const double> samples)
{
    RequireNonEmpty(samples);
    RequireFinite(samples);

    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());

    Distribution result;
    result.median = Quantile(sorted, 0.5);
    result.p95 = Quantile(sorted, 0.95);
    result.p99 = Quantile(sorted, 0.99);
    result.maximum = sorted.back();

    std::vector<double> deviations;
    deviations.reserve(sorted.size());
    for (const double value : sorted)
    {
        deviations.push_back(std::abs(value - result.median));
    }
    std::sort(deviations.begin(), deviations.end());
    result.mad = Quantile(deviations, 0.5);
    return result;
}

HitchCounts CountHitches(std::span<const double> samples)
{
    RequireNonEmpty(samples);
    RequireFinite(samples);

    HitchCounts counts;
    for (const double value : samples)
    {
        if (value > 16.67)
        {
            ++counts.over16_67;
        }
        if (value > 33.33)
        {
            ++counts.over33_33;
        }
        if (value > 50.0)
        {
            ++counts.over50;
        }
    }
    return counts;
}

double RelativeMad(const Distribution& distribution)
{
    if (!std::isfinite(distribution.median) || distribution.median <= 0.0 || !std::isfinite(distribution.mad) ||
        distribution.mad < 0.0)
    {
        throw std::invalid_argument("relative MAD requires a finite positive median and non-negative MAD");
    }
    return distribution.mad / distribution.median;
}

double MinimumUsefulChange(const Distribution& baseline)
{
    return std::max(0.03, 2.0 * RelativeMad(baseline));
}

RunStatistics ComputeStatistics(const std::vector<FrameSample>& samples)
{
    if (samples.empty())
    {
        throw std::invalid_argument("ComputeStatistics requires at least one measured frame");
    }

    const auto describe = [&](auto member)
    {
        const std::vector<double> values = Extract(samples, member);
        return Describe(values);
    };

    RunStatistics result;
    result.sampleCount = static_cast<std::uint32_t>(samples.size());
    result.cpuFrameMs = describe(&FrameSample::cpuFrameMs);
    result.gpuFrameMs = describe(&FrameSample::gpuFrameMs);
    result.fixedUpdateMs = describe(&FrameSample::fixedUpdateMs);
    result.worldExtractMs = describe(&FrameSample::worldExtractMs);
    result.packetBuildMs = describe(&FrameSample::packetBuildMs);
    result.cullMs = describe(&FrameSample::cullMs);
    result.packetMergeMs = describe(&FrameSample::packetMergeMs);
    result.packetWaitMs = describe(&FrameSample::packetWaitMs);
    result.packetSortMs = describe(&FrameSample::packetSortMs);
    result.layoutCullMs = describe(&FrameSample::layoutCullMs);
    result.renderGraphBuildMs = describe(&FrameSample::renderGraphBuildMs);
    result.renderGraphCompileMs = describe(&FrameSample::renderGraphCompileMs);
    result.rhiSubmitMs = describe(&FrameSample::rhiSubmitMs);
    result.presentMs = describe(&FrameSample::presentMs);
    result.visibleCount = describe(&FrameSample::visibleCount);
    result.drawCount = describe(&FrameSample::drawCount);
    result.residentBytes = describe(&FrameSample::residentBytes);
    result.allocationCount = describe(&FrameSample::allocationCount);
    result.hitches = CountHitches(Extract(samples, &FrameSample::cpuFrameMs));

    result.activeWorkers = samples.back().activeWorkers;
    for (const FrameSample& sample : samples)
    {
        result.totalUploadsCommitted += sample.uploadsCommitted;
        result.totalStealAttempts += sample.stealAttempts;
        result.totalStealSuccesses += sample.stealSuccesses;
    }
    return result;
}

bool Validate(const BenchmarkRun& run, std::string& error)
{
    const auto fail = [&error](const char* message)
    {
        error = message;
        return false;
    };
    const auto require = [&fail](bool condition, const char* message) { return condition ? true : fail(message); };

    if (run.schemaVersion != kSchemaVersion)
    {
        return fail("unsupported schemaVersion");
    }
    if (!require(!run.experimentId.empty(), "experimentId is empty") ||
        !require(!run.variant.empty(), "variant is empty") || !require(!run.sceneName.empty(), "sceneName is empty") ||
        !require(!run.rhi.empty(), "rhi is empty") || !require(!run.loaderMode.empty(), "loaderMode is empty") ||
        !require(!run.packetBuildMode.empty(), "packetBuildMode is empty") ||
        !require(!run.schedulerMode.empty(), "schedulerMode is empty") ||
        !require(run.packetBuildMode != "serial" || run.chunkReserve, "chunkReserve is only meaningful for parallel"))
    {
        return false;
    }
    if (!require(!run.sceneManifestSha256.empty(), "sceneManifestSha256 is empty") ||
        !require(!run.cameraPathSha256.empty(), "cameraPathSha256 is empty") ||
        !require(!run.executableSha256.empty(), "executableSha256 is empty") ||
        !require(!run.sourceCommit.empty(), "sourceCommit is empty"))
    {
        return false;
    }
    if (!require(run.workers >= 1, "workers must be >= 1") || !require(run.chunkSize >= 1, "chunkSize must be >= 1") ||
        !require(run.warmupFrames >= 1, "warmupFrames must be >= 1") ||
        !require(run.measuredFrames >= 1, "measuredFrames must be >= 1"))
    {
        return false;
    }
    // M7-07（v3）：异步上传流水线的控制变量必须自洽（预算三约束都存在且份额在 0..100）。
    const bool asyncUploads = run.loaderMode.rfind("async", 0) == 0;
    // M7-08（v4）：布局变体必须是三者之一（默认 aos）+ M7-09 的 none（显式声明"不做布局
    // 实验"：不运行布局内核，帧时间口径与 M7-05/07 采集一致）；校验帧数不得少于不一致数。
    if (!require(run.layoutVariant == "aos" || run.layoutVariant == "hot-cold" || run.layoutVariant == "soa" ||
                     run.layoutVariant == "soa-batched" || run.layoutVariant == "none",
                 "layoutVariant must be aos|hot-cold|soa|soa-batched|none"))
    {
        return false;
    }
    if (!require(run.statistics.layoutCheckMismatches <= run.statistics.layoutCheckFrames,
                 "layoutCheckMismatches cannot exceed layoutCheckFrames"))
    {
        return false;
    }
    if (asyncUploads && (!require(run.uploadMiBPerFrame > 0, "async loaderMode requires uploadMiBPerFrame > 0") ||
                         !require(run.uploadBudgetCpuMs > 0.0, "async loaderMode requires uploadBudgetCpuMs > 0") ||
                         !require(run.uploadBudgetRequests > 0, "async loaderMode requires uploadBudgetRequests > 0") ||
                         !require(run.uploadBudgetReloadPercent <= 100, "uploadBudgetReloadPercent must be <= 100")))
    {
        return false;
    }
    if (!require(run.samples.size() == run.measuredFrames, "sample count does not match measuredFrames"))
    {
        return false;
    }
    if (!require(run.correctness.status == "PASS", "correctness evidence is not PASS"))
    {
        return false;
    }
    for (const FrameSample& sample : run.samples)
    {
        if (!std::isfinite(sample.cpuFrameMs) || sample.cpuFrameMs <= 0.0)
        {
            return fail("cpuFrameMs samples must be finite and positive");
        }
        if (!std::isfinite(sample.packetWaitMs) || sample.packetWaitMs < 0.0)
        {
            return fail("packetWaitMs samples must be finite and non-negative");
        }
    }
    return true;
}
} // namespace MiniEngine::Benchmark
