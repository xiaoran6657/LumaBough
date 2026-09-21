#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace MiniEngine::Rhi
{
// 进程内引用，非持久化 ID、非所有权。owner 是注册表身份，不是 native 指针。
// owner/index/generation 三者同时匹配才可访问；注册表在 Release 同样验证。
template <class Tag> class RhiHandle final
{
  public:
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();
    constexpr RhiHandle() noexcept = default;
    // 供注册表和诊断重建；畸形编码统一归一为唯一 invalid 值。
    constexpr RhiHandle(std::uint32_t index, std::uint32_t generation, std::uint64_t owner) noexcept
    {
        if (index != kInvalidIndex && generation != 0 && owner != 0)
        {
            m_index = index;
            m_generation = generation;
            m_owner = owner;
        }
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_owner != 0;
    }
    constexpr explicit operator bool() const noexcept
    {
        return IsValid();
    }
    [[nodiscard]] constexpr std::uint32_t Index() const noexcept
    {
        return m_index;
    }
    [[nodiscard]] constexpr std::uint32_t Generation() const noexcept
    {
        return m_generation;
    }
    [[nodiscard]] constexpr std::uint64_t Owner() const noexcept
    {
        return m_owner;
    }
    auto operator<=>(const RhiHandle&) const = default;

  private:
    std::uint32_t m_index = kInvalidIndex;
    std::uint32_t m_generation = 0;
    std::uint64_t m_owner = 0;
};

using BufferHandle = RhiHandle<struct BufferTag>;
using TextureHandle = RhiHandle<struct TextureTag>;
using SamplerHandle = RhiHandle<struct SamplerTag>;
using ShaderHandle = RhiHandle<struct ShaderTag>;
using ResourceSetLayoutHandle = RhiHandle<struct ResourceSetLayoutTag>;
using ResourceSetHandle = RhiHandle<struct ResourceSetTag>;
using PipelineLayoutHandle = RhiHandle<struct PipelineLayoutTag>;
using GraphicsPipelineHandle = RhiHandle<struct GraphicsPipelineTag>;
using SwapChainHandle = RhiHandle<struct SwapChainTag>;
using TimestampQueryHandle = RhiHandle<struct TimestampQueryTag>;
} // namespace MiniEngine::Rhi

namespace std
{
template <class Tag> struct hash<MiniEngine::Rhi::RhiHandle<Tag>>
{
    size_t operator()(const MiniEngine::Rhi::RhiHandle<Tag>& value) const noexcept
    {
        // 逐字段组合，绝不读取 padding。跨运行稳定资源身份使用资产 ID，不能用此 hash。
        size_t result = hash<uint64_t>{}(value.Owner());
        const uint32_t parts[] = {value.Index(), value.Generation()};
        for (auto part : parts)
        {
            result ^= static_cast<size_t>(part) + static_cast<size_t>(0x9E3779B9U) + (result << 6U) + (result >> 2U);
        }
        return result;
    }
};
} // namespace std
