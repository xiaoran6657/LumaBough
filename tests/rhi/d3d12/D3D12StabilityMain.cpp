// ============================================================================
// D3D12StabilityMain.cpp — M5-10 稳定性矩阵 console harness
// 目的：在真实窗口和 D3D12 direct queue 上运行固定帧压力，并逐段消费
//       InfoQueue。该程序不替代 sandbox 的业务装配；manifest 由上层提供，
//       所有计数均来自真实 Renderer API，不用常量伪造通过结果。
// ============================================================================
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <MiniEngine/World/RenderPacket.h>
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <MiniEngine/World/World.h>
#include <MiniEngine/World/WorldLoader.h>

#include "D3D12DescriptorHeap.h"
#include "D3D12Renderer.h"
#include "D3D12RootSignature.h"
#include "D3D12StabilityPressure.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef MINIENGINE_D3D12_SHADER_DIR
#error "MINIENGINE_D3D12_SHADER_DIR must be defined by the executable target"
#endif

namespace MiniEngine::Rhi::D3D12
{
// 仅由稳定性 executable 使用的测试访问器；生产 renderer 不暴露 failure 注入开关。
struct D3D12StabilityAccess final
{
    static void FailIblAfterDraws(D3D12Renderer& renderer, std::uint32_t draws) noexcept
    {
        renderer.m_iblFailureAfterDraws = draws;
    }

    static const void* ActiveIbl(const D3D12Renderer& renderer) noexcept
    {
        return renderer.m_ibl.get();
    }
};
} // namespace MiniEngine::Rhi::D3D12

namespace
{
using MiniEngine::Rhi::D3D12::D3D12DescriptorHeap;
using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::D3D12Renderer;
using MiniEngine::Rhi::D3D12::D3D12RendererOptions;

struct Options final
{
    std::filesystem::path manifest;
    std::filesystem::path output;
    std::string mode = "debug";
    std::uint32_t frames = 10000U;
    std::uint32_t cycles = 100U;
};

struct BakedScene final
{
    MiniEngine::Assets::AssetManager assets;
    std::unique_ptr<MiniEngine::World::World> world;
    std::unordered_map<MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset>, MiniEngine::World::Sphere,
                       MiniEngine::Assets::AssetHandleHasher<MiniEngine::Assets::MeshAsset>>
        meshBounds;

    [[nodiscard]] bool Load(const std::filesystem::path& manifestPath, std::string& error)
    {
        if (!assets.PrepareManifestLoad(manifestPath, error))
            return false;
        assets.CommitPending();
        assets.ApplyPendingRemovals();
        const auto* registry = assets.Registry();
        if (registry == nullptr)
        {
            error = "no active asset registry after commit";
            return false;
        }
        const MiniEngine::Assets::RegistryEntry* worldEntry = nullptr;
        for (std::size_t i = 0; i < registry->EntryCount(); ++i)
        {
            const auto* entry = registry->EntryAt(i);
            if (entry != nullptr && entry->kind == MiniEngine::Assets::AssetKind::World)
            {
                worldEntry = entry;
                break;
            }
        }
        if (worldEntry == nullptr)
        {
            error = "manifest contains no world entry";
            return false;
        }
        const auto artifactPath = manifestPath.parent_path() / worldEntry->artifactRelativePath;
        std::ifstream artifact(artifactPath, std::ios::binary | std::ios::ate);
        if (!artifact)
        {
            error = "cannot open world artifact: " + artifactPath.string();
            return false;
        }
        const auto size = artifact.tellg();
        artifact.seekg(0, std::ios::beg);
        if (size <= 0)
        {
            error = "world artifact is empty";
            return false;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!artifact.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            error = "cannot read world artifact: " + artifactPath.string();
            return false;
        }
        MiniEngine::World::WorldLoadResult result;
        if (!MiniEngine::World::TryBuildWorldFromArtifact(bytes, assets, result, error) || result.world == nullptr)
            return false;
        world = std::move(result.world);
        world->UpdateTransforms();
        return true;
    }
};

[[nodiscard]] MiniEngine::World::RenderPacket BuildStablePacket(BakedScene& scene)
{
    if (scene.world == nullptr)
        throw std::runtime_error{"stable world is not loaded"};
    scene.world->UpdateTransforms();
    const auto items = scene.world->BuildRenderItems();
    MiniEngine::World::RenderPacketCamera camera;
    // 禁用两次 culling，只改变判别路径，不改变 World 的 30 个 RenderItem 输入。
    const MiniEngine::World::DirectionalLight light{};
    const auto meshInfo = [&scene](const MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MeshAsset> handle)
        -> std::optional<MiniEngine::World::MeshDrawInfo>
    {
        const auto* id = scene.assets.Meshes().TryGetAssetId(handle);
        const auto view = scene.assets.Meshes().TryGet(handle);
        if (id == nullptr || !view.has_value())
            return std::nullopt;
        auto found = scene.meshBounds.find(handle);
        if (found == scene.meshBounds.end())
        {
            const auto bounds = MiniEngine::World::ComputeMeshLocalBounds(*view->asset);
            if (!bounds.has_value())
                return std::nullopt;
            found = scene.meshBounds.emplace(handle, *bounds).first;
        }
        return MiniEngine::World::MeshDrawInfo{*id, found->second, view->asset->indexCount};
    };
    const auto materialInfo = [&scene](const MiniEngine::Assets::AssetHandle<MiniEngine::Assets::MaterialAsset> handle)
        -> std::optional<MiniEngine::Assets::AssetId>
    {
        const auto* id = scene.assets.Materials().TryGetAssetId(handle);
        return id == nullptr ? std::nullopt : std::optional{*id};
    };
    return MiniEngine::World::BuildRenderPacket(items, camera, light, meshInfo,
                                                MiniEngine::World::CullingOptions{false, false}, materialInfo);
}

LONG g_focusActivations = 0;

LRESULT CALLBACK FocusWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE)
        InterlockedIncrement(&g_focusActivations);
    return DefWindowProcW(window, message, wParam, lParam);
}

void PumpFor(const ULONGLONG milliseconds)
{
    const auto deadline = GetTickCount64() + milliseconds;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
}

void SendAltTab()
{
    INPUT input[4]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_MENU;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = VK_TAB;
    input[2] = input[1];
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3] = input[0];
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(4U, input, sizeof(INPUT)) != 4U)
        throw std::runtime_error{"SendInput Alt+Tab failed"};
    // 等待 Windows shell 完成 Alt+Tab 动画和前台激活；不能只等待输入入队。
    PumpFor(500);
}

class Window final
{
  public:
    Window()
    {
        m_instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.hInstance = m_instance;
        klass.lpfnWndProc = DefWindowProcW;
        klass.lpszClassName = L"MiniEngineM510StabilityWindow";
        RegisterClassExW(&klass);
        m_handle = CreateWindowExW(0, klass.lpszClassName, L"MiniEngine M5-10 stability", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, m_instance, nullptr);
        if (m_handle == nullptr)
            throw std::runtime_error{"CreateWindowExW failed"};
        ShowWindow(m_handle, SW_SHOW);
        // 自动化隐藏 console 的 STARTUPINFO 可能覆盖首次 ShowWindow，测试窗口仍必须可见。
        if (!IsWindowVisible(m_handle))
            ShowWindow(m_handle, SW_SHOW);
        UpdateWindow(m_handle);
    }
    ~Window()
    {
        if (m_handle != nullptr)
            DestroyWindow(m_handle);
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] HWND Handle() const noexcept
    {
        return m_handle;
    }

    bool Pump() const
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0)
        {
            if (message.message == WM_QUIT)
                return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return true;
    }

  private:
    HINSTANCE m_instance = nullptr;
    HWND m_handle = nullptr;
};

std::string NarrowAscii(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value)
    {
        if (character > 0x7f)
            throw std::invalid_argument{"non-ASCII command line value"};
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::uint32_t ParseUint(std::wstring_view value, std::string_view name)
{
    std::uint32_t result = 0;
    try
    {
        const std::wstring text{value};
        std::size_t consumed = 0U;
        const unsigned long parsed = std::stoul(text, &consumed, 10);
        if (consumed != text.size() || parsed > UINT32_MAX)
            throw std::invalid_argument{"range"};
        result = static_cast<std::uint32_t>(parsed);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument{std::string{"invalid --"} + std::string{name}};
    }
    if (result == 0U && value != L"0")
        throw std::invalid_argument{std::string{"invalid --"} + std::string{name}};
    return result;
}

Options ParseOptions(const wchar_t* commandLine)
{
    Options options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine, &argc);
    if (argv == nullptr)
        throw std::runtime_error{"CommandLineToArgvW failed"};
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring_view arg{argv[i]};
        const auto split = arg.find(L'=');
        const std::wstring_view key = arg.substr(0, split);
        const std::wstring_view value = split == std::wstring_view::npos ? std::wstring_view{} : arg.substr(split + 1);
        if (key == L"--manifest")
            options.manifest = value;
        else if (key == L"--output")
            options.output = value;
        else if (key == L"--mode")
            options.mode = NarrowAscii(value);
        else if (key == L"--frames")
            options.frames = ParseUint(value, "frames");
        else if (key == L"--cycles")
            options.cycles = ParseUint(value, "cycles");
        else
            throw std::invalid_argument{"unknown stability argument"};
    }
    LocalFree(argv);
    if (options.manifest.empty() || options.output.empty())
        throw std::invalid_argument{"--manifest and --output are required"};
    if (options.mode != "debug" && options.mode != "gbv" && options.mode != "profile")
        throw std::invalid_argument{"--mode must be debug, gbv, or profile"};
    if (options.frames == 0U || options.cycles == 0U)
        throw std::invalid_argument{"frames and cycles must be non-zero"};
    return options;
}

std::string JsonEscape(std::string_view text)
{
    std::string escaped;
    for (const unsigned char character : text)
    {
        switch (character)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(static_cast<char>(character));
            break;
        }
    }
    return escaped;
}

void WriteGate(std::ofstream& output, std::uint64_t frame, const MiniEngine::Rhi::D3D12::ValidationReport& report)
{
    static bool first = true;
    if (!first)
        output << ",\n";
    first = false;
    output << "{\"frame\":" << frame << ",\"messages\":[";
    for (std::size_t i = 0; i < report.messages.size(); ++i)
    {
        if (i != 0U)
            output << ',';
        const auto& message = report.messages[i];
        output << "{\"category\":" << message.category << ",\"severity\":" << message.severity
               << ",\"id\":" << message.id << ",\"description\":\"" << JsonEscape(message.description) << "\"}";
    }
    output << "]}";
}

void CheckGate(D3D12Renderer& renderer, std::ofstream& output, std::uint64_t frame)
{
    renderer.FlushGpu("m5-10-gate");
    const auto report = renderer.DrainInfoQueue();
    WriteGate(output, frame, report);
    if (report.HasFailure())
        throw std::runtime_error{"D3D12 stability gate observed warning/error"};
}

bool CommitMaterialRevision(BakedScene& scene, const MiniEngine::World::RenderPacket& packet, std::uint32_t revision)
{
    if (packet.mainOpaque.empty() || !packet.mainOpaque.front().material.IsValid())
        return false;
    const auto handle = packet.mainOpaque.front().material;
    const auto view = scene.assets.Materials().TryGet(handle);
    if (!view.has_value())
        return false;
    auto replacement = *view->asset;
    replacement.baseColorFactor[0] += 0.0001F * static_cast<float>(revision + 1U);
    return scene.assets.Materials().Commit(handle, std::move(replacement));
}

std::optional<MiniEngine::Assets::AssetHandle<MiniEngine::Assets::TextureAsset>> FindEnvironmentHandle(
    BakedScene& scene)
{
    return scene.assets.Textures().TryFind(MiniEngine::Assets::DeriveAssetId("asset://environments/m4-baseline"));
}

bool CommitEnvironmentRevision(BakedScene& scene,
                               MiniEngine::Assets::AssetHandle<MiniEngine::Assets::TextureAsset> handle,
                               std::uint32_t revision)
{
    const auto view = scene.assets.Textures().TryGet(handle);
    if (!view.has_value())
        return false;
    auto replacement = *view->asset;
    if (!replacement.pixels.empty())
        replacement.pixels[0] = static_cast<std::byte>(revision & 0xFFU);
    return scene.assets.Textures().Commit(handle, std::move(replacement));
}

void ResizeCycle(D3D12Renderer& renderer, std::uint32_t index)
{
    const auto& current = renderer.SwapChainFacts();
    const bool currentIsLarge = current.width == 1920U && current.height == 1080U;
    const bool requestedLarge = !currentIsLarge;
    static_cast<void>(index);
    const std::uint32_t width = requestedLarge ? 1920U : 1280U;
    const std::uint32_t height = requestedLarge ? 1080U : 720U;
    if (current.width == width && current.height == height)
        throw std::runtime_error{"stability resize target unexpectedly equals current size"};
    renderer.RequestResize(width, height);
    if (!renderer.ApplyPendingResizeIfNeeded())
        throw std::runtime_error{"stability resize did not apply"};
    const auto& applied = renderer.SwapChainFacts();
    if (applied.width != width || applied.height != height)
        throw std::runtime_error{"stability resize applied unexpected dimensions"};
}

void FocusCycle(HWND mainWindow, std::uint32_t index)
{
    const wchar_t* className = L"MiniEngineM510FocusHelper";
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpfnWndProc = FocusWindowProc;
    klass.lpszClassName = className;
    RegisterClassExW(&klass);
    HWND helper = CreateWindowExW(0, className, L"M5-10 focus helper", WS_OVERLAPPEDWINDOW, 20, 20, 200, 120, nullptr,
                                  nullptr, klass.hInstance, nullptr);
    if (helper == nullptr)
        throw std::runtime_error{"focus helper creation failed"};
    ShowWindow(helper, SW_SHOW);
    if (!IsWindowVisible(helper))
        ShowWindow(helper, SW_SHOW);
    SetForegroundWindow(mainWindow);
    PumpFor(40);
    SetForegroundWindow(helper);
    PumpFor(40);
    SetForegroundWindow(mainWindow);
    PumpFor(40);
    if (GetForegroundWindow() != mainWindow)
    {
        DestroyWindow(helper);
        throw std::runtime_error{"focus setup could not activate the stability window"};
    }
    const LONG before = g_focusActivations;
    SendAltTab();
    if (GetForegroundWindow() != helper || g_focusActivations <= before)
    {
        DestroyWindow(helper);
        throw std::runtime_error{"Alt+Tab did not activate helper window"};
    }
    SendAltTab();
    if (GetForegroundWindow() != mainWindow)
    {
        DestroyWindow(helper);
        throw std::runtime_error{"Alt+Tab did not restore stability window"};
    }
    DestroyWindow(helper);
    (void)index;
}

} // namespace

int main()
{
    try
    {
        const Options options = ParseOptions(GetCommandLineW());
        if (!std::filesystem::is_regular_file(options.manifest))
            throw std::runtime_error{"manifest is not a regular file: " + options.manifest.string()};

        std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error{"cannot open stability output: " + options.output.string()};
        output << "{\"schemaVersion\":1,\"mode\":\"" << options.mode << "\",\"framesRequested\":" << options.frames
               << ",\"cyclesRequested\":" << options.cycles << ",\"gates\":[\n";

        MiniEngine::Rhi::D3D12::DeviceCreateOptions deviceOptions;
        deviceOptions.debugLayer = options.mode != "profile";
        deviceOptions.gpuValidation = options.mode == "gbv";
        deviceOptions.dred = deviceOptions.debugLayer;
        auto device = D3D12Device::Create(deviceOptions);
        const auto debugLayerActive = device->Metadata().debugLayerActive;
        const auto gpuValidationActive = device->Metadata().gpuValidationActive;
        const auto dredActive = device->Metadata().dredActive;
        const auto startupMessages = device->DrainInfoQueue();
        WriteGate(output, 0U, startupMessages);
        if (startupMessages.HasFailure())
            throw std::runtime_error{"device creation validation failed"};
        const D3D12PressureFacts pressure = RunD3D12Pressure(*device);
        const auto pressureReport = device->DrainInfoQueue();
        WriteGate(output, 0U, pressureReport);
        if (pressureReport.HasFailure())
            throw std::runtime_error{"D3D12 pressure gate observed warning/error"};
        output << ",\n{\"pressure\":{\"capacity\":" << pressure.capacity
               << ",\"ringAllocations\":" << pressure.ringAllocations << ",\"wraps\":" << pressure.wraps
               << ",\"noSpanEvents\":" << pressure.noSpanEvents << ",\"waits\":" << pressure.waits
               << ",\"dedicated\":" << pressure.dedicated << ",\"comparedBytes\":" << pressure.comparedBytes
               << ",\"descriptorExhaustions\":" << pressure.descriptorExhaustions
               << ",\"descriptorReclaims\":" << pressure.descriptorReclaims << "}}\n";

        std::uint64_t presentedFrames = 0U;
        std::uint32_t resizeCount = 0U, minimizeCount = 0U, focusCount = 0U, screenshotCount = 0U;
        std::uint32_t materialReloads = 0U, environmentReloads = 0U, shaderReloads = 0U;
        {
            // 依赖对象先声明，renderer 最后声明；离开此作用域时 renderer 先析构，
            // 然后 scene/heap/root signature/window，device 留到最外层 live-object gate。
            Window window;
            MiniEngine::Rhi::D3D12::RootSignatureFacts rootFacts{};
            auto rootSignature = MiniEngine::Rhi::D3D12::CreateM5RootSignature(
                *static_cast<ID3D12Device*>(device->NativeDeviceHandle()), rootFacts);
            D3D12DescriptorHeap srvHeap;
            srvHeap.Initialize(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()),
                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16384U, true, L"M5.10.Stability.SrvHeap");
            BakedScene scene;
            std::string sceneError;
            if (!scene.Load(options.manifest, sceneError))
                throw std::runtime_error{"frozen world load failed: " + sceneError};
            auto packet = BuildStablePacket(scene);
            if (packet.mainOpaque.size() != 30U)
                throw std::runtime_error{"frozen world must provide exactly 30 opaque draws"};

            D3D12Renderer renderer;
            D3D12RendererOptions rendererOptions;
            rendererOptions.width = 1280U;
            rendererOptions.height = 720U;
            rendererOptions.vsync = false;
            renderer.Initialize(*device, window.Handle(), rendererOptions);
            renderer.BindRootSignature(*rootSignature.Get(), 0U);
            renderer.BindDescriptorHeap(srvHeap);
            renderer.CreateBaselinePsoSet(MINIENGINE_D3D12_SHADER_DIR);
            renderer.SetLightingEnabled(true, true, true);
            const auto environmentHandle = FindEnvironmentHandle(scene);
            if (environmentHandle.has_value())
                renderer.SetEnvironment(*environmentHandle);

            auto render = [&]()
            {
                packet = BuildStablePacket(scene);
                renderer.SetFrameInput(&packet, &scene.assets);
                if (!renderer.RenderFrame())
                    throw std::runtime_error{"stability frame was not presented"};
            };
            if (environmentHandle.has_value())
            {
                bool ready = false;
                for (std::uint32_t i = 0U; i < 120U && !ready; ++i)
                {
                    render();
                    ready = renderer.IblReady();
                }
                if (!ready)
                    throw std::runtime_error{"initial IBL did not become Ready within warmup"};
            }

            const std::uint32_t focusTargets = (std::min)(options.cycles, 50U);
            std::uint32_t screenshotsInFlight = 0U;
            bool iblFailureChecked = false;
            const void* lastGoodIbl = nullptr;
            for (std::uint32_t frame = 0U; frame < options.frames; ++frame)
            {
                if (!window.Pump())
                    throw std::runtime_error{"stability window closed"};
                if (resizeCount < options.cycles)
                    ResizeCycle(renderer, resizeCount++);
                if (minimizeCount < focusTargets)
                {
                    ShowWindow(window.Handle(), SW_MINIMIZE);
                    renderer.RequestResize(0U, 0U);
                    static_cast<void>(renderer.ApplyPendingResizeIfNeeded());
                    static_cast<void>(renderer.RenderFrame());
                    ShowWindow(window.Handle(), SW_RESTORE);
                    SetForegroundWindow(window.Handle());
                    renderer.RequestResize(1280U, 720U);
                    static_cast<void>(renderer.ApplyPendingResizeIfNeeded());
                    ++minimizeCount;
                }
                if (focusCount < focusTargets)
                    FocusCycle(window.Handle(), focusCount++);
                if (screenshotCount < 30U && screenshotsInFlight < 3U)
                {
                    if (renderer.SwapChainFacts().width != 1280U || renderer.SwapChainFacts().height != 720U)
                    {
                        renderer.RequestResize(1280U, 720U);
                        if (!renderer.ApplyPendingResizeIfNeeded())
                            throw std::runtime_error{"screenshot restore resize did not apply"};
                    }
                    renderer.RequestScreenshot();
                    ++screenshotsInFlight;
                }
                if (materialReloads < options.cycles)
                {
                    if (environmentHandle.has_value() && !renderer.IblReady())
                        throw std::runtime_error{"environment reload started before Ready"};
                    if (materialReloads == 0U && environmentHandle.has_value())
                    {
                        lastGoodIbl = MiniEngine::Rhi::D3D12::D3D12StabilityAccess::ActiveIbl(renderer);
                        MiniEngine::Rhi::D3D12::D3D12StabilityAccess::FailIblAfterDraws(renderer, 2U);
                    }
                    if (!CommitMaterialRevision(scene, packet, materialReloads))
                        throw std::runtime_error{"material revision commit failed"};
                    ++materialReloads;
                    if (environmentHandle.has_value())
                    {
                        if (!CommitEnvironmentRevision(scene, *environmentHandle, environmentReloads))
                            throw std::runtime_error{"environment revision commit failed"};
                        ++environmentReloads;
                        renderer.SetEnvironment(*environmentHandle);
                    }
                    renderer.BeginPsoHotReload(renderer.ShaderRevision() + 1U);
                    renderer.CommitPsoHotReload(renderer.Queue().NextFenceValue() - 1U);
                    ++shaderReloads;
                }
                render();
                // 每个事务完成后才启动下一次重载；失败也等待真正的提交 fence。
                for (std::uint32_t attempt = 0U; environmentHandle && !renderer.IblReady(); ++attempt)
                {
                    if (attempt >= 120U)
                        throw std::runtime_error{"IBL transaction did not complete"};
                    renderer.FlushGpu("IBL transaction completion");
                    renderer.ReclaimCompletedWork();
                    if (!iblFailureChecked && renderer.IblFailureCount() != 0U)
                    {
                        if (MiniEngine::Rhi::D3D12::D3D12StabilityAccess::ActiveIbl(renderer) != lastGoodIbl ||
                            renderer.IblLastError().empty())
                            throw std::runtime_error{"IBL failure did not preserve the active set and error"};
                        const auto drawsBefore = renderer.ForwardDrawCount();
                        render();
                        if (renderer.ForwardDrawCount() <= drawsBefore ||
                            MiniEngine::Rhi::D3D12::D3D12StabilityAccess::ActiveIbl(renderer) != lastGoodIbl)
                            throw std::runtime_error{"old IBL did not remain usable after failure"};
                        renderer.RetryEnvironment();
                        iblFailureChecked = true;
                    }
                    render();
                }
                if (screenshotsInFlight == 3U)
                {
                    renderer.FlushGpu("m5-10-screenshot-batch");
                    for (std::uint32_t slot = 0U; slot < 3U; ++slot)
                    {
                        const auto screenshot = renderer.TakeScreenshot();
                        if (!screenshot.has_value() || screenshot->width != 1280U || screenshot->height != 720U ||
                            screenshot->pixels.empty())
                            throw std::runtime_error{"screenshot batch slot is empty or has unexpected dimensions"};
                        std::uint8_t minRgb = 255U;
                        std::uint8_t maxRgb = 0U;
                        for (std::size_t pixel = 0U; pixel + 3U < screenshot->pixels.size(); pixel += 4U)
                        {
                            minRgb = (std::min)({minRgb, screenshot->pixels[pixel], screenshot->pixels[pixel + 1U],
                                                 screenshot->pixels[pixel + 2U]});
                            maxRgb = (std::max)({maxRgb, screenshot->pixels[pixel], screenshot->pixels[pixel + 1U],
                                                 screenshot->pixels[pixel + 2U]});
                        }
                        if (minRgb == maxRgb)
                            throw std::runtime_error{"screenshot RGB channels contain no rendered variation"};
                        ++screenshotCount;
                    }
                    screenshotsInFlight = 0U;
                }
                if (frame % 25U == 0U)
                    CheckGate(renderer, output, frame + 1U);
            }
            renderer.FlushGpu("m5-10-stability-shutdown");
            renderer.ReclaimCompletedWork();
            CheckGate(renderer, output, renderer.PresentedFrameCount());
            presentedFrames = renderer.PresentedFrameCount();
            if (materialReloads != options.cycles || environmentReloads != options.cycles ||
                shaderReloads != options.cycles || resizeCount != options.cycles || minimizeCount != focusTargets ||
                focusCount != focusTargets || screenshotCount != 30U)
                throw std::runtime_error{"stability action counts did not reach requested targets"};
            if (environmentHandle.has_value() && !iblFailureChecked)
                throw std::runtime_error{"IBL failure injection was not observed"};
            const auto retirements = renderer.RetirementCounts();
            if (retirements.assets != 0U || retirements.ibl != 0U || retirements.pendingIbl != 0U ||
                retirements.psos != 0U || retirements.descriptors != 0U || retirements.uploads != 0U ||
                retirements.constantBytes != 0U || retirements.uploadBytes != 0U)
                throw std::runtime_error{"stability shutdown left deferred GPU work"};
            output << ",\n{\"shutdown\":{\"assets\":" << retirements.assets << ",\"ibl\":" << retirements.ibl
                   << ",\"pendingIbl\":" << retirements.pendingIbl << ",\"psos\":" << retirements.psos
                   << ",\"descriptors\":" << retirements.descriptors << ",\"uploads\":" << retirements.uploads
                   << ",\"constantBytes\":" << retirements.constantBytes
                   << ",\"uploadBytes\":" << retirements.uploadBytes
                   << "},\"iblFailureCount\":" << renderer.IblFailureCount()
                   << ",\"opaqueDraws\":" << renderer.ForwardDrawCount()
                   << ",\"shadowDraws\":" << renderer.ShadowDrawCount() << "}";
            output.flush();
        }
        // renderer、root signature、descriptor heap、scene、window 已全部销毁后才报告 DXGI live objects。
        device.reset();
        const auto dxgi = D3D12Device::ReportLiveDxgiObjects();
        MiniEngine::Rhi::D3D12::ValidationReport dxgiGate;
        for (const auto& message : dxgi.messages)
            dxgiGate.messages.push_back(MiniEngine::Rhi::D3D12::ValidationMessage{message.category, message.severity,
                                                                                  message.id, message.description});
        WriteGate(output, std::numeric_limits<std::uint64_t>::max(), dxgiGate);
        if (options.mode != "profile" && !dxgi.available)
            throw std::runtime_error{"post-release DXGI live-object report was unavailable"};
        for (const auto& message : dxgi.messages)
        {
            if (message.severity <= 2U || message.isLiveObject)
                throw std::runtime_error{"post-release DXGI live-object report contains warning/error/live object"};
        }
        output << "],\"debugLayerActive\":" << (debugLayerActive ? "true" : "false")
               << ",\"gpuValidationActive\":" << (gpuValidationActive ? "true" : "false")
               << ",\"dredActive\":" << (dredActive ? "true" : "false") << ",\"framesPresented\":" << presentedFrames
               << ",\"resizeCycles\":" << resizeCount << ",\"minimizeCycles\":" << minimizeCount
               << ",\"focusCycles\":" << focusCount << ",\"screenshotRequests\":" << screenshotCount
               << ",\"materialReloads\":" << materialReloads << ",\"environmentReloads\":" << environmentReloads
               << ",\"shaderReloads\":" << shaderReloads << ",\"dxgiLiveObjectCount\":" << dxgi.liveObjectCount
               << ",\"dxgiLiveReportAvailable\":" << (dxgi.available ? "true" : "false")
               << ",\"profileEvidenceOnly\":" << (options.mode == "profile" ? "true" : "false") << "}\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "M5-10 stability FAILED: " << exception.what() << '\n';
        return 3;
    }
}
