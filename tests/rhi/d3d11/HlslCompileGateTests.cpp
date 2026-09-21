// ============================================================================
// HlslCompileGateTests.cpp — 运行时 HLSL 编译 gate
// 里程碑：M4-02（审查后新增）
// 职责：用与运行时 D3D11Renderer 完全相同的编译口径（MiniEngine::CompileShader：
//       Debug = ENABLE_STRICTNESS/WARNINGS_ARE_ERRORS/DEBUG/SKIP_OPTIMIZATION，
//       Release = OPTIMIZATION_LEVEL3）编译 shaders/d3d11 下全部 .hlsl 的全部
//       入口，把"只在 Sandbox 启动时才暴露"的 HLSL 缺陷挡在 ctest 门口。
// 背景：FXC 在 /Od（SKIP_OPTIMIZATION）下对提前 return 的 BRDF 函数误报
//       X4000（use of potentially uninitialized variable），被 /WX 提升为
//       编译错误——一个编译不过的 shader 曾通过全部 237 项测试直到 Debug
//       Sandbox 启动失败（2026-09-06 审查定性，见 STATUS M4-02 记录）。
//       Debug 配置跑本 gate = /Od /WX 严格口径；双配置 ctest 共同覆盖两种口径。
// 维护约定：新增 .hlsl（03 篇 ShadowDepth、04 篇 IBL 系列、05 篇 ToneMap…）时
//       必须同步在此登记其全部入口——EveryShaderFileIsRegistered 会把"有文件
//       没登记"变成测试失败，忘记登记不再静默放行；.hlsli 只作 include，
//       不单独编译。
// TexturedCube.hlsl 说明：M2 遗留、已不被运行时编译（现为 PbrForward.hlsl），
//       保留在入口表作为 gate 的回归对照网；删除时须同步本表。
// 关联：engine/rhi/d3d11/src/D3D11ShaderCompiler.cpp（编译口径唯一来源）
//       docs/architecture/README.md「Shader 编译」（warning 视为构建失败）
// ============================================================================

#include "D3D11ShaderCompiler.h" // 引擎内部头（src/）；经测试目标的 include 路径暴露，口径与运行时唯一同源

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

#ifndef MINIENGINE_SHADER_DIR
#error "MINIENGINE_SHADER_DIR must be defined by the test target (see tests/rhi/d3d11/CMakeLists.txt)"
#endif

namespace
{
struct ShaderEntry final
{
    const char* file;
    const char* entryPoint;
    const char* target;
};

// 运行时会被 D3D11Renderer 编译的全部 (文件, 入口, Profile)。
// 注意保持与渲染端实际编译的入口一一对应；新增 pass 时同步登记。
// ShadowDepth.hlsl 是 depth-only pass，只有 VSMain（无 PS）。
// M4-04 IBL 生成系列：EquirectToCube（VS/PS）、IrradianceConvolution（VS/PS/
// PSDownsample——确定性 mip 链入口）、PrefilterEnvironment（VS/PS）、
// IntegrateBrdf（VS/PS，fullscreen SV_VertexID 无 VB）。生成 shader 由
// D3D11IblResources::Generator 在运行时以同一口径编译，gate 同步覆盖。
// M4-05 后处理：ToneMap（VS/PS，fullscreen 无 VB）、Skybox（VS/PS，cube 几何）。
constexpr ShaderEntry kShaderEntries[]{
    {"GraphTriangle.hlsl", "VSMain", "vs_5_0"},
    {"GraphTriangle.hlsl", "PSMain", "ps_5_0"},
    {"PbrForward.hlsl", "VSMain", "vs_5_0"},
    {"PbrForward.hlsl", "PSMain", "ps_5_0"},
    {"ShadowDepth.hlsl", "VSMain", "vs_5_0"},
    {"EquirectToCube.hlsl", "VSMain", "vs_5_0"},
    {"EquirectToCube.hlsl", "PSMain", "ps_5_0"},
    {"IrradianceConvolution.hlsl", "VSMain", "vs_5_0"},
    {"IrradianceConvolution.hlsl", "PSMain", "ps_5_0"},
    {"IrradianceConvolution.hlsl", "PSDownsample", "ps_5_0"},
    {"PrefilterEnvironment.hlsl", "VSMain", "vs_5_0"},
    {"PrefilterEnvironment.hlsl", "PSMain", "ps_5_0"},
    {"IntegrateBrdf.hlsl", "VSMain", "vs_5_0"},
    {"IntegrateBrdf.hlsl", "PSMain", "ps_5_0"},
    {"ToneMap.hlsl", "VSMain", "vs_5_0"},
    {"ToneMap.hlsl", "PSMain", "ps_5_0"},
    {"Skybox.hlsl", "VSMain", "vs_5_0"},
    {"Skybox.hlsl", "PSMain", "ps_5_0"},
    {"TexturedCube.hlsl", "VSMain", "vs_5_0"},
    {"TexturedCube.hlsl", "PSMain", "ps_5_0"},
};
} // namespace

TEST(HlslCompileGateTests, AllShadersCompileWithRuntimeFlagProfile)
{
    const std::filesystem::path shaderDir = MINIENGINE_SHADER_DIR;
    ASSERT_TRUE(std::filesystem::is_directory(shaderDir)) << shaderDir.string();

    for (const ShaderEntry& entry : kShaderEntries)
    {
        const std::filesystem::path file = shaderDir / entry.file;
        ASSERT_TRUE(std::filesystem::is_regular_file(file)) << file.string();

        // 显式 try/catch 而非 EXPECT_NO_THROW：CompileShader 抛出的
        // std::runtime_error 携带 FXC 诊断文本——那正是 2026-09-06 定性
        // Debug Sandbox 失败时最缺的东西，必须保证它出现在失败输出里。
        try
        {
            const auto bytecode = MiniEngine::CompileShader(file, entry.entryPoint, entry.target);
            EXPECT_NE(bytecode.Get(), nullptr);
            EXPECT_GT(bytecode->GetBufferSize(), 0U);
        }
        catch (const std::exception& error)
        {
            ADD_FAILURE() << entry.file << " [" << entry.entryPoint << "/" << entry.target
                          << "] 编译失败（口径见 D3D11ShaderCompiler.cpp，Debug=/Od /WX）: " << error.what();
        }
    }
}

// 交叉校验：shaders/d3d11 下每个 .hlsl 都必须在入口表登记，反之亦然。
// "有 .hlsl 没登记"等价于没有 gate——忘记登记是最可能的失效模式，
// 本用例把"靠自觉"变成"靠断言"（M4-02 二次审查加固）。
TEST(HlslCompileGateTests, EveryShaderFileIsRegistered)
{
    const std::filesystem::path shaderDir = MINIENGINE_SHADER_DIR;
    ASSERT_TRUE(std::filesystem::is_directory(shaderDir)) << shaderDir.string();

    std::set<std::string> onDisk;
    for (const std::filesystem::directory_entry& item : std::filesystem::directory_iterator{shaderDir})
    {
        if (item.is_regular_file() && item.path().extension() == ".hlsl")
        {
            onDisk.insert(item.path().filename().string());
        }
    }

    std::set<std::string> registered;
    for (const ShaderEntry& entry : kShaderEntries)
    {
        registered.insert(entry.file);
    }

    EXPECT_EQ(onDisk, registered) << "shaders/d3d11 与 gate 入口表不一致：磁盘上有 .hlsl 未登记"
                                     "（新 pass 必须登记全部入口），或表里有文件已被删除/改名";
}
