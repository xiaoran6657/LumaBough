// ============================================================================
// BenchmarkSchema.h — M7 性能实验机读契约（raw JSON 的唯一字段来源）
// 里程碑：M7-01（性能契约与串行基线）
// 职责：冻结 FrameSample / AssetRequestSample / BenchmarkRun 等字段。raw JSON 由
//       BenchmarkJson 按固定字段顺序序列化；compare_m7_results.ps1 与后续实验账本
//       只按本文件的字段名读取。新增指标必须先改本文件，不能在写入端私加字段。
// 关联：docs/architecture/README.md（第 4 步指标清单）
//       docs/architecture/README.md（raw schema 与控制变量）
// ============================================================================

#pragma once

#include <MiniEngine/Benchmark/Statistics.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Benchmark
{
// 当前 schema 版本。字段语义变化（改名、改单位、改含义）必须提升版本并同步
// compare_m7_results.ps1 的校验；纯新增可选字段也要提升，避免旧解析器静默读错。
// v2（M7-05）：新增 packetBuildMode/schedulerMode（控制变量）与 packetWaitMs（并行等待段）。
// v1 解析器把缺失的 packetBuildMode 视为 "serial"、schedulerMode 视为 "none"、packetWaitMs 视为 0。
// v3（M7-07）：新增上传预算控制变量（uploadBudgetCpuMs/Requests/ReloadPercent）与上传证据
// （FrameSample.uploadBytes/uploadPendingCount/uploadDeferredRetires、RunStatistics 的
// totalUploadsStarted/Failed、reload/retire/fairness/aging 计数、估计-实际字节与误差、
// residentUploadBytes/uploadBytesReleased、cancelWasteBytes）。v2 解析器把缺失值视为
// "未观测"（0），但 loaderMode != async-* 时这些字段本来就为空。
// v4（M7-08）：新增数据布局实验的控制变量与证据：BenchmarkRun.layoutVariant、
// FrameSample.layoutCullMs（同帧内布局内核的扫描耗时）、CorrectnessEvidence.layoutSemanticHash、
// RunStatistics.layoutCheckFrames/layoutCheckMismatches。v3 解析器把缺失的 layoutVariant
// 视为 "aos"、layoutCullMs 视为 0（未观测）。
inline constexpr std::uint32_t kSchemaVersion = 4;

// 单帧 raw 采样。时间单位统一为毫秒（double），计数为本帧累计值。
// 未被本场景观测的字段保持 0，并在 BenchmarkRun::unavailableMetrics 中显式列出，
// 禁止用 0 冒充"已测量且为零"。
struct FrameSample final
{
    std::uint32_t frameSerial = 0;     // 场景内 1 基帧号（含 warmup 段）
    double cpuFrameMs = 0.0;           // CPU 帧总时长（主循环入口到 Present 返回）
    double gpuFrameMs = 0.0;           // GPU timestamp 对（不可用时保持 0 并列入 unavailable）
    double fixedUpdateMs = 0.0;        // 固定步长更新（World/场景状态推进）
    double worldExtractMs = 0.0;       // UpdateTransforms + BuildRenderItems（冻结快照前）
    double packetBuildMs = 0.0;        // BuildRenderPacket/BuildParallel 全量（含 cull + 合并 + 排序 + 装配）
    double cullMs = 0.0;               // 视锥分类 + 逐项提取（RenderPacket 构建的 culling 段；并行时是各 chunk 之和）
    double packetMergeMs = 0.0;        // chunk 合并；串行 P0 无 chunk，保持 0 并列入 unavailable
    double packetWaitMs = 0.0;         // 并行 Wait 段（含 main-help 执行）；串行保持 0 并列入 unavailable
    double packetSortMs = 0.0;         // 稳定排序段（并行时只统计合并后的全局排序）
    double renderGraphBuildMs = 0.0;   // 图声明（Declare）
    double renderGraphCompileMs = 0.0; // 编译（Compile）
    double rhiSubmitMs = 0.0;          // 图执行 + 提交（Execute + EndFrame 之前的命令录制）
    double presentMs = 0.0;            // Present 返回耗时（D3D12 视为提交完成时点）
    double ioReadMs = 0.0;             // 本帧阻塞读取耗时（m7-streaming 串行基线）
    double decodeMs = 0.0;             // 本帧 cooked 载荷校验/解码耗时
    double uploadMs = 0.0;             // 本帧 GPU 上传提交耗时
    std::uint32_t taskQueueDepth = 0;
    std::uint32_t activeWorkers = 0; // 含主线程在内的实际执行者数量
    std::uint64_t stealAttempts = 0;
    std::uint64_t stealSuccesses = 0;
    std::uint64_t uploadQueueBytes = 0;
    std::uint64_t uploadsCommitted = 0;
    // M7-07（v3）：本帧实际上传字节、上传队列里等待的候选数、本帧进入延迟退休的资源数。
    std::uint64_t uploadBytes = 0;
    std::uint32_t uploadPendingCount = 0;
    std::uint32_t uploadDeferredRetires = 0;
    // M7-08（v4）：同帧内布局内核（AoS/hot-cold/SoA）的提取 + 分类扫描耗时。
    // 生产 packet 构建路径不变，因此这是"单变量对照"的端到端口径。
    double layoutCullMs = 0.0;
    std::uint64_t allocationCount = 0;
    std::uint64_t allocatedBytes = 0;
    std::uint64_t residentBytes = 0; // 帧结束时仍驻留的渲染器内存（RHI 池高水位口径见场景说明）
    std::uint32_t visibleCount = 0;
    std::uint32_t drawCount = 0;
};

// 单个资产请求的完整时序（m7-streaming）。串行基线中读取/解码都在请求帧内完成，
// queueTo* 字段为 0 并列入 unavailableMetrics；M7-06 起由真实队列填充。
struct AssetRequestSample final
{
    std::uint64_t requestId = 0;
    std::uint64_t assetId = 0;
    std::uint64_t revision = 0;
    std::uint64_t bytes = 0;
    std::uint32_t requestFrame = 0; // 脚本规定的请求帧号
    std::uint32_t priority = 0;     // 0=Critical 1=Visible 2=Prefetch（与脚本一致）
    double queueToReadMs = 0.0;
    double readMs = 0.0;
    double queueToDecodeMs = 0.0;
    double decodeMs = 0.0;
    double queueToUploadMs = 0.0;
    double uploadToReadyMs = 0.0;
    double requestToReadyMs = 0.0;
    std::string result; // "ready" / "failed" / "cancelled" / ...
};

// 运行期可自证的环境信息（由 sandbox 自己观测）。机器级信息（CPU/内存/电源计划等）
// 由 tools/performance/capture_m7_environment.ps1 单独采集为环境 manifest，并在 BenchmarkRun 中
// 以 machineManifestPath/Sha256 引用，避免每份 raw JSON 重复且互相漂移。
struct EnvironmentManifest final
{
    std::string hostName;
    std::string osVersion;
    std::string buildType;    // Debug / Release / RelWithDebInfo
    std::string sourceCommit; // 组合根编译期注入的 commit；工作区可能 dirty
    std::string gpuAdapter;   // RHI Capabilities().adapterName
    std::string gpuDriver;    // RHI Capabilities().driverVersion
    std::uint64_t adapterLuid = 0;
    std::string displayMode; // "1920x1080"
    bool debugLayer = false;
    bool gpuValidation = false;
    bool warp = false;
    bool captureToolActive = false; // RenderDoc/PIX 注入检测；性能 run 必须为 false
    bool tracyConnected = false;
    std::string capturedUtc; // ISO-8601 UTC
};

// 正确性证据。任何一项缺失都不得把 run 视为可用于性能结论（compare 脚本强制 PASS）。
struct CorrectnessEvidence final
{
    std::string status; // "PASS" / "FAIL"
    std::string packetSequenceHash;
    std::string graphHash;
    std::string commandHash;
    std::string screenshotHash; // 未采集时为空串（不得填占位值）
    std::uint32_t visibleCount = 0;
    std::uint32_t drawCount = 0;
    std::uint64_t trianglesSubmitted = 0;
    std::uint32_t validationMessages = 0; // Debug Layer / GBV 消息数；Profile 构建应为 0
    // M7-08（v4）：布局内核与生产 packet 的等价性证据（跑过校验的帧与不一致计数），
    // 以及布局内核自己的语义 hash（hex 字符串，未做布局实验时为空串）。
    std::uint32_t layoutCheckFrames = 0;
    std::uint32_t layoutCheckMismatches = 0;
    std::string layoutSemanticHash;
};

// 聚合统计（由 Benchmark::ComputeStatistics 从 samples 重算，禁止手填）。
struct RunStatistics final
{
    std::uint32_t sampleCount = 0;
    Distribution cpuFrameMs;
    Distribution gpuFrameMs;
    Distribution fixedUpdateMs;
    Distribution worldExtractMs;
    Distribution packetBuildMs;
    Distribution cullMs;
    Distribution packetMergeMs;
    Distribution packetWaitMs;
    Distribution packetSortMs;
    // M7-08（v4）：布局内核扫描耗时的整轮分布（未做布局实验时全 0 并列入 unavailableMetrics）。
    Distribution layoutCullMs;
    Distribution renderGraphBuildMs;
    Distribution renderGraphCompileMs;
    Distribution rhiSubmitMs;
    Distribution presentMs;
    Distribution visibleCount;
    Distribution drawCount;
    Distribution residentBytes;
    Distribution allocationCount;
    HitchCounts hitches;
    std::uint32_t activeWorkers = 0;
    std::uint64_t totalUploadsCommitted = 0;
    std::uint64_t totalStealAttempts = 0;
    std::uint64_t totalStealSuccesses = 0;
    // M7-08（v4）：布局内核的端到端校验证据（校验帧数 + 不一致数；非布局场景为 0）。
    std::uint32_t layoutCheckFrames = 0;
    std::uint32_t layoutCheckMismatches = 0;
    // M7-07（v3）：上传预算/优先级/提交/退休的整轮证据。非 async loaderMode 时保持 0。
    std::uint64_t totalUploadsStarted = 0;
    std::uint64_t totalUploadsFailed = 0;
    std::uint64_t totalReloadCommits = 0;
    std::uint64_t totalDeferredRetires = 0;
    std::uint64_t totalFairnessHolds = 0;
    std::uint64_t totalAgingPromotions = 0;
    std::uint64_t totalUploadEvents = 0;
    std::uint64_t uploadBytesEstimated = 0;
    std::uint64_t uploadBytesActual = 0;
    std::uint64_t uploadEstimatedErrorBytes = 0;
    std::uint64_t residentUploadBytes = 0;
    std::uint64_t uploadBytesReleased = 0;
    std::uint64_t cancelWasteBytes = 0;
    std::uint32_t uploadPendingHighWater = 0;
    std::uint32_t uploadInFlightHighWater = 0;
};

// 一次基准运行的完整记录。控制变量字段必须全部来自运行期实际解析值：缺省值不允许
// 静默生效，缺省即报错（见 sandbox 的 M7 scene 参数校验）。
struct BenchmarkRun final
{
    std::uint32_t schemaVersion = kSchemaVersion;
    std::string experimentId; // 例如 "E-M7-BASELINE"
    std::string variant;      // "baseline" / "candidate"
    std::string sceneName;    // "m7-cpu-scale" / "m7-layout" / "m7-streaming"
    std::string sceneManifestPath;
    std::string sceneManifestSha256;
    std::string cameraPathSha256;
    std::string streamScriptSha256; // 非流式场景为空串
    std::string machineManifestPath;
    std::string machineManifestSha256;
    std::string sourceCommit;
    std::string executablePath;
    std::string executableSha256;
    std::string rhi; // "d3d11" / "d3d12"
    std::uint32_t workers = 1;
    std::uint32_t chunkSize = 256;
    // M7-05 控制变量：并行 RenderPacket 构建的选择（"serial"/"parallel"）与调度器
    // （"none"/"global"/"per-worker"）。串行 P0 为 serial + none。
    std::string packetBuildMode;
    std::string schedulerMode;
    // E-M7-RP-004：chunk 输出 reserve 开关；串行模式恒为 true（无 chunk）。
    bool chunkReserve = true;
    // 前台 gate 是否启用（自动化会话可显式关闭；实际 foregroundRatio 始终记录在
    // run notes）。关闭时 raw JSON 保留 false，读数字必须带这个前提。
    bool foregroundGate = true;
    std::uint32_t seed = 0;
    std::uint32_t runIndex = 0; // 1 基；同一 variant 内的重复采集序号
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 场景定义的一部分：culling/统计/hash 覆盖全部代理，实际提交给渲染器的 draw 取
    // 稳定排序后的前 N 个。两个数字必须与 evidence 一起读，否则会把"渲染上限"误读成
    // "可见数量"。0 表示不截断。
    std::uint32_t renderDrawLimit = 0;
    std::uint32_t shadowDrawLimit = 0;
    bool vsync = false;
    std::uint32_t warmupFrames = 0;
    std::uint32_t measuredFrames = 0;
    std::uint32_t uploadMiBPerFrame = 0; // 控制变量；M7-07 起生效（maxUploadBytesPerFrame）
    // M7-07 控制变量（v3）：上传预算的另外两条约束与热重载份额/aging 阈值。
    double uploadBudgetCpuMs = 0.0;              // maxUploadCpuMsPerFrame
    std::uint32_t uploadBudgetRequests = 0;      // maxUploadsPerFrame
    std::uint32_t uploadBudgetReloadPercent = 0; // 热重载可用的字节份额
    double uploadAgingThresholdMs = 0.0;         // 低优先级 aging 阈值
    std::string loaderMode;                      // "serial-sync-read-validate" / "async-budget-pipeline"
    // M7-08 控制变量（v4）：每帧同帧运行的布局内核（"aos" / "hot-cold" / "soa" / "soa-batched"）。
    // "soa-batched" 是 M7-LAYOUT-SIMD 的批量化变体（块级 early-out），结果与 "soa" 逐字节相同。
    // 生产 packet 构建路径不随它变化，因此跨布局比较是单变量对照。
    std::string layoutVariant = "aos";
    std::string runOrderNote;                    // A/B 交错顺序等人工记录
    std::string defaultsUsed;                    // 逗号分隔的已使用默认项；空串表示全部显式给出
    std::vector<std::string> unavailableMetrics; // 本场景未观测的 schema 字段
    EnvironmentManifest environment;
    CorrectnessEvidence correctness;
    RunStatistics statistics;
    std::vector<FrameSample> samples;
    std::vector<AssetRequestSample> assetRequests;
};

// 从 measured 段 samples 重算 statistics；samples 为空时抛 std::invalid_argument。
[[nodiscard]] RunStatistics ComputeStatistics(const std::vector<FrameSample>& samples);

// 运行前/写盘前的自检：schemaVersion、身份字段、样本数、控制变量、有限值、
// correctness.status == "PASS"。失败返回 false 并写入 error（第一个问题）。
[[nodiscard]] bool Validate(const BenchmarkRun& run, std::string& error);
} // namespace MiniEngine::Benchmark
