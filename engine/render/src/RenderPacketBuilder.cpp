// RenderPacketBuilder.cpp - M7-05 parallel RenderPacket build (part 1).
// Structure: fixed chunk partition (stable slots) -> per-chunk reuse of
// World::BuildRenderPacket (same cull/extract/sort contract, sorted per chunk)
// -> merge by chunk index -> global sort with the serial comparators -> publish.
// Determinism: sort keys come from content identity and entityIndex (unique).

#include <MiniEngine/Render/RenderPacketBuilder.h>

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Profiling/Profile.h>
#include <MiniEngine/World/RenderQueueBuilder.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace MiniEngine::Render
{
namespace
{
using Clock = std::chrono::steady_clock;

// Count/timing statistics are summed per chunk (data-independent): the守恒项必须逐项
// reduce，漏掉 trianglesSubmitted 会让并行 packet 与串行 oracle 不一致（M7-A14）。
void AccumulateChunkStats(World::CullingStats& stats, const World::CullingStats& other) noexcept
{
    stats.candidateObjects += other.candidateObjects;
    stats.mainVisible += other.mainVisible;
    stats.mainCulled += other.mainCulled;
    stats.mainInside += other.mainInside;
    stats.mainIntersecting += other.mainIntersecting;
    stats.shadowCandidates += other.shadowCandidates;
    stats.shadowVisible += other.shadowVisible;
    stats.shadowCulled += other.shadowCulled;
    stats.trianglesSubmitted += other.trianglesSubmitted;
    stats.cullingCpuMicroseconds += other.cullingCpuMicroseconds;
    stats.sortCpuMicroseconds += other.sortCpuMicroseconds;
}

// 一个"已排序 run"的游标（指向某个 chunk 的数组）。
struct SortedRun final
{
    World::RenderDraw* draws = nullptr;
    std::size_t size = 0;
    std::size_t cursor = 0;
};

// M7-RP-ORDER：k 路归并，替代原来的"拼接复制 + 全局 std::sort"。
// 前提（由构建路径保证）：每个 chunk 的数组已按**同一个**比较器排好序（chunk 内复用
// World::BuildRenderPacket），且排序键唯一（entityIndex）——因此全局顺序就是各 run 的归并，
// 与串行/旧实现逐位相同（键重复时也不会有歧义，这里再按 run 索引稳定取舍）。
// 算法：小根堆选最小 run 头；当一个 run 的全部剩余键都小于下一个 run 的头键时，整段一次
// 移动（快路）。快路的实际意义：键分布集中的场景（同一 material/mesh）退化为接近纯移动；
// 通用场景仍保证 O(N log k) 次比较、每个元素只移动一次（旧实现 std::sort 平均 N log N 次移动）。
template <typename Select, typename Less>
void MergeSortedRuns(std::vector<World::RenderDraw>& target, std::vector<RenderPacketChunk>& chunks, Select select,
                     Less less)
{
    std::vector<SortedRun> runs;
    runs.reserve(chunks.size());
    std::size_t total = 0;
    for (RenderPacketChunk& chunk : chunks)
    {
        std::vector<World::RenderDraw>& source = select(chunk);
        if (!source.empty())
        {
            runs.push_back(SortedRun{source.data(), source.size(), 0});
            total += source.size();
        }
    }

    target.clear();
    if (total == 0)
        return;
    target.reserve(total);

    if (runs.size() == 1)
    {
        // 唯一 run：直接整段移动（堆路径也能处理，但这里省掉逐元素比较）。
        target.insert(target.end(), std::make_move_iterator(runs.front().draws),
                      std::make_move_iterator(runs.front().draws + runs.front().size));
        return;
    }

    const auto headOf = [&runs](const std::size_t index) -> World::RenderDraw&
    { return runs[index].draws[runs[index].cursor]; };
    // 小根堆：`true` 表示 left 应排在 right 之后（std::*_heap 的"更差"语义）。
    const auto heapWorse = [&runs, &less, &headOf](const std::size_t left, const std::size_t right)
    {
        if (less(headOf(left), headOf(right)))
            return false;
        if (less(headOf(right), headOf(left)))
            return true;
        return left > right; // 键相等（不应发生）：run 索引小者优先，保持确定性。
    };

    std::vector<std::size_t> heap;
    heap.reserve(runs.size());
    for (std::size_t index = 0; index < runs.size(); ++index)
        heap.push_back(index);
    std::make_heap(heap.begin(), heap.end(), heapWorse);

    while (!heap.empty())
    {
        std::pop_heap(heap.begin(), heap.end(), heapWorse);
        const std::size_t runIndex = heap.back();
        SortedRun& run = runs[runIndex];
        const std::size_t remaining = run.size - run.cursor;

        // 快路：整个 run 的剩余键都小于当前堆顶（下一最小头键）时，整段移动。
        if (remaining > 1 && !heap.empty() && less(run.draws[run.size - 1], headOf(heap.front())))
        {
            target.insert(target.end(), std::make_move_iterator(run.draws + run.cursor),
                          std::make_move_iterator(run.draws + run.size));
            run.cursor = run.size;
            heap.pop_back();
            continue;
        }

        target.push_back(std::move(headOf(runIndex)));
        ++run.cursor;
        if (run.cursor == run.size)
            heap.pop_back();
        else
            std::push_heap(heap.begin(), heap.end(), heapWorse);
    }
}

// Matrix4/Sphere/Float3 do not define operator==: compare component-wise with
// exact equality (same inputs, same code path per item, therefore bit-identical).
[[nodiscard]] bool SameMatrix(const World::Matrix4& left, const World::Matrix4& right) noexcept
{
    for (std::size_t index = 0; index < 16; ++index)
    {
        if (left.values[index] != right.values[index])
            return false;
    }
    return true;
}

[[nodiscard]] bool SameSphere(const World::Sphere& left, const World::Sphere& right) noexcept
{
    return left.center.x == right.center.x && left.center.y == right.center.y && left.center.z == right.center.z &&
           left.radius == right.radius;
}

// Semantic equivalence of one RenderDraw: every field (timing excluded).
[[nodiscard]] bool SameRenderDraw(const World::RenderDraw& left, const World::RenderDraw& right) noexcept
{
    return left.mesh == right.mesh && left.material == right.material && left.materialId == right.materialId &&
           left.meshId == right.meshId && SameMatrix(left.world, right.world) &&
           SameMatrix(left.normal, right.normal) && SameSphere(left.worldBounds, right.worldBounds) &&
           left.entityIndex == right.entityIndex && left.mirrored == right.mirrored &&
           left.castsShadow == right.castsShadow && left.receivesShadow == right.receivesShadow;
}

// Semantic equivalence of two packets: queue elements field-by-field plus the
// count statistics (timing microseconds are measurements and are excluded).
[[nodiscard]] bool SameSemantics(const World::RenderPacket& serial, const World::RenderPacket& parallel,
                                 std::uint32_t& firstDifference) noexcept
{
    if (serial.mainOpaque.size() != parallel.mainOpaque.size() ||
        serial.shadowCasters.size() != parallel.shadowCasters.size())
        return false;

    for (std::size_t index = 0; index < serial.mainOpaque.size(); ++index)
    {
        if (!SameRenderDraw(serial.mainOpaque[index], parallel.mainOpaque[index]))
        {
            firstDifference = parallel.mainOpaque[index].entityIndex;
            return false;
        }
    }
    for (std::size_t index = 0; index < serial.shadowCasters.size(); ++index)
    {
        if (!SameRenderDraw(serial.shadowCasters[index], parallel.shadowCasters[index]))
        {
            firstDifference = parallel.shadowCasters[index].entityIndex;
            return false;
        }
    }

    const World::CullingStats& a = serial.stats;
    const World::CullingStats& b = parallel.stats;
    const bool countsMatch = a.candidateObjects == b.candidateObjects && a.mainVisible == b.mainVisible &&
                             a.mainCulled == b.mainCulled && a.mainInside == b.mainInside &&
                             a.mainIntersecting == b.mainIntersecting && a.shadowCandidates == b.shadowCandidates &&
                             a.shadowVisible == b.shadowVisible && a.shadowCulled == b.shadowCulled &&
                             a.opaqueDrawCalls == b.opaqueDrawCalls && a.shadowDrawCalls == b.shadowDrawCalls &&
                             a.trianglesSubmitted == b.trianglesSubmitted;
    if (!countsMatch)
    {
        firstDifference = 0;
        return false;
    }
    return true;
}
} // namespace

struct RenderPacketBuilder::ChunkContext
{
    std::span<const World::RenderItem> items{};
    World::RenderPacketCamera camera{};
    World::DirectionalLight light{};
    const World::MeshInfoLookup* meshInfo = nullptr;
    const World::MaterialInfoLookup* materialInfo = nullptr;
    World::CullingOptions options{};
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    RenderPacketChunk* output = nullptr;
};

RenderPacketBuilder::RenderPacketBuilder(Tasks::TaskSystem& tasks) : m_tasks(tasks)
{
}

World::RenderPacket RenderPacketBuilder::BuildSerial(const std::span<const World::RenderItem> items,
                                                     const World::RenderPacketCamera& camera,
                                                     const World::DirectionalLight& light,
                                                     const World::MeshInfoLookup& meshInfo,
                                                     const World::CullingOptions& options,
                                                     const World::MaterialInfoLookup& materialInfo) const
{
    // Serial oracle: M4's single builder, must stay (Debug shadow builds, tests).
    return World::BuildRenderPacket(items, camera, light, meshInfo, options, materialInfo);
}

World::RenderPacket RenderPacketBuilder::BuildParallel(
    const std::span<const World::RenderItem> items, const World::RenderPacketCamera& camera,
    const World::DirectionalLight& light, const World::MeshInfoLookup& meshInfo, const World::CullingOptions& options,
    const World::MaterialInfoLookup& materialInfo, const RenderPacketBuildConfig& config, RenderPacketBuildStats* stats)
{
    ME_PROFILE_ZONE_NAMED("RenderPacketBuildParallel");
    ME_VERIFY(config.chunkSize > 0, "chunk size must be positive");

    const std::uint32_t entityCount = static_cast<std::uint32_t>(items.size());
    const std::uint32_t chunkCount = entityCount / config.chunkSize + (entityCount % config.chunkSize != 0 ? 1U : 0U);
    const auto buildStart = Clock::now();
    RenderPacketBuildStats localStats;
    localStats.chunkCount = chunkCount;

    // Empty input still needs a valid packet header (camera/light/shadow frustum
    // validation): run the serial path once with an empty span.
    World::RenderPacket packet = World::BuildRenderPacket(items.first(static_cast<std::size_t>(0)), camera, light,
                                                          meshInfo, options, materialInfo);

    std::vector<RenderPacketChunk> chunks(chunkCount);
    std::vector<ChunkContext> contexts(chunkCount);
    Tasks::TaskGroup group;

    for (std::uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
    {
        const std::uint32_t begin = chunkIndex * config.chunkSize;
        const std::uint32_t end = std::min(begin + config.chunkSize, entityCount);

        RenderPacketChunk& chunk = chunks[chunkIndex];
        if (config.reserveChunkCapacity)
        {
            // 估计可见率 60%（M7-01 固定场景的实测可见率 52% 附近）：预留不足时按几何
            // 增长扩容；E-M7-RP-004 的 A/B 对照是完全不 reserve。
            chunk.mainOpaque.reserve(static_cast<std::size_t>(end - begin) * 3U / 5U + 1U);
        }

        contexts[chunkIndex] =
            ChunkContext{items, camera, light, &meshInfo, &materialInfo, options, begin, end, &chunk};
        Tasks::Task task{&BuildChunkTask,
                         &contexts[chunkIndex],
                         nullptr,
                         Tasks::TaskFlags::MainHelpAllowed,
                         {0, "RenderPacketChunk", 0}};

        const Tasks::SubmitResult result = m_tasks.Submit(task, group);
        if (result == Tasks::SubmitResult::QueueFull)
        {
            // Bounded backpressure is expected: run this chunk synchronously and
            // keep the deterministic slot order.
            ++localStats.syncChunks;
            BuildChunkTask(&contexts[chunkIndex]);
            continue;
        }
        if (result != Tasks::SubmitResult::Accepted)
        {
            // Stopping/InvalidTask is a lifecycle error: drain accepted tasks
            // first so contexts never go out of scope while tasks run.
            m_tasks.Wait(group, Tasks::TaskWaitRole::MainThread);
            throw std::runtime_error("RenderPacket task submission rejected");
        }
        ++localStats.taskChunks;
    }

    {
        const auto waitStart = Clock::now();
        m_tasks.Wait(group, Tasks::TaskWaitRole::MainThread);
        localStats.waitCpuMicroseconds = std::chrono::duration<double, std::micro>(Clock::now() - waitStart).count();
    }

    // Chunk errors are rethrown on the main thread in chunk order (stable report).
    for (RenderPacketChunk& chunk : chunks)
    {
        if (chunk.error != nullptr)
            std::rethrow_exception(chunk.error);
    }

    {
        ME_PROFILE_ZONE_NAMED("RenderPacketMerge");
        const auto mergeStart = Clock::now();
        // 统计与数据合并解耦：chunk 计数逐字段 reduce（与数据布局无关）。
        for (const RenderPacketChunk& chunk : chunks)
            AccumulateChunkStats(packet.stats, chunk.statistics);

        // M7-RP-ORDER：k 路归并（含"不相交 run 整段移动"快路）取代"拼接复制 + 全局 std::sort"。
        // 计时口径保持可比：`mergeCpuMicroseconds` = 拼接/复制段（新实现已无此段 → 0），
        // `sortCpuMicroseconds` = 定序段（旧 = std::sort，新 = k 路归并）。
        // Exactly the serial comparators: entityIndex is unique, so the output is
        // independent of the chunk partition.
        MergeSortedRuns(
            packet.mainOpaque, chunks, [](RenderPacketChunk& chunk) -> std::vector<World::RenderDraw>&
            { return chunk.mainOpaque; }, World::StableOpaqueLess);
        MergeSortedRuns(
            packet.shadowCasters, chunks, [](RenderPacketChunk& chunk) -> std::vector<World::RenderDraw>&
            { return chunk.shadowCasters; }, World::StableShadowLess);

        const double mergeElapsed = std::chrono::duration<double, std::micro>(Clock::now() - mergeStart).count();
        localStats.mergedDraws = static_cast<std::uint32_t>(packet.mainOpaque.size() + packet.shadowCasters.size());
        localStats.mergeCpuMicroseconds = 0.0;
        localStats.sortCpuMicroseconds = mergeElapsed;
    }

    packet.stats.opaqueDrawCalls = static_cast<std::uint32_t>(packet.mainOpaque.size());
    packet.stats.shadowDrawCalls = static_cast<std::uint32_t>(packet.shadowCasters.size());
    packet.stats.queueBuildCpuMicroseconds =
        std::chrono::duration<double, std::micro>(Clock::now() - buildStart).count();
    localStats.totalCpuMicroseconds = packet.stats.queueBuildCpuMicroseconds;

    if (stats != nullptr)
        *stats = localStats;

    if (config.validateAgainstSerial)
    {
        const World::RenderPacket reference = BuildSerial(items, camera, light, meshInfo, options, materialInfo);
        std::uint32_t firstDifference = 0;
        if (!SameSemantics(reference, packet, firstDifference))
            throw std::runtime_error("parallel RenderPacket differs from serial oracle (first entityIndex " +
                                     std::to_string(firstDifference) + ")");
    }

    return packet;
}

void RenderPacketBuilder::BuildChunkTask(void* rawContext) noexcept
{
    auto& context = *static_cast<ChunkContext*>(rawContext);
    try
    {
        World::RenderPacket chunk =
            World::BuildRenderPacket(context.items.subspan(context.begin, context.end - context.begin), context.camera,
                                     context.light, *context.meshInfo, context.options, *context.materialInfo);

        // entityIndex 的 M4 契约是"输入 RenderItem 在快照 BuildRenderItems() 顺序中的
        // 位置"（RenderQueueBuilder.h），而 BuildRenderPacket 用**传入 span 的局部位置**
        // 填充它。chunk 是对全量快照的稳定切片，因此必须把局部索引偏移回全局位置：
        // 否则同一 entityIndex 会在多个 chunk 中重复，排序 tie-breaker 退化，串行/并行
        // 输出在可见集合上不再逐字段一致（M7-A14 的第一个真实缺陷）。
        const std::uint32_t base = context.begin;
        for (World::RenderDraw& draw : chunk.mainOpaque)
            draw.entityIndex += base;
        for (World::RenderDraw& draw : chunk.shadowCasters)
            draw.entityIndex += base;

        context.output->mainOpaque = std::move(chunk.mainOpaque);
        context.output->shadowCasters = std::move(chunk.shadowCasters);
        context.output->statistics = chunk.stats;
    }
    catch (...)
    {
        // Task functions are noexcept: park the error in the slot; the merge
        // stage rethrows it on the main thread.
        context.output->error = std::current_exception();
    }
}
} // namespace MiniEngine::Render
