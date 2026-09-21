#include "RhiSelection.h"
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace MiniEngine::Sandbox
{
namespace
{
std::uint32_t Number(std::string_view value, const std::string& name, std::uint32_t maximum)
{
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0 || result > maximum)
        throw std::invalid_argument(name + " requires a positive integer <= " + std::to_string(maximum));
    return result;
}
// M7 控制变量允许 0（seed / run-index / entity-count 语义上可以是 0）。
std::uint32_t WholeNumber(std::string_view value, const std::string& name, std::uint32_t maximum)
{
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > maximum)
        throw std::invalid_argument(name + " requires an integer in [0, " + std::to_string(maximum) + "]");
    return result;
}
// M7 比例参数：限定 [0, 1] 且必须是有限值。
float Ratio(std::string_view value, const std::string& name)
{
    float result = 0.0F;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result) ||
        result < 0.0F || result > 1.0F)
        throw std::invalid_argument(name + " requires a finite ratio in [0, 1]");
    return result;
}
// M7-07 预算参数：允许 0（表示"该条约束不限制"），但必须是有限非负值。
double Decimal(std::string_view value, const std::string& name, const double maximum)
{
    double result = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result) ||
        result < 0.0 || result > maximum)
        throw std::invalid_argument(name + " requires a finite value in [0, " + std::to_string(maximum) + "]");
    return result;
}
bool OnOff(std::string_view value, const std::string& name)
{
    if (value == "on" || value == "true")
        return true;
    if (value == "off" || value == "false")
        return false;
    throw std::invalid_argument(name + " requires on|off");
}
} // namespace
std::string RhiUsage()
{
    return "MiniEngineSandbox [--rhi=d3d11|d3d12] [--renderer=rhi] "
           "[--scene=m6-adapter-smoke|m4-visual-baseline] "
           "[--migration-level=1..9] [--manifest=path] [--debug-view=0..10] [--fixed-frame=N] "
           "[--benchmark --warmup=N --measure=N] [--diagnostics-interval=N] [--exercise-changes] "
           "[--smoke-level=1..6] [--frames=N] [--width=N] [--height=N] "
           "[--debug] [--gbv] [--warp] [--headless] [--output=directory]\n"
           "Default backend: d3d12. Backend names are lowercase. No automatic fallback. "
           "--warmup/--measure are benchmark-only; --frames and --fixed-frame are mutually exclusive; "
           "--exercise-changes performs a programmatic minimize/resize/reload at the mid frame "
           "(combine with --headless for capture runs).\n"
           "Demo readability (--exercise-changes only): [--tour-hold-frames=N] "
           "[--tour-temporary-width=N --tour-temporary-height=N] [--vsync=on|off] — "
           "they only change how long a state is held and which temporary extent is used; "
           "the event sequence and its checks are unchanged.\n"
           "M7 performance scenes: [--scene=m7-cpu-scale|m7-layout|m7-streaming] "
           "[--benchmark --warmup-frames=300 --measure-frames=1800 --vsync=off --seed=N --workers=1 "
           "--chunk-size=256 --packet-build=serial|parallel --scheduler=global|per-worker "
           "[--chunk-reserve=on|off] [--foreground-gate=on|off] "
           "--metrics=run.json --machine-manifest=environment.json] "
           "[--entity-count=N --visible-ratio=0..1 --update-ratio=0..1] "
           "[--upload-mib-per-frame=N] [--camera=path] [--stream-script=path] "
           "[--experiment-id=ID] [--variant=name] [--run-index=N] [--run-order-note=text] [--frames=N] "
           "[--layout=aos|hot-cold|soa|soa-batched|none] "
           "[--soak-telemetry-every=N --stability-stress-every=N --loader-reload-every=N] "
           "[--exercise-resize] [--inject-device-lost-after=N] "
           "[--profile-detail=full|coarse] [--tracy-wait-connect=MS] [--tracy-capture-frames=N]\n"
           "M7 benchmark runs require every control variable to be explicit and reject silent defaults.\n"
           "--exercise-resize makes the stability stress really resize the window client area "
           "(WM_SIZE -> swapchain follow + viewport/readback re-derivation); non-benchmark only.\n"
           "--inject-device-lost-after=N removes the device (D3D12 only) after N rendered frames; "
           "the next RHI call must fail loudly (non-benchmark, real window).\n";
}
RhiLaunchOptions ParseRhiOptions(std::span<const std::string_view> args)
{
    RhiLaunchOptions result;
    std::set<std::string> seen;
    // M7-12：接受"打包成单个 token"的命令行形态——某些启动器（PIX 的 pixtool launch
    // --command-line）无法表达多参数命令行，只能把整串参数当一个 argv 传进来，甚至
    // 不允许值里出现空格。分隔符取两种：`" --"`（空格，便于人工书写）与 `",--"`
    // （逗号，空格受限的启动器用；分号不行——它是 pixtool 自己的命令分隔符）。
    // 拆分只发生在选项边界，因此单个选项的值**可以**含空格
    // （例如 --run-order-note=seq=3/10 variant=baseline run=1），不会误拆。
    static constexpr std::string_view kSeparators[] = {" --", ",--"};
    const auto findBoundary = [](std::string_view text)
    {
        std::size_t boundary = std::string_view::npos;
        for (const auto separator : kSeparators)
        {
            const auto found = text.find(separator);
            if (found != std::string_view::npos && (boundary == std::string_view::npos || found < boundary))
                boundary = found;
        }
        return boundary;
    };
    // 响应文件：token 形如 `@path`，文件按空白切分成参数（M7-12）。空格受限的启动器
    // （PIX pixtool 的 --command-line 不允许空格与等号）可以用 `@args.txt` 传整套参数。
    std::vector<std::string> owned;
    owned.reserve(args.size());
    for (const auto argument : args)
    {
        if (argument.empty() || argument.front() != '@')
        {
            owned.emplace_back(argument);
            continue;
        }
        const std::string path(argument.substr(1));
        if (path.empty())
            throw std::invalid_argument("response file token must be @path");
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::invalid_argument("response file not readable: " + path);
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::istringstream stream(content);
        std::string piece;
        std::size_t expanded = 0;
        while (stream >> piece)
        {
            // 文件可能来自 Windows 工具（CRLF）：显式去掉尾部 '\r'，避免它粘在
            // 数值/枚举值的尾部导致 "requires a positive integer" 这类误报。
            while (!piece.empty() && (piece.back() == '\r' || piece.back() == '\n'))
                piece.pop_back();
            if (piece.empty())
                continue;
            owned.push_back(piece);
            ++expanded;
        }
        if (expanded == 0)
            throw std::invalid_argument("response file is empty: " + path);
    }
    // 拆分结果必须先落进**拥有型**容器：token 视图只在 owned/split 都不再变动后才建立，
    // 否则视图会指向每轮迭代即销毁的局部副本（M7-12 实测到的悬空样本）。
    std::vector<std::string> split;
    split.reserve(owned.size());
    for (const auto& rawArgument : owned)
    {
        // 某些启动器（pixtool 的 --command-line 要求值带字面引号）会把引号原样带进
        // argv：这里剥离一层首尾引号再拆分，避免整串参数被判成非法位置参数。
        auto argument = rawArgument;
        if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"')
            argument = argument.substr(1, argument.size() - 2);
        std::string_view remaining = argument;
        while (true)
        {
            const auto boundary = findBoundary(remaining);
            if (boundary == std::string_view::npos)
            {
                // 空 token 直接丢弃：某些启动器会给子进程补一个空参数，它不携带信息，
                // 但会被解析器判成非法位置参数（M7-12 实测）。
                if (!remaining.empty())
                    split.emplace_back(remaining);
                break;
            }
            if (boundary > 0)
                split.emplace_back(remaining.substr(0, boundary));
            remaining = remaining.substr(boundary + 1);
        }
    }
    std::vector<std::string_view> tokens;
    tokens.reserve(split.size());
    for (const auto& token : split)
        tokens.push_back(token);
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        auto arg = tokens[i];
        if (!arg.starts_with("--"))
            throw std::invalid_argument("unexpected positional argument: " + std::string(arg));
        const auto separator = arg.find('=');
        const std::string key(arg.substr(2, separator == std::string_view::npos ? separator : separator - 2));
        if (!seen.insert(key).second)
            throw std::invalid_argument("duplicate option: --" + key);
        const bool flag = key == "debug" || key == "gbv" || key == "headless" || key == "warp" || key == "help" ||
                          key == "benchmark" || key == "exercise-changes" || key == "exercise-resize";
        std::string_view value;
        if (flag)
        {
            if (separator != std::string_view::npos)
                throw std::invalid_argument("flag does not take a value: --" + key);
        }
        else if (separator != std::string_view::npos)
            value = arg.substr(separator + 1);
        else
        {
            if (i + 1 == tokens.size() || tokens[i + 1].starts_with("--"))
                throw std::invalid_argument("missing value for --" + key);
            value = tokens[++i];
        }
        if (key == "rhi")
        {
            result.backend = Rhi::ParseRhiBackend(value);
            result.backendSource = "explicit";
        }
        else if (key == "renderer")
        {
            if (value.starts_with("legacy-"))
                throw std::invalid_argument("legacy A/B renderer retired (LEGACY-SAMPLES-RETIRE, 2026-09-16); use "
                                            "--renderer=rhi and git history");
            if (value != "rhi")
                throw std::invalid_argument("renderer must be rhi");
            result.renderer = value;
        }
        else if (key == "manifest")
        {
            if (value.empty())
                throw std::invalid_argument("manifest path must not be empty");
            result.manifest = value;
        }
        else if (key == "pix-capture")
        {
            if (value.empty())
                throw std::invalid_argument("PIX capture path must not be empty");
            result.pixCapture = value;
        }
        else if (key == "renderdoc-capture")
        {
            if (value.empty())
                throw std::invalid_argument("RenderDoc capture path must not be empty");
            result.renderdocCapture = value;
        }
        else if (key == "migration-level")
            result.migrationLevel = Number(value, key, 9);
        else if (key == "fixed-frame")
            result.fixedFrame = Number(value, key, 1000000);
        else if (key == "warmup")
        {
            result.warmup = Number(value, key, 1000000);
            result.warmupSpecified = true;
        }
        else if (key == "measure")
        {
            result.measure = Number(value, key, 1000000);
            result.measureSpecified = true;
        }
        else if (key == "warmup-frames")
        {
            result.warmup = Number(value, key, 1000000);
            result.warmupSpecified = true;
        }
        else if (key == "measure-frames")
        {
            result.measure = Number(value, key, 1000000);
            result.measureSpecified = true;
        }
        else if (key == "debug-view")
            result.debugView = value == "0" ? 0 : Number(value, key, 10);
        else if (key == "diagnostics-interval")
            result.diagnosticsInterval = Number(value, key, 1000000);
        else if (key == "exposure")
        {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.exposure);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                !std::isfinite(result.exposure) || std::abs(result.exposure) > 20)
                throw std::invalid_argument("invalid exposure");
        }
        else if (key == "benchmark")
            result.benchmark = true;
        else if (key == "exercise-changes")
            result.exerciseChanges = true;
        else if (key == "exercise-resize")
            result.exerciseResize = true;
        else if (key == "tour-hold-frames")
            result.tourHoldFrames = Number(value, key, 1000000);
        else if (key == "tour-temporary-width")
            result.tourTemporaryWidth = Number(value, key, 7680);
        else if (key == "tour-temporary-height")
            result.tourTemporaryHeight = Number(value, key, 7680);
        else if (key == "inject-device-lost-after")
            result.injectDeviceLostAfter = Number(value, key, 1000000);
        else if (key == "scene")
            result.scene = value;
        else if (key == "frames")
            result.frames = Number(value, key, 1000000);
        else if (key == "width")
        {
            result.width = Number(value, key, 16384);
            result.widthSpecified = true;
        }
        else if (key == "height")
        {
            result.height = Number(value, key, 16384);
            result.heightSpecified = true;
        }
        else if (key == "smoke-level")
            result.smokeLevel = Number(value, key, 6);
        else if (key == "output")
        {
            if (value.empty())
                throw std::invalid_argument("output directory must not be empty");
            result.outputDirectory = value;
        }
        else if (key == "metrics")
        {
            if (value.empty())
                throw std::invalid_argument("metrics path must not be empty");
            result.metricsPath = value;
            result.metricsSpecified = true;
        }
        else if (key == "machine-manifest")
        {
            if (value.empty())
                throw std::invalid_argument("machine manifest path must not be empty");
            result.machineManifest = value;
            result.machineManifestSpecified = true;
        }
        else if (key == "experiment-id")
        {
            if (value.empty())
                throw std::invalid_argument("experiment id must not be empty");
            result.experimentId = value;
        }
        else if (key == "variant")
        {
            if (value.empty())
                throw std::invalid_argument("variant must not be empty");
            result.variant = value;
        }
        else if (key == "soak-telemetry-every")
            result.soakTelemetryEvery = Number(value, key, 1000000);
        else if (key == "stability-stress-every")
            result.stabilityStressEvery = Number(value, key, 1000000);
        else if (key == "loader-reload-every")
            result.loaderReloadEvery = Number(value, key, 1000000);
        else if (key == "run-order-note")
        {
            // M7-09：A/B 交错采集的顺序记录（例如 "seq=3/10 variant=baseline run=1"）。
            // 只做人工可追溯，不参与任何判定；raw JSON 的 runOrderNote 字段承载它。
            if (value.empty())
                throw std::invalid_argument("run order note must not be empty");
            result.runOrderNote = value;
        }
        else if (key == "run-index")
            result.runIndex = Number(value, key, 1000000);
        else if (key == "workers")
        {
            result.workers = Number(value, key, 256);
            result.workersSpecified = true;
        }
        else if (key == "chunk-size")
        {
            result.chunkSize = Number(value, key, 4096);
            result.chunkSizeSpecified = true;
        }
        else if (key == "packet-build")
        {
            if (value != "serial" && value != "parallel")
                throw std::invalid_argument("--packet-build requires serial|parallel");
            result.packetBuild = value;
            result.packetBuildSpecified = true;
        }
        else if (key == "scheduler")
        {
            if (value != "global" && value != "per-worker")
                throw std::invalid_argument("--scheduler requires global|per-worker");
            result.scheduler = value;
            result.schedulerSpecified = true;
        }
        else if (key == "chunk-reserve")
        {
            result.chunkReserve = OnOff(value, key);
            result.chunkReserveSpecified = true;
        }
        else if (key == "foreground-gate")
        {
            result.foregroundGate = OnOff(value, key);
            result.foregroundGateSpecified = true;
        }
        else if (key == "seed")
        {
            result.seed = WholeNumber(value, key, 1000000000);
            result.seedSpecified = true;
        }
        else if (key == "entity-count")
        {
            result.entityCount = Number(value, key, 1000000);
            result.entityCountSpecified = true;
        }
        else if (key == "upload-mib-per-frame")
            result.uploadMiBPerFrame = WholeNumber(value, key, 4096);
        else if (key == "async-assets")
        {
            result.asyncAssets = OnOff(value, key);
            result.asyncAssetsSpecified = true;
        }
        else if (key == "upload-budget-cpu-ms")
            result.uploadBudgetCpuMs = Decimal(value, key, 1000.0);
        else if (key == "upload-budget-requests")
            result.uploadBudgetRequests = WholeNumber(value, key, 4096);
        else if (key == "upload-budget-reload-percent")
            result.uploadBudgetReloadPercent = WholeNumber(value, key, 100);
        else if (key == "upload-aging-ms")
            result.uploadAgingMs = Decimal(value, key, 60000.0);
        else if (key == "layout")
        {
            // M7-09：`none` = 显式声明"本次不做布局实验"（不运行布局内核，保持 M7-05/M7-07
            // 采集的帧时间口径可比；raw JSON 记 layoutVariant="none" 并把 layoutCullMs
            // 列入 unavailableMetrics）。aos/hot-cold/soa 才会真的跑布局内核。
            if (value != "aos" && value != "hot-cold" && value != "soa" && value != "soa-batched" &&
                value != "none")
                throw std::invalid_argument("--layout requires aos|hot-cold|soa|soa-batched|none");
            result.layout = value;
            result.layoutEnabled = (value != "none");
            result.layoutSpecified = true;
        }
        else if (key == "visible-ratio")
            result.visibleRatio = Ratio(value, key);
        else if (key == "update-ratio")
            result.updateRatio = Ratio(value, key);
        else if (key == "vsync")
        {
            result.vsync = OnOff(value, key);
            result.vsyncSpecified = true;
        }
        else if (key == "profile-detail")
        {
            if (value == "full")
                result.profileDetail = true;
            else if (value == "coarse")
                result.profileDetail = false;
            else
                throw std::invalid_argument("--profile-detail requires full|coarse");
        }
        else if (key == "tracy-wait-connect")
            result.tracyWaitConnectMs = WholeNumber(value, key, 600000);
        else if (key == "tracy-capture-frames")
            result.tracyCaptureFrames = WholeNumber(value, key, 1000000);
        else if (key == "camera")
        {
            if (value.empty())
                throw std::invalid_argument("camera path must not be empty");
            result.cameraPath = value;
        }
        else if (key == "stream-script")
        {
            if (value.empty())
                throw std::invalid_argument("stream script path must not be empty");
            result.streamScript = value;
        }
        else if (key == "debug")
            result.debug = true;
        else if (key == "gbv")
        {
            result.debug = true;
            result.gpuValidation = true;
        }
        else if (key == "headless")
            result.headless = true;
        else if (key == "warp")
            result.warp = true;
        else if (key == "help")
            result.help = true;
        else
            throw std::invalid_argument("unknown option: --" + key);
    }
    if (!seen.contains("scene") &&
        (seen.contains("migration-level") || seen.contains("renderer") || seen.contains("manifest") ||
         seen.contains("debug-view") || seen.contains("fixed-frame") || seen.contains("diagnostics-interval") ||
         !result.pixCapture.empty() || !result.renderdocCapture.empty() || result.benchmark || result.exerciseChanges))
        result.scene = "m4-visual-baseline";
    const bool m7Scene = result.scene.starts_with("m7-");
    if (result.scene != "m6-adapter-smoke")
    {
        // M7 场景按 recipe 冻结 1920×1080；其他迁移场景维持 M6 的 1280×720。
        const std::uint32_t defaultWidth = m7Scene ? 1920 : 1280;
        const std::uint32_t defaultHeight = m7Scene ? 1080 : 720;
        if (!seen.contains("width"))
            result.width = defaultWidth;
        if (!seen.contains("height"))
            result.height = defaultHeight;
    }
    if (result.scene == "m6-adapter-smoke" &&
        (seen.contains("migration-level") || result.benchmark || result.exerciseChanges || seen.contains("manifest") ||
         seen.contains("debug-view") || seen.contains("fixed-frame") || seen.contains("diagnostics-interval") ||
         !result.pixCapture.empty() || !result.renderdocCapture.empty()))
        throw std::invalid_argument("migration options require the migration scene");
    if (result.scene != "m6-adapter-smoke" && seen.contains("smoke-level"))
        throw std::invalid_argument("smoke-level is only valid for the adapter smoke scene");
    if (!m7Scene && (result.packetBuildSpecified || result.schedulerSpecified || result.foregroundGateSpecified))
        throw std::invalid_argument("--packet-build/--scheduler/--foreground-gate require an M7 performance scene");
    if (result.benchmark && (seen.contains("frames") || result.exerciseChanges))
        throw std::invalid_argument("benchmark frame count comes from warmup/measure and excludes resize/reload");
    // 静默忽略与静默优先级都是契约漏洞：显式拒绝，而不是让用户以为选项生效。
    if ((seen.contains("warmup") || seen.contains("measure") || seen.contains("warmup-frames") ||
         seen.contains("measure-frames")) &&
        !result.benchmark)
        throw std::invalid_argument("--warmup/--measure (--warmup-frames/--measure-frames) require --benchmark");
    if (seen.contains("frames") && seen.contains("fixed-frame"))
        throw std::invalid_argument("--frames and --fixed-frame are mutually exclusive");
    if (result.exerciseChanges && (result.frames ? result.frames : result.fixedFrame) < 6)
        throw std::invalid_argument("resize/reload exercise requires at least six frames");
    // 计划文档里的 m6-pass-migration 没有独立行为：迁移就是 m4-visual-baseline + level 9。
    // 显式拒绝并给出正确调用，避免"被接受但 metadata 仍报 m4-visual-baseline"的幽灵取值。
    if (result.scene == "m6-pass-migration")
        throw std::invalid_argument(
            "m6-pass-migration is not a distinct scene; use --scene=m4-visual-baseline --migration-level=9");
    if (result.scene != "m6-adapter-smoke" && result.scene != "m4-visual-baseline" && result.scene != "m7-cpu-scale" &&
        result.scene != "m7-layout" && result.scene != "m7-streaming")
        throw std::invalid_argument("unsupported scene: " + result.scene);
    if (result.benchmark &&
        (result.debug || result.gpuValidation || !result.pixCapture.empty() || !result.renderdocCapture.empty()))
        throw std::invalid_argument("benchmark requires Release without debug/GBV/capture");
    if (!result.pixCapture.empty() && result.backend != Rhi::RhiBackend::D3D12)
        throw std::invalid_argument("PIX capture entry requires D3D12");
    if (!result.pixCapture.empty() && !result.renderdocCapture.empty())
        throw std::invalid_argument("PIX and RenderDoc captures must run separately");
    if (!result.renderdocCapture.empty() && result.backend != Rhi::RhiBackend::D3D11)
        throw std::invalid_argument("RenderDoc capture entry requires D3D11");
    if (result.exerciseChanges && result.migrationLevel != 9)
        throw std::invalid_argument("resize/reload exercise requires migration level 9");
    // D 批次可读性控制：停留/临时尺寸只服务 --exercise-changes 的连续 Demo，
    // 不允许悄悄改变 benchmark 或普通渲染路径。
    if (result.exerciseChanges)
    {
        if ((result.tourTemporaryWidth == 0) != (result.tourTemporaryHeight == 0))
            throw std::invalid_argument("--tour-temporary-width and --tour-temporary-height require each other");
        if (result.tourTemporaryWidth && (result.tourTemporaryWidth < 64 || result.tourTemporaryHeight < 64))
            throw std::invalid_argument("--tour-temporary-width/height must be at least 64");
        if (result.tourHoldFrames && result.frames && result.tourHoldFrames * 4 + 8 >= result.frames)
            throw std::invalid_argument("--tour-hold-frames leaves no frames for the tour itself");
    }
    else if (result.tourHoldFrames || result.tourTemporaryWidth || result.tourTemporaryHeight)
        throw std::invalid_argument("--tour-hold-frames/--tour-temporary-* require --exercise-changes");
    if (result.headless && result.frames == 0 && result.scene == "m6-adapter-smoke")
        result.frames = 3;
    if (m7Scene)
    {
        if (seen.contains("migration-level") || seen.contains("manifest") || seen.contains("debug-view") ||
            seen.contains("fixed-frame") || seen.contains("diagnostics-interval") || seen.contains("smoke-level") ||
            result.exerciseChanges)
            throw std::invalid_argument("migration/smoke options are not valid for M7 performance scenes");
        // M7-12（A25/A26）：capture 对 M7 场景开放——RenderDoc 限 D3D11、PIX 限 D3D12
        // （配对校验在上面统一做），两者必须分开运行；benchmark 下仍禁止（见上）。
        if (result.vsync)
            throw std::invalid_argument("M7 performance scenes require vsync off (--vsync=off)");
        if (result.frames == 0 && !result.benchmark)
            throw std::invalid_argument("M7 performance scenes require --frames=N (smoke) or --benchmark");
        if (result.scene == "m7-cpu-scale")
        {
            // recipe 冻结 50k 可渲染代理；显式给出时必须一致，未给出则按 recipe 取值。
            if (result.entityCountSpecified && result.entityCount != 50000)
                throw std::invalid_argument("m7-cpu-scale fixes 50000 renderable proxies (recipe)");
            result.entityCount = 50000;
        }
        if (result.scene == "m7-streaming" && result.entityCount == 0)
            result.entityCount = 500; // 流式场景的渲染背景固定 500 代理，只在 JSON 中记录
        // M7-07：异步上传流水线只在 m7-streaming 上有意义（其它场景没有资产请求脚本）。
        if (result.asyncAssets && result.scene != "m7-streaming")
            throw std::invalid_argument("--async-assets=on requires --scene=m7-streaming");
        // M7-10（A27）：稳定性开关的约束——热重载需要异步流水线；稳定性压力会改变
        // swapchain 尺寸，不能与 benchmark 的固定协议共存。
        if (result.loaderReloadEvery > 0 && !result.asyncAssets)
            throw std::invalid_argument("--loader-reload-every requires --async-assets=on");
        // M7-RESIZE-PATH：真实窗口 resize 是交互/长跑路径，benchmark 的固定 extent 契约不适用。
        // 这条必须排在稳定性压力规则**之前**：同一条命令两处都会拒绝时，先说清最本质的那条
        //（"benchmark 不接受 resize"），而不是"压力开关不能进 benchmark"）。
        if (result.exerciseResize)
        {
            if (result.benchmark)
                throw std::invalid_argument("--exercise-resize is not allowed in benchmark mode");
            if (result.stabilityStressEvery == 0)
                throw std::invalid_argument("--exercise-resize requires --stability-stress-every=N");
            if (result.headless)
                throw std::invalid_argument("--exercise-resize requires a real window (no --headless)");
        }
        // M7-DEVICE-LOST-INJECT：注入只用于负向/集成测试，且必须走"真实窗口 + 非 benchmark"。
        if (result.injectDeviceLostAfter > 0)
        {
            if (result.benchmark)
                throw std::invalid_argument("--inject-device-lost-after is not allowed in benchmark mode");
            if (result.headless)
                throw std::invalid_argument("--inject-device-lost-after requires a real window (no --headless)");
        }
        if (result.stabilityStressEvery > 0 && result.benchmark)
            throw std::invalid_argument("--stability-stress-every is not allowed in benchmark mode");
        if (result.soakTelemetryEvery > 0 && result.frames == 0 && !result.benchmark)
            throw std::invalid_argument("--soak-telemetry-every requires --frames=N (soak) or --benchmark");
        if (result.benchmark)
        {
            // 控制变量显式性是 M7-01 的契约：缺省值不允许静默进入性能证据。
            const auto require = [&seen](const char* key)
            {
                if (!seen.contains(key))
                    throw std::invalid_argument(std::string("benchmark requires explicit --") + key);
            };
            require("seed");
            require("workers");
            require("chunk-size");
            require("packet-build");
            require("vsync");
            require("warmup-frames");
            require("measure-frames");
            require("metrics");
            require("machine-manifest");
            if (result.headless)
                throw std::invalid_argument("benchmark requires a visible foreground window (no --headless)");
            if (result.scene == "m7-layout" && !result.entityCountSpecified)
                throw std::invalid_argument("m7-layout requires explicit --entity-count");
            if (result.scene == "m7-streaming")
            {
                // 加载模式与（异步模式下的）三条预算都是控制变量：缺一即报错，不静默取默认。
                require("async-assets");
                require("upload-mib-per-frame");
                if (result.asyncAssets)
                {
                    require("upload-budget-cpu-ms");
                    require("upload-budget-requests");
                    require("upload-budget-reload-percent");
                }
            }
            // M7-08：布局变体是 m7-cpu-scale / m7-layout 的控制变量（缺一即报错）。
            if (result.scene == "m7-cpu-scale" || result.scene == "m7-layout")
            {
                require("layout");
            }
        }
        // serial 路径没有任务系统参与：workers 必须为 1，scheduler 无意义；
        // parallel 路径在 benchmark 中必须显式给出 scheduler（E-M7-RP-001/002 的变量）。
        if (result.packetBuild == "serial")
        {
            if (result.workers != 1)
                throw std::invalid_argument("--packet-build=serial requires --workers=1 "
                                            "(parallel packet build is opt-in: --packet-build=parallel)");
            if (result.schedulerSpecified)
                throw std::invalid_argument("--scheduler is only valid with --packet-build=parallel");
            if (result.chunkReserveSpecified)
                throw std::invalid_argument("--chunk-reserve is only valid with --packet-build=parallel");
            result.scheduler = "none";
            result.chunkReserve = true;
        }
        else if (result.benchmark && !result.schedulerSpecified)
        {
            throw std::invalid_argument("benchmark requires explicit --scheduler with --packet-build=parallel");
        }
        else if (!result.schedulerSpecified)
        {
            // 烟测路径的默认调度器：v1（per-worker + 窃取，M7-04 已验证）；
            // 证据路径下不允许静默默认，所以这里只在非 benchmark 时生效。
            result.scheduler = "per-worker";
        }
    }
    return result;
}
} // namespace MiniEngine::Sandbox
