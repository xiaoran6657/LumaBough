// ============================================================================
// DualShaderPackage.h — M6-04 双后端 shader package 离线证据
// 里程碑：M6-04（Pipeline、Binding 与 Shader 契约）
// 职责：声明一次性构建 D3D11/D3D12 shader package 的工具入口。实现位于
//       同目录的 DualShaderPackage.cpp；它只服务离线工具，不进入运行时 RHI。
// 关联：docs/architecture/README.md
//       tools/shader_compiler/src/DxcCompiler.h（既有 DXC 入口）
// ============================================================================
#pragma once

#include <cstddef>
#include <filesystem>

namespace MiniEngine::ShaderCompiler
{
// 编译固定的 M4/M5 graphics pass shader，并在所有 variant 成功且 reflection
// 语义一致后发布 package manifest.json。失败时保留既有 manifest 不变。
//
// 返回：成功写入的 logical shader asset 数量。
// 失败：任一源文件、FXC/DXC 编译、真实 reflection、语义比较或原子发布失败时
//       抛出 std::runtime_error；调用方负责将异常转换为 CLI 失败码。
[[nodiscard]] std::size_t BuildDualShaderPackage(const std::filesystem::path& repositoryRoot,
                                                 const std::filesystem::path& outputDirectory);
} // namespace MiniEngine::ShaderCompiler
