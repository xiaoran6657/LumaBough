// ============================================================================
// D3D11ShaderCompiler.cpp — 运行时 HLSL 编译实现
// 里程碑：M2
// 职责：封装 D3DCompileFromFile，按构建配置选择调试/优化编译标志，并把编译诊断
//   文本写入日志；失败时抛异常并附文件、入口与目标信息。
// 关联：docs/architecture/README.md
// ============================================================================
#include "D3D11ShaderCompiler.h"

#include "D3D11Error.h"

#include <MiniEngine/Core/Log.h>

#include <string>

namespace MiniEngine
{
// 从 HLSL 文件编译指定入口与 Profile 的着色器，返回字节码；编译诊断写日志，失败抛异常。
Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const std::string_view entryPoint,
                                               const std::string_view target)
{
    // 严格模式并视警告为错误；Debug 保留调试符号且跳过优化以便断点，Release 采用三级优化。
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;

#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    // D3DCompileFromFile 的入口点与目标 Profile 参数是 NUL 结尾的 ANSI（LPCSTR）字符串。
    const std::string entry{entryPoint};
    const std::string shaderTarget{target};
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;

    const HRESULT result = D3DCompileFromFile(file.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.c_str(),
                                              shaderTarget.c_str(), flags, 0, bytecode.ReleaseAndGetAddressOf(),
                                              diagnostics.ReleaseAndGetAddressOf());

    // 编译诊断（警告或错误文本）优先输出到日志：失败为 Error 级，成功但有警告为 Warning 级。
    if (diagnostics != nullptr)
    {
        const auto* message = static_cast<const char*>(diagnostics->GetBufferPointer());
        const std::string text{message, diagnostics->GetBufferSize()};
        WriteLog(FAILED(result) ? LogLevel::Error : LogLevel::Warning, text);
    }

    // 编译失败在此抛出，异常文本同时含文件、入口与目标，便于定位出错着色器。
    ThrowIfFailed(result, "D3DCompileFromFile: " + file.string() + " [" + entry + "/" + shaderTarget + "]");
    return bytecode;
}
} // namespace MiniEngine
