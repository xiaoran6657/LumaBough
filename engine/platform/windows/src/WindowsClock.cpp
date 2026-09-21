#include <MiniEngine/Platform/Windows/WindowsClock.h>

#include <Windows.h>

#include <chrono>
#include <system_error>

namespace MiniEngine
{
namespace
{
std::system_error MakeLastError(const char* operation)
{
    return std::system_error{static_cast<int>(GetLastError()), std::system_category(), operation};
}
} // namespace

WindowsClock::WindowsClock()
{
    LARGE_INTEGER frequency{};

    if (QueryPerformanceFrequency(&frequency) == FALSE)
    {
        throw MakeLastError("QueryPerformanceFrequency");
    }

    LARGE_INTEGER origin{};

    if (QueryPerformanceCounter(&origin) == FALSE)
    {
        throw MakeLastError("QueryPerformanceCounter");
    }

    m_frequency = frequency.QuadPart;
    m_origin = origin.QuadPart;
}

Duration WindowsClock::Now() const
{
    LARGE_INTEGER counter{};

    if (QueryPerformanceCounter(&counter) == FALSE)
    {
        throw MakeLastError("QueryPerformanceCounter");
    }

    const auto ticks = counter.QuadPart - m_origin;
    const auto seconds =
        std::chrono::duration<long double>{static_cast<long double>(ticks) / static_cast<long double>(m_frequency)};

    return std::chrono::duration_cast<Duration>(seconds);
}
} // namespace MiniEngine