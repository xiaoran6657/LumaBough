// ============================================================================
// Sha256.cpp — FIPS 180-4 SHA-256 的纯 C++ 实现
// 里程碑：M3（7-A）
// 职责：实现 Sha256Builder 的消息调度与压缩函数、Finish 填充，以及一次性便捷函数。
//       与 Cooker 的 CNG 实现并存是刻意设计（Assets 不依赖平台 API），
//       正确性由官方 SHA-256 测试向量（tests/assets/Sha256Tests.cpp）锁定。
// 关联：docs/architecture/DECISIONS.md「影响」
//       engine/assets/include/MiniEngine/Assets/Sha256.h
// ============================================================================

#include <MiniEngine/Assets/Sha256.h>

#include <algorithm>
#include <cstring>

namespace MiniEngine::Assets
{
namespace
{
// 64 个轮常量 K[0..63]：前 64 个素数的立方根小数部分的前 32 位（FIPS 180-4 §4.2.2）。
// 表值逐字取自标准，不得重排或重算，否则与任何合规实现（含 Cooker 的 CNG）不互通。
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

// 8 个初始哈希值 H(0)：前 8 个素数的平方根小数部分的前 32 位（FIPS 180-4 §5.3.3）。
constexpr std::array<std::uint32_t, 8> kInitialStates{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

// 32 位循环右移；count 必须落在 (0, 32)，标准里所有移位量都是常数，因此无需保护。
std::uint32_t RotateRight(const std::uint32_t value, const unsigned count) noexcept
{
    return (value >> count) | (value << (32U - count));
}

// 压缩一个 512 位分组：先由分组展开出 64 字的消息调度表 w，再跑 64 轮压缩。
// 这是无分支的纯算术过程，输入完全来自 block 与 state，输出直接累加回 state。
void ProcessBlock(const std::array<std::byte, 64>& block, std::array<std::uint32_t, 8>& state) noexcept
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        w[i] = (std::to_integer<std::uint32_t>(block[i * 4]) << 24) |
               (std::to_integer<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (std::to_integer<std::uint32_t>(block[i * 4 + 2]) << 8) |
               std::to_integer<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = RotateRight(w[i - 15], 7U) ^ RotateRight(w[i - 15], 18U) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 = RotateRight(w[i - 2], 17U) ^ RotateRight(w[i - 2], 19U) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
} // namespace

Sha256Builder::Sha256Builder() noexcept
    : m_state{kInitialStates}, m_bitLength{}, m_buffer{}, m_bufferBytes{}, m_finished{}
{
}

bool Sha256Builder::Append(const std::span<const std::byte> bytes) noexcept
{
    if (m_finished)
    {
        return false;
    }

    m_bitLength += static_cast<std::uint64_t>(bytes.size()) * 8U;

    const std::byte* cursor = bytes.data();
    std::size_t remaining = bytes.size();

    if (m_bufferBytes > 0)
    {
        const std::size_t take = std::min(remaining, m_buffer.size() - m_bufferBytes);
        std::memcpy(m_buffer.data() + m_bufferBytes, cursor, take);
        m_bufferBytes += take;
        cursor += take;
        remaining -= take;
        if (m_bufferBytes == m_buffer.size())
        {
            ProcessBlock(m_buffer, m_state);
            m_bufferBytes = 0;
        }
    }

    while (remaining >= m_buffer.size())
    {
        std::array<std::byte, 64> block{};
        std::memcpy(block.data(), cursor, block.size());
        ProcessBlock(block, m_state);
        cursor += block.size();
        remaining -= block.size();
    }

    if (remaining > 0)
    {
        std::memcpy(m_buffer.data() + m_bufferBytes, cursor, remaining);
        m_bufferBytes += remaining;
    }

    return true;
}

bool Sha256Builder::Finish(Sha256Digest& out) noexcept
{
    if (m_finished)
    {
        return false;
    }

    const std::uint64_t bitLength = m_bitLength;

    // FIPS 180-4 §5.1.1：追加 0x80 → 零填充到 56 字节边界 → 追加 64 位大端比特长度。
    //
    // 关键边界（P1，2026-09-10 审查修复）：缓冲内已有 56..63 字节消息时，0x80 与
    // 8 字节长度放不进同一个分组——必须先把当前分组补满并压缩，再在全新分组里
    // 写填充与长度。旧实现在该区间直接把长度写进 m_buffer[56..63]，覆盖了刚写入
    // 的 0x80（甚至消息字节），且缺少第二次 ProcessBlock，导致 mod 64 ∈ [56,63]
    // 的输入摘要完全错误（外部工具对照发现的 M5-01 P1 缺陷即此）。
    constexpr std::byte kPaddingByte{0x80U};
    m_buffer[m_bufferBytes++] = kPaddingByte;
    if (m_bufferBytes == m_buffer.size())
    {
        // 0x80 恰好落在本组最后一字节：压缩本组，长度进入下一组。
        ProcessBlock(m_buffer, m_state);
        m_bufferBytes = 0;
    }
    if (m_bufferBytes > 56)
    {
        // 长度字段在本组放不下：余下空间补零并压缩，长度进全新分组。
        while (m_bufferBytes < m_buffer.size())
        {
            m_buffer[m_bufferBytes++] = std::byte{0U};
        }
        ProcessBlock(m_buffer, m_state);
        m_bufferBytes = 0;
    }
    while (m_bufferBytes < 56)
    {
        m_buffer[m_bufferBytes++] = std::byte{0U};
    }
    for (std::size_t i = 0; i < 8; ++i)
    {
        const auto shift = static_cast<unsigned>(56U - i * 8U);
        m_buffer[56 + i] = static_cast<std::byte>((bitLength >> shift) & 0xFFU);
    }
    ProcessBlock(m_buffer, m_state);

    for (std::size_t i = 0; i < m_state.size(); ++i)
    {
        out[i * 4] = static_cast<std::byte>((m_state[i] >> 24) & 0xFFU);
        out[i * 4 + 1] = static_cast<std::byte>((m_state[i] >> 16) & 0xFFU);
        out[i * 4 + 2] = static_cast<std::byte>((m_state[i] >> 8) & 0xFFU);
        out[i * 4 + 3] = static_cast<std::byte>(m_state[i] & 0xFFU);
    }

    m_finished = true;
    return true;
}

Sha256Digest Sha256(const std::span<const std::byte> bytes) noexcept
{
    Sha256Builder builder;
    Sha256Digest digest{};
    static_cast<void>(builder.Append(bytes));
    static_cast<void>(builder.Finish(digest));
    return digest;
}

std::string ToHexDigest(const Sha256Digest& digest)
{
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        const auto value = std::to_integer<std::uint8_t>(digest[i]);
        result[i * 2] = kHexDigits[value >> 4];
        result[i * 2 + 1] = kHexDigits[value & 0x0FU];
    }
    return result;
}
} // namespace MiniEngine::Assets
