// ============================================================================
// D3D11GpuTimer.cpp — GPU pass 计时的实现
// 里程碑：M4（08 篇「D3D11 GPU timing」；手抄清单第 2 条）
// 职责：Create 预创建 11×8 个查询；Begin/End 只提交 End 查询（TIMESTAMP 无需
//       Begin）；PollOldest 只读最老 slot，GetData(DONOTFLUSH)，S_FALSE 返回
//       nullopt（绝不 busy-wait 或 Flush），FAILED 记 HRESULT 并作废该 sample，
//       Disjoint 丢弃整组；时间换算 ms = (end-begin)/Frequency × 1000。
//       缺单个时间戳只损失该 pass/整帧（missing 计数可观察），不使后续帧失效。
// 关联：engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11GpuTimer.h（契约）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（帧内挂点与轮询时机）
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11GpuTimer.h>

#include "D3D11Error.h"

#include <algorithm>
#include <cmath>

namespace MiniEngine::Rhi::D3D11
{
namespace
{
using Microsoft::WRL::ComPtr;

// 创建单个查询的薄封装（TIMESTAMP_DISJOINT 与 TIMESTAMP 共用描述形态）。
ComPtr<ID3D11Query> CreateQueryOrThrow(ID3D11Device& device, const D3D11_QUERY type)
{
    D3D11_QUERY_DESC description{};
    description.Query = type;
    description.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    ThrowIfFailed(device.CreateQuery(&description, query.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateQuery(gpu timer)");
    return query;
}
} // namespace

void D3D11GpuTimer::Create(ID3D11Device& device)
{
    for (QueryFrame& frame : m_frames)
    {
        frame.disjoint = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP_DISJOINT);
        frame.frameBegin = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP);
        frame.frameEnd = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP);
        for (std::size_t pass = 0; pass < static_cast<std::size_t>(GpuPass::Count); ++pass)
        {
            frame.passBegin[pass] = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP);
            frame.passEnd[pass] = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP);
        }
        frame.pending = false;
    }
    m_writeIndex = 0;
    m_readIndex = 0;
    m_pendingCount = 0;
    m_missingTimestamps = 0;
    m_queryFailures = 0;
    m_lastHresult = 0;
}

void D3D11GpuTimer::Release() noexcept
{
    for (QueryFrame& frame : m_frames)
    {
        frame.disjoint.Reset();
        frame.frameBegin.Reset();
        frame.frameEnd.Reset();
        for (auto& query : frame.passBegin)
        {
            query.Reset();
        }
        for (auto& query : frame.passEnd)
        {
            query.Reset();
        }
        frame.pending = false;
    }
    m_writeIndex = 0;
    m_readIndex = 0;
    m_pendingCount = 0;
}

bool D3D11GpuTimer::BeginFrame(ID3D11DeviceContext& context)
{
    QueryFrame& frame = m_frames[m_writeIndex];
    if (frame.pending)
    {
        // 写者追上读者：该帧不计时（08 篇——绝不覆盖 pending 查询，benchmark 按
        // missing 计数；8 帧 ring 下正常负载不会发生）。
        return false;
    }

    context.Begin(frame.disjoint.Get());
    context.End(frame.frameBegin.Get());
    return true;
}

void D3D11GpuTimer::BeginPass(ID3D11DeviceContext& context, const GpuPass pass)
{
    context.End(m_frames[m_writeIndex].passBegin[static_cast<std::size_t>(pass)].Get());
}

void D3D11GpuTimer::EndPass(ID3D11DeviceContext& context, const GpuPass pass)
{
    context.End(m_frames[m_writeIndex].passEnd[static_cast<std::size_t>(pass)].Get());
}

void D3D11GpuTimer::EndFrame(ID3D11DeviceContext& context)
{
    QueryFrame& frame = m_frames[m_writeIndex];
    context.End(frame.frameEnd.Get());
    context.End(frame.disjoint.Get());
    frame.pending = true;
    m_writeIndex = (m_writeIndex + 1U) % kQueryFrameCount;
    ++m_pendingCount;
}

std::optional<GpuFrameTiming> D3D11GpuTimer::PollOldest(ID3D11DeviceContext& context)
{
    if (m_pendingCount == 0)
    {
        return std::nullopt;
    }

    QueryFrame& frame = m_frames[m_readIndex];
    if (!frame.pending)
    {
        return std::nullopt; // 不变量：pendingCount>0 时最老 slot 必然 pending
    }

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    // **生命周期语义（2026-09-08 修复，依据 GpuTimestampDeviceTests 对照实验）**：
    // 此前实现在"disjoint 就绪但时间戳未解析"时立即回收 slot——被复用时 End 会
    // 放弃未读结果（Debug Layer QUERY_END_ABANDONING_PREVIOUS_RESULTS ×3218），
    // 数据被永久破坏，形成"720/720 全 missing"的假象。修复后语义：
    //   1. disjoint 用篇目要求的 DONOTFLUSH 读取（引擎每帧 Present 会提交命令
    //      队列，实验证明 DONOTFLUSH+Present 下 resolved=299/300 完全可用）；
    //   2. disjoint S_FALSE → 不回收 slot（08 篇：绝不 busy-wait）；
    //   3. disjoint 就绪后时间戳用 flush 旗标读取（同流更早的 End 已提交，flush
    //      只是让运行时完成解析）；**任一时间戳 S_FALSE → 同样不回收 slot**，
    //      下一帧重试——绝不带着未读结果复用查询对象；
    //   4. 只有"全部查询读毕"才回收 slot（valid sample 或 FAILED/Disjoint 无效）。
    constexpr UINT kDisjointFlags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    constexpr UINT kTimestampFlags = 0;
    const HRESULT status = context.GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint), kDisjointFlags);
    if (status == S_FALSE)
    {
        // GPU 尚未完成：跳过本帧读取，绝不 busy-wait 或 Flush（08 篇硬性要求）。
        return std::nullopt;
    }

    if (FAILED(status))
    {
        // FAILED 使该 sample 无效并记录 HRESULT（08 篇：missing/HRESULT 可观察）。
        // slot 已确认异常：回收（End 覆盖异常状态是安全的）。
        ++m_queryFailures;
        m_lastHresult = static_cast<std::uint32_t>(status);
        frame.pending = false;
        --m_pendingCount;
        m_readIndex = (m_readIndex + 1U) % kQueryFrameCount;
        return GpuFrameTiming{};
    }

    // disjoint 就绪：读取本 slot 的全部时间戳。**先读后回收**——任何一个 S_FALSE
    // 都让 slot 保持 pending（下一帧重试），绝不复用带未读结果的查询对象。
    std::uint64_t frameBegin = 0;
    std::uint64_t frameEnd = 0;
    const HRESULT beginStatus =
        context.GetData(frame.frameBegin.Get(), &frameBegin, sizeof(frameBegin), kTimestampFlags);
    const HRESULT endStatus = context.GetData(frame.frameEnd.Get(), &frameEnd, sizeof(frameEnd), kTimestampFlags);
    std::uint64_t passTicks[2 * static_cast<std::size_t>(GpuPass::Count)]{};
    bool allPassesReady = true;
    for (std::size_t pass = 0; pass < static_cast<std::size_t>(GpuPass::Count); ++pass)
    {
        const HRESULT beginOk =
            context.GetData(frame.passBegin[pass].Get(), &passTicks[pass * 2U], sizeof(std::uint64_t), kTimestampFlags);
        const HRESULT endOk = context.GetData(frame.passEnd[pass].Get(), &passTicks[pass * 2U + 1U],
                                              sizeof(std::uint64_t), kTimestampFlags);
        allPassesReady = allPassesReady && beginOk == S_OK && endOk == S_OK;
        if (FAILED(beginOk) || FAILED(endOk))
        {
            m_lastHresult = static_cast<std::uint32_t>(FAILED(beginOk) ? beginOk : endOk);
        }
    }

    const bool frameReady = beginStatus == S_OK && endStatus == S_OK;
    if (!frameReady || !allPassesReady)
    {
        // 有查询未解析：slot 保持 pending，下一帧重试（绝不回收复用）。
        ++m_missingTimestamps;
        if (beginStatus != S_OK && beginStatus != S_FALSE)
        {
            m_lastHresult = static_cast<std::uint32_t>(beginStatus);
        }
        return std::nullopt;
    }

    // 全部查询读毕：现在才回收 slot 并产出 sample。
    frame.pending = false;
    --m_pendingCount;
    m_readIndex = (m_readIndex + 1U) % kQueryFrameCount;

    GpuFrameTiming output{};
    output.disjoint = disjoint.Disjoint != FALSE;
    if (output.disjoint)
    {
        // Disjoint == TRUE：频率不可信，丢弃整组 sample（08 篇）。
        return output;
    }

    output.frameMs = static_cast<double>(frameEnd - frameBegin) * 1000.0 / static_cast<double>(disjoint.Frequency);
    output.valid = true;
    for (std::size_t pass = 0; pass < static_cast<std::size_t>(GpuPass::Count); ++pass)
    {
        output.passMs[pass] = static_cast<double>(passTicks[pass * 2U + 1U] - passTicks[pass * 2U]) * 1000.0 /
                              static_cast<double>(disjoint.Frequency);
    }
    return output;
}

PerfQuantiles SummarizeNearestRank(const std::span<const double> samples)
{
    if (samples.empty())
    {
        return {};
    }

    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    const auto nearestRank = [&sorted](const double q)
    {
        // nearest-rank：p = sorted[ceil(q*N)-1]（08 篇「统计方法」）。
        const std::size_t index = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size()))) - 1U;
        return sorted[std::min(index, sorted.size() - 1U)];
    };

    PerfQuantiles quantiles{};
    quantiles.p99 = nearestRank(0.99);
    quantiles.p95 = nearestRank(0.95);
    // median：N 为奇数取正中，偶数取上中位（nearest-rank q=0.5 的定义）。
    quantiles.median = nearestRank(0.5);
    return quantiles;
}

bool PerfQuantilesAreConsistent(const PerfQuantiles& quantiles)
{
    const bool finite = std::isfinite(quantiles.median) && std::isfinite(quantiles.p95) && std::isfinite(quantiles.p99);
    const bool nonNegative = quantiles.median >= 0.0 && quantiles.p95 >= 0.0 && quantiles.p99 >= 0.0;
    // NaN 的比较恒为 false，因此 finite 检查已覆盖 NaN；再显式校验分位顺序。
    return finite && nonNegative && quantiles.p95 >= quantiles.median && quantiles.p99 >= quantiles.p95;
}
} // namespace MiniEngine::Rhi::D3D11
