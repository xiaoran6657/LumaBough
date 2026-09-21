// ============================================================================
// UploadBudget.h — 每帧上传预算（M7-07）
// 里程碑：M7-07（上传预算、热重载与背压）
// 职责：把 render thread 的 GPU 上传限制在每帧 bytes / CPU time / 请求数三约束内，
//       并给"热重载"单独留份额，防止它无限抢占 gameplay streaming。
// 关联：docs/architecture/README.md「每帧预算」「优先级与公平」
//       AssetUploadCoordinator（唯一消费者；worker/IO 线程不得读取或推进预算）
// ============================================================================

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace MiniEngine::Assets
{
// 每帧预算。默认值来自 M5/M6 的上传测量（上传环 high-water ≈6.66 MiB / 600 帧、stall=0）
// 与 recipe 的 uploadMiBPerFrame=4；M7-07 的 sweep 再按 latency/hitch 折中复核。
struct UploadBudget final
{
    std::size_t maxBytes = 4u * 1024u * 1024u;
    std::chrono::microseconds maxCpuTime{500};
    std::uint32_t maxRequests = 16;
    // 热重载份额：已发布过的资产（revision N+1）每帧最多占 maxBytes 的百分比。
    // 100 = 不区分；0 = 热重载只能使用"首个请求例外"通道（不建议）。
    std::uint32_t reloadBytesPercent = 50;
};

// 一帧内的实际消耗（成功分支才累计；失败/取消不计入 usage）。
struct UploadUsage final
{
    std::size_t bytes = 0;
    std::chrono::microseconds cpuTime{0};
    std::uint32_t requests = 0;
    std::size_t streamBytes = 0; // 首次发布的资产
    std::size_t reloadBytes = 0; // 覆盖已有发布版本的资产（热重载）
    // 单请求超过 maxBytes 的净超出（首个请求例外：大资产不会被小预算永久饿死）。
    std::size_t estimatedOverrunBytes = 0;

    // 三约束同时生效：请求数、CPU 时间、字节。
    // 字节规则：`bytes == 0` 时允许首个请求超预算（防止大资产饿死）；
    // 之后必须严格落在剩余预算内。
    [[nodiscard]] bool CanStart(const std::size_t estimatedBytes, const UploadBudget& budget) const noexcept
    {
        return requests < budget.maxRequests && cpuTime < budget.maxCpuTime &&
               (bytes == 0 || bytes + estimatedBytes <= budget.maxBytes);
    }

    // 含份额的判定：reload 类请求额外受 reloadBytesPercent 约束（同样保留首个请求例外）。
    [[nodiscard]] bool CanStartClass(const std::size_t estimatedBytes, const bool reload,
                                     const UploadBudget& budget) const noexcept
    {
        if (!CanStart(estimatedBytes, budget))
        {
            return false;
        }
        if (!reload)
        {
            return true;
        }
        const std::size_t reloadCap = budget.maxBytes * budget.reloadBytesPercent / 100u;
        return reloadBytes == 0 || reloadBytes + estimatedBytes <= reloadCap;
    }

    [[nodiscard]] std::size_t BytesOfClass(const bool reload) const noexcept
    {
        return reload ? reloadBytes : streamBytes;
    }
};
} // namespace MiniEngine::Assets
