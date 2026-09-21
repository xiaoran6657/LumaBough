#pragma once
#include "RhiSelection.h"

namespace MiniEngine::Sandbox
{
// M7 性能场景（m7-cpu-scale / m7-layout / m7-streaming）的唯一入口。
//
// 职责边界：本运行器只做串行基线（workers=1、无后台 task、无 chunk 合并），
// 把每帧 raw 采样写进 BenchmarkRun JSON；并行化与真实异步流水线分别属于
// M7-05（RenderPacket chunks）与 M7-06（I/O + decode + upload）。
int RunM7Scene(const RhiLaunchOptions& options);
} // namespace MiniEngine::Sandbox
