// ============================================================================
// Sha256.h — 引擎侧可移植的流式 SHA-256
// 里程碑：M3（7-A）
// 职责：提供 FIPS 180-4 的流式 SHA-256 与一次性便捷函数，供 AssetId 派生、
//       Manifest digest 与 artifact 完整性校验使用。纯 C++ 实现、不依赖平台 API，
//       与 Cooker 侧的 CNG 实现（tools/asset_cooker/src/Sha256.cpp）有意并存，
//       两侧以官方测试向量互证，勿合并（ADR-0004「影响」）。
// 关联：docs/architecture/DECISIONS.md「影响」
//       engine/assets/src/AssetRegistry.cpp（AssetId 域分隔派生的实现）
// ============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace MiniEngine::Assets
{
// SHA-256 摘要：32 字节。与 BuildKey / ContentHash 的 bytes 同宽度但语义不同
// （前者是"身份派生 / 完整性"结果，后两者是"构建身份 / 内容哈希"的载体）。
using Sha256Digest = std::array<std::byte, 32>;

// 流式 SHA-256（FIPS 180-4）的可移植实现，供引擎侧使用：
// AssetId 域分隔派生、Manifest/artifact 完整性校验。
// Cooker 侧另有 CNG 实现（tools/asset_cooker），两者算法一致；
// Assets 不依赖平台 API，因此这里用纯 C++ 实现，由官方向量测试锁定。
class Sha256Builder final
{
  public:
    // 以标准规定的 8 个初始状态字初始化，内部缓冲与计长清零。
    Sha256Builder() noexcept;

    // 禁止拷贝：Builder 持有正在进行的压缩状态与内部缓冲，拷贝语义无意义且易误用。
    Sha256Builder(const Sha256Builder&) = delete;
    Sha256Builder& operator=(const Sha256Builder&) = delete;

    // 追加一段字节到流式哈希。
    //
    // 可按任意大小切分调用：尾部不足一个分组的部分留在内部缓冲，与下一次 Append
    // 拼接；凑满整组立即压缩，因此内存占用与消息总长度无关。
    //
    // 参数：
    //   bytes —— 待追加的字节；空 span 合法
    //
    // Finish 之后继续 Append 返回 false。
    [[nodiscard]] bool Append(std::span<const std::byte> bytes) noexcept;

    // 收尾并输出摘要。
    //
    // 按 FIPS 180-4 追加填充（0x80 → 零填充到 56 字节边界 → 追加 64 位大端比特
    // 长度），压缩最后一个分组后写出 32 字节摘要。调用后进入终态，不可续用。
    //
    // 参数：
    //   out —— 输出的 32 字节摘要
    // 返回：成功为 true；已 Finish 过再调用为 false。
    [[nodiscard]] bool Finish(Sha256Digest& out) noexcept;

  private:
    // 8 个 32 位工作变量（FIPS 180-4 的 a..h），随每个 512 位分组滚动更新。
    std::array<std::uint32_t, 8> m_state;
    // 已追加消息的总比特长度；Finish 时作为 64 位大端长度字段参与填充。
    std::uint64_t m_bitLength;
    // 尾部缓冲（64 字节，即一个分组），凑满才压缩。
    std::array<std::byte, 64> m_buffer;
    // 尾部缓冲中的有效字节数（0..63）。
    std::size_t m_bufferBytes;
    // 是否已 Finish：终态后 Append / Finish 一律返回 false，防止摘要被续写。
    bool m_finished;
};

// 一次性计算一段字节的 SHA-256（内部构造 Builder 并 Append + Finish）。
//
// 适合 Manifest digest、artifact 哈希这类"整块内存一次算完"的场景；
// 需要边读边算的流式场景请直接使用 Sha256Builder。
//
// 参数：
//   bytes —— 待计算的字节
// 返回：32 字节摘要。
[[nodiscard]] Sha256Digest Sha256(std::span<const std::byte> bytes) noexcept;

// 小写十六进制（64 字符）；与 Cooker 的 ToHexDigest 输出一致。
[[nodiscard]] std::string ToHexDigest(const Sha256Digest& digest);
} // namespace MiniEngine::Assets
