// ============================================================================
// LayoutBenchmarks.cpp — M7-08 布局微基准（Google Benchmark）
// 里程碑：M7-08（数据布局实验）
// 职责：对**纯 CPU culling 内核**做 AoS / hot-cold / SoA 的单变量对照：
//       * 夹具在计时循环外构建（`tests/performance/LayoutTestSupport.h`，
//         与等价性测试共用同一份数据与同一条 FRNG）；
//       * 计时循环内只跑内核，并用 DoNotOptimize/ClobberMemory 防止工作被删；
//       * counters 记录逐实体字节账，便于"时间 vs 内存足迹"一起读。
// 边界：微基准变快不自动代表 frame 变快——引擎端到端数据见 E-M7-LAYOUT-001/002
//       （`tools/performance/run_m7_layout_sweep.ps1` + m7-cpu-scale 场景）。
// 构建：默认关闭（`ME_BUILD_BENCHMARKS=OFF`），不进入 CI 计时 Gate。
// 关联：docs/architecture/README.md「Microbenchmark」
// ============================================================================

#include <benchmark/benchmark.h>

#include "LayoutTestSupport.h"

#include <MiniEngine/World/RenderProxyLayouts.h>

#include <cstdint>

namespace
{
using MiniEngine::TestSupport::LayoutFixture;
using namespace MiniEngine::World;

// 夹具与三个布局由同一份数据派生：任何布局差异都只能来自内存形态。
[[nodiscard]] LayoutFixture MakeFixture(const std::size_t count)
{
    return MiniEngine::TestSupport::MakeLayoutFixture(static_cast<std::uint32_t>(count), 0.5, 6657);
}

void ReportByteCounters(benchmark::State& state, const MiniEngine::TestSupport::LayoutBytesSummary& bytes,
                        const char* scanCounter)
{
    state.counters["bytes_per_entity"] = static_cast<double>(bytes.aosBytesPerEntity);
    state.counters[scanCounter] = static_cast<double>(bytes.soaScanBytesPerEntity);
    state.counters["hot_cold_hot_bytes"] = static_cast<double>(bytes.hotColdHotBytesPerEntity);
    state.counters["hot_cold_cold_bytes"] = static_cast<double>(bytes.hotColdColdBytesPerEntity);
}

void BM_LayoutCullAoS(benchmark::State& state)
{
    const LayoutFixture fixture = MakeFixture(static_cast<std::size_t>(state.range(0)));
    const MiniEngine::TestSupport::LayoutBytesSummary bytes = MiniEngine::TestSupport::SummarizeLayoutBytes(fixture);

    for (auto _ : state)
    {
        const LayoutCullResult result = CullAoS(fixture.aos, fixture.cameraFrustum, fixture.lightFrustum);
        benchmark::DoNotOptimize(result.mainVisible);
        benchmark::DoNotOptimize(result.semanticHash);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
    ReportByteCounters(state, bytes, "soa_scan_bytes");
}

void BM_LayoutCullHotCold(benchmark::State& state)
{
    const LayoutFixture fixture = MakeFixture(static_cast<std::size_t>(state.range(0)));
    const MiniEngine::TestSupport::LayoutBytesSummary bytes = MiniEngine::TestSupport::SummarizeLayoutBytes(fixture);

    for (auto _ : state)
    {
        const LayoutCullResult result = CullHotCold(fixture.hotCold, fixture.cameraFrustum, fixture.lightFrustum);
        benchmark::DoNotOptimize(result.mainVisible);
        benchmark::DoNotOptimize(result.semanticHash);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
    ReportByteCounters(state, bytes, "soa_scan_bytes");
}

void BM_LayoutCullSoA(benchmark::State& state)
{
    const LayoutFixture fixture = MakeFixture(static_cast<std::size_t>(state.range(0)));
    const MiniEngine::TestSupport::LayoutBytesSummary bytes = MiniEngine::TestSupport::SummarizeLayoutBytes(fixture);

    for (auto _ : state)
    {
        const LayoutCullResult result = CullSoA(fixture.soa, fixture.cameraFrustum, fixture.lightFrustum);
        benchmark::DoNotOptimize(result.mainVisible);
        benchmark::DoNotOptimize(result.semanticHash);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
    ReportByteCounters(state, bytes, "soa_scan_bytes");
}
} // namespace

BENCHMARK(BM_LayoutCullAoS)->Arg(10000)->Arg(50000)->Arg(100000);
BENCHMARK(BM_LayoutCullHotCold)->Arg(10000)->Arg(50000)->Arg(100000);
BENCHMARK(BM_LayoutCullSoA)->Arg(10000)->Arg(50000)->Arg(100000);
BENCHMARK_MAIN();
