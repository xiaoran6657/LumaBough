// ============================================================================
// Sha256.cpp — Cooker 侧 CNG（BCrypt）流式 SHA-256 实现
// 里程碑：M3-05
// 职责：以 BCryptOpenAlgorithmProvider / BCryptCreateHash / BCryptHashData /
//       BCryptFinishHash 实现流式哈希，RAII 管理 hash 对象与 provider 句柄。
//       与引擎侧纯 C++ 实现并存是 ADR-0004 的决策，输出由官方向量互证。
// 关联：Microsoft Learn：BCryptCreateHash（CNG 流式哈希契约）
//       engine/assets/src/Sha256.cpp（引擎侧可移植实现，勿合并）
// ============================================================================

#include "Sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <limits>
#include <memory>
#include <vector>

namespace MiniEngine::Tools
{
namespace
{
class AlgorithmProvider final
{
  public:
    ~AlgorithmProvider()
    {
        if (m_handle != nullptr)
        {
            BCryptCloseAlgorithmProvider(m_handle, 0);
        }
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE* Address() noexcept
    {
        return &m_handle;
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept
    {
        return m_handle;
    }

  private:
    BCRYPT_ALG_HANDLE m_handle{};
};

class HashHandle final
{
  public:
    ~HashHandle()
    {
        if (m_handle != nullptr)
        {
            BCryptDestroyHash(m_handle);
        }
    }

    [[nodiscard]] BCRYPT_HASH_HANDLE* Address() noexcept
    {
        return &m_handle;
    }

    [[nodiscard]] BCRYPT_HASH_HANDLE Get() const noexcept
    {
        return m_handle;
    }

  private:
    BCRYPT_HASH_HANDLE m_handle{};
};

bool GetDwordProperty(
    const BCRYPT_HANDLE handle,
    const wchar_t* property,
    unsigned long& value)
{
    unsigned long written{};
    return BCRYPT_SUCCESS(BCryptGetProperty(
        handle,
        property,
        reinterpret_cast<unsigned char*>(&value),
        sizeof(value),
        &written,
        0)) &&
           written == sizeof(value);
}
} // namespace

class Sha256Builder::Impl final
{
  public:
    Impl()
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                m_algorithm.Address(),
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0)))
        {
            return;
        }

        unsigned long objectSize{};
        unsigned long hashSize{};
        if (!GetDwordProperty(m_algorithm.Get(), BCRYPT_OBJECT_LENGTH, objectSize) ||
            !GetDwordProperty(m_algorithm.Get(), BCRYPT_HASH_LENGTH, hashSize) ||
            hashSize != sizeof(Sha256Digest))
        {
            return;
        }

        // SHA256 的 BCRYPT_OBJECT_LENGTH 通常非零；若为 0，空对象缓冲区会令
        // BCryptCreateHash 失败——与 ComputeSha256 的已知限制一致。
        m_object.resize(objectSize);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                m_algorithm.Get(),
                m_hash.Address(),
                m_object.data(),
                static_cast<unsigned long>(m_object.size()),
                nullptr,
                0,
                0)))
        {
            return;
        }

        m_ready = true;
    }

    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_ready;
    }

    [[nodiscard]] bool AppendBytes(const std::span<const std::byte> bytes) noexcept
    {
        if (!m_ready || m_finished)
        {
            return false;
        }

        if (bytes.empty())
        {
            return true;
        }

        if (bytes.size() > (std::numeric_limits<unsigned long>::max)())
        {
            return false;
        }

        return BCRYPT_SUCCESS(BCryptHashData(
            m_hash.Get(),
            reinterpret_cast<unsigned char*>(const_cast<std::byte*>(bytes.data())),
            static_cast<unsigned long>(bytes.size()),
            0));
    }

    [[nodiscard]] bool Finish(Sha256Digest& digest) noexcept
    {
        if (!m_ready || m_finished)
        {
            return false;
        }

        if (!BCRYPT_SUCCESS(BCryptFinishHash(
                m_hash.Get(),
                reinterpret_cast<unsigned char*>(digest.data()),
                static_cast<unsigned long>(digest.size()),
                0)))
        {
            return false;
        }

        m_finished = true;
        return true;
    }

  private:
    AlgorithmProvider m_algorithm;
    HashHandle m_hash;
    std::vector<unsigned char> m_object;
    bool m_ready{};
    bool m_finished{};
};

Sha256Builder::Sha256Builder()
    : m_impl{std::make_unique<Impl>()}
{
}

Sha256Builder::~Sha256Builder() = default;

Sha256Builder::Sha256Builder(Sha256Builder&&) noexcept = default;

Sha256Builder& Sha256Builder::operator=(Sha256Builder&&) noexcept = default;

bool Sha256Builder::IsReady() const noexcept
{
    return m_impl != nullptr && m_impl->IsReady();
}

bool Sha256Builder::AppendBytes(const std::span<const std::byte> bytes) noexcept
{
    return m_impl != nullptr && m_impl->AppendBytes(bytes);
}

bool Sha256Builder::AppendU32LE(const std::uint32_t value) noexcept
{
    const std::array<std::byte, 4> littleEndian{
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
    };
    return AppendBytes(littleEndian);
}

bool Sha256Builder::AppendU64LE(const std::uint64_t value) noexcept
{
    const std::array<std::byte, 8> littleEndian{
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
        static_cast<std::byte>((value >> 32U) & 0xFFU),
        static_cast<std::byte>((value >> 40U) & 0xFFU),
        static_cast<std::byte>((value >> 48U) & 0xFFU),
        static_cast<std::byte>((value >> 56U) & 0xFFU),
    };
    return AppendBytes(littleEndian);
}

bool Sha256Builder::AppendUtf8WithLength(const std::string_view text) noexcept
{
    if (text.size() > (std::numeric_limits<std::uint32_t>::max)())
    {
        return false;
    }

    if (!AppendU32LE(static_cast<std::uint32_t>(text.size())))
    {
        return false;
    }

    return AppendBytes(std::as_bytes(std::span{text.data(), text.size()}));
}

bool Sha256Builder::Finish(Sha256Digest& digest) noexcept
{
    return m_impl != nullptr && m_impl->Finish(digest);
}

bool ComputeSha256(const std::span<const std::byte> bytes, Sha256Digest& digest)
{
    if (bytes.size() > (std::numeric_limits<unsigned long>::max)())
    {
        return false;
    }

    AlgorithmProvider algorithm;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            algorithm.Address(),
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0)))
    {
        return false;
    }

    unsigned long objectSize{};
    unsigned long hashSize{};
    if (!GetDwordProperty(algorithm.Get(), BCRYPT_OBJECT_LENGTH, objectSize) ||
        !GetDwordProperty(algorithm.Get(), BCRYPT_HASH_LENGTH, hashSize) ||
        hashSize != digest.size())
    {
        return false;
    }

    // SHA256 的 BCRYPT_OBJECT_LENGTH 通常非零；若目标算法返回 0，
    // 空对象缓冲区会令 BCryptCreateHash 失败——作为已知限制记录。
    std::vector<unsigned char> object(objectSize);
    HashHandle hash;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm.Get(),
            hash.Address(),
            object.data(),
            static_cast<unsigned long>(object.size()),
            nullptr,
            0,
            0)))
    {
        return false;
    }

    if (!bytes.empty() &&
        !BCRYPT_SUCCESS(BCryptHashData(
            hash.Get(),
            reinterpret_cast<unsigned char*>(const_cast<std::byte*>(bytes.data())),
            static_cast<unsigned long>(bytes.size()),
            0)))
    {
        return false;
    }

    return BCRYPT_SUCCESS(BCryptFinishHash(
        hash.Get(),
        reinterpret_cast<unsigned char*>(digest.data()),
        static_cast<unsigned long>(digest.size()),
        0));
}
} // namespace MiniEngine::Tools
