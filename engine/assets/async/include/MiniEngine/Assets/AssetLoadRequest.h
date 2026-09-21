// ============================================================================
// AssetLoadRequest.h — 单个资产加载请求的身份、状态与逐阶段时间戳（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：定义请求的全部可观测字段与唯一的状态转移入口（CAS + Debug 断言）。
//       线程归属（ADR-0008）：
//         * requestId/assetId/revision/priority/cookedRelativePath/期望哈希与字节数
//           —— 在**主线程**准入时写好，之后只读；
//         * fileBytes —— I/O 线程写入并移交给 decode task（同一时刻只有一个所有者）；
//         * cpuPayload —— decode task 写入，render thread 取走上传；
//         * timestamps —— 每阶段只写自己的字段（单写者），终态后由主线程读取；
//         状态与其他跨线程可见的字段用 acquire/release 的 CAS 建立可见性。
// 关联：docs/architecture/README.md「每次请求携带」
//       engine/assets/async/src/AsyncAssetLoader.cpp
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/AssetLoadState.h>
#include <MiniEngine/Assets/AssetRegistry.h> // AssetKind（decode 派发字段）
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Core/Assert.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MiniEngine::Assets
{
// 请求身份：进程内单调递增，跨 run 不保证稳定（不是内容寻址身份）。
using RequestId = std::uint64_t;

// 请求优先级：决定准入排序（高优先级先被 I/O 线程取走）。同资产同 revision 的
// 重复请求会把已有请求提升到较高的优先级，而不是复制一份。
enum class LoadPriority : std::uint8_t
{
    Prefetch = 0,
    Visible = 1,
    Critical = 2
};

// 失败分类：写进请求与诊断；不含绝对路径与敏感信息。
enum class AssetLoadErrorCode : std::uint8_t
{
    None = 0,
    UnknownAsset,   // registry 里没有该 AssetId
    AssetTooLarge,  // 超过 maxSingleAssetBytes / maxDecodedAssetBytes
    PathRejected,   // 路径沙箱拒绝（绝对路径/.. 段/分隔符）
    FileMissing,    // 文件不存在或无法打开
    FileEmpty,      // 0 字节
    ReadFailed,     // short read / 权限 / I/O 错误
    SizeMismatch,   // 实际字节数 != manifest fileSize
    HashMismatch,   // SHA-256 != manifest artifactHash
    HeaderRejected, // BakedReader 结构校验失败（magic/version/chunk 表…）
    DecodeFailed,   // chunk → typed payload 失败（schema/尺寸/因子域）
    DecodeRejected, // decode task 未被执行器接受（队列满/系统停止）
    UploadFailed,   // 上传阶段失败（M7-07 起由 render thread 上报）
    Cancelled,      // 无剩余 waiter 的取消
    Stale,          // 被更高 revision 取代
    Shutdown        // 关闭时仍在途（drain=false）
};

// 逐阶段时间戳（steady_clock 纳秒，仅用于间隔/聚合，不是墙钟）。
struct LoadTimestamps final
{
    std::uint64_t enqueueNs = 0;      // 准入（主线程）
    std::uint64_t readStartNs = 0;    // I/O 线程取得请求
    std::uint64_t readEndNs = 0;      // 读+哈希校验完成
    std::uint64_t decodeStartNs = 0;  // decode task 开始
    std::uint64_t decodeEndNs = 0;    // payload 生成完毕
    std::uint64_t uploadQueuedNs = 0; // 进入上传队列（Pump 推进到 UploadQueued）
    std::uint64_t uploadStartNs = 0;  // render thread 取走（UploadQueued → Uploading）
    std::uint64_t readyNs = 0;        // 发布（Ready）
};

// 请求对象。含 std::atomic，因此不可拷贝/移动：生命周期由 shared_ptr 管理，
// 终态后仍保留到调用方 ReleaseRequest（"保留错误历史但不无限自动重试"）。
struct AssetLoadRequest final
{
    // ---- 身份（准入时写，之后只读） ----
    RequestId requestId = 0;
    AssetId assetId{};
    std::uint64_t revision = 0;
    std::uint64_t dependencyHash = 0;
    std::string cookedRelativePath;               // registry 解析后的相对路径（已过沙箱）
    Sha256Digest expectedArtifactHash{};          // manifest 声明的产物哈希
    std::uint64_t expectedBytes = 0;              // manifest 声明的产物字节数
    std::array<std::byte, 32> expectedBuildKey{}; // BakedReader 期望的 BuildKey
    AssetKind kind = AssetKind::Mesh;             // 决定 decode 派发（不重复查 registry）

    // ---- 并发字段 ----
    std::atomic<AssetLoadState> state{AssetLoadState::Unloaded};
    // 取消代数：0 = 未请求取消；非 0 = 已请求（值本身只用于诊断与"请求代"语义）。
    std::atomic<std::uint32_t> cancelGeneration{0};
    // 剩余需求数：归零才真正请求取消（共享请求不被单个 waiter 取消）。
    std::atomic<std::uint32_t> waiterCount{1};
    // 优先级可在合并时被提升（低优先级预取遇到 Critical 请求）。
    std::atomic<std::uint8_t> priority{static_cast<std::uint8_t>(LoadPriority::Prefetch)};
    // decode task 是否已产出 payload（Decoding 态下为真表示"等 CpuReady 名额"）。
    std::atomic<bool> decodeFinished{false};

    // ---- 阶段所有权字段（同一时刻只有一个所有者） ----
    std::vector<std::byte> fileBytes;                // I/O 线程 → decode task
    std::unique_ptr<struct CpuAssetPayload> payload; // decode task → render thread

    // ---- 观测 ----
    LoadTimestamps timestamps{};
    AssetLoadErrorCode error = AssetLoadErrorCode::None;
    std::string errorText;               // 可读原因（不含绝对路径）
    bool decodeExecutedOnWorker = false; // decode 是否在 compute worker 上执行（线程归属证据）
    // 阶段执行线程的进程内标识（std::thread::id 的哈希；同一进程内可比，跨进程不稳定）。
    // A17「线程归属 trace」的机读证据：读了文件的线程 ≠ 执行 decode 的线程 ≠ 上传线程。
    std::uint64_t ioThreadId = 0;     // 写者：I/O 线程
    std::uint64_t decodeThreadId = 0; // 写者：decode task

    // 唯一的状态转移入口：CAS + Debug 断言非法边。
    // 返回：成功（或不满足前置）见返回值；非法边在 Debug 断言，Release 下拒绝并返回 false。
    [[nodiscard]] bool TryTransition(const AssetLoadState expected, const AssetLoadState desired) noexcept
    {
        if (!IsLegalTransition(expected, desired))
        {
            ME_ASSERT(false, "illegal asset load state transition");
            return false;
        }
        AssetLoadState observed = expected;
        return state.compare_exchange_strong(observed, desired, std::memory_order_acq_rel);
    }

    // 请求取消（可重入；只有 waiter 归零时由 loader 调用）。
    void RequestCancel() noexcept
    {
        cancelGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool IsCancelRequested() const noexcept
    {
        return cancelGeneration.load(std::memory_order_acquire) != 0;
    }

    [[nodiscard]] AssetLoadState State() const noexcept
    {
        return state.load(std::memory_order_acquire);
    }

    [[nodiscard]] LoadPriority Priority() const noexcept
    {
        return static_cast<LoadPriority>(priority.load(std::memory_order_acquire));
    }

    // 提升优先级（合并语义：只升不降）。
    void PromotePriority(const LoadPriority candidate) noexcept
    {
        std::uint8_t current = priority.load(std::memory_order_acquire);
        while (static_cast<std::uint8_t>(candidate) > current)
        {
            if (priority.compare_exchange_weak(current, static_cast<std::uint8_t>(candidate),
                                               std::memory_order_acq_rel))
            {
                break;
            }
        }
    }
};

using AssetLoadRequestPtr = std::shared_ptr<AssetLoadRequest>;

// 失败分类的稳定名称（raw JSON / 测试 / 诊断共用）。
[[nodiscard]] const char* ToString(AssetLoadErrorCode code) noexcept;
} // namespace MiniEngine::Assets
