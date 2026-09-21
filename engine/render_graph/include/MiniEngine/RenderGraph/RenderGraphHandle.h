#pragma once
#include <compare>
#include <cstdint>
#include <limits>

namespace MiniEngine::RenderGraph
{
namespace Detail
{
struct GraphState;
}

// virtual handle 不拥有资源。owner 隔离不同 graph；generation 隔离 Reset；
// version 标识同一物理资源被下一次写覆盖前的内容。外部不能通过 index 签发 handle。
template <class Tag> class RgHandle final
{
  public:
    constexpr RgHandle() = default;
    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return m_owner != 0;
    }
    [[nodiscard]] constexpr std::uint32_t Resource() const noexcept
    {
        return m_resource;
    }
    [[nodiscard]] constexpr std::uint32_t Version() const noexcept
    {
        return m_version;
    }
    [[nodiscard]] constexpr std::uint64_t Owner() const noexcept
    {
        return m_owner;
    }
    [[nodiscard]] constexpr std::uint64_t Generation() const noexcept
    {
        return m_generation;
    }
    auto operator<=>(const RgHandle&) const = default;

  private:
    friend struct Detail::GraphState;
    constexpr RgHandle(std::uint64_t owner, std::uint64_t generation, std::uint32_t resource, std::uint32_t version)
        : m_owner(owner), m_generation(generation), m_resource(resource), m_version(version)
    {
    }
    std::uint64_t m_owner = 0;
    std::uint64_t m_generation = 0;
    std::uint32_t m_resource = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t m_version = 0;
};
using RgTexture = RgHandle<struct RgTextureTag>;
using RgBuffer = RgHandle<struct RgBufferTag>;
} // namespace MiniEngine::RenderGraph
