// ============================================================================
// AsyncAssetLoader.h — 异步资产加载流水线（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：把"会卡住主线程的文件读取与 decode"拆成三段可观测、可取消、可去重的流水线：
//
//     I/O 线程（唯一阻塞读者）        compute worker / main-help       render thread
//     ─────────────────────────      ─────────────────────────────    ───────────────
//     Queued → Reading → →→→→→       Decoding → CpuReady  →→→→→→       UploadQueued
//        （路径沙箱/尺寸上限/取消）      （纯 CPU，不碰 RHI）              → Uploading → Ready
//
// 线程归属（ADR-0008，硬约束）：
//   * 只有 I/O 线程读文件；compute worker 与 render thread 都不做磁盘 I/O；
//   * decode 在 compute worker（或等待线程帮助）执行，绝不调用 RHI / 不创建 GPU 句柄；
//   * 上传与资源切换只在 render thread（M7-07 的预算上传器消费 UploadTicket）；
//   * 完成通知只进 loader 的内部记录，主线程在帧点显式 Pump/取样，I/O 与 worker
//     绝不回调 gameplay 或修改 World。
// 背压（A19）：I/O 队列（请求数 + 准入字节）、decode 提交（TaskSystem 有界队列）、
//   CpuReady 停车区（请求数 + 字节）、上传队列（请求数）全部有界；满载行为是
//   "拒绝并给分类结果"，不是阻塞调用者或无声丢请求。
// 关联：docs/architecture/README.md
//       engine/tasks/include/MiniEngine/Tasks/TaskSystem.h（提交/等待角色契约）
//       engine/assets/src/AssetManager.cpp（同步基线；两条路径共享解码原语）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetLoadPayload.h>
#include <MiniEngine/Assets/AssetLoadRequest.h>
#include <MiniEngine/Assets/CookedFileSource.h>
#include <MiniEngine/Assets/UploadSink.h> // UploadResourceToken（提交事务的返回值）
#include <MiniEngine/Tasks/Task.h>        // Tasks::TaskFunction（decode 入口签名）

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace MiniEngine::Tasks
{
class TaskSystem;
}

namespace MiniEngine::Assets
{
// 队列与上限配置。默认值面向 50k 场景的流式加载；测试用小值逼出背压。
struct AsyncAssetLoaderConfig final
{
    // I/O 阶段：同时处于 Queued+Reading 的请求数上限，以及它们的准入字节预算
    // （按 manifest fileSize 预留；读取完成后释放）。
    std::uint32_t maxIoRequests = 256;
    std::size_t maxIoQueuedBytes = 256u * 1024u * 1024u;
    // CpuReady 停车区：等待 render thread 取走的 payload 上限（请求数 + 字节）。
    std::uint32_t maxCpuReadyRequests = 128;
    std::size_t maxCpuReadyBytes = 512u * 1024u * 1024u;
    // 上传队列：已 Pump 但未被 render thread 取走的上限。
    std::uint32_t maxUploadQueuedRequests = 32;
    // M7-LOAD-RECORDS：终态请求记录的**保留上限**（0 = 不限制，只建议测试/单次运行使用）。
    // 记录默认保留到调用方 ReleaseRequest（"保留错误历史"）；但长跑里若调用方只采样不释放，
    // payload 会随请求数线性增长（M7-10 soak：10k 帧热重载 +499 MiB 进程工作集）。
    // 超上限时在 Pump 里按 requestId 从旧到新淘汰**终态**记录；在途请求永不淘汰。
    // 淘汰会移除该 (asset, revision) 的"已就绪复用"入口，但**不影响**已发布 revision 与资源。
    std::uint32_t maxRetainedRecords = 1024;
    // 单产物上限（读取前判定，避免为大文件分配缓冲）。
    std::size_t maxSingleAssetBytes = 256u * 1024u * 1024u;
    // decode 输出上限（乘法/尺寸防御的第二道闸）。
    std::size_t maxDecodedAssetBytes = 256u * 1024u * 1024u;
    // I/O 线程名（同时进系统线程名与 Tracy 线程名）。
    const char* ioThreadName = "ME Asset IO";
};

// 准入结果：调用方按它决定"已合并/已就绪/可重试/参数错误"。
enum class RequestResult : std::uint8_t
{
    Accepted,     // 新建请求并准入
    Coalesced,    // 同 (AssetId, revision) 已有在途请求：增加 waiter（可能提升优先级）
    AlreadyReady, // 同 (AssetId, revision) 已是 Ready：直接复用
    QueueFull,    // I/O 阶段请求数或字节预算已满（背压；调用方自行决定重试时机）
    TooLarge,     // 超过 maxSingleAssetBytes：读取前拒绝
    Invalid,      // registry 无该 AssetId / AssetId 无效
    Stopping      // Shutdown 已开始
};

// decode 提交端口：生产实现是 TaskSystemDecodeDispatcher（compute worker + main-help）；
// 测试实现可手动驱动完成顺序（去重/乱序 revision 的确定性验证）。
class DecodeDispatcher
{
  public:
    DecodeDispatcher() = default;
    virtual ~DecodeDispatcher() = default;

    DecodeDispatcher(const DecodeDispatcher&) = delete;
    DecodeDispatcher& operator=(const DecodeDispatcher&) = delete;

    // 提交一次 decode。返回 false 表示执行器拒绝（队列满/已停止）：调用方按失败处理，
    // 不阻塞 I/O 线程、不丢所有权。
    [[nodiscard]] virtual bool SubmitDecode(Tasks::TaskFunction entry, void* context) noexcept = 0;
    // 等待已提交的 decode 完成；调用线程会帮助执行（main-help 语义）。
    // 只允许工具/测试/主线程调用，禁止 render thread 与 I/O 线程调用。
    virtual void WaitDecodes() noexcept = 0;
};

// render thread 取走的待上传条目（payload 指针在 CommitUpload/CompleteUpload/ReleaseRequest 前有效）。
struct UploadTicket final
{
    RequestId requestId = 0;
    AssetId assetId{};
    AssetKind kind = AssetKind::Mesh;
    std::uint64_t revision = 0;
    std::uint64_t dependencyHash = 0;
    LoadPriority priority = LoadPriority::Prefetch;
    std::size_t bytes = 0;
    std::uint64_t uploadQueuedNs = 0; // 排序/aging 用（enqueue→ready 的分段证据）
    const CpuAssetPayload* payload = nullptr;
};

// 上传队列候选（只读快照；携带顺序由等待方决定，选择用 SelectUpload 原子生效）。
struct UploadCandidate final
{
    RequestId requestId = 0;
    AssetId assetId{};
    AssetKind kind = AssetKind::Mesh;
    std::uint64_t revision = 0;
    std::uint64_t dependencyHash = 0;
    LoadPriority priority = LoadPriority::Prefetch;
    std::size_t bytes = 0;
    std::uint64_t uploadQueuedNs = 0;
    // 该资产已有已发布资源 → 这次提交会覆盖旧版本（热重载类）。
    bool reload = false;
};

// fence 完成后的原子提交结果（render thread）。
// 事务语义：复检 state == Uploading、期望 revision、依赖 hash 之后，在**同一把锁**里
// 换（published revision + resource）与（Uploading → Ready），读者不会看到
// "Ready/旧资源"或"新资源/Uploading"的瞬态组合。
struct UploadCommitResult final
{
    bool committed = false;
    std::uint64_t committedRevision = 0;
    UploadResourceToken oldResource = 0;                      // 提交成功时被替换下来的旧资源（交还给 sink 延迟退休）
    AssetLoadState terminalState = AssetLoadState::Uploading; // 未提交时的落点
    std::string rejectReason; // "stale-revision" / "dependency-mismatch" / "cancelled" / "not-uploading"
};

// 终态请求的机读快照（报告/测试/账本共用；不持有 payload）。
struct AsyncLoadRecord final
{
    RequestId requestId = 0;
    AssetId assetId{};
    std::uint64_t revision = 0;
    LoadPriority priority = LoadPriority::Prefetch;
    AssetLoadState state = AssetLoadState::Unloaded;
    AssetLoadErrorCode error = AssetLoadErrorCode::None;
    std::uint64_t bytes = 0;
    bool decodeExecutedOnWorker = false;
    std::uint64_t ioThreadId = 0;
    std::uint64_t decodeThreadId = 0;
    LoadTimestamps timestamps{};
    std::string errorText;
};

// 聚合指标（文档第 6 步）：分位数用最近邻法，输入为空时为 0。
struct AsyncLoadMetrics final
{
    std::uint32_t totalRequests = 0;
    std::uint32_t readyRequests = 0;
    std::uint32_t failedRequests = 0;
    std::uint32_t cancelledRequests = 0;
    std::uint32_t staleRequests = 0;
    std::uint32_t coalescedRequests = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t cancelWasteBytes = 0;
    double readyP50Ms = 0.0;
    double readyP95Ms = 0.0;
    double readyP99Ms = 0.0;
    double readP50Ms = 0.0;
    double decodeP50Ms = 0.0;
    double queueWaitP50Ms = 0.0; // enqueue → readStart（I/O 队列等待）
    double readBandwidthMiBPerSecond = 0.0;
    std::uint32_t ioQueueHighWater = 0;
    std::uint32_t cpuReadyHighWater = 0;
    std::uint32_t maxConcurrentReads = 1;
};

// 累计计数器（诊断与测试断言；不参与控制流）。
struct AsyncAssetLoaderStats final
{
    std::uint64_t requestsCreated = 0;
    std::uint64_t requestsCoalesced = 0;
    std::uint64_t requestsAlreadyReady = 0;
    std::uint64_t rejectedQueueFull = 0;
    std::uint64_t rejectedTooLarge = 0;
    std::uint64_t rejectedInvalid = 0;
    std::uint64_t rejectedStopping = 0;
    std::uint64_t cancelsRequested = 0; // waiter 归零触发的取消
    std::uint64_t cancelWasteBytes = 0; // 取消/过期请求已读入的字节（浪费证据）
    std::uint64_t bytesRead = 0;
    std::uint64_t decodeRejected = 0;
    std::uint32_t ioQueueHighWater = 0;
    std::uint32_t ioAdmittedHighWater = 0;
    std::uint32_t cpuReadyHighWater = 0;
    std::uint32_t uploadQueueHighWater = 0;
    std::uint64_t recordsReleased = 0;
    std::uint64_t recordsEvicted = 0;      // 超上限被淘汰的终态记录（M7-LOAD-RECORDS）
    std::uint64_t releaseRejectedLive = 0; // 对在途请求调用 ReleaseRequest 被拒绝的次数
    std::uint64_t shutdowns = 0;
    // M7-07 提交事务：成功提交数、其中覆盖旧版本的（热重载）数、被拒绝的提交数。
    std::uint64_t uploadsCommitted = 0;
    std::uint64_t reloadCommits = 0;
    std::uint64_t commitsRejected = 0;
};

// 记录聚合（纯函数）。stats 非空时把队列 high-water/合并计数/带宽等并入结果。
[[nodiscard]] AsyncLoadMetrics SummarizeLoads(std::span<const AsyncLoadRecord> records);

class AsyncAssetLoader final
{
  public:
    // 依赖顺序：tasks / source / dispatcher / registry 的生命周期都必须覆盖 loader。
    // 关闭顺序约定：render system → asset loader → task system（见 TaskSystem.h 头注释）。
    AsyncAssetLoader(const AsyncAssetLoaderConfig& config, Tasks::TaskSystem& tasks, CookedFileSource& source,
                     DecodeDispatcher& dispatcher, const AssetRegistry& registry);
    ~AsyncAssetLoader();

    AsyncAssetLoader(const AsyncAssetLoader&) = delete;
    AsyncAssetLoader& operator=(const AsyncAssetLoader&) = delete;

    // ---- 帧边界：registry 切换（hot reload；M7-07 的提交路径复用） ----
    void SetRegistry(const AssetRegistry& registry) noexcept;
    [[nodiscard]] const AssetRegistry* Registry() const noexcept;

    // ---- 请求 ----
    // 以 (AssetId, revision) 合并重复请求：
    //   * 已有在途同 key 请求：waiterCount++ 并提升优先级 → Coalesced；
    //   * 已 Ready 的同 key 请求：直接返回其 RequestId → AlreadyReady；
    //   * 终态为 Failed/Cancelled/Stale 的同 key 请求：视为历史，创建新请求（重试语义）；
    //   * 新 revision：创建新请求；旧在途请求在完成时被判为 Stale（不覆盖新版本）。
    [[nodiscard]] RequestResult Request(const AssetId& assetId, std::uint64_t revision, LoadPriority priority,
                                        RequestId& outRequest);
    // M7-07：带依赖身份的请求。dependencyHash 是调用方对"该 revision 的依赖组合"的声明；
    // 提交（CommitUpload）时会复检它是否仍是当前声明，不等则拒绝发布（事务式提交）。
    [[nodiscard]] RequestResult Request(const AssetId& assetId, std::uint64_t revision, LoadPriority priority,
                                        std::uint64_t dependencyHash, RequestId& outRequest);
    // 释放一个 waiter；只有 waiter 归零才向 I/O/decode 阶段发起取消。
    void CancelWaiter(RequestId request);

    // ---- 查询（主线程） ----
    // 返回请求当前状态；RequestId 不存在（已释放/从未创建）时返回 nullopt。
    [[nodiscard]] std::optional<AssetLoadState> Query(RequestId request) const noexcept;
    [[nodiscard]] std::optional<AsyncLoadRecord> Record(RequestId request) const;
    [[nodiscard]] std::uint32_t LiveRequestCount() const noexcept;
    // 当前保留的记录数（在途 + 终态未释放）。长跑证据用它证明"记录不随请求数增长"。
    [[nodiscard]] std::uint32_t RetainedRecordCount() const noexcept;
    // 已完成发布（Ready）的最高 revision；从未发布过为 0。测试断言"旧完成不覆盖新版本"用它。
    [[nodiscard]] std::uint64_t PublishedRevision(const AssetId& assetId) const;
    // 最近一次请求声明的期望 revision（提交事务用它判 stale；从未请求过为 0）。
    [[nodiscard]] std::uint64_t RequestedRevision(const AssetId& assetId) const;
    // 已发布的 GPU 资源 token（0 = 没有已提交的资源）。render 侧据此取当前资源绘制。
    [[nodiscard]] UploadResourceToken PublishedResource(const AssetId& assetId) const;

    // 工具/测试专用：等待请求到达 CpuReady（等待线程帮助执行 decode）。
    // 超时返回 false（默认 5 s：工具路径不应无限挂起）；禁止在 render thread 调用。
    [[nodiscard]] bool WaitForCpuReady(RequestId request, std::uint32_t timeoutMs = 5000);

    // ---- 帧点推进（主线程） ----
    // 1) 把 decode 已完成但受 CpuReady 名额限制的请求推进到 CpuReady；
    // 2) 把 CpuReady 请求推进到上传队列（受上传队列上限限制）；已取消的直接落 Cancelled。
    void Pump();

    // ---- 上传阶段（render thread） ----
    // 上传队列的只读快照：排序/预算/aging 的策略在 AssetUploadCoordinator 里，
    // 这里只给机制（候选 + 原子选取）。
    [[nodiscard]] std::vector<UploadCandidate> PendingUploads() const;
    // 把候选原子推进 UploadQueued → Uploading 并返回 ticket；已被取消/过期/不在队列返回 nullopt。
    [[nodiscard]] std::optional<UploadTicket> SelectUpload(RequestId request);
    // 旧接口：取队首（保持 M7-06 的简单用法）；新代码用 PendingUploads + SelectUpload。
    [[nodiscard]] std::optional<UploadTicket> BeginNextUpload();
    // 上传失败的落点（state == Uploading 且无资源可提交）。
    // errorText 为空时用默认 "upload failed"；sink 的具体原因（RHI 异常文本）应原样传入，
    // 否则证据只能看到"失败了"而看不到"为什么"。
    void CompleteUpload(RequestId request, bool success, std::string_view errorText = {});
    // M7-07 成功路径：fence 已完成后在**同一把锁**里完成"复检 + 换资源 + 换 revision + Ready"。
    // 返回被替换下来的旧资源 token（0 = 无旧资源）；未提交时返回拒绝原因与终态。
    [[nodiscard]] UploadCommitResult CommitUpload(RequestId request, UploadResourceToken resource);

    // 记账回收：终态请求采样完 timestamps 后调用（不影响已发布状态与 payload 的所有权）。
    void ReleaseRequest(RequestId request);

    // drain=true：停止准入、排空 I/O 队列与 decode、让在途请求走到终态；
    // drain=false：未开始的任务直接取消，Queued 请求全部落 Cancelled（fatal 路径）。
    // 幂等；析构函数在未显式关闭时按 drain=true 兜底。
    void Shutdown(bool drain);

    // 计数器按值返回：调用点（报告/测试）与 I/O/decode 线程并发，拿快照而不是引用。
    [[nodiscard]] AsyncAssetLoaderStats Stats() const;
    // 终态请求的快照集合（未释放的）。
    [[nodiscard]] std::vector<AsyncLoadRecord> CollectRecords() const;
    [[nodiscard]] AsyncLoadMetrics Metrics() const;
    // 专用 I/O 线程的进程内标识（0 表示尚未启动）；用于断言"读文件只发生在这条线程"。
    [[nodiscard]] std::uint64_t IoThreadId() const noexcept;

  private:
    // 实现体（pimpl）：线程、队列与记账都在 .cpp 内，public header 不暴露实现细节。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Assets
