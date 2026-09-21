#pragma once

#include <cstdint>

namespace MiniEngine::Platform::Windows
{
// 进程内存快照（BACKLOG `M7-SOAK-RSS`）。用途：长跑稳定性证据里区分"引擎自报的池字节"
// 与"操作系统看到的进程占用"——前者是子系统口径，后者才能回答"10k 帧有没有长内存"。
//
// 口径（Windows PSAPI）：
//   workingSetBytes = PROCESS_MEMORY_COUNTERS::WorkingSetSize（物理内存中驻留的部分）；
//   privateBytes    = PROCESS_MEMORY_COUNTERS::PagefileUsage（私有提交字节，不含共享映射）。
// 两者都不是"峰值"：调用方按需自行取 peak（本项目在遥测里累计）。
struct ProcessMemorySnapshot final
{
    std::uint64_t workingSetBytes = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t peakWorkingSetBytes = 0;
    bool available = false; // 读取失败时为 false（调用方必须显式处理，不把 0 当"没涨"）
};

// 读取当前进程的内存快照。失败不抛异常：返回 available=false（长跑遥测不应该因为
// 一次读取失败而中断），调用方负责在证据里标注不可用。
[[nodiscard]] ProcessMemorySnapshot QueryProcessMemory() noexcept;
} // namespace MiniEngine::Platform::Windows
