// ============================================================================
// Sha256.h — Cooker 侧流式 SHA-256（CNG/BCrypt）
// 里程碑：M3-05（7-A 起与引擎侧互证）
// 职责：提供 BuildKey preimage 与文件 ContentHash 共用的流式哈希。与引擎侧的
//       纯 C++ 实现（engine/assets/src/Sha256.cpp）是有意双重实现：Cooker 本就
//       驻留 Windows 平台层，Assets 则不得依赖平台 API（ADR-0004「影响」，勿合并）；
//       两侧输出以官方 SHA-256 测试向量互证。
// 关联：docs/architecture/DECISIONS.md「影响」
//       tools/asset_cooker/src/BuildKey.cpp（preimage 编码的调用方）
// ============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace MiniEngine::Tools
{
using Sha256Digest = std::array<std::byte, 32>;

// Convenience for already-bounded in-memory payloads. File hashing and
// BuildKey construction still need Sha256Builder (stateful, streamed).
[[nodiscard]] bool ComputeSha256(std::span<const std::byte> bytes, Sha256Digest& digest);

// 流式 SHA-256（CNG/BCrypt）：
//   - 通过 AppendBytes/AppendU32LE/AppendU64LE/AppendUtf8WithLength 送入数据，
//     全部数据必须经过同一个 hash handle（禁止每块独立 hash 再拼接）。
//   - Finish() 输出 32 字节 digest，之后不可继续 append。
//   - RAII：析构时销毁 hash handle 并关闭 algorithm provider。
//   - 构造失败或任何 BCrypt NTSTATUS 失败时，对应调用返回 false。
class Sha256Builder final
{
  public:
    // 打开 BCrypt 算法 provider 并创建 hash 对象；失败不抛异常，用 IsReady 表达。
    Sha256Builder();
    // RAII：销毁 hash 对象并关闭 algorithm provider。
    ~Sha256Builder();

    // 禁止拷贝：hash handle 是独占的内核资源。
    Sha256Builder(const Sha256Builder&) = delete;
    Sha256Builder& operator=(const Sha256Builder&) = delete;
    // 移动即转移 handle 所有权，被移出方进入 IsReady() == false 的空状态。
    Sha256Builder(Sha256Builder&&) noexcept;
    Sha256Builder& operator=(Sha256Builder&&) noexcept;

    // 构造（或最近一次移动赋值）是否成功拿到可用的 hash handle。
    [[nodiscard]] bool IsReady() const noexcept;
    // 追加原始字节；失败（句柄不可用或 BCrypt 报错）返回 false，后续调用同样失败。
    [[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes) noexcept;
    // 以小端 4 字节 / 8 字节追加一个整数，供 versioned binary preimage 使用。
    [[nodiscard]] bool AppendU32LE(std::uint32_t value) noexcept;
    [[nodiscard]] bool AppendU64LE(std::uint64_t value) noexcept;
    // 追加 u32 LE 长度前缀 + UTF-8 字节：变长字符串的消歧编码（避免 "ab"+"c" 歧义）。
    [[nodiscard]] bool AppendUtf8WithLength(std::string_view text) noexcept;
    // 取摘要并进入终态；此后任何 Append 均返回 false。
    [[nodiscard]] bool Finish(Sha256Digest& digest) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Tools
