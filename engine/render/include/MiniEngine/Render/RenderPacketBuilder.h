// ============================================================================
// RenderPacketBuilder.h — M7-05 并行 RenderPacket 构建（chunk 划分 + serial oracle）
// 里程碑：M7-05。线程边界（ADR-0008 第 8 条）：FixedUpdate 完成后，worker 只读取
//   冻结的 RenderItem 快照，cull/extract 写入 chunk 独占 slot，merge 按 chunkIndex
//   （绝不按完成时间），最后用与串行完全相同的比较器做全局排序。worker 不触碰
//   RHI/World/组件，不共享 vector push_back，不逐 entity 写共享 atomic。
// 生命周期：BuildParallel 是同步调用——items/lookup 在调用期间保持有效（Wait 返回
//   即没有任务再访问它们），"快照冻结后存活"由调用结构保证。
// 关联：docs/architecture/README.md
//       engine/world/src/RenderQueueBuilder.cpp（chunk 单元与串行 oracle）
// ============================================================================

#pragma once

#include <MiniEngine/Tasks/TaskSystem.h>
#include <MiniEngine/World/DirectionalLight.h>
#include <MiniEngine/World/RenderPacket.h>
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <MiniEngine/World/WorldTypes.h>

#include <cstdint>
#include <exception>
#include <span>
#include <vector>

namespace MiniEngine::Render
{
struct RenderPacketBuildConfig final
{
    // 固定划分：[0,chunk), [chunk,2*chunk), ...；每个 chunk 有稳定 slot。
    // sweep 结论（M7-A12）：chunk 必须 ≥256（更细会被任务开销淹没）。
    std::uint32_t chunkSize = 256;
    // Debug/测试抽样：并行构建后跑一次串行 oracle 并逐字段比对（语义字段，不含计时）。
    bool validateAgainstSerial = false;
    // E-M7-RP-004 的实验开关：chunk 输出按估计可见率 reserve（默认）或完全依赖扩容增长。
    // 默认 true；关闭只用于 A/B 实验，不改变语义输出。
    bool reserveChunkCapacity = true;
};

// chunk 独占输出：每个 chunk 独立 cull/extract/排序，merge 时按 chunkIndex 串联。
struct RenderPacketChunk final
{
    std::vector<World::RenderDraw> mainOpaque;
    std::vector<World::RenderDraw> shadowCasters;
    World::CullingStats statistics{};
    // chunk 单元内的异常：任务函数 noexcept，异常存放在 slot，由合并阶段在主线程重抛。
    std::exception_ptr error{};
};

// 并行构建的阶段统计（M7-A16 实验与测试证据）。计时是 CPU 测量值，不参与语义比对；
// 该结构是可选输出，缺省时零开销（指针为空即不写）。
struct RenderPacketBuildStats final
{
    std::uint32_t chunkCount = 0;     // 固定划分的 chunk 总数（空输入为 0）
    std::uint32_t taskChunks = 0;     // 成功提交进任务系统的 chunk
    std::uint32_t syncChunks = 0;     // 队列满时在主线程同步执行的 chunk（有界背压回退）
    std::uint32_t mergedDraws = 0;    // 合并进 packet 的 draw 总数（main + shadow）
    double waitCpuMicroseconds = 0.0; // Wait（含 main-help 执行）
    double mergeCpuMicroseconds = 0.0;
    double sortCpuMicroseconds = 0.0;
    double totalCpuMicroseconds = 0.0; // 整个 BuildParallel（不含 validateAgainstSerial）
};

class RenderPacketBuilder final
{
  public:
    explicit RenderPacketBuilder(Tasks::TaskSystem& tasks);

    // 串行 oracle：直接使用 World::BuildRenderPacket（M4 契约）。并行落地后保留，
    // 供 Debug 抽样 shadow build、测试与 validation 使用，不得删除。
    [[nodiscard]] World::RenderPacket BuildSerial(const std::span<const World::RenderItem> items,
                                                  const World::RenderPacketCamera& camera,
                                                  const World::DirectionalLight& light,
                                                  const World::MeshInfoLookup& meshInfo,
                                                  const World::CullingOptions& options,
                                                  const World::MaterialInfoLookup& materialInfo = {}) const;

    // 并行构建：chunk 固定划分 → chunk-local cull/extract（复用 World::BuildRenderPacket
    // 同一实现与排序契约）→ 按 chunkIndex 合并 → 全局排序（同一比较器）→ 发布。
    // 失败：任一 chunk 抛出的异常在 Wait 之后于主线程重新抛出。
    // stats 非空时写入阶段统计（含 QueueFull 同步回退计数），供测试与实验读取。
    [[nodiscard]] World::RenderPacket BuildParallel(
        const std::span<const World::RenderItem> items, const World::RenderPacketCamera& camera,
        const World::DirectionalLight& light, const World::MeshInfoLookup& meshInfo,
        const World::CullingOptions& options, const World::MaterialInfoLookup& materialInfo,
        const RenderPacketBuildConfig& config, RenderPacketBuildStats* stats = nullptr);

  private:
    struct ChunkContext;

    static void BuildChunkTask(void* rawContext) noexcept;

    Tasks::TaskSystem& m_tasks;
};
} // namespace MiniEngine::Render
