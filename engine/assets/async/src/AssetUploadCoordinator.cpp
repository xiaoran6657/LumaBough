// ============================================================================
// AssetUploadCoordinator.cpp — 上传预算、优先级与原子提交实现（M7-07）
// 关联：docs/architecture/README.md
//       engine/assets/async/src/AsyncAssetLoader.cpp（提交事务 CommitUpload）
// 规则来源：code/engine/assets/async/src/UploadBudget.cpp.snippet.txt（每帧处理）
//           code/engine/assets/async/src/AtomicCommit.cpp.snippet.txt（fence 后提交）
//           两处片段按本仓库真实接口适配：RHI 经 UploadSink 注入；registry 事务在 loader 内
//           （同一把锁里换资源 + 换 revision），因此不存在"先 CAS 后换 registry"的瞬态窗口。
// ============================================================================

#include <MiniEngine/Assets/AssetUploadCoordinator.h>

#include <MiniEngine/Profiling/Profile.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <string>

namespace MiniEngine::Assets
{
namespace
{
using Clock = std::chrono::steady_clock;

struct AssetIdLess final
{
    [[nodiscard]] bool operator()(const AssetId& left, const AssetId& right) const noexcept
    {
        for (std::size_t index = 0; index < left.bytes.size(); ++index)
        {
            if (left.bytes[index] != right.bytes[index])
            {
                return left.bytes[index] < right.bytes[index];
            }
        }
        return false;
    }
};

// LoadPriority 的数值语义：Prefetch(0) < Visible(1) < Critical(2)，数值越大越紧急。
[[nodiscard]] std::uint8_t PriorityLevel(const LoadPriority priority) noexcept
{
    return static_cast<std::uint8_t>(priority);
}

constexpr std::uint8_t kCriticalLevel = static_cast<std::uint8_t>(LoadPriority::Critical);

[[nodiscard]] LoadPriority PriorityFromLevel(const std::uint8_t level) noexcept
{
    if (level >= kCriticalLevel)
    {
        return LoadPriority::Critical;
    }
    if (level == static_cast<std::uint8_t>(LoadPriority::Visible))
    {
        return LoadPriority::Visible;
    }
    return LoadPriority::Prefetch;
}

// 有效等级：等待越久等级越高（最多 maxAgingSteps 档，封顶 Critical），防低优先级永久饥饿。
[[nodiscard]] std::uint8_t EffectiveLevel(const LoadPriority base, const std::uint64_t waitNs,
                                          const UploadCoordinatorConfig& config) noexcept
{
    const std::uint8_t baseLevel = PriorityLevel(base);
    if (config.agingThreshold.count() <= 0 || config.maxAgingSteps == 0)
    {
        return baseLevel;
    }
    const auto thresholdNs = static_cast<std::uint64_t>(config.agingThreshold.count());
    const std::uint64_t steps = waitNs / thresholdNs;
    const auto aging = static_cast<std::uint8_t>(std::min<std::uint64_t>(steps, config.maxAgingSteps));
    return std::min<std::uint8_t>(static_cast<std::uint8_t>(baseLevel + aging), kCriticalLevel);
}

void Marker([[maybe_unused]] const char* stage, [[maybe_unused]] const RequestId requestId,
            [[maybe_unused]] const std::uint64_t revision, [[maybe_unused]] const std::uint8_t baseRank,
            [[maybe_unused]] const std::uint8_t rank) noexcept
{
#if ME_ENABLE_TRACY
    if (!ME_PROFILE_IS_CONNECTED())
    {
        return;
    }
    const std::string text = "upload request=" + std::to_string(requestId) + " rev=" + std::to_string(revision) +
                             " prio=" + std::to_string(baseRank) + "->" + std::to_string(rank) + " stage=" + stage;
    ME_PROFILE_MESSAGE(text.c_str());
#endif
}

struct InFlightUpload final
{
    RequestId requestId = 0;
    AssetId assetId{};
    std::uint64_t revision = 0;
    UploadResourceToken resource = 0;
    std::uint64_t fence = 0;
    std::size_t actualBytes = 0;
    bool reload = false;
};
} // namespace

class AssetUploadCoordinator::Impl final
{
  public:
    Impl(AsyncAssetLoader& loader, UploadSink& sink, UploadCoordinatorConfig config)
        : m_loader(loader), m_sink(sink), m_config(config)
    {
    }

    // ---- render thread：每帧预算内推进上传 ----
    UploadUsage ProcessFrame(const UploadBudget& budget)
    {
        ME_PROFILE_ZONE_NAMED("AssetUploadBudget");
        UploadUsage usage;
        const auto frameStart = Clock::now();
        ++m_stats.framesProcessed;

        const std::vector<UploadCandidate> pending = m_loader.PendingUploads();
        if (pending.size() > m_stats.pendingUploadHighWater)
        {
            m_stats.pendingUploadHighWater = static_cast<std::uint32_t>(pending.size());
        }

        struct Ordered final
        {
            UploadCandidate candidate;
            std::uint8_t baseLevel = 0;
            std::uint8_t level = 0;
        };
        std::vector<Ordered> ordered;
        ordered.reserve(pending.size());
        const std::uint64_t now = NowNanoseconds();
        for (const UploadCandidate& candidate : pending)
        {
            const std::uint64_t waitNs = now > candidate.uploadQueuedNs ? now - candidate.uploadQueuedNs : 0;
            Ordered entry;
            entry.candidate = candidate;
            entry.baseLevel = PriorityLevel(candidate.priority);
            entry.level = EffectiveLevel(candidate.priority, waitNs, m_config);
            ordered.push_back(entry);
        }
        // 排序键：有效等级降序（Critical 在前）→ 入队时间（同级 FIFO 稳定）→ RequestId（完全确定）。
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const Ordered& left, const Ordered& right)
                         {
                             if (left.level != right.level)
                             {
                                 return left.level > right.level;
                             }
                             if (left.candidate.uploadQueuedNs != right.candidate.uploadQueuedNs)
                             {
                                 return left.candidate.uploadQueuedNs < right.candidate.uploadQueuedNs;
                             }
                             return left.candidate.requestId < right.candidate.requestId;
                         });

        for (const Ordered& entry : ordered)
        {
            const UploadCandidate& candidate = entry.candidate;
            const std::size_t estimatedBytes = candidate.bytes;
            usage.cpuTime = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - frameStart);

            if (!usage.CanStartClass(estimatedBytes, candidate.reload, budget))
            {
                if (!usage.CanStart(estimatedBytes, budget))
                {
                    ++m_stats.budgetStops; // 三约束任一耗尽：本帧到此为止，剩余留到下一帧
                    RecordDecision(entry, "budget-stop");
                    break;
                }
                ++m_stats.fairnessHolds; // 只超了热重载份额：跳过，让 streaming 用掉这一帧余量
                RecordDecision(entry, "fairness-hold");
                continue;
            }

            if (entry.level > entry.baseLevel)
            {
                ++m_stats.agingPromotions;
                RecordDecision(entry, "aging");
                Marker("aging", candidate.requestId, candidate.revision, entry.baseLevel, entry.level);
            }
            if (usage.bytes == 0 && estimatedBytes > budget.maxBytes)
            {
                // 首个请求可以超预算：大资产不会被小预算永久饿死（净超出单独记账）。
                usage.estimatedOverrunBytes += estimatedBytes - budget.maxBytes;
                ++m_stats.singleRequestOverruns;
                RecordDecision(entry, "single-request-overrun");
            }

            const std::optional<UploadTicket> ticket = m_loader.SelectUpload(candidate.requestId);
            if (!ticket.has_value())
            {
                ++m_stats.selectionMisses; // 期间被取消/已取走：换下一个候选
                continue;
            }

            UploadRequestDescription description;
            description.requestId = ticket->requestId;
            description.assetId = ticket->assetId;
            description.revision = ticket->revision;
            description.dependencyHash = ticket->dependencyHash;
            description.kind = ticket->kind;
            description.byteCount = ticket->bytes;
            description.payload = ticket->payload;

            const UploadResult result = m_sink.CreateAndUpload(description);
            ++m_stats.uploadsStarted;
            if (!result.success)
            {
                // 失败不计入 usage（预算只记成功消耗），请求落 Failed；sink 的原因原样入证据。
                ++m_stats.uploadsFailed;
                m_loader.CompleteUpload(ticket->requestId, false, result.errorText);
                RecordDecision(entry, "create-failed");
                continue;
            }

            const std::size_t actualBytes = result.actualBytes != 0 ? result.actualBytes : estimatedBytes;
            usage.bytes += actualBytes;
            (candidate.reload ? usage.reloadBytes : usage.streamBytes) += actualBytes;
            ++usage.requests;
            m_stats.estimatedBytes += estimatedBytes;
            m_stats.actualBytes += actualBytes;
            m_stats.estimatedErrorBytes +=
                actualBytes > estimatedBytes ? actualBytes - estimatedBytes : estimatedBytes - actualBytes;

            InFlightUpload inFlight;
            inFlight.requestId = ticket->requestId;
            inFlight.assetId = ticket->assetId;
            inFlight.revision = ticket->revision;
            inFlight.resource = result.resource;
            inFlight.fence = result.completionFence;
            inFlight.actualBytes = actualBytes;
            inFlight.reload = candidate.reload;
            m_inFlight.push_back(inFlight);
            if (m_inFlight.size() > m_stats.inFlightHighWater)
            {
                m_stats.inFlightHighWater = static_cast<std::uint32_t>(m_inFlight.size());
            }
            RecordDecision(entry, "started");
            Marker("gpu-create", candidate.requestId, candidate.revision, entry.baseLevel, entry.level);
        }

        usage.cpuTime = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - frameStart);
        m_stats.uploadCpuMicros += static_cast<std::uint64_t>(usage.cpuTime.count());
        return usage;
    }

    // ---- render thread：fence 完成 → 复检 → 原子提交 / 退休 ----
    std::uint32_t RetireCompletedUploads()
    {
        ME_PROFILE_ZONE_NAMED("AssetUploadRetire");
        std::uint32_t processed = 0;
        for (auto iterator = m_inFlight.begin(); iterator != m_inFlight.end();)
        {
            if (!m_sink.IsFenceComplete(iterator->fence))
            {
                ++iterator;
                continue;
            }
            const UploadCommitResult commit = m_loader.CommitUpload(iterator->requestId, iterator->resource);
            if (commit.committed)
            {
                if (commit.oldResource != 0)
                {
                    // 旧 revision 进入延迟退休（D3D11/D3D12 各自保证"逻辑不可用 + GPU 用完后释放"）。
                    m_sink.DeferDestroy(commit.oldResource);
                    ++m_stats.deferredRetires;
                    const auto old = m_resourceBytes.find(iterator->assetId);
                    if (old != m_resourceBytes.end())
                    {
                        m_stats.residentBytes -=
                            old->second <= m_stats.residentBytes ? old->second : m_stats.residentBytes;
                        m_stats.releasedBytes += old->second;
                    }
                }
                m_resourceBytes[iterator->assetId] = iterator->actualBytes;
                m_stats.residentBytes += iterator->actualBytes;
                ++m_stats.uploadsCommitted;
                const bool reload = iterator->reload || commit.oldResource != 0;
                if (reload)
                {
                    ++m_stats.reloadCommits;
                }
                PushEvent(*iterator, reload ? UploadEventKind::Reloaded : UploadEventKind::Ready);
                Marker("commit", iterator->requestId, iterator->revision, 0, 0);
            }
            else
            {
                // 未提交（stale/取消/依赖漂移）：新资源立即退休，绝不发布半提交状态。
                m_sink.DeferDestroy(iterator->resource);
                ++m_stats.deferredRetires;
                ++m_stats.commitsRejected;
                PushEvent(*iterator, EventKindFor(commit));
                Marker("commit-rejected", iterator->requestId, iterator->revision, 0, 0);
            }
            iterator = m_inFlight.erase(iterator);
            ++processed;
        }
        return processed;
    }

    void Shutdown()
    {
        if (m_shutdownCalled)
        {
            return;
        }
        // fence 已完成的照常提交（不丢已经正确完成的结果）；其余在飞一律取消 + 退休。
        RetireCompletedUploads();
        for (const InFlightUpload& upload : m_inFlight)
        {
            m_sink.DeferDestroy(upload.resource);
            ++m_stats.deferredRetires;
            m_loader.CompleteUpload(upload.requestId, false);
            ++m_stats.shutdownCancels;
            PushEvent(upload, UploadEventKind::Cancelled);
        }
        m_inFlight.clear();
        m_sink.ReleaseAll();
        m_shutdownCalled = true;
    }

    // ---- 主线程：帧点派发 ----
    std::size_t DrainEvents(std::vector<UploadEvent>& out)
    {
        const std::size_t count = m_events.size();
        out.insert(out.end(), m_events.begin(), m_events.end());
        m_events.clear();
        return count;
    }

    [[nodiscard]] UploadCoordinatorStats Stats() const
    {
        return m_stats;
    }

    [[nodiscard]] std::vector<UploadDecision> Decisions() const
    {
        return {m_decisions.begin(), m_decisions.end()};
    }

    [[nodiscard]] std::uint32_t InFlightUploads() const noexcept
    {
        return static_cast<std::uint32_t>(m_inFlight.size());
    }

    [[nodiscard]] std::size_t InFlightBytes() const noexcept
    {
        std::size_t bytes = 0;
        for (const InFlightUpload& upload : m_inFlight)
        {
            bytes += upload.actualBytes;
        }
        return bytes;
    }

    [[nodiscard]] bool ShutdownCalled() const noexcept
    {
        return m_shutdownCalled;
    }

  private:
    [[nodiscard]] static std::uint64_t NowNanoseconds() noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    }

    [[nodiscard]] static UploadEventKind EventKindFor(const UploadCommitResult& commit) noexcept
    {
        switch (commit.terminalState)
        {
        case AssetLoadState::Stale:
            return UploadEventKind::Stale;
        case AssetLoadState::Failed:
            return UploadEventKind::Failed;
        case AssetLoadState::Cancelled:
            return UploadEventKind::Cancelled;
        default:
            return commit.rejectReason == "cancelled" ? UploadEventKind::Cancelled : UploadEventKind::Stale;
        }
    }

    void PushEvent(const InFlightUpload& upload, const UploadEventKind kind)
    {
        if (m_events.size() >= m_config.maxEvents)
        {
            m_events.pop_front();
            ++m_stats.eventsDropped;
        }
        UploadEvent event;
        event.requestId = upload.requestId;
        event.assetId = upload.assetId;
        event.revision = upload.revision;
        event.kind = kind;
        event.resource = kind == UploadEventKind::Ready || kind == UploadEventKind::Reloaded ? upload.resource : 0U;
        event.bytes = upload.actualBytes;
        m_events.push_back(event);
    }

    // 决策 trace：只记要点（aging/budget/fairness/overrun/启动/失败），普通排队等待不记。
    template <typename OrderedEntry> void RecordDecision(const OrderedEntry& entry, const char* reason)
    {
        if (m_decisions.size() >= m_config.maxDecisions)
        {
            m_decisions.pop_front();
        }
        UploadDecision decision;
        decision.requestId = entry.candidate.requestId;
        decision.requestedPriority = entry.candidate.priority;
        decision.effectivePriority = PriorityFromLevel(entry.level);
        decision.agingSteps =
            entry.level > entry.baseLevel ? static_cast<std::uint8_t>(entry.level - entry.baseLevel) : 0;
        decision.reload = entry.candidate.reload;
        decision.bytes = entry.candidate.bytes;
        decision.reason = reason;
        m_decisions.push_back(decision);
    }

    AsyncAssetLoader& m_loader;
    UploadSink& m_sink;
    UploadCoordinatorConfig m_config;
    std::vector<InFlightUpload> m_inFlight;
    std::deque<UploadEvent> m_events;
    std::deque<UploadDecision> m_decisions;
    std::map<AssetId, std::size_t, AssetIdLess> m_resourceBytes;
    UploadCoordinatorStats m_stats;
    bool m_shutdownCalled = false;
};

AssetUploadCoordinator::AssetUploadCoordinator(AsyncAssetLoader& loader, UploadSink& sink,
                                               UploadCoordinatorConfig config)
    : m_impl(std::make_unique<Impl>(loader, sink, config))
{
}

AssetUploadCoordinator::~AssetUploadCoordinator() = default;

UploadUsage AssetUploadCoordinator::ProcessFrame(const UploadBudget& budget)
{
    return m_impl->ProcessFrame(budget);
}

std::uint32_t AssetUploadCoordinator::RetireCompletedUploads()
{
    return m_impl->RetireCompletedUploads();
}

void AssetUploadCoordinator::Shutdown()
{
    m_impl->Shutdown();
}

std::size_t AssetUploadCoordinator::DrainEvents(std::vector<UploadEvent>& out)
{
    return m_impl->DrainEvents(out);
}

UploadCoordinatorStats AssetUploadCoordinator::Stats() const
{
    return m_impl->Stats();
}

std::vector<UploadDecision> AssetUploadCoordinator::Decisions() const
{
    return m_impl->Decisions();
}

std::uint32_t AssetUploadCoordinator::InFlightUploads() const
{
    return m_impl->InFlightUploads();
}

std::size_t AssetUploadCoordinator::InFlightBytes() const
{
    return m_impl->InFlightBytes();
}

bool AssetUploadCoordinator::ShutdownCalled() const noexcept
{
    return m_impl->ShutdownCalled();
}
} // namespace MiniEngine::Assets
