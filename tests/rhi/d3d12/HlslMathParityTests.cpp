// ============================================================================
// HlslMathParityTests.cpp — M4(D3D11/FXC) 与 M5(D3D12/DXC) 共享 PBR 公式的等价证明
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 5 条）
// 职责：把"共享 HLSL 边界"从一句承诺变成可执行断言：
//   - `shaders/d3d11/PbrCommon.hlsli` 与 `shaders/d3d12/PbrCommon.hlsli` 的每个
//     **纯函数体**逐 token 相同；
//   - 三个共享常量（PI / DIELECTRIC_F0 / MIN_ROUGHNESS）的取值相同。
// 为什么不合并成一个文件：08 篇明确"不为同一文件编译引入大量条件分支"——
//   D3D11 是 FXC SM5、D3D12 是 DXC SM6，共用一份源会让两边的编译口径互相牵制。
//   代价是"两份源会漂移"，因此必须有这条测试来兜住漂移。
// 为什么只比较函数体：文件头注释、include guard、说明性注释必然不同（两侧的里程碑
//   与调用方都不一样）；真正决定像素值的是**函数体与常量**，只冻结这部分。
// 关联：shaders/d3d11/PbrCommon.hlsli（M4 唯一实现）
//       shaders/d3d12/PbrCommon.hlsli（M5 同源移植）
//       tests/rendering/PbrMathTests.cpp（同一批公式的 CPU 参考）
//       docs/architecture/README.md「共享 HLSL 边界」
// ============================================================================
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MINIENGINE_PROJECT_ROOT
#error "MINIENGINE_PROJECT_ROOT must be defined by the test target"
#endif

namespace
{
const std::filesystem::path kRepositoryRoot{MINIENGINE_PROJECT_ROOT};
const std::filesystem::path kD3d11Common = kRepositoryRoot / "shaders/d3d11/PbrCommon.hlsli";
const std::filesystem::path kD3d12Common = kRepositoryRoot / "shaders/d3d12/PbrCommon.hlsli";

// 08 篇「可以抽取的纯函数」清单：BRDF 四件套 + IBL 采样四件套 + 两个工具函数。
// 新增共享函数时必须同时加进这里，否则它不受本契约保护。
const std::vector<std::string> kSharedFunctions{
    "Pow5(",
    "FresnelSchlick(",
    "FresnelSchlickRoughness(",
    "DistributionGgx(",
    "GeometrySchlickGgx(",
    "GeometrySmith(",
    "EvaluateDirectBrdf(",
    "RadicalInverseVdc(",
    "Hammersley(",
    "ImportanceSampleGgx(",
    "CosineSampleHemisphere(",
};

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return {};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !file.read(text.data(), size))
    {
        return {};
    }
    return text;
}

// 去掉注释并把连续空白压成一个空格：这样"换行位置不同"不会被判成漂移，
// 而真正改了 token（含改了数字）一定会被判成漂移。
std::string StripCommentsAndNormalizeWhitespace(const std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    bool pendingSpace = false;
    for (std::size_t index = 0; index < source.size();)
    {
        if (source[index] == '/' && index + 1U < source.size() && source[index + 1U] == '/')
        {
            // 行注释：吃到行尾。
            while (index < source.size() && source[index] != '\n')
            {
                ++index;
            }
            continue;
        }
        if (source[index] == '/' && index + 1U < source.size() && source[index + 1U] == '*')
        {
            // 块注释：吃到 "*/"（HLSL 无嵌套块注释）。
            index += 2U;
            while (index + 1U < source.size() && !(source[index] == '*' && source[index + 1U] == '/'))
            {
                ++index;
            }
            index = index + 1U < source.size() ? index + 2U : index;
            continue;
        }
        if (static_cast<unsigned char>(source[index]) <= ' ')
        {
            pendingSpace = result.empty() ? false : true;
            ++index;
            continue;
        }
        if (pendingSpace)
        {
            result.push_back(' ');
            pendingSpace = false;
        }
        result.push_back(source[index]);
        ++index;
    }
    return result;
}

// 取出 `name` 的完整定义（含签名）：从签名所在行的行首开始，到花括号配对结束。
//
// 返回空串表示没找到——调用方按"缺函数"处理（新增共享函数漏抄会让断言失败）。
std::string ExtractFunction(const std::string& source, const std::string& signature)
{
    const std::size_t at = source.find(signature);
    if (at == std::string::npos)
    {
        return {};
    }
    const std::size_t lineBegin = source.rfind('\n', at) == std::string::npos ? 0U : source.rfind('\n', at) + 1U;
    const std::size_t brace = source.find('{', at);
    if (brace == std::string::npos)
    {
        return {};
    }

    int depth = 0;
    std::size_t cursor = brace;
    for (; cursor < source.size(); ++cursor)
    {
        if (source[cursor] == '{')
        {
            ++depth;
        }
        else if (source[cursor] == '}')
        {
            --depth;
            if (depth == 0)
            {
                ++cursor;
                break;
            }
        }
    }
    if (depth != 0)
    {
        return {}; // 花括号不配对：宁可报错也不给一个残缺的比较结果
    }
    return source.substr(lineBegin, cursor - lineBegin);
}

// 取常量的初值文本（`static const float NAME = <expr>;` 里的 `<expr>`）。
std::string ExtractConstantValue(const std::string& source, const std::string& name)
{
    const std::string needle = "float " + name + " = ";
    const std::size_t at = source.find(needle);
    if (at == std::string::npos)
    {
        return {};
    }
    const std::size_t begin = at + needle.size();
    const std::size_t end = source.find(';', begin);
    return end == std::string::npos ? std::string{} : source.substr(begin, end - begin);
}
} // namespace

// D3D12 侧的 PBR 公共库必须与 M4 的同名函数逐 token 一致（08 篇共享 HLSL 边界）。
TEST(HlslMathParityTests, SharedPbrFunctionsMatchTheD3d11Original)
{
    const std::string d3d11Source = ReadTextFile(kD3d11Common);
    const std::string d3d12Source = ReadTextFile(kD3d12Common);
    ASSERT_FALSE(d3d11Source.empty()) << kD3d11Common.string();
    ASSERT_FALSE(d3d12Source.empty()) << kD3d12Common.string();

    for (const std::string& signature : kSharedFunctions)
    {
        const std::string d3d11Function = ExtractFunction(d3d11Source, signature);
        const std::string d3d12Function = ExtractFunction(d3d12Source, signature);
        ASSERT_FALSE(d3d11Function.empty()) << "M4 侧缺少共享函数：" << signature;
        ASSERT_FALSE(d3d12Function.empty()) << "D3D12 侧缺少共享函数：" << signature;

        EXPECT_EQ(StripCommentsAndNormalizeWhitespace(d3d11Function),
                  StripCommentsAndNormalizeWhitespace(d3d12Function))
            << "两侧公式已漂移：" << signature;
    }
}

// 共享常量决定 BRDF 的量化结果：值改了而公式没改时，只有这条断言能抓住。
TEST(HlslMathParityTests, SharedPbrConstantsMatchTheD3d11Original)
{
    const std::string d3d11Source = ReadTextFile(kD3d11Common);
    const std::string d3d12Source = ReadTextFile(kD3d12Common);
    ASSERT_FALSE(d3d11Source.empty()) << kD3d11Common.string();
    ASSERT_FALSE(d3d12Source.empty()) << kD3d12Common.string();

    for (const std::string& name : {"PI", "DIELECTRIC_F0", "MIN_ROUGHNESS"})
    {
        const std::string d3d11Value = ExtractConstantValue(d3d11Source, name);
        const std::string d3d12Value = ExtractConstantValue(d3d12Source, name);
        ASSERT_FALSE(d3d11Value.empty()) << "M4 侧缺少共享常量：" << name;
        EXPECT_EQ(d3d11Value, d3d12Value) << "两侧常量已漂移：" << name;
    }
}

// 反退化：上述断言必须真的能发现漂移，否则"比较逻辑写错了"会表现为永久绿灯。
// 这里故意改掉一侧函数体里的一个 token，期望归一化结果不再相等。
TEST(HlslMathParityTests, ParityCheckDetectsAnInjectedDrift)
{
    const std::string original = ExtractFunction(ReadTextFile(kD3d11Common), "DistributionGgx(");
    ASSERT_FALSE(original.empty());

    const std::string drifted = original + " "; // 纯空白差异：归一化后必须仍然相等
    EXPECT_EQ(StripCommentsAndNormalizeWhitespace(original), StripCommentsAndNormalizeWhitespace(drifted))
        << "归一化不得把'只差空白'判成漂移";

    std::string changed = original;
    const std::size_t at = changed.find("1.0e-7");
    ASSERT_NE(at, std::string::npos) << "fixture 依赖 DistributionGgx 里的分母下限";
    changed.replace(at, std::string{"1.0e-7"}.size(), "1.0e-6");
    EXPECT_NE(StripCommentsAndNormalizeWhitespace(original), StripCommentsAndNormalizeWhitespace(changed))
        << "归一化必须能抓到一个 token 的改动";
}
