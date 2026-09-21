// ============================================================================
// D3D12CbufferDriftTests.cpp — HLSL cbuffer 声明与 CPU 常量结构体的机读对账
// 里程碑：M5（09 篇 迁移 M4 Pass 与输出一致性；审查三-3 要求的补强）
// 职责：`D3D12Constants.h` 自称"与 HLSL cbuffer 逐字段一致"，但只断言 CPU 侧
//       偏移**并不能支撑这个声称**——偏移全对而 HLSL 侧多了一个字段，一样会
//       错位。本用例逐条解析 `shaders/d3d12/{PbrForward,ShadowDepth,Skybox,
//       ToneMap}.hlsl` 的 cbuffer 声明，做三件事：
//         1) 字段**个数**一致（HLSL 增删字段必须同批改 CPU 结构体）；
//         2) 字段**顺序与类型**一致（错序同样会错位）；
//         3) 按 HLSL 打包规则算出的**偏移**与 CPU 侧 `offsetof` 逐字段相等，
//            且总大小等于 `sizeof`。
//       任一侧单独改动都会让本用例失败。与 BindingContractDriftTests 同款思路
//       （解析源文件做漂移比对），但对象是**字段布局**而不是寄存器号。
//
// HLSL 打包规则（本文件实现的子集，足够覆盖本仓库的常量布局）：
//   每个成员按 min(自身大小, 16) 对齐，且**不得跨 16 字节边界**；
//   结构体总大小向上取整到 16 的倍数。全 float4/float4x4 的布局下，
//   这条规则退化为"每个成员占满整数个 16 字节行"。
//
// 定位方式：与 BindingContractDriftTests 一致，由测试目标注入 MINIENGINE_SHADER_DIR。
// 关联：docs/architecture/README.md（M4 resource profile 保持）
//       engine/rhi/d3d12/src/D3D12Constants.h（被对账的 CPU 侧唯一真源）
//       tests/rhi/d3d12/BindingContractDriftTests.cpp（同款源文件解析手法）
// ============================================================================
#include "D3D12Constants.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MINIENGINE_SHADER_DIR
#error "MINIENGINE_SHADER_DIR must be defined by the test target (see tests/rhi/d3d12/CMakeLists.txt)"
#endif

using MiniEngine::Rhi::D3D12::FrameConstants;
using MiniEngine::Rhi::D3D12::IblConstants;
using MiniEngine::Rhi::D3D12::MaterialConstants;
using MiniEngine::Rhi::D3D12::ObjectConstants;
using MiniEngine::Rhi::D3D12::PostProcessConstants;
using MiniEngine::Rhi::D3D12::SkyboxConstants;

namespace
{
// 一个 cbuffer 成员的 HLSL 声明 + 它在 CPU 结构体里的对应偏移/大小。
struct ExpectedField final
{
    std::string_view hlslType;
    std::string_view hlslName;
    std::size_t cpuOffset;
    std::size_t cpuSize;
};

// HLSL 标量/向量/矩阵类型的字节大小。只登记本仓库常量布局用到的类型；
// 出现未登记类型时返回 0，调用方据此判定"解析器需要更新"而不是静默算错。
std::size_t SizeOfHlslType(const std::string_view type)
{
    if (type == "float4x4")
    {
        return 64U;
    }
    if (type == "float4")
    {
        return 16U;
    }
    if (type == "float3")
    {
        return 12U;
    }
    if (type == "float2")
    {
        return 8U;
    }
    if (type == "float" || type == "uint" || type == "int")
    {
        return 4U;
    }
    return 0U;
}

std::size_t AlignUp(const std::size_t value, const std::size_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

// 解析结果：成员列表（类型 + 名字，按出现顺序）。
struct ParsedCbuffer final
{
    std::vector<std::pair<std::string, std::string>> fields; // {type, name}
};

// 从单个 .hlsl 文件里取出指定 cbuffer 的成员列表。
//
// 只做本仓库所需的极简解析：定位 `cbuffer <Name>` 行，读到第一个 `};` 为止；
// 每行去掉 `//` 之后的内容，再按 `<type> <name>;` 取前两个 token。
// 解析不到任何成员视为失败（测试会断言非空，避免"文件改名后静默跳过"）。
ParsedCbuffer ParseCbuffer(const std::string& path, const std::string& cbufferName)
{
    ParsedCbuffer parsed;
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << "无法打开 " << path;

    std::string line;
    bool inside = false;
    while (std::getline(file, line))
    {
        // 先剥掉行内注释：`float4 Foo; // xyz = ...` 的尾部不参与解析。
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos)
        {
            line.erase(comment);
        }
        if (!inside)
        {
            if (line.find("cbuffer " + cbufferName) != std::string::npos)
            {
                inside = true;
            }
            continue;
        }
        // 块尾可能是 `};`，也可能是单独的 `}`——PbrForward.hlsl 的 IblConstants 就是后者。
        // 只认 `};` 会越过块尾继续扫全文件，把后续 struct 里形如 `float3 normal : NORMAL;`
        // 的行误判成 cbuffer 成员；本仓库眼下恰好靠"下一个 `};`"兜住，是运气不是正确性。
        const std::size_t firstNonSpace = line.find_first_not_of(" \t\r");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '}')
        {
            break;
        }
        std::istringstream stream{line};
        std::string type;
        std::string name;
        if (!(stream >> type >> name))
        {
            continue; // 空行或 `{`
        }
        if (name.empty() || name.back() != ';')
        {
            continue;
        }
        name.pop_back(); // 去掉分号
        parsed.fields.emplace_back(type, name);
    }
    return parsed;
}

// 按 HLSL 打包规则累计偏移，并与 CPU 侧的 offsetof/大小逐字段比对。
void ExpectLayoutMatches(const ParsedCbuffer& parsed, const std::vector<ExpectedField>& expected,
                         const std::size_t cpuStructSize, const char* cbufferLabel)
{
    ASSERT_EQ(parsed.fields.size(), expected.size())
        << cbufferLabel << "：HLSL 成员数与 CPU 结构体字段数不一致（增删字段必须两侧同批改）";

    std::size_t offset = 0;
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto& [hlslType, hlslName] = parsed.fields[index];
        const ExpectedField& want = expected[index];

        EXPECT_EQ(hlslType, want.hlslType) << cbufferLabel << " 第 " << index << " 个成员类型不一致";
        EXPECT_EQ(hlslName, want.hlslName)
            << cbufferLabel << " 第 " << index << " 个成员名字不一致（顺序错位同样会读错数据）";

        const std::size_t fieldSize = SizeOfHlslType(hlslType);
        ASSERT_NE(fieldSize, 0U) << cbufferLabel << " 出现未登记的 HLSL 类型 " << hlslType
                                 << "（需要更新本测试的类型表）";

        // HLSL 打包：按 min(大小,16) 对齐，且不得跨 16 字节边界。
        offset = AlignUp(offset, (fieldSize < 16U) ? fieldSize : 16U);
        if ((offset % 16U) != 0U && ((offset % 16U) + fieldSize) > 16U)
        {
            offset = AlignUp(offset, 16U);
        }

        EXPECT_EQ(offset, want.cpuOffset)
            << cbufferLabel << " 成员 " << hlslName << " 的 HLSL 偏移与 CPU offsetof 不一致";
        EXPECT_EQ(fieldSize, want.cpuSize) << cbufferLabel << " 成员 " << hlslName << " 的大小不一致";

        offset += fieldSize;
    }

    EXPECT_EQ(AlignUp(offset, 16U), cpuStructSize)
        << cbufferLabel << "：按 HLSL 声明算出的总大小与 sizeof(CPU 结构体) 不一致";
}
} // namespace

TEST(D3D12CbufferDriftTests, FrameConstantsMatchesPbrForwardHlsl)
{
    const ParsedCbuffer parsed =
        ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/PbrForward.hlsl", "FrameConstants");
    const std::vector<ExpectedField> expected{
        {"float4x4", "ViewProjection", offsetof(FrameConstants, viewProjection),
         sizeof(FrameConstants::viewProjection)},
        {"float4", "CameraPositionAndDebugMode", offsetof(FrameConstants, cameraPositionAndDebugMode),
         sizeof(FrameConstants::cameraPositionAndDebugMode)},
        {"float4", "DirectionAndIntensity", offsetof(FrameConstants, directionAndIntensity),
         sizeof(FrameConstants::directionAndIntensity)},
        {"float4", "LightColorAndPadding", offsetof(FrameConstants, lightColorAndPadding),
         sizeof(FrameConstants::lightColorAndPadding)},
        {"float4", "ShadowMapSizeAndPadding", offsetof(FrameConstants, shadowMapSizeAndPadding),
         sizeof(FrameConstants::shadowMapSizeAndPadding)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(FrameConstants), "FrameConstants");
}

TEST(D3D12CbufferDriftTests, ObjectConstantsMatchesPbrForwardHlsl)
{
    const ParsedCbuffer parsed =
        ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/PbrForward.hlsl", "ObjectConstants");
    const std::vector<ExpectedField> expected{
        {"float4x4", "World", offsetof(ObjectConstants, world), sizeof(ObjectConstants::world)},
        {"float4x4", "NormalMatrix", offsetof(ObjectConstants, normalMatrix), sizeof(ObjectConstants::normalMatrix)},
        {"float4x4", "LightWorldViewProjection", offsetof(ObjectConstants, lightWorldViewProjection),
         sizeof(ObjectConstants::lightWorldViewProjection)},
        {"float4", "HandednessAndReceivesShadow", offsetof(ObjectConstants, handednessAndReceivesShadow),
         sizeof(ObjectConstants::handednessAndReceivesShadow)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(ObjectConstants), "ObjectConstants");
}

// ShadowDepth.hlsl 的 b1 与 PbrForward 的 b1 是**同一份契约**（03 篇硬约束：
// forward 与 shadow 用同一组对象常量）。单独对 ShadowDepth 再跑一遍，防止有人
// 只改了一侧——这正是"两份声明、一处修改"最典型的漂移形态。
TEST(D3D12CbufferDriftTests, ObjectConstantsMatchesShadowDepthHlsl)
{
    const ParsedCbuffer parsed =
        ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/ShadowDepth.hlsl", "ObjectConstants");
    const std::vector<ExpectedField> expected{
        {"float4x4", "World", offsetof(ObjectConstants, world), sizeof(ObjectConstants::world)},
        {"float4x4", "NormalMatrix", offsetof(ObjectConstants, normalMatrix), sizeof(ObjectConstants::normalMatrix)},
        {"float4x4", "LightWorldViewProjection", offsetof(ObjectConstants, lightWorldViewProjection),
         sizeof(ObjectConstants::lightWorldViewProjection)},
        {"float4", "HandednessAndReceivesShadow", offsetof(ObjectConstants, handednessAndReceivesShadow),
         sizeof(ObjectConstants::handednessAndReceivesShadow)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(ObjectConstants), "ShadowDepth.ObjectConstants");
}

TEST(D3D12CbufferDriftTests, MaterialConstantsMatchesPbrForwardHlsl)
{
    const ParsedCbuffer parsed =
        ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/PbrForward.hlsl", "MaterialConstants");
    const std::vector<ExpectedField> expected{
        {"float4", "BaseColorFactor", offsetof(MaterialConstants, baseColorFactor),
         sizeof(MaterialConstants::baseColorFactor)},
        {"float4", "EmissiveAndMetallic", offsetof(MaterialConstants, emissiveAndMetallic),
         sizeof(MaterialConstants::emissiveAndMetallic)},
        {"float4", "RoughnessNormalOcclusionFlags", offsetof(MaterialConstants, roughnessNormalOcclusionFlags),
         sizeof(MaterialConstants::roughnessNormalOcclusionFlags)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(MaterialConstants), "MaterialConstants");
}

TEST(D3D12CbufferDriftTests, IblConstantsMatchesPbrForwardHlsl)
{
    const ParsedCbuffer parsed = ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/PbrForward.hlsl", "IblConstants");
    const std::vector<ExpectedField> expected{
        {"float4", "PrefilterMipCountAndFlags", offsetof(IblConstants, prefilterMipCountAndFlags),
         sizeof(IblConstants::prefilterMipCountAndFlags)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(IblConstants), "IblConstants");
}

TEST(D3D12CbufferDriftTests, SkyboxConstantsMatchesSkyboxHlsl)
{
    const ParsedCbuffer parsed = ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/Skybox.hlsl", "SkyboxConstants");
    const std::vector<ExpectedField> expected{
        {"float4x4", "ViewProjectionWithoutTranslation", offsetof(SkyboxConstants, viewProjectionWithoutTranslation),
         sizeof(SkyboxConstants::viewProjectionWithoutTranslation)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(SkyboxConstants), "SkyboxConstants");
}

// ToneMap.hlsl 的 cbuffer 名是 `ToneMapConstants`（继承自 M4 的 D3D11 侧命名），
// 与 CPU 侧的 `PostProcessConstants` 名字不同——这是历史命名差异，不是漂移，
// 因此这里显式记下：**按 HLSL 名解析、按 CPU 类型对账**。
TEST(D3D12CbufferDriftTests, PostProcessConstantsMatchesToneMapHlsl)
{
    const ParsedCbuffer parsed = ParseCbuffer(std::string{MINIENGINE_SHADER_DIR} + "/ToneMap.hlsl", "ToneMapConstants");
    const std::vector<ExpectedField> expected{
        {"float", "ExposureEv", offsetof(PostProcessConstants, exposureEv), sizeof(PostProcessConstants::exposureEv)},
        {"uint", "DebugHdr", offsetof(PostProcessConstants, debugHdr), sizeof(PostProcessConstants::debugHdr)},
        {"float2", "InverseOutputSize", offsetof(PostProcessConstants, inverseOutputSize),
         sizeof(PostProcessConstants::inverseOutputSize)},
    };
    ExpectLayoutMatches(parsed, expected, sizeof(PostProcessConstants), "ToneMapConstants/PostProcessConstants");
}
