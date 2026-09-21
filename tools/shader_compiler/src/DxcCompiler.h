// ============================================================================
// DxcCompiler.h — DXC(SM6) 编译与 manifest 生成（08 篇 DXC contract）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 1 条）
// 职责：按 08 篇冻结的口径编译单个入口并产出三样东西：
//   1) `.dxil` 对象（可被 PSO 直接消费，也可被 PIX 解析）；
//   2) full PDB（由当前 SDK 的 DXC_OUT_PDB 直接产出，并用 IDxcPdbUtils2::IsFullPDB
//      校验；编译器 suggested name 保持不变，含 HLSL 源信息供 PIX 自动定位）；
//   3) JSON manifest（source/entry/target/compilerVersion/arguments/sourceSha256/
//      dxilSha256/pdbName/pdbType）——"可复现"的证据就是这份 manifest 能被逐字段对照。
// 参数口径（08 篇）：`-T <target> -E <entry> -HV 2021 -Ges -WX -Zi -Zss`；
//   warnings-as-errors 是硬要求（`-WX`），且不提供关闭开关——shader 警告不允许
//   在 M5 baseline 里存在。
// 依赖：Windows SDK 自带的 dxcompiler（`dxcompiler.lib` + 运行时 `dxcompiler.dll`）。
//   本工具不引入第三方 fetch；若日后需要 pinned 版本，只需替换链接目标，接口不变。
// 关联：tools/shader_compiler/src/main.cpp（CLI）
//       docs/architecture/README.md（DXC contract / manifest 字段）
// ============================================================================
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace MiniEngine::ShaderCompiler
{
// 一次编译的结果（manifest 的机读形态）。
struct CompileResult final
{
    std::string sourcePath;             // manifest 里的 source（仓库相对路径，正斜杠）
    std::string entryPoint;             // 入口名
    std::string targetProfile;          // vs_6_0 / ps_6_0
    std::string compilerVersion;        // DXC 版本串（IDxcVersionInfo）
    std::vector<std::string> arguments; // 完整编译参数（除 -E/-T，见 manifest 注释）
    std::string sourceSha256;           // 源文件内容的 SHA-256（十六进制小写）
    std::string dxilSha256;             // DXIL 对象的 SHA-256
    std::string pdbName;                // 编译器建议的 PDB 名（唯一，full PDB 与 DXIL 共用）
    bool pdbIsFull = false;             // 由 IDxcPdbUtils2::IsFullPDB 对落盘内容确认
    std::string objectPath;             // 落盘的 .dxil 绝对路径
    std::string pdbPath;                // 落盘的 .pdb 绝对路径
    std::string manifestPath;           // 落盘的 .json 绝对路径
    std::size_t dxilBytes = 0;
};

// 编译一个入口并写出 .dxil + PDB + manifest。
//
// 参数：
//   repositoryRoot —— 仓库根（用于解析源文件与 #include）。**会被规范化为"绝对 +
//                     正斜杠"**再拼源路径：源文件路径被 `-Zi` 写进 DXIL/PDB，路径字符串
//                     形式一变，dxilSha256/pdbName 就变（审查 N-3），因此不能让调用方的
//                     写法影响产物字节。正斜杠是必需的——构建侧传的就是这一种形式。
//   sourceRelative —— 源文件仓库相对路径（正斜杠）
//   entryPoint     —— 入口名（如 VSMain）
//   targetProfile  —— 目标 profile（vs_6_0 / ps_6_0）
//   outputDirectory—— 产物目录（不存在则创建；产物是 build artifact，不入库）
// 失败：DXC 创建/编译/取输出失败、源文件不可读、输出不可写均抛 std::runtime_error
//   或 HResultError 风格异常；编译诊断（含 warning，因 -WX 升级为错误）写进错误文本。
[[nodiscard]] CompileResult CompileEntry(const std::filesystem::path& repositoryRoot, const std::string& sourceRelative,
                                         const std::string& entryPoint, const std::string& targetProfile,
                                         const std::filesystem::path& outputDirectory);
} // namespace MiniEngine::ShaderCompiler
