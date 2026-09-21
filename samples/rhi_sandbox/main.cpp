#include "M604ShaderFixtures.h"
#include "M6SceneRunner.h"
#include "M7SceneRunner.h"
#include "NativeDevice.h"
#include "RhiSelection.h"
#include "SmokeScene.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
using namespace MiniEngine;
using namespace MiniEngine::Rhi;
using namespace MiniEngine::Sandbox;
std::string Escape(std::string_view text)
{
    std::string result;
    for (char c : text)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += c;
        }
    }
    return result;
}
ShaderDesc Shader(std::span<const ShaderPackage> packages, std::string_view asset, RhiBackend backend,
                  ShaderStage stage)
{
    for (const auto& package : packages)
        if (package.assetId == asset)
            return SelectShader(package, backend, stage);
    throw std::runtime_error("offline shader package is missing: " + std::string(asset));
}
void WriteText(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream output(path, std::ios::binary);
    if (!output || !(output << content))
        throw std::runtime_error("cannot write " + path.string());
}
} // namespace
int main(int argc, char** argv)
{
    using namespace MiniEngine;
    using namespace MiniEngine::Rhi;
    using namespace MiniEngine::Sandbox;
    try
    {
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i)
            arguments.emplace_back(argv[i]);
        const auto options = ParseRhiOptions(arguments);
        if (options.help)
        {
            std::cout << RhiUsage();
            return 0;
        }
        if (options.scene.starts_with("m7-"))
            return RunM7Scene(options);
        if (options.scene != "m6-adapter-smoke")
            return RunM6Scene(options);
        SetLogSink([](std::string_view line) { std::clog << line << '\n'; });
        InputState input;
        WindowDesc windowDesc;
        windowDesc.title = L"MiniEngine RHI Sandbox";
        windowDesc.width = options.width;
        windowDesc.height = options.height;
        windowDesc.visible = !options.headless;
        WindowsWindow window(windowDesc, input);
        RhiDeviceCreateInfo create;
        create.nativeWindow = window.NativeHandle();
        create.enableDebugLayer = options.debug;
        create.enableGpuValidation = options.gpuValidation;
        create.useWarp = options.warp;
        auto device = CreateRhiDevice(options.backend, create);
        const auto capabilities = device->Capabilities();
        if (capabilities.backend != options.backend)
            throw std::runtime_error("backend identity mismatch");
        Extent2D extent{options.width, options.height};
        SwapChainDesc chainDesc;
        chainDesc.extent = extent;
        chainDesc.vsync = !options.headless;
        chainDesc.debugName = "M6.sandbox";
        const auto chain = device->CreateSwapChain(chainDesc);
        std::vector<ShaderPackage> packages;
        SmokeShaders shaders;
        if (options.smokeLevel >= 2)
        {
            packages = M604::LoadM604Packages();
            shaders.toneVertex = Shader(packages, "ToneMapVSMain", options.backend, ShaderStage::Vertex);
            shaders.tonePixel = Shader(packages, "ToneMapPSMain", options.backend, ShaderStage::Pixel);
            shaders.depthVertex = Shader(packages, "ShadowDepthVSMain", options.backend, ShaderStage::Vertex);
        }
        std::uint32_t rendered = 0;
        bool suspended = false;
        bool resizeExercised = false;
        bool reloadExercised = false;
        bool minimizeObserved = false;
        bool focusLost = false;
        bool focusRestored = false;
        std::optional<TextureReadbackResult> pixels;
        std::optional<TimestampResult> begin, end;
        {
            SmokeScene scene(*device, shaders, extent, options.smokeLevel);
            SmokeFrameResult latest;
            const bool capture = !options.outputDirectory.empty() || options.smokeLevel >= 5;
            while (window.PumpMessages() && (options.frames == 0 || rendered < options.frames))
            {
                if (!options.headless)
                {
                    if (!window.IsActive())
                        focusLost = true;
                    else if (focusLost)
                        focusRestored = true;
                    const Extent2D actual{window.ClientWidth(), window.ClientHeight()};
                    if (window.IsMinimized() || !actual.width || !actual.height)
                    {
                        minimizeObserved = minimizeObserved || window.IsMinimized();
                        if (!suspended)
                            device->ResizeSwapChain(chain, {0, 0});
                        suspended = true;
                        window.WaitForMessage(30);
                        continue;
                    }
                    if (suspended || actual != extent)
                    {
                        device->ResizeSwapChain(chain, actual);
                        scene.Resize(actual);
                        extent = actual;
                        suspended = false;
                        resizeExercised = true;
                    }
                    if (window.ShouldPause())
                    {
                        window.WaitForMessage(30);
                        continue;
                    }
                }
                latest = scene.Render(chain, capture);
                if (!latest.frame.serial)
                    throw std::runtime_error("unexpected suspended frame");
                ++rendered;
                if (options.headless && options.smokeLevel >= 4 && rendered == 1 &&
                    (options.frames == 0 || rendered < options.frames))
                {
                    device->ResizeSwapChain(chain, {0, 0});
                    if (device->BeginFrame(chain).serial != 0)
                        throw std::runtime_error("zero-size chain did not suspend");
                    extent = {options.width + 17, options.height + 11};
                    device->ResizeSwapChain(chain, extent);
                    scene.Resize(extent);
                    resizeExercised = true;
                }
                if (options.smokeLevel >= 6 && rendered == 2 && (options.frames == 0 || rendered < options.frames))
                {
                    scene.ReloadToneShaders(shaders.toneVertex, shaders.tonePixel);
                    scene.ReloadExposure(1.0F);
                    reloadExercised = true;
                }
            }
            device->WaitIdle();
            if (capture && latest.frame.serial)
            {
                pixels = device->TryReadTextureReadback(latest.readback);
                begin = device->TryReadTimestamp(latest.beginQuery);
                end = device->TryReadTimestamp(latest.endQuery);
                if (!pixels || pixels->unavailable || pixels->bytes.empty() ||
                    pixels->frameSerial != latest.frame.serial || pixels->source != latest.frame.backBuffer)
                    throw std::runtime_error("completed screenshot is missing or has stale identity");
                if (!begin || !end || !TimestampDeltaSeconds(*begin, *end))
                    throw std::runtime_error("completed timestamp pair is invalid");
            }
        }
        device->Destroy(chain);
        device->WaitIdle();
        const auto diagnostic = device->Diagnostics();
        auto& nativeDevice = dynamic_cast<NativeDevice&>(*device);
        nativeDevice.Shutdown();
        const auto nativeReport = nativeDevice.NativeReport(true);
        if (nativeReport.warningErrors || nativeReport.liveResources)
            throw std::runtime_error("native diagnostics or resource census failed:\n" + nativeReport.trace);
        if (diagnostic.aliveObjects || diagnostic.retiringObjects)
            throw std::runtime_error("resources remain after sandbox shutdown");
        std::ostringstream metadata;
        metadata << "{\n  \"schema\":\"miniengine.m6-06.sandbox.v1\","
                 << "\n  \"backend\":\"" << ToString(capabilities.backend) << "\","
                 << "\n  \"backendSource\":\"" << options.backendSource << "\","
                 << "\n  \"adapter\":\"" << Escape(capabilities.adapterName) << "\","
                 << "\n  \"driver\":\"" << Escape(capabilities.driverVersion) << "\","
                 << "\n  \"warpRequested\":" << (options.warp ? "true" : "false") << ',' << "\n  \"fallback\":false,"
                 << "\n  \"scene\":\"" << options.scene << "\","
                 << "\n  \"smokeLevel\":" << options.smokeLevel << ',' << "\n  \"frames\":" << rendered << ','
                 << "\n  \"debugLayer\":" << (capabilities.debugLayerEnabled ? "true" : "false") << ','
                 << "\n  \"gpuValidation\":" << (capabilities.gpuValidationEnabled ? "true" : "false") << ','
                 << "\n  \"resizeExercised\":" << (resizeExercised ? "true" : "false") << ','
                 << "\n  \"minimizeObserved\":" << (minimizeObserved ? "true" : "false") << ','
                 << "\n  \"focusLost\":" << (focusLost ? "true" : "false") << ','
                 << "\n  \"focusRestored\":" << (focusRestored ? "true" : "false") << ','
                 << "\n  \"reloadExercised\":" << (reloadExercised ? "true" : "false") << ','
                 << "\n  \"nativeWarningErrors\":" << nativeReport.warningErrors << ','
                 << "\n  \"nativeLiveResources\":" << nativeReport.liveResources << ','
                 << "\n  \"nativeSubmittedBatches\":" << nativeReport.submittedBatches << ','
                 << "\n  \"nativeCompletedSerial\":" << nativeReport.completedSerial << ','
                 << "\n  \"explicitUnbinds\":" << nativeReport.explicitUnbinds << ','
                 << "\n  \"barriers\":" << nativeReport.barriers << ','
                 << "\n  \"discardNoOps\":" << nativeReport.discardNoOps << ','
                 << "\n  \"aliveObjects\":" << diagnostic.aliveObjects << ','
                 << "\n  \"retiringObjects\":" << diagnostic.retiringObjects << ",\n  \"status\":\"PASS\"\n}\n";
        if (!options.outputDirectory.empty())
        {
            const auto directory = std::filesystem::path(options.outputDirectory);
            std::filesystem::create_directories(directory);
            WriteText(directory / "metadata.json", metadata.str());
            WriteText(directory / "native-trace.txt", nativeReport.trace);
            WriteText(directory / "semantic-trace.txt", nativeDevice.SemanticTrace());
            WriteText(directory / "readback.json", TextureReadbackResultJson(pixels));
            WriteText(directory / "timestamp-begin.json", TimestampResultJson(begin));
            WriteText(directory / "timestamp-end.json", TimestampResultJson(end));
            if (pixels)
            {
                std::ofstream raw(directory / "screenshot.rgba", std::ios::binary);
                raw.write(reinterpret_cast<const char*>(pixels->bytes.data()),
                          static_cast<std::streamsize>(pixels->bytes.size()));
                if (!raw)
                    throw std::runtime_error("cannot write screenshot bytes");
            }
        }
        std::cout << metadata.str();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MiniEngineSandbox failed: " << error.what() << '\n' << RhiUsage();
        return 2;
    }
}
