// ============================================================================
// AssetUploadCoordinator.h — 上传预算、优先级与原子提交协调器（M7-07）
// 里程碑：M7-07（上传预算、热重载与背压）
// 职责：render thread 唯一的"上传推进器"：
//         1) ProcessFrame：按 priority + aging 排序候选 → 每帧预算（bytes/CPU time/请求数
//            + 热重载份额）→ 通过 UploadSink 创建 GPU 资源；
//         2) RetireCompletedUploads：fence 完成后复检 revision/依赖，交给 loader 的
//            提交事务（同一把锁里换资源 + 换 revision + 置 Ready），旧资源进延迟退休；
//         3) 事件队列：主线程在帧点 DrainEvents 派发完成通知（I/O/worker 不回调 gameplay）。
// 线程归属：本类是 render-thread 专属对象（ProcessFrame/RetireCompletedUploads/Shutdown），
//           只有 DrainEvents 允许在主线程调用。worker 与 I/O 线程都不得触碰。
// 边界：不 include 任何 RHI/平台头（tools/validation/check_rhi_boundary.py 覆盖 engine/assets）；
//       GPU 创建/上传/fence 全部经 UploadSink 注入。
// 关联：docs/architecture/README.md
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AsyncAssetLoader.h>
#include <MiniEngine/Assets/UploadBudget.h>
#include <MiniEngine/Assets/UploadSink.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniEngine::Assets
{
struct UploadCoordinatorConfig final
{
    // aging：候选等待超过该阈值就把"有效优先级"提升一档（最多 maxAgingSteps 档），
    // 防止 Prefetch 永久饥饿；每次提升都进决策 trace。阈值用纳秒以便测试确定性验证。
    std::chrono::nanoseconds agingThreshold{std::chrono::milliseconds(50)};
    std::uint8_t maxAgingSteps = 2;
    // 事件队列容量（满时丢最旧并计数；调用方每帧 DrainEvents 就不会丢）。
    std::uint32_t maxEvents = 512;
    // 决策 trace 容量（budget/fairness/aging/overrun 等要点，环形覆盖）。
    std::uint32_t maxDecisions = 128;
};

enum class UploadEventKind : std::uint8_t
{
    Ready,    // 首次发布
    Reloaded, // 覆盖了已有发布版本（热重载）
    Failed,
    Cancelled,
    Stale
};

struct UploadEvent final
{
    RequestId requestId = 0;
    AssetId assetId{};
    std::uint64_t revision = 0;
    UploadEventKind kind = UploadEventKind::Ready;
    UploadResourceToken resource = 0;
    std::size_t bytes = 0;
};

// 决策 trace（"优先级和 aging 决策必须写入 trace"的机读形态）。
struct UploadDecision final
{
    RequestId requestId = 0;
    LoadPriority requestedPriority = LoadPriority::Prefetch;
    LoadPriority effectivePriority = LoadPriority::Prefetch;
    std::uint8_t agingSteps = 0;
    bool reload = false;
    std::size_t bytes = 0;
    const char* reason = ""; // "aging" / "budget-stop" / "fairness-hold" / "single-request-overrun" / "started"
};

struct UploadCoordinatorStats final
{
    std::uint64_t framesProcessed = 0;
    std::uint64_t uploadsStarted = 0;
    std::uint64_t uploadsCommitted = 0; // 提交成功（含热重载）
    std::uint64_t reloadCommits = 0;    // 其中覆盖了旧发布资源
    std::uint64_t uploadsFailed = 0;    // sink 创建/上传失败
    std::uint64_t commitsRejected = 0;  // fence 后被拒绝（stale/取消）
    std::uint64_t deferredRetires = 0;  // 交给 sink 延迟销毁的资源数
    std::uint64_t agingPromotions = 0;
    std::uint64_t budgetStops = 0;   // 因预算耗尽提前结束的帧数
    std::uint64_t fairnessHolds = 0; // 因份额限制被推迟的候选次数
    std::uint64_t singleRequestOverruns = 0;
    std::uint64_t selectionMisses = 0; // 候选在选取前消失（被取消/已取走）
    std::uint64_t shutdownCancels = 0; // 关闭时被取消的在飞上传
    std::uint64_t eventsDropped = 0;
    std::uint32_t pendingUploadHighWater = 0; // 上传队列候选数高水位
    std::uint32_t inFlightHighWater = 0;      // 在飞上传数高水位
    std::size_t estimatedBytes = 0;
    std::size_t actualBytes = 0;
    std::size_t estimatedErrorBytes = 0; // Σ|actual - estimated|
    std::size_t residentBytes = 0;       // 已提交资源占用（按 sink 上报的实际字节）
    std::size_t releasedBytes = 0;       // 已退休掉的总字节
    std::uint64_t uploadCpuMicros = 0;   // 累加上传阶段 CPU 时间（报告用）
};

class AssetUploadCoordinator final
{
  public:
    // 依赖顺序：loader 与 sink 的生命周期都必须覆盖协调器。
    AssetUploadCoordinator(AsyncAssetLoader& loader, UploadSink& sink, UploadCoordinatorConfig config = {});
    ~AssetUploadCoordinator();

    AssetUploadCoordinator(const AssetUploadCoordinator&) = delete;
    AssetUploadCoordinator& operator=(const AssetUploadCoordinator&) = delete;

    // ---- render thread ----
    // 单帧预算内推进上传（失败/取消不计入 usage；实际字节与估计值的误差单独记账）。
    UploadUsage ProcessFrame(const UploadBudget& budget);
    // fence 完成后提交（返回本轮处理的在飞条目数；未完成的留给下一帧）。
    std::uint32_t RetireCompletedUploads();
    // 关闭：先提交 fence 已完成的，再取消其余在飞上传并交还 sink 的全部资源。
    void Shutdown();

    // ---- 主线程（帧点派发） ----
    std::size_t DrainEvents(std::vector<UploadEvent>& out);

    // ---- 查询/取证 ----
    [[nodiscard]] UploadCoordinatorStats Stats() const;
    [[nodiscard]] std::vector<UploadDecision> Decisions() const;
    [[nodiscard]] std::uint32_t InFlightUploads() const;
    [[nodiscard]] std::size_t InFlightBytes() const;
    [[nodiscard]] bool ShutdownCalled() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Assets
