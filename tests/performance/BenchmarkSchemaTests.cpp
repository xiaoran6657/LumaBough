// ============================================================================
// BenchmarkSchemaTests.cpp — raw JSON 契约与自检回归
// 里程碑：M7-01
// 职责：锁定 BenchmarkRun 的控制变量、正确性门槛与序列化可复算性；防止"少写字段
//       但看起来仍然 PASS"的 raw 证据（M7-A06）。
// 关联：tools/benchmark/include/MiniEngine/Benchmark/BenchmarkSchema.h
// ============================================================================

#include <MiniEngine/Benchmark/BenchmarkJson.h>
#include <MiniEngine/Benchmark/BenchmarkSchema.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
using namespace MiniEngine::Benchmark;

BenchmarkRun MakeValidRun()
{
    BenchmarkRun run;
    run.experimentId = "E-M7-BASELINE";
    run.variant = "baseline";
    run.sceneName = "m7-cpu-scale";
    run.sceneManifestPath = "assets/recipes/m7-performance-scenes.json";
    run.sceneManifestSha256 = "aa";
    run.cameraPathSha256 = "bb";
    run.machineManifestPath = "tests/baselines/m7/environment.json";
    run.machineManifestSha256 = "cc";
    run.sourceCommit = "deadbeef";
    run.executablePath = "out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe";
    run.executableSha256 = "dd";
    run.rhi = "d3d12";
    run.workers = 1;
    run.chunkSize = 256;
    run.packetBuildMode = "serial";
    run.schedulerMode = "none";
    run.seed = 6657;
    run.runIndex = 1;
    run.width = 1920;
    run.height = 1080;
    run.vsync = false;
    run.warmupFrames = 300;
    run.measuredFrames = 2;
    run.loaderMode = "serial-sync";
    run.correctness.status = "PASS";
    run.correctness.packetSequenceHash = "0x1";
    run.correctness.graphHash = "0x2";
    run.correctness.commandHash = "0x3";

    FrameSample first;
    first.frameSerial = 1;
    first.cpuFrameMs = 8.25;
    first.cullMs = 1.5;
    first.visibleCount = 25000;
    first.drawCount = 25000;
    first.activeWorkers = 1;
    FrameSample second = first;
    second.frameSerial = 2;
    second.cpuFrameMs = 9.75;
    run.samples = {first, second};
    run.statistics = ComputeStatistics(run.samples);

    AssetRequestSample request;
    request.requestId = 1;
    request.assetId = 42;
    request.revision = 1;
    request.bytes = 4096;
    request.requestFrame = 10;
    request.priority = 1;
    request.readMs = 0.4;
    request.decodeMs = 0.2;
    request.requestToReadyMs = 0.6;
    request.result = "ready";
    run.assetRequests = {request};
    return run;
}

TEST(M7BenchmarkSchema, ValidateAcceptsCompleteRun)
{
    const BenchmarkRun run = MakeValidRun();
    std::string error;
    EXPECT_TRUE(Validate(run, error)) << error;
}

TEST(M7BenchmarkSchema, ValidateRejectsSampleCountMismatch)
{
    BenchmarkRun run = MakeValidRun();
    run.measuredFrames = 3;
    std::string error;
    EXPECT_FALSE(Validate(run, error));
    EXPECT_NE(error.find("measuredFrames"), std::string::npos);
}

TEST(M7BenchmarkSchema, ValidateRejectsMissingControlVariables)
{
    BenchmarkRun run = MakeValidRun();
    run.sceneManifestSha256.clear();
    std::string error;
    EXPECT_FALSE(Validate(run, error));

    BenchmarkRun missingMode = MakeValidRun();
    missingMode.loaderMode.clear();
    EXPECT_FALSE(Validate(missingMode, error));

    // M7-05（schema v2）：packetBuildMode/schedulerMode 与 loaderMode 同级的控制变量。
    BenchmarkRun missingPacketMode = MakeValidRun();
    missingPacketMode.packetBuildMode.clear();
    EXPECT_FALSE(Validate(missingPacketMode, error));

    BenchmarkRun missingScheduler = MakeValidRun();
    missingScheduler.schedulerMode.clear();
    EXPECT_FALSE(Validate(missingScheduler, error));

    // chunkReserve 只在 parallel 下有效：串行 run 不允许声明 reserve=off。
    BenchmarkRun serialWithoutReserve = MakeValidRun();
    serialWithoutReserve.chunkReserve = false;
    EXPECT_FALSE(Validate(serialWithoutReserve, error));
    BenchmarkRun parallelWithoutReserve = MakeValidRun();
    parallelWithoutReserve.packetBuildMode = "parallel";
    parallelWithoutReserve.schedulerMode = "per-worker";
    parallelWithoutReserve.chunkReserve = false;
    EXPECT_TRUE(Validate(parallelWithoutReserve, error)) << error;
}

TEST(M7BenchmarkSchema, ValidateRejectsNonPositiveOrNonFiniteFrames)
{
    BenchmarkRun zero = MakeValidRun();
    zero.samples[0].cpuFrameMs = 0.0;
    std::string error;
    EXPECT_FALSE(Validate(zero, error));

    BenchmarkRun negative = MakeValidRun();
    negative.samples[1].cpuFrameMs = -1.0;
    EXPECT_FALSE(Validate(negative, error));
}

TEST(M7BenchmarkSchema, ValidateRejectsFailedCorrectness)
{
    BenchmarkRun run = MakeValidRun();
    run.correctness.status = "FAIL";
    std::string error;
    EXPECT_FALSE(Validate(run, error));
}

TEST(M7BenchmarkSchema, SerializeIsDeterministicAndComplete)
{
    const BenchmarkRun run = MakeValidRun();
    const std::string first = SerializeRun(run);
    const std::string second = SerializeRun(run);
    EXPECT_EQ(first, second);

    for (const char* key :
         {"\"schemaVersion\":4", "\"experimentId\":\"E-M7-BASELINE\"", "\"rhi\":\"d3d12\"",
          "\"packetBuildMode\":\"serial\"", "\"schedulerMode\":\"none\"", "\"chunkReserve\":true",
          "\"foregroundGate\":true", "\"packetWaitMs\":", "\"statistics\"", "\"hitch16_67\"", "\"samples\"",
          "\"assetRequests\"", "\"correctness\"", "\"unavailableMetrics\"", "\"cpuFrameMs\":8.25",
          // v3（M7-07）：上传预算控制变量与上传证据字段必须出现在 raw JSON 里。
          "\"uploadBudgetCpuMs\":", "\"uploadBudgetRequests\":", "\"uploadBudgetReloadPercent\":",
          "\"uploadAgingThresholdMs\":", "\"totalUploadsStarted\":", "\"totalReloadCommits\":",
          "\"uploadBytesActual\":", "\"uploadPendingHighWater\":", "\"uploadBytes\":", "\"uploadPendingCount\":",
          "\"uploadDeferredRetires\":",
          // v4（M7-08）：布局实验的控制变量与证据字段。
          "\"layoutVariant\":", "\"layoutCullMs\":", "\"layoutSemanticHash\":", "\"layoutCheckFrames\":",
          "\"layoutCheckMismatches\":"})
    {
        EXPECT_NE(first.find(key), std::string::npos) << "missing " << key;
    }
}

TEST(M7BenchmarkSchema, SerializeEscapesTextAndRejectsNonFinite)
{
    BenchmarkRun run = MakeValidRun();
    run.variant = "candidate \"quoted\"";
    const std::string text = SerializeRun(run);
    EXPECT_NE(text.find("candidate \\\"quoted\\\""), std::string::npos);

    BenchmarkRun notFinite = MakeValidRun();
    notFinite.samples[0].cpuFrameMs = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(static_cast<void>(SerializeRun(notFinite)), std::invalid_argument);
}

TEST(M7BenchmarkSchema, SerializeProducesStructurallyValidJson)
{
    // 回归：字段分隔符曾经重复（"{,key" / "key:1,,"），PowerShell ConvertFrom-Json
    // 直接拒绝整份证据。这里先做廉价的结构检查，再由 tests/performance 的 Python
    // 用例用真正的 JSON 解析器复核（MINIENGINE_M7_SCHEMA_OUT 钩子）。
    const BenchmarkRun run = MakeValidRun();
    const std::string text = SerializeRun(run);
    for (const char* pattern : {",,", "{,", "[,", ",}", ",]"})
    {
        EXPECT_EQ(text.find(pattern), std::string::npos) << "invalid JSON sequence: " << pattern;
    }
    // 钩子：Python 校验用例（tests/tools/contracts/test_m7_schema.py）在同一次运行里取走这份 JSON。
    // MSVC 上不用 std::getenv（C4996），改用 _dupenv_s。
    std::string hookPath;
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "MINIENGINE_M7_SCHEMA_OUT") == 0 && buffer != nullptr)
        hookPath.assign(buffer);
    std::free(buffer);
#else
    if (const char* value = std::getenv("MINIENGINE_M7_SCHEMA_OUT"))
        hookPath.assign(value);
#endif
    if (!hookPath.empty())
    {
        std::ofstream out(hookPath, std::ios::binary);
        ASSERT_TRUE(out.good()) << "cannot write " << hookPath;
        out << text;
    }
}

TEST(M7BenchmarkSchema, SerializeKeepsUnavailableMetricsExplicit)
{
    BenchmarkRun run = MakeValidRun();
    run.unavailableMetrics = {"packetMergeMs", "ioReadMs"};
    const std::string text = SerializeRun(run);
    EXPECT_NE(text.find("\"unavailableMetrics\":[\"packetMergeMs\",\"ioReadMs\"]"), std::string::npos);
}
} // namespace
