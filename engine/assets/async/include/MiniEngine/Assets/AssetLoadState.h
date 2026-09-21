// ============================================================================
// AssetLoadState.h — 异步资产加载的状态机（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：定义请求生命周期的 10 个状态与唯一合法边。所有状态转移都必须经
//       IsLegalTransition 校验（Debug 断言 + 诊断），禁止各阶段自行实现"看起来对"
//       的路径——非法回退会让"旧 Ready 资源仍有效"的契约静默失效。
// 状态图：
//   Unloaded → Queued → Reading → Decoding → CpuReady
//            → UploadQueued → Uploading → Ready
//   任何非终态 → Failed / Cancelled / Stale（终态，不可再转移）
// 关联：docs/architecture/README.md「状态机」
//       engine/assets/async/src/AsyncAssetLoader.cpp（唯一的状态转移执行者）
//       docs/architecture/DECISIONS.md（线程归属）
// ============================================================================

#pragma once

#include <cstdint>

namespace MiniEngine::Assets
{
enum class AssetLoadState : std::uint8_t
{
    Unloaded,     // 已创建请求对象，尚未入队（瞬时态）
    Queued,       // 已准入 I/O 队列，等待 I/O 线程
    Reading,      // I/O 线程正在阻塞读（含读前校验与字节上限）
    Decoding,     // 已发布 decode task；payload 具备后等待 CpuReady 名额
    CpuReady,     // CPU payload 就绪，等待 render thread 取走上传
    UploadQueued, // 已进入上传队列（受上传队列上限约束）
    Uploading,    // render thread 正在上传（RHI 调用只在这里）
    Ready,        // 已发布：旧 revision 的 deferred retire 到此才有意义
    Failed,       // 失败终态；旧 Ready 资源不受影响
    Cancelled,    // 无剩余需求时的取消终态（waiter 归零才发起）
    Stale         // 被更高 revision 取代的终态（不得覆盖新版本）
};

// 终态：不变量是"终态不可再转移"（IsLegalTransition 对任何目标都返回 false）。
[[nodiscard]] constexpr bool IsTerminal(const AssetLoadState state) noexcept
{
    return state == AssetLoadState::Ready || state == AssetLoadState::Failed || state == AssetLoadState::Cancelled ||
           state == AssetLoadState::Stale;
}

// 非终态（流水线在途）。命名与 IsTerminal 互补，便于调用方写"任一在途请求"的谓词。
[[nodiscard]] constexpr bool IsActive(const AssetLoadState state) noexcept
{
    return !IsTerminal(state);
}

// 失败类终态（失败或取消）：测试的"失败路径旧资源必须保留"断言用它表达。
[[nodiscard]] constexpr bool IsFailureOrCancellation(const AssetLoadState state) noexcept
{
    return state == AssetLoadState::Failed || state == AssetLoadState::Cancelled || state == AssetLoadState::Stale;
}

// 唯一合法边表。终态一律不可离开；非终态只能前进到相邻阶段或任意失败类终态。
[[nodiscard]] constexpr bool IsLegalTransition(const AssetLoadState from, const AssetLoadState to) noexcept
{
    if (to == AssetLoadState::Failed || to == AssetLoadState::Cancelled || to == AssetLoadState::Stale)
    {
        return !IsTerminal(from);
    }

    switch (from)
    {
    case AssetLoadState::Unloaded:
        return to == AssetLoadState::Queued;
    case AssetLoadState::Queued:
        return to == AssetLoadState::Reading;
    case AssetLoadState::Reading:
        return to == AssetLoadState::Decoding;
    case AssetLoadState::Decoding:
        return to == AssetLoadState::CpuReady;
    case AssetLoadState::CpuReady:
        return to == AssetLoadState::UploadQueued;
    case AssetLoadState::UploadQueued:
        return to == AssetLoadState::Uploading;
    case AssetLoadState::Uploading:
        return to == AssetLoadState::Ready;
    default:
        return false;
    }
}

// 诊断/报告用名称（稳定字符串：raw JSON 与测试断言都依赖它）。
[[nodiscard]] constexpr const char* ToString(const AssetLoadState state) noexcept
{
    switch (state)
    {
    case AssetLoadState::Unloaded:
        return "unloaded";
    case AssetLoadState::Queued:
        return "queued";
    case AssetLoadState::Reading:
        return "reading";
    case AssetLoadState::Decoding:
        return "decoding";
    case AssetLoadState::CpuReady:
        return "cpu-ready";
    case AssetLoadState::UploadQueued:
        return "upload-queued";
    case AssetLoadState::Uploading:
        return "uploading";
    case AssetLoadState::Ready:
        return "ready";
    case AssetLoadState::Failed:
        return "failed";
    case AssetLoadState::Cancelled:
        return "cancelled";
    case AssetLoadState::Stale:
        return "stale";
    }
    return "unknown";
}
} // namespace MiniEngine::Assets
