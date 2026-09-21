// ============================================================================
// BenchmarkJson.cpp — BenchmarkRun 的确定性 JSON 序列化实现
// 里程碑：M7-01
// 关联：tools/benchmark/include/MiniEngine/Benchmark/BenchmarkJson.h
// ============================================================================

#include <MiniEngine/Benchmark/BenchmarkJson.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniEngine::Benchmark
{
namespace
{
class JsonWriter final
{
  public:
    std::string Take()
    {
        return std::move(m_out);
    }

    void BeginObject()
    {
        Separator();
        m_out += '{';
        m_first.emplace_back(true);
    }

    void EndObject()
    {
        m_out += '}';
        m_first.pop_back();
    }

    void BeginArray()
    {
        Separator();
        m_out += '[';
        m_first.emplace_back(true);
    }

    void EndArray()
    {
        m_out += ']';
        m_first.pop_back();
    }

    void Key(std::string_view name)
    {
        // 注意：这里不能再调用 String()——String 自己会写分隔符，会产出 "{,key" 一类
        // 非法 JSON（PowerShell 的 ConvertFrom-Json 直接拒绝）。两者共用转义实现。
        Separator();
        WriteEscaped(name);
        m_out += ':';
        m_afterKey = true;
    }

    void String(std::string_view text)
    {
        Separator();
        WriteEscaped(text);
    }

    void Bool(bool value)
    {
        Separator();
        m_out += value ? "true" : "false";
    }

    void Uint64(std::uint64_t value)
    {
        Separator();
        m_out += std::to_string(value);
    }

    void Double(double value)
    {
        Separator();
        if (!std::isfinite(value))
        {
            // JSON 没有 NaN/Inf。写入端必须显式失败，而不是产出不可复算的文本。
            throw std::invalid_argument("benchmark JSON cannot represent a non-finite number");
        }
        // 最短可往返表示：既不丢精度，也不产生 0.10000000000000001 一类噪声。
        // 输出与 locale 无关（to_chars 不做本地化）。
        char buffer[64];
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (converted.ec != std::errc{})
        {
            throw std::runtime_error("cannot format a benchmark double value");
        }
        m_out.append(buffer, converted.ptr);
    }

    // 采样数组每个元素独占一行，便于人工检查与逐行 diff。
    void Newline()
    {
        m_out += '\n';
    }

  private:
    void WriteEscaped(std::string_view text)
    {
        m_out += '"';
        for (const char raw : text)
        {
            const auto c = static_cast<unsigned char>(raw);
            switch (c)
            {
            case '"':
                m_out += "\\\"";
                break;
            case '\\':
                m_out += "\\\\";
                break;
            case '\n':
                m_out += "\\n";
                break;
            case '\r':
                m_out += "\\r";
                break;
            case '\t':
                m_out += "\\t";
                break;
            default:
                if (c < 32)
                {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    m_out += hex.str();
                }
                else
                {
                    m_out += raw;
                }
                break;
            }
        }
        m_out += '"';
    }

    void Separator()
    {
        if (m_afterKey)
        {
            m_afterKey = false;
            return;
        }
        if (!m_first.empty())
        {
            if (!m_first.back())
            {
                m_out += ',';
            }
            m_first.back() = false;
        }
    }

    std::string m_out;
    std::vector<bool> m_first;
    bool m_afterKey = false;
};

void WriteDistribution(JsonWriter& writer, std::string_view name, const Distribution& distribution)
{
    writer.Key(name);
    writer.BeginObject();
    writer.Key("median");
    writer.Double(distribution.median);
    writer.Key("mad");
    writer.Double(distribution.mad);
    writer.Key("p95");
    writer.Double(distribution.p95);
    writer.Key("p99");
    writer.Double(distribution.p99);
    writer.Key("maximum");
    writer.Double(distribution.maximum);
    writer.EndObject();
}

void WriteEnvironment(JsonWriter& writer, const EnvironmentManifest& environment)
{
    writer.Key("environment");
    writer.BeginObject();
    writer.Key("hostName");
    writer.String(environment.hostName);
    writer.Key("osVersion");
    writer.String(environment.osVersion);
    writer.Key("buildType");
    writer.String(environment.buildType);
    writer.Key("sourceCommit");
    writer.String(environment.sourceCommit);
    writer.Key("gpuAdapter");
    writer.String(environment.gpuAdapter);
    writer.Key("gpuDriver");
    writer.String(environment.gpuDriver);
    writer.Key("adapterLuid");
    writer.Uint64(environment.adapterLuid);
    writer.Key("displayMode");
    writer.String(environment.displayMode);
    writer.Key("debugLayer");
    writer.Bool(environment.debugLayer);
    writer.Key("gpuValidation");
    writer.Bool(environment.gpuValidation);
    writer.Key("warp");
    writer.Bool(environment.warp);
    writer.Key("captureToolActive");
    writer.Bool(environment.captureToolActive);
    writer.Key("tracyConnected");
    writer.Bool(environment.tracyConnected);
    writer.Key("capturedUtc");
    writer.String(environment.capturedUtc);
    writer.EndObject();
}

void WriteCorrectness(JsonWriter& writer, const CorrectnessEvidence& correctness)
{
    writer.Key("correctness");
    writer.BeginObject();
    writer.Key("status");
    writer.String(correctness.status);
    writer.Key("packetSequenceHash");
    writer.String(correctness.packetSequenceHash);
    writer.Key("graphHash");
    writer.String(correctness.graphHash);
    writer.Key("commandHash");
    writer.String(correctness.commandHash);
    writer.Key("screenshotHash");
    writer.String(correctness.screenshotHash);
    writer.Key("visibleCount");
    writer.Uint64(correctness.visibleCount);
    writer.Key("drawCount");
    writer.Uint64(correctness.drawCount);
    writer.Key("trianglesSubmitted");
    writer.Uint64(correctness.trianglesSubmitted);
    writer.Key("validationMessages");
    writer.Uint64(correctness.validationMessages);
    // M7-08（v4）：布局内核语义 hash（未做布局实验时为空串，不填占位值）。
    writer.Key("layoutSemanticHash");
    writer.String(correctness.layoutSemanticHash);
    writer.EndObject();
}

void WriteStatistics(JsonWriter& writer, const RunStatistics& statistics)
{
    writer.Key("statistics");
    writer.BeginObject();
    writer.Key("sampleCount");
    writer.Uint64(statistics.sampleCount);
    WriteDistribution(writer, "cpuFrameMs", statistics.cpuFrameMs);
    WriteDistribution(writer, "gpuFrameMs", statistics.gpuFrameMs);
    WriteDistribution(writer, "fixedUpdateMs", statistics.fixedUpdateMs);
    WriteDistribution(writer, "worldExtractMs", statistics.worldExtractMs);
    WriteDistribution(writer, "packetBuildMs", statistics.packetBuildMs);
    WriteDistribution(writer, "cullMs", statistics.cullMs);
    WriteDistribution(writer, "packetMergeMs", statistics.packetMergeMs);
    WriteDistribution(writer, "packetWaitMs", statistics.packetWaitMs);
    WriteDistribution(writer, "packetSortMs", statistics.packetSortMs);
    WriteDistribution(writer, "layoutCullMs", statistics.layoutCullMs);
    WriteDistribution(writer, "renderGraphBuildMs", statistics.renderGraphBuildMs);
    WriteDistribution(writer, "renderGraphCompileMs", statistics.renderGraphCompileMs);
    WriteDistribution(writer, "rhiSubmitMs", statistics.rhiSubmitMs);
    WriteDistribution(writer, "presentMs", statistics.presentMs);
    WriteDistribution(writer, "visibleCount", statistics.visibleCount);
    WriteDistribution(writer, "drawCount", statistics.drawCount);
    WriteDistribution(writer, "residentBytes", statistics.residentBytes);
    WriteDistribution(writer, "allocationCount", statistics.allocationCount);
    writer.Key("hitches");
    writer.BeginObject();
    writer.Key("hitch16_67");
    writer.Uint64(statistics.hitches.over16_67);
    writer.Key("hitch33_33");
    writer.Uint64(statistics.hitches.over33_33);
    writer.Key("hitch50");
    writer.Uint64(statistics.hitches.over50);
    writer.EndObject();
    writer.Key("activeWorkers");
    writer.Uint64(statistics.activeWorkers);
    writer.Key("totalUploadsCommitted");
    writer.Uint64(statistics.totalUploadsCommitted);
    writer.Key("totalStealAttempts");
    writer.Uint64(statistics.totalStealAttempts);
    writer.Key("totalStealSuccesses");
    writer.Uint64(statistics.totalStealSuccesses);
    // M7-07（v3）：上传预算与提交/退休证据。
    writer.Key("totalUploadsStarted");
    writer.Uint64(statistics.totalUploadsStarted);
    writer.Key("totalUploadsFailed");
    writer.Uint64(statistics.totalUploadsFailed);
    writer.Key("totalReloadCommits");
    writer.Uint64(statistics.totalReloadCommits);
    writer.Key("totalDeferredRetires");
    writer.Uint64(statistics.totalDeferredRetires);
    writer.Key("totalFairnessHolds");
    writer.Uint64(statistics.totalFairnessHolds);
    writer.Key("totalAgingPromotions");
    writer.Uint64(statistics.totalAgingPromotions);
    writer.Key("totalUploadEvents");
    writer.Uint64(statistics.totalUploadEvents);
    writer.Key("uploadBytesEstimated");
    writer.Uint64(statistics.uploadBytesEstimated);
    writer.Key("uploadBytesActual");
    writer.Uint64(statistics.uploadBytesActual);
    writer.Key("uploadEstimatedErrorBytes");
    writer.Uint64(statistics.uploadEstimatedErrorBytes);
    writer.Key("residentUploadBytes");
    writer.Uint64(statistics.residentUploadBytes);
    writer.Key("uploadBytesReleased");
    writer.Uint64(statistics.uploadBytesReleased);
    writer.Key("cancelWasteBytes");
    writer.Uint64(statistics.cancelWasteBytes);
    writer.Key("uploadPendingHighWater");
    writer.Uint64(statistics.uploadPendingHighWater);
    writer.Key("uploadInFlightHighWater");
    writer.Uint64(statistics.uploadInFlightHighWater);
    // M7-08（v4）：布局内核的端到端校验证据。
    writer.Key("layoutCheckFrames");
    writer.Uint64(statistics.layoutCheckFrames);
    writer.Key("layoutCheckMismatches");
    writer.Uint64(statistics.layoutCheckMismatches);
    writer.EndObject();
}

void WriteSamples(JsonWriter& writer, const std::vector<FrameSample>& samples)
{
    writer.Key("samples");
    writer.BeginArray();
    for (const FrameSample& sample : samples)
    {
        writer.Newline();
        writer.BeginObject();
        writer.Key("frameSerial");
        writer.Uint64(sample.frameSerial);
        writer.Key("cpuFrameMs");
        writer.Double(sample.cpuFrameMs);
        writer.Key("gpuFrameMs");
        writer.Double(sample.gpuFrameMs);
        writer.Key("fixedUpdateMs");
        writer.Double(sample.fixedUpdateMs);
        writer.Key("worldExtractMs");
        writer.Double(sample.worldExtractMs);
        writer.Key("packetBuildMs");
        writer.Double(sample.packetBuildMs);
        writer.Key("cullMs");
        writer.Double(sample.cullMs);
        writer.Key("packetMergeMs");
        writer.Double(sample.packetMergeMs);
        writer.Key("packetWaitMs");
        writer.Double(sample.packetWaitMs);
        writer.Key("packetSortMs");
        writer.Double(sample.packetSortMs);
        writer.Key("renderGraphBuildMs");
        writer.Double(sample.renderGraphBuildMs);
        writer.Key("renderGraphCompileMs");
        writer.Double(sample.renderGraphCompileMs);
        writer.Key("rhiSubmitMs");
        writer.Double(sample.rhiSubmitMs);
        writer.Key("presentMs");
        writer.Double(sample.presentMs);
        writer.Key("ioReadMs");
        writer.Double(sample.ioReadMs);
        writer.Key("decodeMs");
        writer.Double(sample.decodeMs);
        writer.Key("uploadMs");
        writer.Double(sample.uploadMs);
        writer.Key("taskQueueDepth");
        writer.Uint64(sample.taskQueueDepth);
        writer.Key("activeWorkers");
        writer.Uint64(sample.activeWorkers);
        writer.Key("stealAttempts");
        writer.Uint64(sample.stealAttempts);
        writer.Key("stealSuccesses");
        writer.Uint64(sample.stealSuccesses);
        writer.Key("uploadQueueBytes");
        writer.Uint64(sample.uploadQueueBytes);
        writer.Key("uploadsCommitted");
        writer.Uint64(sample.uploadsCommitted);
        writer.Key("uploadBytes");
        writer.Uint64(sample.uploadBytes);
        writer.Key("uploadPendingCount");
        writer.Uint64(sample.uploadPendingCount);
        writer.Key("uploadDeferredRetires");
        writer.Uint64(sample.uploadDeferredRetires);
        writer.Key("layoutCullMs");
        writer.Double(sample.layoutCullMs);
        writer.Key("allocationCount");
        writer.Uint64(sample.allocationCount);
        writer.Key("allocatedBytes");
        writer.Uint64(sample.allocatedBytes);
        writer.Key("residentBytes");
        writer.Uint64(sample.residentBytes);
        writer.Key("visibleCount");
        writer.Uint64(sample.visibleCount);
        writer.Key("drawCount");
        writer.Uint64(sample.drawCount);
        writer.EndObject();
    }
    writer.Newline();
    writer.EndArray();
}

void WriteAssetRequests(JsonWriter& writer, const std::vector<AssetRequestSample>& requests)
{
    writer.Key("assetRequests");
    writer.BeginArray();
    for (const AssetRequestSample& request : requests)
    {
        writer.Newline();
        writer.BeginObject();
        writer.Key("requestId");
        writer.Uint64(request.requestId);
        writer.Key("assetId");
        writer.Uint64(request.assetId);
        writer.Key("revision");
        writer.Uint64(request.revision);
        writer.Key("bytes");
        writer.Uint64(request.bytes);
        writer.Key("requestFrame");
        writer.Uint64(request.requestFrame);
        writer.Key("priority");
        writer.Uint64(request.priority);
        writer.Key("queueToReadMs");
        writer.Double(request.queueToReadMs);
        writer.Key("readMs");
        writer.Double(request.readMs);
        writer.Key("queueToDecodeMs");
        writer.Double(request.queueToDecodeMs);
        writer.Key("decodeMs");
        writer.Double(request.decodeMs);
        writer.Key("queueToUploadMs");
        writer.Double(request.queueToUploadMs);
        writer.Key("uploadToReadyMs");
        writer.Double(request.uploadToReadyMs);
        writer.Key("requestToReadyMs");
        writer.Double(request.requestToReadyMs);
        writer.Key("result");
        writer.String(request.result);
        writer.EndObject();
    }
    writer.Newline();
    writer.EndArray();
}
} // namespace

std::string SerializeRun(const BenchmarkRun& run)
{
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("schemaVersion");
    writer.Uint64(run.schemaVersion);
    writer.Key("experimentId");
    writer.String(run.experimentId);
    writer.Key("variant");
    writer.String(run.variant);
    writer.Key("sceneName");
    writer.String(run.sceneName);
    writer.Key("sceneManifestPath");
    writer.String(run.sceneManifestPath);
    writer.Key("sceneManifestSha256");
    writer.String(run.sceneManifestSha256);
    writer.Key("cameraPathSha256");
    writer.String(run.cameraPathSha256);
    writer.Key("streamScriptSha256");
    writer.String(run.streamScriptSha256);
    writer.Key("machineManifestPath");
    writer.String(run.machineManifestPath);
    writer.Key("machineManifestSha256");
    writer.String(run.machineManifestSha256);
    writer.Key("sourceCommit");
    writer.String(run.sourceCommit);
    writer.Key("executablePath");
    writer.String(run.executablePath);
    writer.Key("executableSha256");
    writer.String(run.executableSha256);
    writer.Key("rhi");
    writer.String(run.rhi);
    writer.Key("workers");
    writer.Uint64(run.workers);
    writer.Key("chunkSize");
    writer.Uint64(run.chunkSize);
    writer.Key("packetBuildMode");
    writer.String(run.packetBuildMode);
    writer.Key("schedulerMode");
    writer.String(run.schedulerMode);
    writer.Key("chunkReserve");
    writer.Bool(run.chunkReserve);
    writer.Key("foregroundGate");
    writer.Bool(run.foregroundGate);
    writer.Key("seed");
    writer.Uint64(run.seed);
    writer.Key("runIndex");
    writer.Uint64(run.runIndex);
    writer.Key("width");
    writer.Uint64(run.width);
    writer.Key("height");
    writer.Uint64(run.height);
    writer.Key("renderDrawLimit");
    writer.Uint64(run.renderDrawLimit);
    writer.Key("shadowDrawLimit");
    writer.Uint64(run.shadowDrawLimit);
    writer.Key("vsync");
    writer.Bool(run.vsync);
    writer.Key("warmupFrames");
    writer.Uint64(run.warmupFrames);
    writer.Key("measuredFrames");
    writer.Uint64(run.measuredFrames);
    writer.Key("uploadMiBPerFrame");
    writer.Uint64(run.uploadMiBPerFrame);
    writer.Key("uploadBudgetCpuMs");
    writer.Double(run.uploadBudgetCpuMs);
    writer.Key("uploadBudgetRequests");
    writer.Uint64(run.uploadBudgetRequests);
    writer.Key("uploadBudgetReloadPercent");
    writer.Uint64(run.uploadBudgetReloadPercent);
    writer.Key("uploadAgingThresholdMs");
    writer.Double(run.uploadAgingThresholdMs);
    writer.Key("layoutVariant");
    writer.String(run.layoutVariant);
    writer.Key("loaderMode");
    writer.String(run.loaderMode);
    writer.Key("runOrderNote");
    writer.String(run.runOrderNote);
    writer.Key("defaultsUsed");
    writer.String(run.defaultsUsed);
    writer.Key("unavailableMetrics");
    writer.BeginArray();
    for (const std::string& metric : run.unavailableMetrics)
    {
        writer.String(metric);
    }
    writer.EndArray();
    WriteEnvironment(writer, run.environment);
    WriteCorrectness(writer, run.correctness);
    WriteStatistics(writer, run.statistics);
    WriteSamples(writer, run.samples);
    WriteAssetRequests(writer, run.assetRequests);
    writer.EndObject();
    std::string result = writer.Take();
    result += '\n';
    return result;
}
} // namespace MiniEngine::Benchmark
