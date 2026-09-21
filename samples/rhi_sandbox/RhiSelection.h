#pragma once
#include <MiniEngine/Rhi/RhiFactory.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>

namespace MiniEngine::Sandbox
{
// E5：不再把构建机的绝对工程路径编进二进制（二进制字符串审查的整改）。
// 开发机需要覆盖时用环境变量 M610_PROJECT_ROOT；未设置时返回空路径（即按当前工作目录解析）。
inline std::filesystem::path ProjectRootFallback()
{
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t size = 0;
    std::filesystem::path value;
    if (_dupenv_s(&buffer, &size, "M610_PROJECT_ROOT") == 0 && buffer != nullptr && *buffer != '\0')
        value = std::filesystem::path(buffer);
    std::free(buffer);
    return value;
#else
    if (const char* value = std::getenv("M610_PROJECT_ROOT"); value != nullptr && *value != '\0')
        return std::filesystem::path(value);
    return {};
#endif
}

struct RhiLaunchOptions final
{
    Rhi::RhiBackend backend = Rhi::RhiBackend::D3D12;
    std::string backendSource = "default";
    std::string scene = "m6-adapter-smoke";
    std::string outputDirectory;
    std::string renderer = "rhi";
    std::string manifest;
    std::string pixCapture;
    std::string renderdocCapture;
    std::uint32_t migrationLevel = 9;
    std::uint32_t debugView = 0;
    std::uint32_t fixedFrame = 300;
    std::uint32_t diagnosticsInterval = 300;
    std::uint32_t warmup = 120, measure = 600;
    float exposure = 0;
    bool benchmark = false;
    bool exerciseChanges = false;
    // ---- D 批次：连续 Demo 的可读性控制（只在 --exercise-changes 下生效）----
    // tourHoldFrames：每个 tour 事件之后保持该状态的帧数；0 = 沿用 C 批次的历史节奏
    // （挂起后立即重建，无停留）。停留只改变"状态持续多久"，不改变事件本身。
    // tourTemporaryWidth/Height：临时 extent；0 = 沿用历史的 width+16 / height+8。
    std::uint32_t tourHoldFrames = 0;
    std::uint32_t tourTemporaryWidth = 0;
    std::uint32_t tourTemporaryHeight = 0;
    std::uint32_t frames = 0;
    std::uint32_t width = 320;
    std::uint32_t height = 240;
    std::uint32_t smokeLevel = 3;
    bool headless = false;
    bool debug = false;
    bool gpuValidation = false;
    bool warp = false;
    bool help = false;

    // ---- M7 性能场景（m7-cpu-scale / m7-layout / m7-streaming）----
    // 约定：控制变量在 benchmark 模式下必须显式给出（缺省即报错），运行期把实际
    // 解析值写进 raw JSON；`seedSpecified` 等标志供这里的显式性校验使用。
    std::string metricsPath;
    std::string machineManifest;
    std::string experimentId = "E-M7-BASELINE";
    std::string variant = "baseline";
    std::string runOrderNote; // A/B 交错顺序记录（M7-09；写进 raw JSON 的 runOrderNote）
    // M7-10（A27）：长跑稳定性。soakTelemetryEvery > 0 时每 N 帧采样
    // 句柄/驻留对象/待办/常驻内存；stabilityStressEvery > 0 时每 N 帧触发
    // 零尺寸挂起与不同 extent（resize 路径）；loaderReloadEvery > 0 时每 N 帧
    // 对已提交资产请求新 revision（端到端热重载，需要 --async-assets=on）。
    std::uint32_t soakTelemetryEvery = 0;
    std::uint32_t stabilityStressEvery = 0;
    std::uint32_t loaderReloadEvery = 0;
    // M7-RESIZE-PATH：--exercise-resize 让稳定性压力（stabilityStressEvery）在压力轮换里
    // **真实改变窗口客户区尺寸**（SetWindowPos → WM_SIZE → 跟随窗口 extent 重建交换链 /
    // 清瞬态池 / 重派生 viewport 与截图回读），覆盖"拖动改变窗口尺寸 + streaming"的路径。
    // benchmark 下禁止：M7 性能场景是固定 extent 的测量契约（可变 extent 会让帧时间口径漂移）。
    bool exerciseResize = false;
    // M7-DEVICE-LOST-INJECT：--inject-device-lost-after=N 在第 N 个渲染帧（warmup 之后）
    // 注入一次设备移除（仅 D3D12 支持；D3D11 响亮失败）。约束与 --exercise-resize 同族：
    // 非 benchmark、需要真实窗口；注入后**不得**继续正常渲染——下一帧的 RHI 调用必须响亮
    // 失败并由顶层捕获，进程以非零退出码结束（证据实验 E-M7-DEVICE-LOST-001）。
    // 0 = 不注入（默认）。明确不做：设备丢失后的恢复/重建（独立实验）。
    std::uint32_t injectDeviceLostAfter = 0;
    std::string cameraPath;
    std::string streamScript;
    std::uint32_t workers = 1;
    std::uint32_t chunkSize = 256;
    // M7-05 并行 RenderPacket 构建：serial（M7-01 基线路径）或 parallel（chunk 划分 +
    // TaskSystem）；scheduler 只在 parallel 下有效（global=v0 / per-worker=v1 窃取）。
    std::string packetBuild = "serial";
    std::string scheduler = "none";
    // E-M7-RP-004：chunk 输出是否按估计可见率 reserve（只在 parallel 下有效）。
    bool chunkReserve = true;
    // 前台 gate：M7 契约要求 benchmark 在保持前台的窗口上运行（后台会被 DWM 节流）。
    // 自动化/无人值守会话无法稳定保持前台，可显式关闭；实际比例始终记录在 run notes，
    // 关闭状态写入 raw JSON（foregroundGate=false），性能结论必须带着这个前提解读。
    bool foregroundGate = true;
    std::uint32_t seed = 0;
    std::uint32_t entityCount = 0;
    std::uint32_t uploadMiBPerFrame = 0;
    // ---- M7-07 异步上传流水线（m7-streaming）----
    // asyncAssets=false 时走 M7-01 的串行基线（阻塞读 + 内容哈希校验）；
    // true 时走 I/O 线程 + decode task + 预算上传（loaderMode=async-budget-pipeline）。
    bool asyncAssets = false;
    double uploadBudgetCpuMs = 0.5;               // 每帧 render thread 上传 CPU 预算
    std::uint32_t uploadBudgetRequests = 16;      // 每帧上传请求数预算
    std::uint32_t uploadBudgetReloadPercent = 50; // 热重载字节份额
    double uploadAgingMs = 50.0;                  // 低优先级 aging 阈值
    bool asyncAssetsSpecified = false;
    // ---- M7-08 数据布局实验（m7-cpu-scale / m7-layout）----
    // 每帧同帧运行指定布局的剔除内核（aos/hot-cold/soa）；生产 packet 构建路径不变，
    // 因此跨布局比较是单变量对照，生产侧可见集合/统计仍是判定基准。
    std::string layout = "aos";
    bool layoutSpecified = false;
    // 是否真的运行布局内核（--layout=none 时为 false：显式声明"不做布局实验"，
    // 不改变帧时间口径，供 M7-05/M7-07 一类旧链路复跑）。
    bool layoutEnabled = false;
    std::uint32_t runIndex = 1;
    float visibleRatio = 0.5F;
    float updateRatio = 1.0F;
    bool vsync = false;
    // ---- M7-02 观测（仅影响 profiling，不改变场景控制变量）----
    bool profileDetail = true;            // --profile-detail=full|coarse
    std::uint32_t tracyWaitConnectMs = 0; // --tracy-wait-connect=MS：预热后等待 profiler 连接
    std::uint32_t tracyCaptureFrames = 0; // --tracy-capture-frames=N：连接后采集的帧数（0=全部测量帧）
    bool seedSpecified = false;
    bool workersSpecified = false;
    bool chunkSizeSpecified = false;
    bool packetBuildSpecified = false;
    bool schedulerSpecified = false;
    bool chunkReserveSpecified = false;
    bool foregroundGateSpecified = false;
    bool vsyncSpecified = false;
    bool warmupSpecified = false;
    bool measureSpecified = false;
    bool metricsSpecified = false;
    bool entityCountSpecified = false;
    bool machineManifestSpecified = false;
    bool widthSpecified = false;
    bool heightSpecified = false;
};
RhiLaunchOptions ParseRhiOptions(std::span<const std::string_view> arguments);
std::string RhiUsage();
} // namespace MiniEngine::Sandbox
