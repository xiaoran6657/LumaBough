#pragma once

#include <MiniEngine/Core/Time.h>

#include <cstdint>

namespace MiniEngine
{
class WindowsClock
{
  public:
    WindowsClock();

    [[nodiscard]] Duration Now() const;

  private:
    std::int64_t m_frequency = 0;
    std::int64_t m_origin = 0;
};
} // namespace MiniEngine