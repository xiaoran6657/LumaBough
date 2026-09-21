// ============================================================================
// D3D11ShaderCompiler.h — 运行时 HLSL 着色器编译入口
// 里程碑：M2
// 职责：封装 D3DCompileFromFile，把 HLSL 源码编译为 D3D11 原生 DXBC 字节码，
//   按构建配置选择优化/调试编译标志。M2 使用 SM5（vs_5_0 / ps_5_0），不引入 DXC。
// 关联：docs/architecture/README.md
// ============================================================================
#pragma once

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <filesystem>
#include <string_view>

namespace MiniEngine
{
// 从文件编译 HLSL 着色器，返回可交给 CreateVertexShader/CreatePixelShader 的字节码。
//
// Debug 构建启用 DEBUG 与 SKIP_OPTIMIZATION，Release 启用 OPTIMIZATION_LEVEL3；
// 两种配置均带 ENABLE_STRICTNESS 与 WARNINGS_ARE_ERRORS。编译诊断输出会写入日志
// （失败为 Error 级，成功但有警告为 Warning 级）。
//
// 参数：
//   file       —— HLSL 源文件路径
//   entryPoint —— 入口函数名（如 "VSMain"）
//   target     —— 着色器目标（如 "vs_5_0" / "ps_5_0"）
// 返回：编译成功的 ID3DBlob 字节码。
// 失败：文件缺失或编译失败时抛 std::runtime_error（含编译器诊断文本）。
[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file,
                                                             std::string_view entryPoint, std::string_view target);
} // namespace MiniEngine
