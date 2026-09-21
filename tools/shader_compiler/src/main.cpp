// ============================================================================
// main.cpp — shader_compiler CLI（D3D12 shader 离线编译工具）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 1 条）
// 职责：把 `shaders/d3d12/*.hlsl` 编译成 `.dxil` + PDB + manifest，供运行时 PSO
//       直接加载（帧循环绝不编译 shader——08 篇硬约束，本工具是唯一编译入口）。
// 用法：
//   shader_compiler.exe --out <dir> [--repo <root>] [--source <relative.hlsl> | --all]
//   不带 --source/--all 时编译清单里的全部入口（与 --all 等价）。
//   --repo 会被规范化为"绝对 + 正斜杠"后再嵌进调试信息，因此产物字节与调用形式
//   无关（传 `.`、正斜杠或反斜杠都得到逐位相同的 DXIL/PDB；见 DxcCompiler.h 的说明）。
// 退出码：0 = 全部编译成功；1 = 参数错误；2 = 编译失败（诊断已打印）。
// 关联：tools/shader_compiler/src/DxcCompiler.h、ShaderCompileRegistry.h
//       docs/architecture/README.md（终端命令）
// ============================================================================
#include "DualShaderPackage.h"
#include "DxcCompiler.h"
#include "ShaderCompileRegistry.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
struct Options final
{
    std::filesystem::path repositoryRoot{"."};
    std::filesystem::path outputDirectory{"build/shaders/d3d12"};
    std::string sourceFilter; // 非空时只编译该源文件的入口
    bool package = false;
    bool valid = true;
};

Options ParseOptions(const int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const auto nextValue = [&](std::string& target)
        {
            if (index + 1 >= argc)
            {
                std::cerr << "shader_compiler: missing value for " << argument << "\n";
                options.valid = false;
                return false;
            }
            target = argv[++index];
            return true;
        };

        if (argument == "--repo")
        {
            std::string value;
            if (nextValue(value))
            {
                options.repositoryRoot = value;
            }
        }
        else if (argument == "--out")
        {
            std::string value;
            if (nextValue(value))
            {
                options.outputDirectory = value;
            }
        }
        else if (argument == "--source")
        {
            std::string value;
            if (nextValue(value))
            {
                // 统一为正斜杠，与 registry 的写法一致（跨平台比较与 manifest 都用它）。
                std::filesystem::path normalized{value};
                options.sourceFilter = normalized.generic_string();
            }
        }
        else if (argument == "--all")
        {
            options.sourceFilter.clear();
        }
        else if (argument == "--package")
        {
            options.package = true;
        }
        else
        {
            std::cerr << "shader_compiler: unknown argument " << argument << "\n";
            options.valid = false;
        }
    }
    return options;
}
} // namespace

int main(const int argc, char** argv)
{
    const Options options = ParseOptions(argc, argv);
    if (!options.valid)
    {
        std::cerr
            << "usage: shader_compiler --out <dir> [--repo <root>] [--source <relative.hlsl> | --all | --package]\n";
        return 1;
    }

    if (options.package)
    {
        if (!options.sourceFilter.empty())
        {
            std::cerr << "shader_compiler: --package cannot be combined with --source\n";
            return 1;
        }
        try
        {
            const std::size_t assetCount =
                MiniEngine::ShaderCompiler::BuildDualShaderPackage(options.repositoryRoot, options.outputDirectory);
            std::cout << "shader_compiler: dual shader package ready with " << assetCount << " asset(s) in "
                      << (options.outputDirectory / "manifest.json").string() << "\n";
            return 0;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "shader_compiler: " << exception.what() << "\n";
            return 2;
        }
    }

    std::size_t compiled = 0U;
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        if (!options.sourceFilter.empty() && options.sourceFilter != entry.source)
        {
            continue;
        }

        try
        {
            const MiniEngine::ShaderCompiler::CompileResult result = MiniEngine::ShaderCompiler::CompileEntry(
                options.repositoryRoot, entry.source, entry.entry, entry.target, options.outputDirectory);
            std::cout << "compiled " << result.sourcePath << " [" << result.entryPoint << " " << result.targetProfile
                      << "] dxilBytes=" << result.dxilBytes << " sourceSha256=" << result.sourceSha256
                      << " dxilSha256=" << result.dxilSha256 << " pdb=" << result.pdbName << "\n";
            ++compiled;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "shader_compiler: " << exception.what() << "\n";
            return 2;
        }
    }

    if (compiled == 0U)
    {
        std::cerr << "shader_compiler: no matching entries for --source " << options.sourceFilter << "\n";
        return 1;
    }

    std::cout << "shader_compiler: " << compiled << " entry(ies) compiled into " << options.outputDirectory.string()
              << "\n";
    return 0;
}
