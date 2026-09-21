#include "../../samples/rhi_sandbox/RhiSelection.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
using namespace MiniEngine::Rhi;
using namespace MiniEngine::Sandbox;
namespace
{
TEST(RhiSelection, DefaultIsExplicitlyRecordedD3D12)
{
    const auto options = ParseRhiOptions({});
    EXPECT_EQ(options.backend, RhiBackend::D3D12);
    EXPECT_EQ(options.backendSource, "default");
    EXPECT_FALSE(options.warp);
}
TEST(RhiSelection, AcceptsOnlyExactLowercaseBackendNames)
{
    EXPECT_EQ(ParseRhiBackend("d3d11"), RhiBackend::D3D11);
    EXPECT_EQ(ParseRhiBackend("d3d12"), RhiBackend::D3D12);
    for (auto invalid : {"", "invalid", "D3D12", "D3D11", " d3d12", "d3d12 ", "vulkan"})
    {
        try
        {
            (void)ParseRhiBackend(invalid);
            FAIL();
        }
        catch (const RhiException& error)
        {
            EXPECT_EQ(error.Error().code, RhiErrorCode::InvalidArgument);
            EXPECT_NE(std::string(error.what()).find("d3d11, d3d12"), std::string::npos);
        }
    }
}
TEST(RhiSelection, ExplicitSelectionAndDiagnosticsAreNotFallback)
{
    const std::array<std::string_view, 5> args{"--rhi=d3d11", "--headless", "--frames", "7", "--warp"};
    const auto options = ParseRhiOptions(args);
    EXPECT_EQ(options.backend, RhiBackend::D3D11);
    EXPECT_EQ(options.backendSource, "explicit");
    EXPECT_EQ(options.frames, 7);
    EXPECT_TRUE(options.warp);
}
TEST(RhiSelection, RejectsMissingDuplicateAndMalformedArguments)
{
    for (auto args : {std::vector<std::string_view>{"--rhi"},
                      {"--rhi=d3d11", "--rhi=d3d12"},
                      {"--frames=-1"},
                      {"--frames=0"},
                      {"--frames=2junk"},
                      {"--frames=999999999999"},
                      {"--smoke-level=7"},
                      {"--headless=true"},
                      {"--unknown=value"},
                      {"--output="}})
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        EXPECT_THROW((void)ParseRhiOptions(args), std::exception);
    }
}

TEST(RhiSelection, MigrationSelectorsPreserveFrozenSceneDefaults)
{
    for (auto renderer : {"rhi"})
    {
        const std::string selection = std::string("--renderer=") + renderer;
        const std::array<std::string_view, 1> args{selection};
        const auto options = ParseRhiOptions(args);
        EXPECT_EQ(options.renderer, renderer);
        EXPECT_EQ(options.scene, "m4-visual-baseline");
        EXPECT_EQ(options.width, 1280);
        EXPECT_EQ(options.height, 720);
        EXPECT_EQ(options.frames, 0);
        EXPECT_EQ(options.fixedFrame, 300);
    }
}
TEST(RhiSelection, BenchmarkAndReloadCannotSilentlyRunSmokeScene)
{
    const auto benchmark = ParseRhiOptions(std::array<std::string_view, 1>{"--benchmark"});
    EXPECT_EQ(benchmark.scene, "m4-visual-baseline");
    EXPECT_EQ(benchmark.warmup, 120);
    EXPECT_EQ(benchmark.measure, 600);
    const auto changes = ParseRhiOptions(std::array<std::string_view, 1>{"--exercise-changes"});
    EXPECT_EQ(changes.migrationLevel, 9);
    EXPECT_EQ(changes.scene, "m4-visual-baseline");
    for (auto args : {std::vector<std::string_view>{"--renderer=legacy-d3d11", "--rhi=d3d11"},
                      {"--renderer=legacy-d3d11", "--headless"},
                      {"--renderer=legacy-d3d12", "--headless"},
                      {"--scene=m6-adapter-smoke", "--migration-level=4"},
                      {"--migration-level=0"},
                      {"--migration-level=10"},
                      {"--benchmark", "--debug"},
                      {"--benchmark", "--gbv"},
                      {"--benchmark", "--renderdoc-capture=x.rdc"},
                      {"--rhi=d3d11", "--renderdoc-capture=x.rdc", "--pix-capture=x.wpix"},
                      {"--rhi=d3d12", "--renderdoc-capture=x.rdc"},
                      {"--benchmark", "--frames=300"},
                      {"--benchmark", "--exercise-changes"},
                      {"--exercise-changes", "--migration-level=8"},
                      {"--exercise-changes", "--frames=3"},
                      {"--scene=m4-visual-baseline", "--smoke-level=3"},
                      {"--debug-view=11"},
                      {"--pix-capture="},
                      {"--renderdoc-capture="},
                      {"--scene=m6-adapter-smoke", "--renderdoc-capture=x.rdc", "--rhi=d3d11"},
                      {"--manifest="},
                      {"--exposure=nan"},
                      {"--exposure=inf"},
                      {"--exposure=1junk"}})
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        EXPECT_THROW((void)ParseRhiOptions(args), std::exception);
    }
}
TEST(RhiSelection, DiagnosticsIntervalSelectsMigrationSceneAndRejectsMalformedValues)
{
    EXPECT_EQ(ParseRhiOptions({}).diagnosticsInterval, 300U);
    const auto options = ParseRhiOptions(std::array<std::string_view, 1>{"--diagnostics-interval=300"});
    EXPECT_EQ(options.scene, "m4-visual-baseline");
    EXPECT_EQ(options.diagnosticsInterval, 300U);
    for (auto args : {std::vector<std::string_view>{"--diagnostics-interval=0"},
                      {"--diagnostics-interval=1000001"},
                      {"--diagnostics-interval=junk"},
                      {"--scene=m6-adapter-smoke", "--diagnostics-interval=300"}})
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        EXPECT_THROW((void)ParseRhiOptions(args), std::exception);
    }
}
// M6 审计 C2：被接受的选项不得静默无效或静默优先。
TEST(RhiSelection, BenchmarkOnlyOptionsAreRejectedOutsideBenchmarkMode)
{
    for (auto args :
         {std::vector<std::string_view>{"--warmup=300"}, {"--measure=300"}, {"--warmup=300", "--measure=600"}})
    {
        SCOPED_TRACE(::testing::PrintToString(args));
        EXPECT_THROW((void)ParseRhiOptions(args), std::exception);
    }
    const std::array<std::string_view, 3> benchmark{"--benchmark", "--warmup=100", "--measure=50"};
    const auto options = ParseRhiOptions(benchmark);
    EXPECT_TRUE(options.benchmark);
    EXPECT_EQ(options.warmup, 100U);
    EXPECT_EQ(options.measure, 50U);
}
TEST(RhiSelection, FrameCountAndFixedFrameAreMutuallyExclusive)
{
    const std::array<std::string_view, 3> both{"--frames=600", "--fixed-frame=300", "--headless"};
    EXPECT_THROW((void)ParseRhiOptions(both), std::exception);
    const std::array<std::string_view, 2> fixedOnly{"--fixed-frame=300", "--headless"};
    EXPECT_EQ(ParseRhiOptions(fixedOnly).fixedFrame, 300U);
    const std::array<std::string_view, 2> framesOnly{"--frames=600", "--headless"};
    EXPECT_EQ(ParseRhiOptions(framesOnly).frames, 600U);
}
// M6 审计 B4：计划文档里的幽灵场景名必须带正确调用被拒绝，usage 不再宣传它。
TEST(RhiSelection, PassMigrationSceneNameIsRejectedWithMigrationLevelHint)
{
    try
    {
        (void)ParseRhiOptions(std::array<std::string_view, 1>{"--scene=m6-pass-migration"});
        FAIL() << "m6-pass-migration 没有独立行为，必须被拒绝";
    }
    catch (const std::invalid_argument& error)
    {
        const std::string message{error.what()};
        EXPECT_NE(message.find("m4-visual-baseline"), std::string::npos) << message;
        EXPECT_NE(message.find("migration-level"), std::string::npos) << message;
    }
    EXPECT_EQ(RhiUsage().find("m6-pass-migration"), std::string::npos);
    EXPECT_NE(RhiUsage().find("--warmup"), std::string::npos);
}
// M7-12（A25/A26）：PIX 的 pixtool launch 无法表达多参数命令行（值不允许空格与等号），
// 运行器因此接受把整套参数打包成单个 token 的形态；RenderDoc/其他启动器仍用普通 argv。
TEST(RhiSelection, AcceptsPackedSingleTokenFromArgLimitedLaunchers)
{
    // 打包形态里值必须写成 `--key=value`（拆分只发生在选项边界，值里的空格要保留）。
    for (auto packed :
         {std::string_view{"--rhi=d3d11 --headless --frames=7"}, std::string_view{"--rhi=d3d11,--headless,--frames=7"},
          std::string_view{"\"--rhi=d3d11 --headless --frames=7\""}})
    {
        SCOPED_TRACE(::testing::PrintToString(packed));
        const std::array<std::string_view, 1> args{packed};
        const auto options = ParseRhiOptions(args);
        EXPECT_EQ(options.backend, RhiBackend::D3D11);
        EXPECT_TRUE(options.headless);
        EXPECT_EQ(options.frames, 7U);
    }
}
// 打包形态只在选项边界拆分：值里的空格必须保留；启动器补的空参数要丢弃；
// '=' 与 '--' 缺一不可的错误形态仍要报错。
TEST(RhiSelection, PackedTokenKeepsValueSpacesAndDropsEmptyArguments)
{
    const std::array<std::string_view, 2> args{"--run-order-note=seq=1/2 variant=base run=1", ""};
    EXPECT_EQ(ParseRhiOptions(args).runOrderNote, "seq=1/2 variant=base run=1");
    const std::array<std::string_view, 2> broken{"--headless,--frames", ""};
    EXPECT_THROW((void)ParseRhiOptions(broken), std::exception);
    // 打包形态里"空格分隔的值"不支持（无法与含空格的值区分）：必须报错而不是静默取默认。
    const std::array<std::string_view, 1> spaced{"--rhi=d3d11 --headless --frames 7"};
    EXPECT_THROW((void)ParseRhiOptions(spaced), std::exception);
}
// 响应文件：`@path` 按空白展开（pixtool 连 '=' 都不接受时用它整串传参）。
TEST(RhiSelection, ResponseFileExpandsArgumentsAndReportsMissingOrEmptyFile)
{
    const auto path = std::filesystem::temp_directory_path() / "me-rhi-selection-response.txt";
    {
        std::ofstream file(path, std::ios::binary);
        file << "--rhi=d3d11\r\n--headless\r\n--frames=5\r\n";
    }
    const std::string token = "@" + path.string();
    const std::array<std::string_view, 1> args{token};
    const auto options = ParseRhiOptions(args);
    EXPECT_EQ(options.backend, RhiBackend::D3D11);
    EXPECT_TRUE(options.headless);
    EXPECT_EQ(options.frames, 5U);

    const std::string missing = "@" + (std::filesystem::temp_directory_path() / "me-does-not-exist.txt").string();
    const std::array<std::string_view, 1> missingArgs{missing};
    EXPECT_THROW((void)ParseRhiOptions(missingArgs), std::invalid_argument);

    {
        std::ofstream empty(path, std::ios::binary | std::ios::trunc);
    }
    EXPECT_THROW((void)ParseRhiOptions(args), std::invalid_argument);
    std::filesystem::remove(path);
}
} // namespace
