// ============================================================================
// Statistics.h — M7 性能统计原语（median / MAD / p95 / p99 / hitch）
// 里程碑：M7-01（性能契约与串行基线）
// 职责：提供与 M7-09 方法学一致的分位数定义（线性插值）、MAD（中位数绝对偏差）、
//       hitch 计数与噪声下限。全部结论（noiseFloor、protected metric 判定）都建立
//       在这几个量上，因此实现先于任何优化落地，并有独立单元测试。
// 关联：docs/architecture/README.md（统计字段与阈值）
// ============================================================================

#pragma once

#include <cstdint>
#include <span>

namespace MiniEngine::Benchmark
{
// 一个样本集的分布摘要（单位与输入一致）。
struct Distribution final
{
    double median = 0.0;
    double mad = 0.0; // median(|x - median|)
    double p95 = 0.0;
    double p99 = 0.0;
    double maximum = 0.0;
};

// 固定阈值下的 hitch 计数（>16.67 / >33.33 / >50.0 ms）。
struct HitchCounts final
{
    std::uint32_t over16_67 = 0;
    std::uint32_t over33_33 = 0;
    std::uint32_t over50 = 0;
};

// 线性插值分位数：index = q * (n - 1)，相邻两点按小数部分插值。
// sorted 必须已升序；空集返回 NaN（调用方必须先捕获空样本，见 Describe）。
[[nodiscard]] double Quantile(std::span<const double> sorted, double q);

// 完整分布摘要。samples 为空时抛 std::invalid_argument（空样本不是"零"，
// 不允许悄悄产出 0 参与后续比较）。
[[nodiscard]] Distribution Describe(std::span<const double> samples);

// 逐样本 hitch 计数；非有限值抛 std::invalid_argument。
[[nodiscard]] HitchCounts CountHitches(std::span<const double> samples);

// 相对 MAD = MAD / median；median <= 0 或非有限时抛 std::invalid_argument。
[[nodiscard]] double RelativeMad(const Distribution& distribution);

// minimumUsefulChange = max(3%, 2 × relativeMAD)：M7 判断"改善是否超过噪声"的默认下限。
[[nodiscard]] double MinimumUsefulChange(const Distribution& baseline);
} // namespace MiniEngine::Benchmark
