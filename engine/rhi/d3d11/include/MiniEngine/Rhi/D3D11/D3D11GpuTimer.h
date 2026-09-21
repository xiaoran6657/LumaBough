// ============================================================================
// D3D11GpuTimer.h — M4-08 GPU pass 计时（TIMESTAMP_DISJOINT + 8 帧 query ring）
// 里程碑：M4（08 篇「D3D11 GPU timing」；手抄清单第 1 条）
// 职责：每个 in-flight query frame 含 1 个 TIMESTAMP_DISJOINT + frameBegin/End +
//       4 个 pass（shadow / opaque PBR / skybox / tone map）的 Begin/End 时间戳，
//       共 11 个查询 × 8 帧 ring。读取规则（08 篇硬性要求）：
//         - 只轮询最老 slot，GetData(DONOTFLUSH)，S_FALSE 直接返回不 busy-wait；
//         - FAILED 使该 sample 无效并记录 HRESULT（可观察）；
//         - Disjoint == TRUE 丢弃整组 sample；
//         - 写者追上读者（8 帧全部 pending）时 BeginFrame 返回 false——该帧不计时，
//           绝不覆盖 pending 查询（08 篇：writer 追上 reader 应标 blocked）。
//       另含纯 CPU 的 nearest-rank 分位数与一致性校验（报告统计与负向测试共用）。
// 关联：docs/architecture/README.md「D3D11 GPU timing」「统计方法」
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（Begin/End 的帧内挂点）
//       tests/rendering/TimingTests.cpp（分位数与一致性校验的单元测试）
// ============================================================================
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace MiniEngine::Rhi::D3D11
{
// 计时的四个 pass，顺序固定（08 篇提交顺序）。
enum class GpuPass : std::uint32_t
{
    Shadow = 0,
    OpaquePbr = 1,
    Skybox = 2,
    ToneMap = 3,
    Count = 4
};

// 一次完成的 GPU 帧计时。valid=false 表示该 sample 无效（FAILED 或 disjoint），
// 调用方按 missing/disjoint 计数；passMs 在缺时间戳时保持 0。
struct GpuFrameTiming final
{
    double frameMs = 0.0;
    std::array<double, static_cast<std::size_t>(GpuPass::Count)> passMs{};
    bool valid = false;
    bool disjoint = false;
};

// GPU sample 计数（08 篇「统计方法」：valid/missing/disjoint 数量进报告）。
struct GpuSampleCounters final
{
    std::uint32_t valid{};
    std::uint32_t missing{};
    std::uint32_t disjoint{};
    std::uint32_t failed{};
    std::uint32_t lastHresult{};       // 最近一次 FAILED GetData 的 HRESULT
    std::uint32_t missingTimestamps{}; // 缺失的独立时间戳数（disjoint 就绪后仍不可读）
};

class D3D11GpuTimer final
{
  public:
    // 08 篇模板的 8 组是"至少"——vsync=0 的性能 run 中 CPU 远快于 GPU，GPU 帧延迟
    // 轻松超过 8 帧，8-slot ring 会在数据被读取前被覆盖（Debug Layer 警告
    // QUERY_END_ABANDONING_PREVIOUS_RESULTS ×3710 实证，M4-08）。64 组覆盖
    // ~1s 的 GPU 延迟；仍不足时 BeginFrame 返回 false 由调用方按 missing 计数。
    static constexpr std::size_t kQueryFrameCount = 64;

    // 预创建全部 11×8 个查询（TIMESTAMP_DISJOINT + 2×(1+4) TIMESTAMP）；失败抛出。
    void Create(ID3D11Device& device);
    void Release() noexcept;

    // 开始一帧计时。返回 false 表示 ring 全部 pending（写者追上读者）——该帧不计时，
    // 调用方跳过 Begin/EndPass 与 EndFrame，并按 missing 计数。
    [[nodiscard]] bool BeginFrame(ID3D11DeviceContext& context);
    void BeginPass(ID3D11DeviceContext& context, GpuPass pass);
    void EndPass(ID3D11DeviceContext& context, GpuPass pass);
    void EndFrame(ID3D11DeviceContext& context);

    // 轮询最老 slot：nullopt = 尚未就绪（S_FALSE，绝不 busy-wait / Flush）；
    // 有值 = slot 已回收（valid/disjoint/failed 语义见 GpuFrameTiming 与计数器）。
    [[nodiscard]] std::optional<GpuFrameTiming> PollOldest(ID3D11DeviceContext& context);

    // 可观察计数（08 篇：missing/HRESULT 必须可观察）。
    [[nodiscard]] std::uint32_t MissingTimestampCount() const noexcept
    {
        return m_missingTimestamps;
    }

    [[nodiscard]] std::uint32_t QueryFailureCount() const noexcept
    {
        return m_queryFailures;
    }

    [[nodiscard]] std::uint32_t LastQueryHresult() const noexcept
    {
        return m_lastHresult;
    }

  private:
    struct QueryFrame final
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> frameBegin;
        Microsoft::WRL::ComPtr<ID3D11Query> frameEnd;
        std::array<Microsoft::WRL::ComPtr<ID3D11Query>, static_cast<std::size_t>(GpuPass::Count)> passBegin;
        std::array<Microsoft::WRL::ComPtr<ID3D11Query>, static_cast<std::size_t>(GpuPass::Count)> passEnd;
        bool pending = false;
    };

    std::array<QueryFrame, kQueryFrameCount> m_frames{};
    std::size_t m_writeIndex = 0;
    std::size_t m_readIndex = 0;
    std::size_t m_pendingCount = 0;
    std::uint32_t m_missingTimestamps = 0;
    std::uint32_t m_queryFailures = 0;
    std::uint32_t m_lastHresult = 0;
};

// ---------------------------------------------------------------------------
// 纯 CPU 统计（nearest-rank 分位数 + 一致性校验；08 篇「统计方法」）
// ---------------------------------------------------------------------------

// median/p95/p99 摘要（nearest-rank：p = sorted[ceil(q*N)-1]）。
struct PerfQuantiles final
{
    double median{};
    double p95{};
    double p99{};
};

// 对样本排序副本计算 nearest-rank 分位数。空样本返回全 0（调用方在帧数不足时
// 走 BLOCKED 路径，不依赖此函数报错）。
[[nodiscard]] PerfQuantiles SummarizeNearestRank(std::span<const double> samples);

// 一致性校验（写出前必须通过，08 篇「报告 schema」）：全部 finite、非负、
// p95 >= median、p99 >= p95。NaN 会因比较为 false 被拒绝。
[[nodiscard]] bool PerfQuantilesAreConsistent(const PerfQuantiles& quantiles);
} // namespace MiniEngine::Rhi::D3D11
