// ============================================================================
// AsyncAssetLoader.cpp — 异步加载流水线的唯一实现（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：见 AsyncAssetLoader.h。本文件包含三段流水线的全部状态转移：
//   * I/O 段：单 std::jthread 从有界、按优先级取件的队列里阻塞读；
//   * decode 段：纯 CPU task（通过 DecodeDispatcher 提交），产出 typed payload；
//   * 上传段：主线程 Pump + render thread 的 BeginNextUpload/CompleteUpload（M7-07 接预算）。
// 并发模型（刻意保守）：所有可变簿记（队列、索引、计数器、记录表）由**一把**互斥锁保护；
//   锁内不做阻塞 I/O、不做 decode、不做分配密集操作。基线只有一个 I/O 线程，锁不是热点。
// 关联：docs/architecture/README.md
//       engine/assets/include/MiniEngine/Assets/Internal/AssetDecode.h（共享解码原语）
// ============================================================================

#include <MiniEngine/Assets/AsyncAssetLoader.h>

#include <MiniEngine/Assets/BakedReader.h>
#include <MiniEngine/Assets/Internal/AssetDecode.h>
#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Profiling/Profile.h>
#include <MiniEngine/Tasks/TaskSystem.h>

#include "BoundedQueue.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace MiniEngine::Assets
{
namespace
{
using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t NowNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

[[nodiscard]] double MillisecondsBetween(const std::uint64_t from, const std::uint64_t to) noexcept
{
    return to > from ? static_cast<double>(to - from) / 1.0e6 : 0.0;
}

// 最近邻分位数（sorted 必须已排序；空输入返回 0）。分位数口径写死在报告里，
// 避免"同一指标两种算法"的静默漂移。
[[nodiscard]] double QuantileNearest(const std::vector<double>& sorted, const double quantile)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(position + 0.5);
    return sorted[std::min(index, sorted.size() - 1)];
}

[[nodiscard]] std::size_t StagedBytesOf(const AssetLoadRequest& request) noexcept
{
    return request.payload != nullptr ? request.payload->ByteSize() : 0U;
}

// 进程内线程标识（std::thread::id 的哈希）：A17 的线程归属证据只需进程内可比。
[[nodiscard]] std::uint64_t CurrentThreadId() noexcept
{
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

#if ME_ENABLE_TRACY
// Tracy 时间线消息：把 RequestId/revision 带进捕获，与 raw JSON 用同一 RequestId 关联
// （M7-06 第 6 步）。只有客户端连着时才构造字符串，避免给 profile 构建添常数开销。
[[nodiscard]] std::string StageMessage(const AssetLoadRequest& request, const char* stage)
{
    return "asset request=" + std::to_string(request.requestId) + " rev=" + std::to_string(request.revision) +
           " stage=" + stage;
}
#endif

void MarkerStage(const AssetLoadRequest& request, [[maybe_unused]] const char* stage) noexcept
{
#if ME_ENABLE_TRACY
    if (ME_PROFILE_IS_CONNECTED())
    {
        ME_PROFILE_MESSAGE(StageMessage(request, stage).c_str());
    }
#else
    static_cast<void>(request);
#endif
}
} // namespace

const char* ToString(const AssetLoadErrorCode code) noexcept
{
    switch (code)
    {
    case AssetLoadErrorCode::None:
        return "none";
    case AssetLoadErrorCode::UnknownAsset:
        return "unknown-asset";
    case AssetLoadErrorCode::AssetTooLarge:
        return "asset-too-large";
    case AssetLoadErrorCode::PathRejected:
        return "path-rejected";
    case AssetLoadErrorCode::FileMissing:
        return "file-missing";
    case AssetLoadErrorCode::FileEmpty:
        return "file-empty";
    case AssetLoadErrorCode::ReadFailed:
        return "read-failed";
    case AssetLoadErrorCode::SizeMismatch:
        return "size-mismatch";
    case AssetLoadErrorCode::HashMismatch:
        return "hash-mismatch";
    case AssetLoadErrorCode::HeaderRejected:
        return "header-rejected";
    case AssetLoadErrorCode::DecodeFailed:
        return "decode-failed";
    case AssetLoadErrorCode::DecodeRejected:
        return "decode-rejected";
    case AssetLoadErrorCode::UploadFailed:
        return "upload-failed";
    case AssetLoadErrorCode::Cancelled:
        return "cancelled";
    case AssetLoadErrorCode::Stale:
        return "stale";
    case AssetLoadErrorCode::Shutdown:
        return "shutdown";
    }
    return "unknown";
}

AsyncLoadMetrics SummarizeLoads(const std::span<const AsyncLoadRecord> records)
{
    AsyncLoadMetrics metrics;
    metrics.totalRequests = static_cast<std::uint32_t>(records.size());

    std::vector<double> readyLatency;
    std::vector<double> readLatency;
    std::vector<double> decodeLatency;
    std::vector<double> queueWait;
    std::uint64_t readBytes = 0;
    double readMsTotal = 0.0;
    double readyMsTotal = 0.0;

    for (const AsyncLoadRecord& record : records)
    {
        switch (record.state)
        {
        case AssetLoadState::Ready:
            ++metrics.readyRequests;
            break;
        case AssetLoadState::Failed:
            ++metrics.failedRequests;
            break;
        case AssetLoadState::Cancelled:
            ++metrics.cancelledRequests;
            break;
        case AssetLoadState::Stale:
            ++metrics.staleRequests;
            break;
        default:
            break;
        }

        if (record.timestamps.readStartNs != 0 && record.timestamps.readEndNs != 0)
        {
            readLatency.push_back(MillisecondsBetween(record.timestamps.readStartNs, record.timestamps.readEndNs));
        }
        if (record.timestamps.decodeStartNs != 0 && record.timestamps.decodeEndNs != 0)
        {
            decodeLatency.push_back(
                MillisecondsBetween(record.timestamps.decodeStartNs, record.timestamps.decodeEndNs));
        }
        if (record.timestamps.enqueueNs != 0 && record.timestamps.readStartNs != 0)
        {
            queueWait.push_back(MillisecondsBetween(record.timestamps.enqueueNs, record.timestamps.readStartNs));
        }
        if (record.state == AssetLoadState::Ready && record.timestamps.enqueueNs != 0 && record.timestamps.readyNs != 0)
        {
            const double readyMs = MillisecondsBetween(record.timestamps.enqueueNs, record.timestamps.readyNs);
            readyLatency.push_back(readyMs);
            readyMsTotal += readyMs;
        }

        readBytes += record.bytes;
        if (record.timestamps.readStartNs != 0 && record.timestamps.readEndNs != 0)
        {
            readMsTotal += MillisecondsBetween(record.timestamps.readStartNs, record.timestamps.readEndNs);
        }
    }

    metrics.bytesRead = readBytes;
    std::sort(readyLatency.begin(), readyLatency.end());
    std::sort(readLatency.begin(), readLatency.end());
    std::sort(decodeLatency.begin(), decodeLatency.end());
    std::sort(queueWait.begin(), queueWait.end());
    metrics.readyP50Ms = QuantileNearest(readyLatency, 0.50);
    metrics.readyP95Ms = QuantileNearest(readyLatency, 0.95);
    metrics.readyP99Ms = QuantileNearest(readyLatency, 0.99);
    metrics.readP50Ms = QuantileNearest(readLatency, 0.50);
    metrics.decodeP50Ms = QuantileNearest(decodeLatency, 0.50);
    metrics.queueWaitP50Ms = QuantileNearest(queueWait, 0.50);
    if (readMsTotal > 0.0)
    {
        // 带宽口径：读入字节 / 读取累计时间（单 I/O 线程，因此这就是该线程的有效吞吐）。
        metrics.readBandwidthMiBPerSecond =
            (static_cast<double>(readBytes) / (1024.0 * 1024.0)) / (readMsTotal / 1000.0);
    }
    (void)readyMsTotal;
    return metrics;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
class AsyncAssetLoader::Impl final
{
  public:
    Impl(const AsyncAssetLoaderConfig& config, Tasks::TaskSystem& tasks, CookedFileSource& source,
         DecodeDispatcher& dispatcher, const AssetRegistry& registry)
        : m_config(config), m_tasks(tasks), m_source(source), m_dispatcher(dispatcher), m_registry(&registry),
          m_cpuReadyPark(config.maxCpuReadyRequests)
    {
        ME_VERIFY(m_config.maxIoRequests > 0 && m_config.maxUploadQueuedRequests > 0,
                  "async asset loader requires positive queue capacities");
        // decode 需要至少一个 compute worker（M7-03 契约：workerCount >= 1）。
        ME_VERIFY(m_tasks.WorkerCount() >= 1, "async asset loader requires at least one compute worker");
        m_ioThread = std::jthread([this](const std::stop_token token) { IoMain(token); });
    }

    // M7-10（A13）：owner thread 断言。上传/发布/记录释放这些"调用方线程语义"的公开 API
    // 只允许在构造该 loader 的线程上调用（生产里就是 main/render 同一物理线程）；
    // I/O 线程与 decode worker 走内部路径，不经过这里。Debug 下违反即断言失败。
    void AssertOwnerThread(const char* operation) const noexcept
    {
        ME_ASSERT(std::this_thread::get_id() == m_ownerThread, operation);
        static_cast<void>(operation);
    }

    ~Impl()
    {
        Shutdown(true);
    }

    // ---- 帧边界 ----
    void SetRegistry(const AssetRegistry& registry) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_registry = &registry;
    }

    [[nodiscard]] const AssetRegistry* Registry() const noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_registry;
    }

    // ---- 请求 ----
    RequestResult Request(const AssetId& assetId, const std::uint64_t revision, const LoadPriority priority,
                          RequestId& outRequest)
    {
        return Request(assetId, revision, priority, 0U, outRequest);
    }

    RequestResult Request(const AssetId& assetId, const std::uint64_t revision, const LoadPriority priority,
                          const std::uint64_t dependencyHash, RequestId& outRequest)
    {
        outRequest = 0;
        if (!assetId.IsValid())
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.rejectedInvalid;
            return RequestResult::Invalid;
        }

        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping)
        {
            ++m_stats.rejectedStopping;
            return RequestResult::Stopping;
        }
        const RegistryEntry* entry = m_registry != nullptr ? m_registry->Find(assetId) : nullptr;
        if (entry == nullptr)
        {
            ++m_stats.rejectedInvalid;
            return RequestResult::Invalid;
        }
        if (entry->fileSize > static_cast<std::uint64_t>(m_config.maxSingleAssetBytes))
        {
            ++m_stats.rejectedTooLarge;
            return RequestResult::TooLarge;
        }

        // M7-07：记下"该资产当前的期望 revision + 依赖声明"，提交事务据此判 stale/依赖漂移。
        m_requestedRevision[assetId] = revision;
        m_requestedDependency[assetId] = dependencyHash;

        const AssetRevisionKey key{assetId, revision};
        if (const auto found = m_liveByKey.find(key); found != m_liveByKey.end())
        {
            const auto existing = m_records.find(found->second);
            if (existing != m_records.end())
            {
                if (existing->second->State() == AssetLoadState::Ready)
                {
                    ++m_stats.requestsAlreadyReady;
                    outRequest = existing->second->requestId;
                    return RequestResult::AlreadyReady;
                }
                if (!IsTerminal(existing->second->State()))
                {
                    // 合并：增加 waiter 并提升优先级（低优先级预取遇到 Critical 请求）。
                    existing->second->waiterCount.fetch_add(1, std::memory_order_acq_rel);
                    existing->second->PromotePriority(priority);
                    ++m_stats.requestsCoalesced;
                    outRequest = existing->second->requestId;
                    return RequestResult::Coalesced;
                }
            }
            m_liveByKey.erase(found); // 终态非 Ready：按"重试创建新 RequestId"处理
        }

        // I/O 准入：请求数 + 字节预算（按 manifest fileSize 预留，读完释放）。
        if (m_ioAdmitted >= m_config.maxIoRequests ||
            m_ioAdmittedBytes + static_cast<std::size_t>(entry->fileSize) > m_config.maxIoQueuedBytes)
        {
            ++m_stats.rejectedQueueFull;
            return RequestResult::QueueFull;
        }

        auto request = std::make_shared<AssetLoadRequest>();
        request->requestId = m_nextRequestId++;
        request->assetId = assetId;
        request->revision = revision;
        request->dependencyHash = dependencyHash;
        request->priority.store(static_cast<std::uint8_t>(priority), std::memory_order_relaxed);
        request->cookedRelativePath = entry->artifactRelativePath.generic_string();
        request->expectedArtifactHash = entry->artifactHash.bytes;
        request->expectedBytes = entry->fileSize;
        request->expectedBuildKey = entry->buildKey.bytes;
        request->kind = entry->kind;
        request->timestamps.enqueueNs = NowNanoseconds();
        if (!request->TryTransition(AssetLoadState::Unloaded, AssetLoadState::Queued))
        {
            return RequestResult::Invalid;
        }

        m_records.emplace(request->requestId, request);
        m_liveByKey.emplace(key, request->requestId);
        m_latestRevision[assetId] = std::max(m_latestRevision[assetId], revision);

        m_ioQueue.push_back(request);
        ++m_ioAdmitted;
        m_ioAdmittedBytes += static_cast<std::size_t>(entry->fileSize);
        ++m_stats.requestsCreated;
        m_stats.ioAdmittedHighWater = std::max(m_stats.ioAdmittedHighWater, m_ioAdmitted);
        m_stats.ioQueueHighWater = std::max(m_stats.ioQueueHighWater, static_cast<std::uint32_t>(m_ioQueue.size()));

        outRequest = request->requestId;
        m_ioCv.notify_all();
        m_progressCv.notify_all();
        return RequestResult::Accepted;
    }

    void CancelWaiter(const RequestId request)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_records.find(request);
        if (found == m_records.end())
        {
            return;
        }
        const AssetLoadRequestPtr& target = found->second;
        std::uint32_t remaining = target->waiterCount.load(std::memory_order_acquire);
        while (remaining > 0)
        {
            if (target->waiterCount.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel))
            {
                break;
            }
        }
        if (remaining <= 1 && !IsTerminal(target->State()) && !target->IsCancelRequested())
        {
            // 只有无剩余需求时才真正请求取消（共享请求不被单个 waiter 取消）。
            target->RequestCancel();
            ++m_stats.cancelsRequested;
            m_progressCv.notify_all();
        }
    }

    [[nodiscard]] std::optional<AssetLoadState> Query(const RequestId request) const noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_records.find(request);
        if (found == m_records.end())
        {
            return std::nullopt;
        }
        return found->second->State();
    }

    [[nodiscard]] std::optional<AsyncLoadRecord> Record(const RequestId request) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_records.find(request);
        if (found == m_records.end())
        {
            return std::nullopt;
        }
        return MakeRecord(*found->second);
    }

    [[nodiscard]] std::uint32_t RetainedRecordCount() const noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<std::uint32_t>(m_records.size());
    }

    [[nodiscard]] std::uint32_t LiveRequestCount() const noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::uint32_t live = 0;
        for (const auto& [id, request] : m_records)
        {
            if (!IsTerminal(request->State()))
            {
                ++live;
            }
        }
        return live;
    }

    [[nodiscard]] std::uint64_t PublishedRevision(const AssetId& assetId) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_publishedRevision.find(assetId);
        return found != m_publishedRevision.end() ? found->second : 0U;
    }

    [[nodiscard]] std::uint64_t RequestedRevision(const AssetId& assetId) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_requestedRevision.find(assetId);
        return found != m_requestedRevision.end() ? found->second : 0U;
    }

    [[nodiscard]] UploadResourceToken PublishedResource(const AssetId& assetId) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_publishedResource.find(assetId);
        return found != m_publishedResource.end() ? found->second : 0U;
    }

    bool WaitForCpuReady(const RequestId request, const std::uint32_t timeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const auto found = m_records.find(request);
                if (found == m_records.end())
                {
                    return false;
                }
                const AssetLoadState state = found->second->State();
                // "到达 CpuReady 及之后"：Pump 可能已经把它推进到上传阶段，仍然算成功。
                if (state == AssetLoadState::CpuReady || state == AssetLoadState::UploadQueued ||
                    state == AssetLoadState::Uploading || state == AssetLoadState::Ready)
                {
                    return true;
                }
                if (IsTerminal(state))
                {
                    return false;
                }
                if (Clock::now() >= deadline)
                {
                    return false;
                }
                // 短等而不是等到 deadline：I/O/decode 的"进度通知"可能已经发生（错过通知
                // 不该吃掉整段超时预算），每轮都要重新核对状态并主动帮一把。
                m_progressCv.wait_for(lock, std::chrono::milliseconds(2));
            }
            // 帮助执行 decode（工具/测试路径；render thread 禁止调用本 API）。
            m_dispatcher.WaitDecodes();
            Pump();
        }
    }

    // ---- 帧点推进 ----
    void Pump()
    {
        AssertOwnerThread("Pump called from a non-owner thread"); // M7-10（A13）
        const std::lock_guard<std::mutex> lock(m_mutex);
        // 1) decode 已完成但受 CpuReady 名额限制的请求。
        for (auto& entry : m_records)
        {
            const AssetLoadRequestPtr& request = entry.second;
            if (request->State() == AssetLoadState::Decoding && request->decodeFinished.load(std::memory_order_acquire))
            {
                TryPublishCpuReadyLocked(request);
            }
        }
        // 2) CpuReady → UploadQueued（受上传队列上限约束）；已取消的直接落终态。
        while (m_uploadQueuedCount < m_config.maxUploadQueuedRequests)
        {
            std::optional<AssetLoadRequestPtr> parked = m_cpuReadyPark.TryPop();
            if (!parked.has_value())
            {
                break;
            }
            const AssetLoadRequestPtr& request = *parked;
            if (IsCancelledOrStaleLocked(*request))
            {
                FinishLocked(request, CancelledOrStaleTargetLocked(*request), CancelledOrStaleCodeLocked(*request),
                             "cancelled or superseded before upload");
                continue;
            }
            if (!request->TryTransition(AssetLoadState::CpuReady, AssetLoadState::UploadQueued))
            {
                continue;
            }
            request->timestamps.uploadQueuedNs = NowNanoseconds();
            m_uploadQueue.push_back(request);
            ++m_uploadQueuedCount;
            m_stats.uploadQueueHighWater =
                std::max(m_stats.uploadQueueHighWater, static_cast<std::uint32_t>(m_uploadQueuedCount));
        }
        // 3) 保留上限（M7-LOAD-RECORDS）：淘汰**终态**记录，按 requestId 从旧到新（id 单调 = 插入序）。
        //    在途请求永不淘汰：宁可暂时超上限，也不破坏流水线状态机。
        if (m_config.maxRetainedRecords != 0)
        {
            for (auto it = m_records.begin();
                 it != m_records.end() && m_records.size() > m_config.maxRetainedRecords;)
            {
                if (IsTerminal(it->second->State()))
                {
                    it = EraseRecordLocked(it);
                    ++m_stats.recordsEvicted;
                }
                else
                {
                    ++it;
                }
            }
        }
        m_progressCv.notify_all();
    }

    [[nodiscard]] std::vector<UploadCandidate> PendingUploads() const
    {
        AssertOwnerThread("PendingUploads called from a non-owner thread");
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<UploadCandidate> candidates;
        candidates.reserve(m_uploadQueue.size());
        for (const AssetLoadRequestPtr& request : m_uploadQueue)
        {
            if (request == nullptr || IsCancelledOrStaleLocked(*request))
            {
                continue; // 过期候选不下发给策略层（SelectUpload 仍会兜底清理）
            }
            const auto published = m_publishedResource.find(request->assetId);
            UploadCandidate candidate;
            candidate.requestId = request->requestId;
            candidate.assetId = request->assetId;
            candidate.kind = request->kind;
            candidate.revision = request->revision;
            candidate.dependencyHash = request->dependencyHash;
            candidate.priority = request->Priority();
            candidate.bytes = StagedBytesOf(*request);
            candidate.uploadQueuedNs = request->timestamps.uploadQueuedNs;
            candidate.reload = published != m_publishedResource.end() && published->second != 0;
            candidates.push_back(candidate);
        }
        return candidates;
    }

    [[nodiscard]] std::optional<UploadTicket> SelectUpload(const RequestId request)
    {
        AssertOwnerThread("SelectUpload called from a non-owner thread");
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_uploadQueue.begin(); it != m_uploadQueue.end(); ++it)
        {
            if ((*it)->requestId != request)
            {
                continue;
            }
            const AssetLoadRequestPtr candidate = *it;
            m_uploadQueue.erase(it);
            --m_uploadQueuedCount;
            if (IsCancelledOrStaleLocked(*candidate))
            {
                FinishLocked(candidate, CancelledOrStaleTargetLocked(*candidate),
                             CancelledOrStaleCodeLocked(*candidate), "cancelled or superseded before upload");
                return std::nullopt;
            }
            return MakeUploadTicketLocked(candidate);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<UploadTicket> BeginNextUpload()
    {
        AssertOwnerThread("BeginNextUpload called from a non-owner thread");
        const std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_uploadQueue.empty())
        {
            const AssetLoadRequestPtr request = m_uploadQueue.front();
            m_uploadQueue.pop_front();
            --m_uploadQueuedCount;
            if (IsCancelledOrStaleLocked(*request))
            {
                FinishLocked(request, CancelledOrStaleTargetLocked(*request), CancelledOrStaleCodeLocked(*request),
                             "cancelled or superseded before upload");
                continue;
            }
            if (const auto ticket = MakeUploadTicketLocked(request); ticket.has_value())
            {
                return ticket;
            }
        }
        return std::nullopt;
    }

    UploadCommitResult CommitUpload(const RequestId request, const UploadResourceToken resource)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return CommitUploadLocked(request, resource);
    }

    void CompleteUpload(const RequestId request, const bool success, const std::string_view errorText)
    {
        AssertOwnerThread("CompleteUpload called from a non-owner thread");
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_records.find(request);
        if (found == m_records.end())
        {
            return;
        }
        const AssetLoadRequestPtr& target = found->second;
        if (target->State() != AssetLoadState::Uploading)
        {
            return;
        }
        if (success)
        {
            // 走同一条事务：没有 GPU 资源时 resource == 0（简单用法/测试路径），
            // revision 与依赖复检仍然生效（旧完成不得覆盖新期望）。
            const UploadCommitResult committed = CommitUploadLocked(request, 0);
            if (committed.committed)
            {
                return;
            }
        }
        if (IsCancelledOrStaleLocked(*target))
        {
            FinishLocked(target, CancelledOrStaleTargetLocked(*target), CancelledOrStaleCodeLocked(*target),
                         "cancelled or superseded before publish");
            return;
        }
        FinishLocked(target, AssetLoadState::Failed, AssetLoadErrorCode::UploadFailed,
                     errorText.empty() ? std::string("upload failed") : std::string(errorText));
    }

    // 回收一条记录：清 (asset, revision) 的复用入口、退掉暂存字节、释放 payload。
    // 返回 erase 之后的下一个迭代器（淘汰循环要在遍历中删除）。
    std::map<RequestId, AssetLoadRequestPtr>::iterator EraseRecordLocked(
        const std::map<RequestId, AssetLoadRequestPtr>::iterator found)
    {
        const RequestId released = found->first;
        for (auto it = m_liveByKey.begin(); it != m_liveByKey.end(); ++it)
        {
            if (it->second == released)
            {
                m_liveByKey.erase(it);
                break;
            }
        }
        // Ready 请求的载荷在发布时未释放（等待上传/池提交）；此处回收其暂存字节。
        if (found->second->State() == AssetLoadState::Ready)
        {
            const std::size_t staged = StagedBytesOf(*found->second);
            m_stagedBytes -= staged <= m_stagedBytes ? staged : m_stagedBytes;
        }
        found->second->payload.reset();
        return m_records.erase(found);
    }

    void ReleaseRequest(const RequestId request)
    {
        AssertOwnerThread("ReleaseRequest called from a non-owner thread");
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_records.find(request);
        if (found == m_records.end())
        {
            return;
        }
        // M7-LOAD-RECORDS：契约是"终态请求采样完 timestamps 后释放"。在途请求的记录仍被
        // I/O/decode/上传阶段持有，必须拒绝——原先会静默删掉在途请求的状态机。
        if (!IsTerminal(found->second->State()))
        {
            ++m_stats.releaseRejectedLive;
            return;
        }
        EraseRecordLocked(found);
        ++m_stats.recordsReleased;
    }

    void Shutdown(const bool drain)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdownDone)
            {
                return;
            }
            m_shutdownDone = true;
            m_stopping = true;
            m_drainRequested = drain;
            ++m_stats.shutdowns;
        }
        m_ioThread.request_stop();
        m_ioCv.notify_all();
        m_progressCv.notify_all();
        if (m_ioThread.joinable())
        {
            m_ioThread.join();
        }
        if (drain)
        {
            // 让在途 decode 落地（主线程帮助执行）；此后不再有 worker 触碰本对象。
            m_dispatcher.WaitDecodes();
        }

        const std::lock_guard<std::mutex> lock(m_mutex);
        // 关闭后不再有 render thread 上传：所有未发布请求统一取消（不发布半完成状态）。
        for (auto& entry : m_records)
        {
            const AssetLoadRequestPtr& request = entry.second;
            if (!IsTerminal(request->State()))
            {
                FinishLocked(request, AssetLoadState::Cancelled, AssetLoadErrorCode::Shutdown,
                             "shutdown before publish");
            }
        }
        while (m_cpuReadyPark.TryPop().has_value())
        {
            // 请求已在上面统一取消；这里只清空停车区。
        }
        m_uploadQueue.clear();
        m_uploadQueuedCount = 0;
        m_stagedBytes = 0;
        m_ioQueue.clear();
        m_ioAdmitted = 0;
        m_ioAdmittedBytes = 0;
        m_progressCv.notify_all();
    }

    [[nodiscard]] AsyncAssetLoaderStats Stats() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    [[nodiscard]] std::uint64_t IoThreadId() const noexcept
    {
        return m_ioThreadId.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<AsyncLoadRecord> CollectRecords() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<AsyncLoadRecord> records;
        records.reserve(m_records.size());
        for (const auto& entry : m_records)
        {
            records.push_back(MakeRecord(*entry.second));
        }
        return records;
    }

  private:
    // (AssetId, revision) 索引键：deterministic 排序（诊断与 AlreadyReady 复用需要）。
    struct AssetRevisionKey final
    {
        AssetId asset{};
        std::uint64_t revision = 0;

        [[nodiscard]] bool operator<(const AssetRevisionKey& other) const noexcept
        {
            for (std::size_t index = 0; index < asset.bytes.size(); ++index)
            {
                if (asset.bytes[index] != other.asset.bytes[index])
                {
                    return asset.bytes[index] < other.asset.bytes[index];
                }
            }
            return revision < other.revision;
        }
    };

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

    struct DecodeContext final
    {
        Impl* loader = nullptr;
        AssetLoadRequestPtr request;
    };

    [[nodiscard]] static AsyncLoadRecord MakeRecord(const AssetLoadRequest& request)
    {
        AsyncLoadRecord record;
        record.requestId = request.requestId;
        record.assetId = request.assetId;
        record.revision = request.revision;
        record.priority = request.Priority();
        record.state = request.State();
        record.error = request.error;
        record.bytes = request.expectedBytes;
        record.decodeExecutedOnWorker = request.decodeExecutedOnWorker;
        record.ioThreadId = request.ioThreadId;
        record.decodeThreadId = request.decodeThreadId;
        record.timestamps = request.timestamps;
        record.errorText = request.errorText;
        return record;
    }

    [[nodiscard]] bool IsCancelledOrStaleLocked(const AssetLoadRequest& request) const
    {
        if (request.IsCancelRequested())
        {
            return true;
        }
        const auto latest = m_latestRevision.find(request.assetId);
        return latest != m_latestRevision.end() && request.revision < latest->second;
    }

    // UploadQueued → Uploading 并组装 ticket（调用方必须已持锁）。
    [[nodiscard]] std::optional<UploadTicket> MakeUploadTicketLocked(const AssetLoadRequestPtr& request)
    {
        if (!request->TryTransition(AssetLoadState::UploadQueued, AssetLoadState::Uploading))
        {
            return std::nullopt;
        }
        request->timestamps.uploadStartNs = NowNanoseconds();
        MarkerStage(*request, "upload-start");
        UploadTicket ticket;
        ticket.requestId = request->requestId;
        ticket.assetId = request->assetId;
        ticket.kind = request->kind;
        ticket.revision = request->revision;
        ticket.dependencyHash = request->dependencyHash;
        ticket.priority = request->Priority();
        ticket.bytes = StagedBytesOf(*request);
        ticket.uploadQueuedNs = request->timestamps.uploadQueuedNs;
        ticket.payload = request->payload.get();
        return ticket;
    }

    // 提交事务（调用方必须已持锁）。要点：
    //   1) 先复检"这个上传还有效吗"——取消、被更高 revision 取代、期望 revision 已变、
    //      依赖声明已变，任一不满足就不提交（旧版本继续可用）；
    //   2) 换资源与换 revision、Uploading → Ready 在**同一临界区**完成：读者要么看到
    //      旧 revision + 旧资源，要么看到新 revision + 新资源，不存在混合瞬态；
    //   3) 返回被替换下来的旧资源，由 render 侧交给 sink 延迟退休。
    [[nodiscard]] UploadCommitResult CommitUploadLocked(const RequestId request, const UploadResourceToken resource)
    {
        UploadCommitResult result;
        const auto found = m_records.find(request);
        if (found == m_records.end())
        {
            result.rejectReason = "unknown-request";
            return result;
        }
        const AssetLoadRequestPtr& target = found->second;
        if (target->State() != AssetLoadState::Uploading)
        {
            result.terminalState = target->State();
            result.rejectReason = "not-uploading";
            return result;
        }
        if (IsCancelledOrStaleLocked(*target))
        {
            const AssetLoadState state = CancelledOrStaleTargetLocked(*target);
            const AssetLoadErrorCode code = CancelledOrStaleCodeLocked(*target);
            FinishLocked(target, state, code, "cancelled or superseded before commit");
            result.terminalState = state;
            result.rejectReason = state == AssetLoadState::Cancelled ? "cancelled" : "stale-revision";
            ++m_stats.commitsRejected;
            return result;
        }
        const auto requested = m_requestedRevision.find(target->assetId);
        if (requested != m_requestedRevision.end() && requested->second != target->revision)
        {
            FinishLocked(target, AssetLoadState::Stale, AssetLoadErrorCode::Stale,
                         "a newer revision was requested before commit");
            result.terminalState = AssetLoadState::Stale;
            result.rejectReason = "stale-revision";
            ++m_stats.commitsRejected;
            return result;
        }
        const auto dependency = m_requestedDependency.find(target->assetId);
        if (dependency != m_requestedDependency.end() && dependency->second != target->dependencyHash)
        {
            // 依赖组合已变（例如 material N+1 引用的 texture 已换版本）：不发布混合组合。
            FinishLocked(target, AssetLoadState::Stale, AssetLoadErrorCode::Stale,
                         "dependency hash changed before commit");
            result.terminalState = AssetLoadState::Stale;
            result.rejectReason = "dependency-mismatch";
            ++m_stats.commitsRejected;
            return result;
        }
        if (!target->TryTransition(AssetLoadState::Uploading, AssetLoadState::Ready))
        {
            result.terminalState = target->State();
            result.rejectReason = "not-uploading";
            return result;
        }

        target->timestamps.readyNs = NowNanoseconds();
        MarkerStage(*target, "ready");
        const auto publishedRevision = m_publishedRevision.find(target->assetId);
        if (publishedRevision == m_publishedRevision.end() || publishedRevision->second < target->revision)
        {
            m_publishedRevision[target->assetId] = target->revision;
        }
        if (resource != 0)
        {
            const auto publishedResource = m_publishedResource.find(target->assetId);
            if (publishedResource != m_publishedResource.end())
            {
                result.oldResource = publishedResource->second;
            }
            m_publishedResource[target->assetId] = resource;
        }
        result.committed = true;
        result.committedRevision = target->revision;
        result.terminalState = AssetLoadState::Ready;
        ++m_stats.uploadsCommitted;
        if (result.oldResource != 0)
        {
            ++m_stats.reloadCommits; // 覆盖了已有发布资源 = 热重载提交
        }
        m_progressCv.notify_all();
        return result;
    }

    // 取消/过期请求的终态与错误码（被更高 revision 取代 → Stale；显式取消 → Cancelled）。
    [[nodiscard]] AssetLoadState CancelledOrStaleTargetLocked(const AssetLoadRequest& request) const
    {
        if (request.IsCancelRequested())
        {
            return AssetLoadState::Cancelled;
        }
        const auto latest = m_latestRevision.find(request.assetId);
        if (latest != m_latestRevision.end() && request.revision < latest->second)
        {
            return AssetLoadState::Stale;
        }
        return AssetLoadState::Cancelled;
    }

    [[nodiscard]] AssetLoadErrorCode CancelledOrStaleCodeLocked(const AssetLoadRequest& request) const
    {
        return CancelledOrStaleTargetLocked(request) == AssetLoadState::Stale ? AssetLoadErrorCode::Stale
                                                                              : AssetLoadErrorCode::Cancelled;
    }

    // 终态落地（必须持锁）：状态机校验 → 记录错误 → 释放载荷与暂存字节预算 → 清 key 索引。
    void FinishLocked(const AssetLoadRequestPtr& request, const AssetLoadState desired, const AssetLoadErrorCode code,
                      std::string text)
    {
        const AssetLoadState from = request->State();
        if (IsTerminal(from) || !IsLegalTransition(from, desired) || !request->TryTransition(from, desired))
        {
            return;
        }
        request->error = code;
        request->errorText = std::move(text);
        if (desired != AssetLoadState::Ready)
        {
            const std::size_t staged = StagedBytesOf(*request);
            m_stagedBytes -= staged <= m_stagedBytes ? staged : m_stagedBytes;
            request->payload.reset();
            const AssetRevisionKey key{request->assetId, request->revision};
            if (const auto found = m_liveByKey.find(key);
                found != m_liveByKey.end() && found->second == request->requestId)
            {
                m_liveByKey.erase(found);
            }
        }
        m_progressCv.notify_all();
    }

    // decode 完成后的发布：CpuReady 停车区（请求数 + 字节双上限）。
    // 名额不足时保持 Decoding（payload 已在请求上），由主线程 Pump 重试。
    void TryPublishCpuReadyLocked(const AssetLoadRequestPtr& request)
    {
        if (request->State() != AssetLoadState::Decoding)
        {
            return;
        }
        if (IsCancelledOrStaleLocked(*request))
        {
            FinishLocked(request, CancelledOrStaleTargetLocked(*request), CancelledOrStaleCodeLocked(*request),
                         "cancelled or superseded before publish");
            return;
        }
        const std::size_t bytes = StagedBytesOf(*request);
        if (m_stagedBytes + bytes > m_config.maxCpuReadyBytes)
        {
            return;
        }
        if (!m_cpuReadyPark.TryPush(request))
        {
            return;
        }
        m_stagedBytes += bytes;
        m_stats.cpuReadyHighWater =
            std::max(m_stats.cpuReadyHighWater, static_cast<std::uint32_t>(m_cpuReadyPark.Size()));
        m_progressCv.notify_all(); // 等待者（工具/测试的 WaitForCpuReady）需要看到这一格进度
        if (!request->TryTransition(AssetLoadState::Decoding, AssetLoadState::CpuReady))
        {
            // 理论上不可达（持锁且状态已核对）；保持可诊断。
            ME_LOG_WARNING("asset load: CpuReady transition rejected");
        }
    }

    [[nodiscard]] AssetLoadRequestPtr TakeHighestPriorityLocked()
    {
        if (m_ioQueue.empty())
        {
            return nullptr;
        }
        // 小规模（≤ maxIoRequests）线性挑最高优先级；同优先级按 requestId 升序（FIFO 稳定）。
        std::size_t best = 0;
        for (std::size_t index = 1; index < m_ioQueue.size(); ++index)
        {
            const AssetLoadRequest& candidate = *m_ioQueue[index];
            const AssetLoadRequest& current = *m_ioQueue[best];
            if (static_cast<std::uint8_t>(candidate.Priority()) > static_cast<std::uint8_t>(current.Priority()) ||
                (candidate.Priority() == current.Priority() && candidate.requestId < current.requestId))
            {
                best = index;
            }
        }
        const AssetLoadRequestPtr request = m_ioQueue[best];
        m_ioQueue.erase(m_ioQueue.begin() + static_cast<std::ptrdiff_t>(best));
        return request;
    }

    void ReleaseIoAdmissionLocked(const AssetLoadRequestPtr& request)
    {
        --m_ioAdmitted;
        const std::size_t reserved = static_cast<std::size_t>(request->expectedBytes);
        m_ioAdmittedBytes -= reserved <= m_ioAdmittedBytes ? reserved : m_ioAdmittedBytes;
    }

    // ---- I/O 线程 ----
    void IoMain(const std::stop_token token)
    {
        ME_PROFILE_THREAD(m_config.ioThreadName);
        m_ioThreadId.store(CurrentThreadId(), std::memory_order_release);
        for (;;)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ioCv.wait(lock, token, [this] { return !m_ioQueue.empty() || m_stopping; });
            if (m_stopping && !m_drainRequested)
            {
                // fatal 关闭：不再触碰磁盘，未开始的请求统一取消。
                while (!m_ioQueue.empty())
                {
                    const AssetLoadRequestPtr request = TakeHighestPriorityLocked();
                    FinishLocked(request, AssetLoadState::Cancelled, AssetLoadErrorCode::Shutdown,
                                 "shutdown (cancelling)");
                    ReleaseIoAdmissionLocked(request);
                }
                break;
            }
            if (m_ioQueue.empty())
            {
                if (m_stopping)
                {
                    break; // drain 完成
                }
                continue;
            }
            const AssetLoadRequestPtr request = TakeHighestPriorityLocked();
            lock.unlock();
            ProcessRead(request);
            lock.lock();
            ReleaseIoAdmissionLocked(request);
        }
    }

    void ProcessRead(const AssetLoadRequestPtr& request)
    {
        if (!request->TryTransition(AssetLoadState::Queued, AssetLoadState::Reading))
        {
            return;
        }
        request->timestamps.readStartNs = NowNanoseconds();
        request->ioThreadId = CurrentThreadId();
        MarkerStage(*request, "read-start");
        m_progressCv.notify_all();

        {
            ME_PROFILE_ZONE_NAMED("AssetRead");
            if (IsCancelledOrStaleNoLock(*request))
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, CancelledOrStaleTargetLocked(*request), CancelledOrStaleCodeLocked(*request),
                             "cancelled or superseded before read");
                return;
            }
            CookedFileRead read = m_source.Read(request->cookedRelativePath, m_config.maxSingleAssetBytes);
            request->timestamps.readEndNs = NowNanoseconds();
            MarkerStage(*request, "read-end");
            if (!read.Succeeded())
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, read.error, read.errorText);
                return;
            }
            if (request->expectedBytes != 0 && read.bytes.size() != request->expectedBytes)
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::SizeMismatch,
                             "artifact size differs from the manifest");
                return;
            }

            const std::uint64_t bytes = read.bytes.size();
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.bytesRead += bytes;
                if (!IsCancelledOrStaleLocked(*request))
                {
                    request->fileBytes = std::move(read.bytes);
                }
                else
                {
                    // 取消/过期发生在读取期间：字节已读入，记入浪费并释放。
                    m_stats.cancelWasteBytes += bytes;
                    FinishLocked(request, CancelledOrStaleTargetLocked(*request), CancelledOrStaleCodeLocked(*request),
                                 "cancelled or superseded during read");
                    return;
                }
            }
        }

        if (IsCancelledOrStaleNoLock(*request))
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (FinishLockedIfActive(request, "cancelled or superseded before decode"))
            {
                m_stats.cancelWasteBytes += request->fileBytes.size();
                request->fileBytes.clear();
            }
            return;
        }
        if (!request->TryTransition(AssetLoadState::Reading, AssetLoadState::Decoding))
        {
            return;
        }
        m_progressCv.notify_all();

        // 所有权转移：fileBytes + 请求本体交给 decode task；执行器拒绝时由本函数清理。
        auto* context = new DecodeContext{this, request};
        if (!m_dispatcher.SubmitDecode(&Impl::DecodeEntry, context))
        {
            delete context;
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.decodeRejected;
            FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::DecodeRejected,
                         "decode executor rejected the task");
        }
    }

    // I/O / decode 阶段在**不持锁**时调用的预检：内部取锁读 m_latestRevision。
    // 结果只用于"提前失败"，因此不需要与后续状态转移构成原子对（终态仍由 FinishLocked 把关）。
    [[nodiscard]] bool IsCancelledOrStaleNoLock(const AssetLoadRequest& request) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return IsCancelledOrStaleLocked(request);
    }

    [[nodiscard]] bool FinishLockedIfActive(const AssetLoadRequestPtr& request, std::string text)
    {
        const AssetLoadState from = request->State();
        if (IsTerminal(from))
        {
            return false;
        }
        FinishLocked(request, CancelledOrStaleTargetLocked(*request), CancelledOrStaleCodeLocked(*request),
                     std::move(text));
        return true;
    }

    // ---- decode 任务 ----
    static void DecodeEntry(void* rawContext) noexcept
    {
        std::unique_ptr<DecodeContext> context(static_cast<DecodeContext*>(rawContext));
        try
        {
            context->loader->ProcessDecode(context->request);
        }
        catch (...)
        {
            const std::lock_guard<std::mutex> lock(context->loader->m_mutex);
            context->loader->FinishLocked(context->request, AssetLoadState::Failed, AssetLoadErrorCode::DecodeFailed,
                                          "decode task threw an exception");
        }
    }

    void ProcessDecode(const AssetLoadRequestPtr& request)
    {
        request->decodeExecutedOnWorker = Tasks::TaskSystem::CurrentWorkerIndex() != Tasks::kNonWorkerIndex;
        request->timestamps.decodeStartNs = NowNanoseconds();
        request->decodeThreadId = CurrentThreadId();
        MarkerStage(*request, "decode-start");

        {
            ME_PROFILE_ZONE_NAMED("AssetDecode");
            if (IsCancelledOrStaleNoLock(*request))
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (FinishLockedIfActive(request, "cancelled or superseded during decode"))
                {
                    m_stats.cancelWasteBytes += request->fileBytes.size();
                    request->fileBytes.clear();
                }
                return;
            }
            if (request->fileBytes.empty())
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::FileEmpty,
                             "artifact bytes missing at decode");
                return;
            }

            // 1) 内容哈希：manifest 声明的 artifactHash 必须与读到的字节一致。
            const Sha256Digest digest = Sha256(std::span<const std::byte>(request->fileBytes));
            if (digest != request->expectedArtifactHash)
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::HashMismatch,
                             "artifact hash differs from the manifest");
                return;
            }

            // 2) 结构校验（magic/version/chunk 表/BuildKey）。
            BakedReadExpectation expectation{};
            expectation.kind = Internal::ToBakedKind(request->kind);
            expectation.buildKey = request->expectedBuildKey;
            BakedReadResult readResult;
            std::string stageError;
            if (!BakedReader::Parse(std::span<const std::byte>(request->fileBytes), expectation, readResult,
                                    stageError))
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::HeaderRejected, stageError);
                return;
            }

            // 3) 分配前的输出上界：chunk payload 是文件的子集，因此用文件字节数做闸门。
            if (request->fileBytes.size() > m_config.maxDecodedAssetBytes)
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::AssetTooLarge,
                             "decoded payload would exceed the configured limit");
                return;
            }

            // 4) typed decode：与同步路径共用同一实现（Internal/AssetDecode.h）。
            auto payload = std::make_unique<CpuAssetPayload>();
            bool decoded = true;
            switch (request->kind)
            {
            case AssetKind::Mesh:
            {
                MeshAsset mesh;
                decoded = Internal::DecodeMeshChunks(request->fileBytes, readResult, mesh, stageError);
                if (decoded)
                {
                    payload->kind = AssetPayloadKind::Mesh;
                    payload->mesh = std::move(mesh);
                }
                break;
            }
            case AssetKind::Texture:
            {
                TextureAsset texture;
                decoded = Internal::DecodeTextureChunks(request->fileBytes, readResult, texture, stageError);
                if (decoded)
                {
                    payload->kind = AssetPayloadKind::Texture;
                    payload->texture = std::move(texture);
                }
                break;
            }
            case AssetKind::Material:
            {
                MaterialAsset material;
                decoded = Internal::DecodeMaterialChunks(request->fileBytes, readResult, material, stageError);
                if (decoded)
                {
                    payload->kind = AssetPayloadKind::Material;
                    payload->material = std::move(material);
                }
                break;
            }
            case AssetKind::World:
                // World 的两遍实例化属于 engine/world；assets 层只交付已校验字节。
                payload->kind = AssetPayloadKind::RawBytes;
                payload->rawBytes = std::move(request->fileBytes);
                break;
            }
            if (!decoded)
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                FinishLocked(request, AssetLoadState::Failed, AssetLoadErrorCode::DecodeFailed, stageError);
                return;
            }

            request->timestamps.decodeEndNs = NowNanoseconds();
            MarkerStage(*request, "decode-end");
            request->fileBytes.clear();
            request->fileBytes.shrink_to_fit();
            request->payload = std::move(payload);
            request->decodeFinished.store(true, std::memory_order_release);
        }

        const std::lock_guard<std::mutex> lock(m_mutex);
        TryPublishCpuReadyLocked(request);
    }

    // ---- 数据成员 ----
    const AsyncAssetLoaderConfig m_config;
    Tasks::TaskSystem& m_tasks;
    CookedFileSource& m_source;
    DecodeDispatcher& m_dispatcher;
    const AssetRegistry* m_registry = nullptr;

    mutable std::mutex m_mutex;
    std::condition_variable_any m_ioCv;
    std::condition_variable m_progressCv;
    std::jthread m_ioThread;
    std::atomic<std::uint64_t> m_ioThreadId{0};
    const std::thread::id m_ownerThread{std::this_thread::get_id()}; // M7-10（A13）

    bool m_stopping = false;
    bool m_drainRequested = false;
    bool m_shutdownDone = false;

    // I/O 准入（Queued + Reading）：请求数与按 manifest fileSize 预留的字节预算。
    std::vector<AssetLoadRequestPtr> m_ioQueue;
    std::uint32_t m_ioAdmitted = 0;
    std::size_t m_ioAdmittedBytes = 0;

    // CpuReady 停车区（请求数上限由 BoundedQueue 保证，字节由 m_stagedBytes 计费）。
    BoundedQueue<AssetLoadRequestPtr> m_cpuReadyPark;
    std::size_t m_stagedBytes = 0;

    // 上传队列（FIFO，请求数上限）。
    std::deque<AssetLoadRequestPtr> m_uploadQueue;
    std::uint32_t m_uploadQueuedCount = 0;

    std::map<RequestId, AssetLoadRequestPtr> m_records;
    std::map<AssetRevisionKey, RequestId> m_liveByKey;
    std::map<AssetId, std::uint64_t, AssetIdLess> m_latestRevision;
    std::map<AssetId, std::uint64_t, AssetIdLess> m_publishedRevision;
    // M7-07 提交事务的状态：期望 revision / 依赖声明 / 当前发布资源 token。
    std::map<AssetId, std::uint64_t, AssetIdLess> m_requestedRevision;
    std::map<AssetId, std::uint64_t, AssetIdLess> m_requestedDependency;
    std::map<AssetId, UploadResourceToken, AssetIdLess> m_publishedResource;
    RequestId m_nextRequestId = 1;
    AsyncAssetLoaderStats m_stats;
};

// ---------------------------------------------------------------------------
// 门面转发
// ---------------------------------------------------------------------------
AsyncAssetLoader::AsyncAssetLoader(const AsyncAssetLoaderConfig& config, Tasks::TaskSystem& tasks,
                                   CookedFileSource& source, DecodeDispatcher& dispatcher,
                                   const AssetRegistry& registry)
    : m_impl(std::make_unique<Impl>(config, tasks, source, dispatcher, registry))
{
}

AsyncAssetLoader::~AsyncAssetLoader() = default;

void AsyncAssetLoader::SetRegistry(const AssetRegistry& registry) noexcept
{
    m_impl->SetRegistry(registry);
}

const AssetRegistry* AsyncAssetLoader::Registry() const noexcept
{
    return m_impl->Registry();
}

RequestResult AsyncAssetLoader::Request(const AssetId& assetId, const std::uint64_t revision,
                                        const LoadPriority priority, RequestId& outRequest)
{
    return m_impl->Request(assetId, revision, priority, outRequest);
}

RequestResult AsyncAssetLoader::Request(const AssetId& assetId, const std::uint64_t revision,
                                        const LoadPriority priority, const std::uint64_t dependencyHash,
                                        RequestId& outRequest)
{
    return m_impl->Request(assetId, revision, priority, dependencyHash, outRequest);
}

void AsyncAssetLoader::CancelWaiter(const RequestId request)
{
    m_impl->CancelWaiter(request);
}

std::optional<AssetLoadState> AsyncAssetLoader::Query(const RequestId request) const noexcept
{
    return m_impl->Query(request);
}

std::optional<AsyncLoadRecord> AsyncAssetLoader::Record(const RequestId request) const
{
    return m_impl->Record(request);
}

std::uint32_t AsyncAssetLoader::LiveRequestCount() const noexcept
{
    return m_impl->LiveRequestCount();
}

std::uint32_t AsyncAssetLoader::RetainedRecordCount() const noexcept
{
    return m_impl->RetainedRecordCount();
}

std::uint64_t AsyncAssetLoader::PublishedRevision(const AssetId& assetId) const
{
    return m_impl->PublishedRevision(assetId);
}

std::uint64_t AsyncAssetLoader::RequestedRevision(const AssetId& assetId) const
{
    return m_impl->RequestedRevision(assetId);
}

UploadResourceToken AsyncAssetLoader::PublishedResource(const AssetId& assetId) const
{
    return m_impl->PublishedResource(assetId);
}

bool AsyncAssetLoader::WaitForCpuReady(const RequestId request, const std::uint32_t timeoutMs)
{
    return m_impl->WaitForCpuReady(request, timeoutMs);
}

void AsyncAssetLoader::Pump()
{
    m_impl->Pump();
}

std::optional<UploadTicket> AsyncAssetLoader::BeginNextUpload()
{
    return m_impl->BeginNextUpload();
}

std::vector<UploadCandidate> AsyncAssetLoader::PendingUploads() const
{
    return m_impl->PendingUploads();
}

std::optional<UploadTicket> AsyncAssetLoader::SelectUpload(const RequestId request)
{
    return m_impl->SelectUpload(request);
}

void AsyncAssetLoader::CompleteUpload(const RequestId request, const bool success, const std::string_view errorText)
{
    m_impl->CompleteUpload(request, success, errorText);
}

UploadCommitResult AsyncAssetLoader::CommitUpload(const RequestId request, const UploadResourceToken resource)
{
    return m_impl->CommitUpload(request, resource);
}

void AsyncAssetLoader::ReleaseRequest(const RequestId request)
{
    m_impl->ReleaseRequest(request);
}

void AsyncAssetLoader::Shutdown(const bool drain)
{
    m_impl->Shutdown(drain);
}

AsyncAssetLoaderStats AsyncAssetLoader::Stats() const
{
    return m_impl->Stats();
}

std::vector<AsyncLoadRecord> AsyncAssetLoader::CollectRecords() const
{
    return m_impl->CollectRecords();
}

std::uint64_t AsyncAssetLoader::IoThreadId() const noexcept
{
    return m_impl->IoThreadId();
}

AsyncLoadMetrics AsyncAssetLoader::Metrics() const
{
    const std::vector<AsyncLoadRecord> records = CollectRecords();
    AsyncLoadMetrics metrics = SummarizeLoads(records);
    const AsyncAssetLoaderStats& stats = m_impl->Stats();
    metrics.coalescedRequests = static_cast<std::uint32_t>(stats.requestsCoalesced);
    metrics.ioQueueHighWater = stats.ioQueueHighWater;
    metrics.cpuReadyHighWater = stats.cpuReadyHighWater;
    metrics.cancelWasteBytes = stats.cancelWasteBytes;
    metrics.bytesRead = stats.bytesRead;
    metrics.maxConcurrentReads = 1; // 单 I/O 线程基线；多线程实验另开篇目
    return metrics;
}
} // namespace MiniEngine::Assets
