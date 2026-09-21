#include <MiniEngine/Platform/Windows/ProcessMemoryWin32.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <psapi.h>

namespace MiniEngine::Platform::Windows
{
ProcessMemorySnapshot QueryProcessMemory() noexcept
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == FALSE)
        return ProcessMemorySnapshot{};
    ProcessMemorySnapshot snapshot{};
    snapshot.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    snapshot.privateBytes = static_cast<std::uint64_t>(counters.PagefileUsage);
    snapshot.peakWorkingSetBytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    snapshot.available = true;
    return snapshot;
}
} // namespace MiniEngine::Platform::Windows
