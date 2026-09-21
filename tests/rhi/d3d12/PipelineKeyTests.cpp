// ============================================================================
// PipelineKeyTests.cpp — PSO key 与格式表的纯 CPU 契约测试
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 2 条）
// 职责：把 08 篇「PSO key」与「格式必须匹配」两张表变成可执行断言：
//   键的四个维度各自独立（pass/mirrored/shader 版本/root signature 版本）、
//   格式表逐 pass 与文档一致、DebugMode **不在**键里（它是 b0 常量）。
// 为什么不用设备：键与格式表都是纯值，设备级只验证"用这些值创建出的 PSO 能成功"
//   （见 D3D12PsoFactoryDeviceTests）。
// 关联：docs/architecture/README.md（PSO key / 格式表）
// ============================================================================
#include "D3D12PsoFactory.h"

#include <MiniEngine/Rhi/D3D12/D3D12PsoKey.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <type_traits>

#ifndef MINIENGINE_PROJECT_ROOT
#error "MINIENGINE_PROJECT_ROOT must be defined by the test target"
#endif

namespace D12 = MiniEngine::Rhi::D3D12;

namespace
{
// 读整个文本文件（机读对账用；读不到返回空串，由调用方断言）。
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
} // namespace

// 键的四个维度必须各自产生不同的键（少一个维度就会把两种状态错当作同一个 PSO）。
TEST(PipelineKeyTests, EveryKeyDimensionSeparatesPsos)
{
    D12::PsoKey baseline;
    baseline.pass = D12::PassKind::PbrOpaque;
    baseline.mirrored = false;
    baseline.shaderRevision = 3U;
    baseline.rootSignatureRevision = 1U;

    D12::PsoKey mirrored = baseline;
    mirrored.mirrored = true;
    D12::PsoKey otherPass = baseline;
    otherPass.pass = D12::PassKind::Skybox;
    D12::PsoKey newShaders = baseline;
    newShaders.shaderRevision = 4U;
    D12::PsoKey newRootSignature = baseline;
    newRootSignature.rootSignatureRevision = 2U;

    const std::set<D12::PsoKey> keys{baseline, mirrored, otherPass, newShaders, newRootSignature};
    EXPECT_EQ(keys.size(), 5U) << "五个键必须互不相等（任一维度参与唯一性）";
}

// DebugMode 不在键里：它由 b0 里的常量表达（08 篇明确要求），因此"同一 pass 的不同
// debug view"必须落回同一个 PSO。
//
// 这条测试的强度来自**结构化绑定的成员个数**而不是运行期判定：往 PsoKey 里加任何
// 字段（例如 debugMode）都会让下面的解构编译失败——"键里混入了不该有的维度"因此
// 不可能悄悄通过。
TEST(PipelineKeyTests, DebugModeIsNotPartOfTheKey)
{
    D12::PsoKey key;
    key.pass = D12::PassKind::PbrOpaque;

    const auto& [pass, mirrored, shaderRevision, rootSignatureRevision] = key;
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(pass)>, D12::PassKind>);
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(mirrored)>, bool>);
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(shaderRevision)>, std::uint64_t>);
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(rootSignatureRevision)>, std::uint64_t>);
    // 逐一读一次同时也让"四个维度恰好都被列出"这件事在运行期可见。
    EXPECT_EQ(pass, D12::PassKind::PbrOpaque);
    EXPECT_FALSE(mirrored);
    EXPECT_EQ(shaderRevision, 0U);
    EXPECT_EQ(rootSignatureRevision, 0U);

    // 两个"不同 debug view 的绘制"只能得到同一个键（debug view 走常量）。
    const D12::PsoKey sameKey = key;
    EXPECT_TRUE(key == sameKey);
    EXPECT_FALSE(key < sameKey || sameKey < key) << "同一 key 必须落在同一个 map 槽位";
}

// 格式表（08 篇「格式必须匹配」）：逐 pass 对照文档表格。
TEST(PipelineKeyTests, PassFormatsMatchTheFrozenTable)
{
    const D12::PassFormatEntry shadow = D12::PassFormats(D12::PassKind::Shadow);
    EXPECT_EQ(shadow.renderTargetCount, 0U) << "Shadow 无 RTV（depth-only）";
    EXPECT_EQ(shadow.depthStencilFormat, DXGI_FORMAT_D32_FLOAT);

    for (const D12::PassKind pass : {D12::PassKind::PbrOpaque, D12::PassKind::Skybox})
    {
        const D12::PassFormatEntry entry = D12::PassFormats(pass);
        EXPECT_EQ(entry.renderTargetCount, 1U) << D12::PassKindName(pass);
        EXPECT_EQ(entry.renderTargetFormat, DXGI_FORMAT_R16G16B16A16_FLOAT) << D12::PassKindName(pass);
        EXPECT_EQ(entry.depthStencilFormat, DXGI_FORMAT_D32_FLOAT) << D12::PassKindName(pass);
    }

    const D12::PassFormatEntry toneMap = D12::PassFormats(D12::PassKind::ToneMap);
    EXPECT_EQ(toneMap.renderTargetFormat, DXGI_FORMAT_R8G8B8A8_UNORM) << "ToneMap 写 UNORM 后备缓冲";
    EXPECT_EQ(toneMap.depthStencilFormat, DXGI_FORMAT_UNKNOWN) << "ToneMap 不用深度";

    const D12::PassFormatEntry brdfLut = D12::PassFormats(D12::PassKind::BrdfLut);
    EXPECT_EQ(brdfLut.renderTargetCount, 1U);
    EXPECT_EQ(brdfLut.renderTargetFormat, DXGI_FORMAT_R16G16_FLOAT) << "BRDF LUT 是 RG16F";
    EXPECT_EQ(brdfLut.depthStencilFormat, DXGI_FORMAT_UNKNOWN);

    const D12::PassFormatEntry triangle = D12::PassFormats(D12::PassKind::TriangleSmoke);
    EXPECT_EQ(triangle.renderTargetCount, 1U);
    EXPECT_EQ(triangle.renderTargetFormat, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(triangle.depthStencilFormat, DXGI_FORMAT_D32_FLOAT);

    for (const D12::PassKind pass : {D12::PassKind::EquirectToCube, D12::PassKind::Irradiance,
                                     D12::PassKind::EnvironmentDownsample, D12::PassKind::Prefilter})
    {
        EXPECT_EQ(D12::PassFormats(pass).renderTargetFormat, DXGI_FORMAT_R16G16B16A16_FLOAT)
            << D12::PassKindName(pass) << " 写 cube（HDR）";
    }
}

// 绕序契约：非镜像 = CCW 正面（与 M4/D3D11 的 `FrontCounterClockwise = TRUE` 一致），
// 镜像取反。这条断言的价值来自它抓到过的真实缺陷：工厂原实现与 M4 相反，导致
// D3D12 端每个网格被背面剔除（现象是"draw 数正常、像素全空"，见 09 篇第 1 步记录）。
// 结构性差异不适合用像素指标兜，因此在这里钉死。
TEST(PipelineKeyTests, RasterizerWindingMatchesTheD3d11Convention)
{
    EXPECT_TRUE(D12::FrontCounterClockwiseIsFront(false)) << "非镜像必须是 CCW 正面（M4 约定）";
    EXPECT_FALSE(D12::FrontCounterClockwiseIsFront(true)) << "镜像必须是 CW 正面（取反）";
}

// 与 M4 源码的机读对账（同 BindingContractDriftTests 的手法）：直接读
// `D3D11Renderer.cpp` 里主 pass 与镜像光栅化状态的绕序赋值，与 D3D12 的契约函数比对。
//
// 为什么必须机读而不是靠注释：本轮抓到的真实缺陷正是"两侧约定相反"（见
// RasterizerWindingMatchesTheD3d11Convention 的注释），而它的现象是像素整片消失，
// 不是可解释的色差。约定一旦在 M4 侧被改动，这条断言立刻失败。
TEST(PipelineKeyTests, RasterizerWindingMatchesTheD3d11Source)
{
    const std::string source =
        ReadTextFile(std::filesystem::path{MINIENGINE_PROJECT_ROOT} / "engine/rhi/d3d11/src/D3D11Renderer.cpp");
    ASSERT_FALSE(source.empty()) << "读不到 D3D11Renderer.cpp（MINIENGINE_PROJECT_ROOT 是否正确？）";

    EXPECT_NE(source.find("rasterizerDesc.FrontCounterClockwise = TRUE;"), std::string::npos)
        << "M4 主 pass 的『非镜像 = CCW 正面』锚点变了；D3D12 侧必须同步";
    EXPECT_NE(source.find("mirroredDesc.FrontCounterClockwise = FALSE;"), std::string::npos)
        << "M4 镜像 pass 的绕序锚点变了；D3D12 侧必须同步";

    // D3D12 侧的取值必须与上面两个锚点一致。
    EXPECT_TRUE(D12::FrontCounterClockwiseIsFront(false));
    EXPECT_FALSE(D12::FrontCounterClockwiseIsFront(true));
}

// 剔除模式契约：ToneMap/Skybox 的几何不携带绕序语义（超屏三角形/内表面），
// 必须与绕序约定解耦。这条断言的价值同样来自它抓到的真实缺陷：工厂原实现给
// 所有 pass 都上 CULL_BACK，tone map 全屏三角形被背面剔除（现象是"31 个 draw
// 全部发出、后备缓冲只剩 clear 色"，RenderDoc pixel history 实测
// `passed=no flags=backfaceCulled`，见 09 篇第 2 步记录）。
TEST(PipelineKeyTests, RasterizerCullModeMatchesTheD3d11PassClasses)
{
    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::ToneMap), D3D12_CULL_MODE_NONE)
        << "超屏三角形的绕序不携带语义，CULL_BACK 会把整个 tone map 剔掉";
    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::Skybox), D3D12_CULL_MODE_NONE)
        << "相机位于 skybox 立方体内部，必须画到内表面";
    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::PbrOpaque), D3D12_CULL_MODE_BACK);
    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::Shadow), D3D12_CULL_MODE_BACK);
    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::TriangleSmoke), D3D12_CULL_MODE_BACK);
    for (const D12::PassKind pass :
         {D12::PassKind::EquirectToCube, D12::PassKind::Irradiance, D12::PassKind::EnvironmentDownsample,
          D12::PassKind::Prefilter, D12::PassKind::BrdfLut})
    {
        EXPECT_EQ(D12::CullModeForPass(pass), D3D12_CULL_MODE_NONE) << D12::PassKindName(pass);
    }
}

// Shadow 镜像 key 必须独立存在；它沿用普通几何的剔除类别，但绕序取反。
TEST(PipelineKeyTests, ShadowMirroredKeyAndBiasContract)
{
    D12::PsoKey normal;
    normal.pass = D12::PassKind::Shadow;
    D12::PsoKey mirrored = normal;
    mirrored.mirrored = true;
    EXPECT_NE(normal, mirrored) << "Shadow 镜像实例必须使用独立 PSO key";
    EXPECT_TRUE(D12::FrontCounterClockwiseIsFront(normal.mirrored));
    EXPECT_FALSE(D12::FrontCounterClockwiseIsFront(mirrored.mirrored));

    const std::string source =
        ReadTextFile(std::filesystem::path{MINIENGINE_PROJECT_ROOT} / "engine/rhi/d3d12/src/D3D12PsoFactory.cpp");
    ASSERT_FALSE(source.empty()) << "读不到 D3D12PsoFactory.cpp";
    EXPECT_NE(source.find("rasterizer.SlopeScaledDepthBias = pass == PassKind::Shadow ? 0.1F : 0.0F;"),
              std::string::npos)
        << "Shadow slope scaled depth bias 必须固定为 0.1";
}

// 与 M4 源码的机读对账：D3D11 侧的 noCull 光栅化锚点（noCullDesc + "skybox 与
// tone map 共用"注释）若被改动，D3D12 的 pass 分类必须同步。
TEST(PipelineKeyTests, RasterizerCullModeMatchesTheD3d11Source)
{
    const std::string source =
        ReadTextFile(std::filesystem::path{MINIENGINE_PROJECT_ROOT} / "engine/rhi/d3d11/src/D3D11Renderer.cpp");
    ASSERT_FALSE(source.empty()) << "读不到 D3D11Renderer.cpp（MINIENGINE_PROJECT_ROOT 是否正确？）";

    EXPECT_NE(source.find("noCullDesc.CullMode = D3D11_CULL_NONE;"), std::string::npos)
        << "M4 的 noCull 光栅化锚点变了；D3D12 侧的 CullModeForPass 分类必须同步";
    EXPECT_NE(source.find("skybox 与 tone map 共用"), std::string::npos)
        << "noCull 的适用范围注释变了；确认 D3D12 侧 ToneMap/Skybox 分类是否仍成立";

    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::ToneMap), D3D12_CULL_MODE_NONE);
    EXPECT_EQ(D12::CullModeForPass(D12::PassKind::Skybox), D3D12_CULL_MODE_NONE);
}

// PassKind 名唯一且稳定（进 PSO 调试名与 metadata；重名会让 PIX inventory 无法区分）。
//
// 边界用 `kPassKindCount` 而不是写死数字：新增一个 pass 却忘记把计数 +1 时，下面的
// "计数本身必须是 Unknown" 断言会失败——否则新 pass 会静默逃出重名检查
// （M5-08 审查见过的"清单/计数漏项"同型）。另一个方向（计数加多了）由"每一项都必须
// 有可读名"兜住。
TEST(PipelineKeyTests, PassKindNamesAreUnique)
{
    std::map<std::string, D12::PassKind> byName;
    for (std::uint32_t index = 0; index < D12::kPassKindCount; ++index)
    {
        const auto pass = static_cast<D12::PassKind>(index);
        const std::string name = D12::PassKindName(pass);
        EXPECT_NE(name, "Unknown") << "pass " << index << " 缺少可读名（kPassKindCount 是否漏 +1？）";
        EXPECT_TRUE(byName.emplace(name, pass).second) << "重名：" << name;
    }
    EXPECT_EQ(byName.size(), static_cast<std::size_t>(D12::kPassKindCount));
    EXPECT_EQ(std::string{D12::PassKindName(static_cast<D12::PassKind>(D12::kPassKindCount))}, "Unknown")
        << "kPassKindCount 之外的取值不得有可读名（否则枚举项比计数多）";
}
