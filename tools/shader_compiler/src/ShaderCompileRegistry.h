// ============================================================================
// ShaderCompileRegistry.h — D3D12 shader 的编译入口清单（工具与 gate 的唯一真源）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 1 条）
// 职责：登记 `shaders/d3d12/*.hlsl` 的每个编译入口（源文件 + 入口名 + target）。
//       工具 `shader_compiler.exe` 与编译 gate（ShaderCompileGateTests）消费同一份
//       清单——"运行时能编译的集合"与"被验证的集合"因此不可能分叉；新增 shader
//       入口时只改这里一处，两边同时生效。
// 为什么需要清单而不是扫描：#include 的 .hlsli 不是入口，且同一 .hlsl 可能有两个
//       入口（VS+PS）；只有显式登记才能保证 target/entry 的正确组合（M4 的 D3D11
//       gate 采用同一手法）。
// 关联：tools/shader_compiler/src/DxcCompiler.h（编译实现）
//       tools/shader_compiler/src/main.cpp（CLI）
// ============================================================================
#pragma once

#include <cstdint>
#include <span>

namespace MiniEngine::ShaderCompiler
{
// 一条入口：source 相对仓库根，entry/target 是 DXC 参数。
struct ShaderEntry final
{
    const char* source;  // 例如 "shaders/d3d12/PbrForward.hlsl"
    const char* entry;   // 例如 "VSMain"
    const char* target;  // 例如 "vs_6_0"
};

// 全部入口（顺序稳定；manifest 与 gate 都按此顺序处理）。
[[nodiscard]] std::span<const ShaderEntry> Entries() noexcept;
} // namespace MiniEngine::ShaderCompiler
