// ============================================================================
// tasks_bench — M7-04 / M7-A12 调度器级微基准（workers × chunk × 版本）
// 里程碑：M7-04（scaling sweep）
// 职责：用固定输入测量三种执行版本的每帧 build 成本：
//   serial      ：主线程直接跑同一份递归切分逻辑（有效并行度 = 1）
//   global      ：SchedulerMode::GlobalQueue（M7-03 基线）
//   per-worker  ：SchedulerMode::PerWorkerDeque（M7-04 v1：local deque + 窃取）
// 负载：N 个实体的视锥测试（6 个平面 + 半径），按 chunk 递归二分切分；叶子把
//   visible/checksum 写进**固定索引的结果槽**，因此输出与调度顺序无关（可确定性校验）。
// 指标：每帧 buildMs（提交 + 等待）分布（median/p95/p99/MAD）、任务计数、调度器计数
//   （steal/local/inject/wakeup/sleep）与相对 serial 的加速比。
// 依赖纪律：只链接 MiniEngineTasks（Core/Profiling）与 MiniEngineBenchmark（统计口径与
//   M7-01/M7-09 一致）；不链接 RHI/asset/render。
// 关联：docs/architecture/README.md、tools/performance/run_m7_task_sweep.ps1
// ============================================================================

#include <MiniEngine/Benchmark/Statistics.h>
#include <MiniEngine/Tasks/TaskSystem.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Plane
{
    float nx, ny, nz, d;
};

struct Entity
{
    float x, y, z, radius;
};

struct ChunkResult
{
    std::uint32_t visible = 0;
    std::uint64_t checksum = 0;
};

struct CullJob
{
    const Entity* entities = nullptr;
    const Plane* planes = nullptr;
    ChunkResult* slots = nullptr;
    MiniEngine::Tasks::TaskSystem* tasks = nullptr;
    MiniEngine::Tasks::TaskGroup* group = nullptr;
    std::atomic<std::uint32_t>* submitFailures = nullptr;
    std::atomic<std::uint32_t>* tasksSubmitted = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t chunk = 0;
};

[[nodiscard]] bool InsideFrustum(const Entity& entity, const Plane* planes) noexcept
{
    for (int plane = 0; plane < 6; ++plane)
    {
        const Plane& p = planes[plane];
        const float distance = p.nx * entity.x + p.ny * entity.y + p.nz * entity.z + p.d;
        if (distance < -entity.radius)
            return false;
    }
    return true;
}

// 叶子：处理 [begin, end) 并写入固定槽（槽索引 = begin / chunk），与调度顺序无关。
void ProcessLeaf(const CullJob& job) noexcept
{
    std::uint32_t visible = 0;
    std::uint64_t checksum = 0;
    for (std::uint32_t index = job.begin; index < job.end; ++index)
    {
        if (InsideFrustum(job.entities[index], job.planes))
        {
            ++visible;
            checksum = checksum * 1099511628211ULL + index;
        }
    }
    ChunkResult& slot = job.slots[job.begin / job.chunk];
    slot.visible = visible;
    slot.checksum = checksum;
}

// 切分点：**按 chunk 对齐**。叶子因此总是从 chunk 的整数倍开始，槽位索引 begin/chunk
// 唯一（若按 count/2 二分，不同叶子会写同一个槽 → 校验和随机不匹配）。
[[nodiscard]] bool TrySplit(const CullJob& job, std::uint32_t& mid) noexcept
{
    const std::uint32_t count = job.end - job.begin;
    if (count <= job.chunk)
        return false;
    const std::uint32_t chunks = (count + job.chunk - 1) / job.chunk;
    if (chunks < 2)
        return false;
    mid = job.begin + (chunks / 2) * job.chunk;
    if (mid <= job.begin || mid >= job.end)
        mid = job.begin + job.chunk; // 兜底：至少推进一个 chunk
    return mid > job.begin && mid < job.end;
}

// 递归切分：> chunk 就二分并提交两个子任务（同一 group），否则直接处理。
// 子任务由执行它的 worker 提交 → 进入该 worker 的 local deque（v1 的窃取来源）。
void CullNode(void* raw) noexcept
{
    auto* job = static_cast<CullJob*>(raw);
    std::uint32_t mid = 0;
    if (!TrySplit(*job, mid))
    {
        ProcessLeaf(*job);
        delete job;
        return;
    }

    for (const std::uint32_t range : {0U, 1U})
    {
        auto* child = new CullJob(*job);
        child->begin = range == 0 ? job->begin : mid;
        child->end = range == 0 ? mid : job->end;
        const MiniEngine::Tasks::SubmitResult result = job->tasks->Submit(
            MiniEngine::Tasks::Task{&CullNode, child, nullptr, MiniEngine::Tasks::TaskFlags::MainHelpAllowed,
                                    {0, "CullNode", 0}},
            *job->group);
        if (result != MiniEngine::Tasks::SubmitResult::Accepted)
        {
            job->submitFailures->fetch_add(1, std::memory_order_relaxed);
            delete child; // 发布失败：owner 立刻回收
        }
        else
        {
            job->tasksSubmitted->fetch_add(1, std::memory_order_relaxed);
        }
    }
    delete job;
}

// serial 版本：同一份切分与算法，但完全在主线程内联执行（无任务、无队列）。
void CullNodeSerial(CullJob& job) noexcept
{
    std::uint32_t mid = 0;
    if (!TrySplit(job, mid))
    {
        ProcessLeaf(job);
        return;
    }
    CullJob left = job;
    left.begin = job.begin;
    left.end = mid;
    CullNodeSerial(left);
    CullJob right = job;
    right.begin = mid;
    right.end = job.end;
    CullNodeSerial(right);
}

[[nodiscard]] std::string ReadArgument(const int argc, char** argv, const std::string_view key, const std::string& fallback)
{
    const std::string prefix = "--" + std::string(key) + "=";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument.starts_with(prefix))
            return std::string(argument.substr(prefix.size()));
    }
    return fallback;
}

[[nodiscard]] std::uint32_t ReadNumber(const int argc, char** argv, const std::string_view key, const std::uint32_t fallback)
{
    const std::string text = ReadArgument(argc, argv, key, std::string());
    if (text.empty())
        return fallback;
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        std::fprintf(stderr, "invalid --%s=%s\n", std::string(key).c_str(), text.c_str());
        std::exit(2);
    }
    return value;
}

[[nodiscard]] double MillisecondsSince(const Clock::time_point start) noexcept
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// 固定输入：位置与半径由 LCG 生成（同一 seed → 同一输入，跨进程可复现）。
[[nodiscard]] std::vector<Entity> MakeEntities(const std::uint32_t count, const std::uint64_t seed)
{
    std::vector<Entity> entities(count);
    std::uint64_t state = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<float>(state % 2000001) / 1000000.0F - 1.0F; // [-1, 1)
    };
    for (Entity& entity : entities)
    {
        entity.x = next() * 100.0F;
        entity.y = next() * 60.0F;
        entity.z = next() * 100.0F;
        entity.radius = 0.5F + std::fabs(next()) * 2.0F;
    }
    return entities;
}

struct Counters
{
    std::uint64_t executed = 0;
    std::uint64_t sleeps = 0;
    std::uint64_t wakeups = 0;
    std::uint64_t stealAttempts = 0;
    std::uint64_t stealSuccesses = 0;
    std::uint64_t localPushes = 0;
    std::uint64_t injectedPushes = 0;
    std::uint64_t localHighWater = 0;
    std::uint64_t queueLatencyNanoseconds = 0;
};

[[nodiscard]] Counters GatherCounters(const MiniEngine::Tasks::TaskSystem& tasks)
{
    Counters counters;
    for (std::uint32_t worker = 0; worker < tasks.WorkerCount(); ++worker)
    {
        const MiniEngine::Tasks::WorkerCounters workerCounters = tasks.WorkerCountersFor(worker);
        counters.executed += workerCounters.executed;
        counters.sleeps += workerCounters.sleeps;
        counters.wakeups += workerCounters.wakeups;
        counters.stealAttempts += workerCounters.stealAttempts;
        counters.stealSuccesses += workerCounters.stealSuccesses;
        counters.localPushes += workerCounters.localPushes;
        counters.injectedPushes += workerCounters.injectedPushes;
        counters.localHighWater = std::max(counters.localHighWater, workerCounters.localQueueHighWater);
        counters.queueLatencyNanoseconds += workerCounters.queueLatencyNanoseconds;
    }
    return counters;
}

void WriteJson(const std::string& path, const std::string& mode, const std::uint32_t workers, const std::uint32_t chunk,
               const std::uint32_t entities, const std::uint32_t warmupFrames, const std::uint32_t measuredFrames,
               const std::uint64_t seed, const std::uint32_t sampleCount,
               const MiniEngine::Benchmark::Distribution& build, const std::uint32_t tasksPerFrame,
               const std::uint64_t visibleEntities, const std::uint64_t checksum, const double serialMedianMs,
               const Counters& counters, const bool checksumMatchedSerial)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(2);
    }

    out.precision(10);
    out << "{\n";
    out << "  \"schema\": \"miniengine.task-sweep.v1\",\n";
    out << "  \"cell\": {\"mode\": \"" << mode << "\", \"workers\": " << workers << ", \"chunk\": " << chunk
        << ", \"entities\": " << entities << ", \"warmupFrames\": " << warmupFrames
        << ", \"measuredFrames\": " << measuredFrames << ", \"seed\": " << seed << "},\n";
    out << "  \"workload\": {\"tasksPerFrame\": " << tasksPerFrame << ", \"visibleEntities\": " << visibleEntities
        << ", \"checksum\": \"" << checksum << "\", \"checksumMatchesSerial\": "
        << (checksumMatchedSerial ? "true" : "false") << "},\n";
    out << "  \"buildMs\": {\"sampleCount\": " << sampleCount << ", \"median\": " << build.median
        << ", \"p95\": " << build.p95 << ", \"p99\": " << build.p99 << ", \"maximum\": " << build.maximum
        << ", \"mad\": " << build.mad << ", \"relativeMad\": " << MiniEngine::Benchmark::RelativeMad(build) << "},\n";
    out << "  \"serialMedianMs\": " << serialMedianMs << ",\n";
    out << "  \"speedupVsSerial\": " << (build.median > 0.0 ? serialMedianMs / build.median : 0.0) << ",\n";
    out << "  \"counters\": {\"executed\": " << counters.executed << ", \"sleeps\": " << counters.sleeps
        << ", \"wakeups\": " << counters.wakeups << ", \"stealAttempts\": " << counters.stealAttempts
        << ", \"stealSuccesses\": " << counters.stealSuccesses << ", \"localPushes\": " << counters.localPushes
        << ", \"injectedPushes\": " << counters.injectedPushes << ", \"localHighWater\": " << counters.localHighWater
        << ", \"queueLatencyMeanMicroseconds\": "
        << (counters.executed > 0 ? static_cast<double>(counters.queueLatencyNanoseconds) / 1000.0 /
                                        static_cast<double>(counters.executed)
                                  : 0.0)
        << "},\n";
    out << "  \"threads\": {\"hardware\": " << std::thread::hardware_concurrency() << "},\n";
    out << "  \"generatedBy\": \"tools/tasks_bench (M7-04 sweep)\"\n";
    out << "}\n";
}
} // namespace

int main(const int argc, char** argv)
{
    const std::string mode = ReadArgument(argc, argv, "mode", "per-worker");
    const std::uint32_t workers = ReadNumber(argc, argv, "workers", 4);
    const std::uint32_t chunk = ReadNumber(argc, argv, "chunk", 256);
    const std::uint32_t entities = ReadNumber(argc, argv, "entities", 50000);
    const std::uint32_t warmupFrames = ReadNumber(argc, argv, "warmup-frames", 20);
    const std::uint32_t measuredFrames = ReadNumber(argc, argv, "measure-frames", 60);
    const std::uint64_t seed = ReadNumber(argc, argv, "seed", 6657);
    const std::uint32_t topLevelTasks = ReadNumber(argc, argv, "top-level-tasks", 8);
    const std::string output = ReadArgument(argc, argv, "output", "");

    if (mode != "serial" && mode != "global" && mode != "per-worker")
    {
        std::fprintf(stderr, "unknown --mode=%s (serial|global|per-worker)\n", mode.c_str());
        return 2;
    }
    if (output.empty())
    {
        std::fprintf(stderr, "missing --output=path\n");
        return 2;
    }

    const std::vector<Entity> entityData = MakeEntities(entities, seed);
    // 视锥平面：固定值，保证跨进程/跨配置完全一致。
    const Plane planes[6] = {{1.0F, 0.0F, 0.0F, 100.0F},  {-1.0F, 0.0F, 0.0F, 100.0F}, {0.0F, 1.0F, 0.0F, 60.0F},
                             {0.0F, -1.0F, 0.0F, 60.0F}, {0.0F, 0.0F, 1.0F, 100.0F},  {0.0F, 0.0F, -1.0F, 100.0F}};
    const std::uint32_t slots = (entities + chunk - 1) / chunk;
    std::vector<ChunkResult> results(slots);

    // 参考：serial 逐帧跑同一份递归切分（同时得到 serial 中位耗时与参考校验和）。
    std::vector<double> serialSamples;
    serialSamples.reserve(measuredFrames);
    CullJob reference{entityData.data(), planes, results.data(), nullptr, nullptr, nullptr, nullptr, 0, entities, chunk};
    std::uint64_t referenceChecksum = 0;
    for (std::uint32_t frame = 0; frame < warmupFrames + measuredFrames; ++frame)
    {
        std::fill(results.begin(), results.end(), ChunkResult{});
        const auto start = Clock::now();
        CullNodeSerial(reference);
        const double milliseconds = MillisecondsSince(start);
        if (frame >= warmupFrames)
            serialSamples.push_back(milliseconds);
    }
    std::uint64_t visibleEntities = 0;
    for (const ChunkResult& result : results)
    {
        visibleEntities += result.visible;
        referenceChecksum = referenceChecksum * 1315423911ULL + result.checksum;
    }
    const MiniEngine::Benchmark::Distribution serialDistribution = MiniEngine::Benchmark::Describe(serialSamples);
    const double serialMedian = serialDistribution.median;

    if (mode == "serial")
    {
        const std::uint32_t tasksPerFrame = static_cast<std::uint32_t>(std::ceil(static_cast<double>(entities) / chunk));
        WriteJson(output, mode, 1, chunk, entities, warmupFrames, measuredFrames, seed,
                  static_cast<std::uint32_t>(serialSamples.size()), serialDistribution, tasksPerFrame, visibleEntities,
                  referenceChecksum, serialMedian, Counters{}, true);
        std::printf("serial chunk=%u median=%.3fms visible=%llu checksum=%llu\n", chunk, serialDistribution.median,
                    (unsigned long long)visibleEntities, (unsigned long long)referenceChecksum);
        return 0;
    }

    MiniEngine::Tasks::TaskSystemConfig config;
    config.mode = mode == "per-worker" ? MiniEngine::Tasks::SchedulerMode::PerWorkerDeque
                                       : MiniEngine::Tasks::SchedulerMode::GlobalQueue;
    config.workerCount = std::max(1U, workers);
    config.injectQueueCapacity = 4096;
    config.localQueueCapacity = 64;
    config.stealAttemptsBeforeSleep = 4;
    config.randomSeed = seed;
    MiniEngine::Tasks::TaskSystem tasks(config);

    std::atomic<std::uint32_t> submitFailures{0};
    std::atomic<std::uint32_t> tasksSubmitted{0};
    std::vector<double> buildSamples;
    buildSamples.reserve(measuredFrames);
    // 顶层区间必须**按 chunk 对齐**：否则子区间的 begin/chunk 会与其他区间冲突（同一结果槽
    // 被两个叶子写）→ 校验和随机不匹配。对齐后每个叶子的槽位索引唯一。
    const std::uint32_t rawTopLevel = (entities + topLevelTasks - 1) / topLevelTasks;
    const std::uint32_t perTopLevel = std::max(chunk, ((rawTopLevel + chunk - 1) / chunk) * chunk);
    std::uint32_t tasksPerFrame = 0;
    bool checksumMatched = true;

    for (std::uint32_t frame = 0; frame < warmupFrames + measuredFrames; ++frame)
    {
        std::fill(results.begin(), results.end(), ChunkResult{});
        MiniEngine::Tasks::TaskGroup group;
        const std::uint32_t submittedBefore = tasksSubmitted.load(std::memory_order_relaxed);
        const auto start = Clock::now();
        // 顶层任务由外部提交（进 inject queue），子任务由 worker 提交（进 local deque）。
        for (std::uint32_t index = 0; index < topLevelTasks; ++index)
        {
            const std::uint32_t begin = index * perTopLevel;
            const std::uint32_t end = std::min(entities, begin + perTopLevel);
            if (begin >= end)
                break;
            auto* job = new CullJob{entityData.data(), planes, results.data(), &tasks,      &group,
                                    &submitFailures, &tasksSubmitted, begin, end, chunk};
            MiniEngine::Tasks::Task task{&CullNode, job, nullptr, MiniEngine::Tasks::TaskFlags::MainHelpAllowed,
                                         {0, "CullRoot", 0}};
            for (;;)
            {
                const MiniEngine::Tasks::SubmitResult result = tasks.Submit(task, group);
                if (result == MiniEngine::Tasks::SubmitResult::Accepted)
                {
                    tasksSubmitted.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (result != MiniEngine::Tasks::SubmitResult::QueueFull)
                {
                    std::fprintf(stderr, "submit failed with %d\n", static_cast<int>(result));
                    delete job;
                    return 2;
                }
                std::this_thread::yield(); // 正常背压：重试是安全的（失败的提交已回滚 group）
            }
        }
        tasks.Wait(group, MiniEngine::Tasks::TaskWaitRole::MainThread);
        const double milliseconds = MillisecondsSince(start);
        tasksPerFrame = tasksSubmitted.load(std::memory_order_relaxed) - submittedBefore;
        if (frame >= warmupFrames)
            buildSamples.push_back(milliseconds);

        std::uint64_t checksum = 0;
        std::uint32_t visible = 0;
        for (const ChunkResult& result : results)
        {
            visible += result.visible;
            checksum = checksum * 1315423911ULL + result.checksum;
        }
        if (frame + 1 == warmupFrames + measuredFrames)
        {
            checksumMatched = checksum == referenceChecksum && visible == visibleEntities;
            visibleEntities = visible;
        }
    }



    const MiniEngine::Benchmark::Distribution distribution = MiniEngine::Benchmark::Describe(buildSamples);
    const Counters counters = GatherCounters(tasks);
    tasks.Shutdown(true);

    WriteJson(output, mode, config.workerCount, chunk, entities, warmupFrames, measuredFrames, seed,
              static_cast<std::uint32_t>(buildSamples.size()), distribution, tasksPerFrame, visibleEntities,
              referenceChecksum, serialMedian, counters, checksumMatched);

    std::printf("%s workers=%u chunk=%u median=%.3fms p95=%.3fms speedup=%.2fx steal=%llu/%llu checksum=%s\n",
                mode.c_str(), config.workerCount, chunk, distribution.median, distribution.p95,
                serialMedian > 0.0 ? serialMedian / distribution.median : 0.0,
                (unsigned long long)counters.stealSuccesses, (unsigned long long)counters.stealAttempts,
                checksumMatched ? "ok" : "MISMATCH");
    if (!checksumMatched || submitFailures.load() != 0)
        return 1;
    return 0;
}
