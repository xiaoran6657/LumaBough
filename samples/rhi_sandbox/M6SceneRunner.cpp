#include "M6SceneRunner.h"
#include "../common/ParityIdentity.h"
#include "M604ShaderFixtures.h"
#include "NativeDevice.h"
#include <DirectXMath.h>
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Render/M6SceneResources.h>
#include <MiniEngine/RenderGraph/TransientResourcePool.h>
#include <MiniEngine/Rhi/RhiFactory.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <MiniEngine/World/WorldLoader.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <pix3.h>

#include "CaptureScopes.h"
#include <sstream>

namespace MiniEngine::Sandbox
{
namespace
{
using namespace Rhi;
namespace RG = RenderGraph;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}
std::string Quoted(std::string_view text)
{
    std::string result = "\"";
    for (unsigned char c : text)
    {
        if (c == '\\' || c == '"')
        {
            result += '\\';
            result += static_cast<char>(c);
        }
        else if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else if (c == '\t')
            result += "\\t";
        else if (c < 32)
            throw std::runtime_error("unsupported JSON control character");
        else
            result += static_cast<char>(c);
    }
    return result + '"';
}
void Save(const fs::path& path, std::string_view value)
{
    std::ofstream out(path, std::ios::binary);
    if (!out || !(out << value))
        throw std::runtime_error("cannot write " + path.string());
}
std::string Hex(std::uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}
std::string Sha(std::string_view bytes)
{
    Assets::Sha256Builder builder;
    Assets::Sha256Digest result{};
    if (!builder.Append(std::as_bytes(std::span(bytes.data(), bytes.size()))) || !builder.Finish(result))
        throw std::runtime_error("SHA256 failed");
    return Assets::ToHexDigest(result);
}
std::string PacketHash(const World::RenderPacket& packet)
{
    const auto hashDraws = [](const std::vector<World::RenderDraw>& draws)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        const auto bytes = [&](const auto& value)
        {
            for (std::byte b : std::as_bytes(std::span(&value, 1)))
            {
                hash ^= std::to_integer<unsigned char>(b);
                hash *= 1099511628211ULL;
            }
        };
        bytes(static_cast<std::uint64_t>(draws.size()));
        for (const auto& draw : draws)
        {
            bytes(draw.meshId.bytes);
            bytes(draw.materialId.bytes);
            bytes(static_cast<std::uint64_t>(draw.entityIndex));
            bytes(static_cast<std::uint64_t>(draw.mirrored));
            bytes(static_cast<std::uint64_t>(draw.castsShadow));
            bytes(static_cast<std::uint64_t>(draw.receivesShadow));
        }
        return hash;
    };
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : {hashDraws(packet.mainOpaque), hashDraws(packet.shadowCasters),
                             static_cast<std::uint64_t>(packet.stats.trianglesSubmitted)})
        for (std::uint32_t shift = 0; shift < 64; shift += 8)
        {
            hash ^= (value >> shift) & 255;
            hash *= 1099511628211ULL;
        }
    return Hex(hash);
}
struct Scene final
{
    Assets::AssetManager assets;
    std::unique_ptr<World::World> world;
    std::string environmentHash;
    explicit Scene(const fs::path& manifest)
    {
        std::string error;
        if (!assets.PrepareManifestLoad(manifest, error))
            throw std::runtime_error(error);
        assets.CommitPending();
        assets.ApplyPendingRemovals();
        const auto* registry = assets.Registry();
        if (!registry)
            throw std::runtime_error("manifest registry missing");
        const Assets::RegistryEntry* worldEntry = nullptr;
        for (std::size_t i = 0; i < registry->EntryCount(); ++i)
            if (const auto* entry = registry->EntryAt(i); entry && entry->kind == Assets::AssetKind::World)
            {
                worldEntry = entry;
                break;
            }
        if (!worldEntry)
            throw std::runtime_error("world artifact missing");
        std::ifstream stream(manifest.parent_path() / worldEntry->artifactRelativePath,
                             std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() <= 0)
            throw std::runtime_error("invalid world artifact");
        std::vector<std::byte> bytes(static_cast<std::size_t>(stream.tellg()));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("world artifact read failed");
        World::WorldLoadResult loaded;
        if (!World::TryBuildWorldFromArtifact(bytes, assets, loaded, error))
            throw std::runtime_error(error);
        world = std::move(loaded.world);
        const auto* env = registry->Find(Assets::DeriveAssetId("asset://environments/m4-baseline"));
        if (!env)
            throw std::runtime_error("baseline environment missing");
        std::ifstream envFile(manifest.parent_path() / env->artifactRelativePath, std::ios::binary);
        std::ostringstream envBytes;
        envBytes << envFile.rdbuf();
        if (!envFile || envBytes.str().empty())
            throw std::runtime_error("environment read failed");
        environmentHash = Sha(envBytes.str());
    }
    World::RenderPacket Packet(Extent2D extent)
    {
        World::RenderPacketCamera camera;
        const auto store = [](DirectX::FXMMATRIX value)
        {
            DirectX::XMFLOAT4X4 matrix;
            DirectX::XMStoreFloat4x4(&matrix, value);
            World::Matrix4 result;
            std::memcpy(result.values.data(), &matrix, sizeof(matrix));
            return result;
        };
        camera.view = store(DirectX::XMMatrixLookToLH(
            DirectX::XMVectorSet(0, 3, -10, 0), DirectX::XMVectorSet(0, 0, 1, 0), DirectX::XMVectorSet(0, 1, 0, 0)));
        camera.projection = store(DirectX::XMMatrixPerspectiveFovLH(
            3.14159265358979323846F / 3.0F, static_cast<float>(extent.width) / extent.height, 0.1F, 100.0F));
        camera.worldPosition = {0, 3, -10};
        world->UpdateTransforms();
        return World::BuildRenderPacket(
            world->BuildRenderItems(), camera, {},
            [&](auto handle) -> std::optional<World::MeshDrawInfo>
            {
                const auto* id = assets.Meshes().TryGetAssetId(handle);
                const auto view = assets.Meshes().TryGet(handle);
                if (!id || !view)
                    throw std::runtime_error("baseline mesh missing");
                const auto bounds = World::ComputeMeshLocalBounds(*view->asset);
                if (!bounds)
                    throw std::runtime_error("baseline mesh bounds invalid");
                return World::MeshDrawInfo{*id, *bounds, view->asset->indexCount};
            },
            {},
            [&](auto handle) -> std::optional<Assets::AssetId>
            {
                const auto* id = assets.Materials().TryGetAssetId(handle);
                if (!id)
                    throw std::runtime_error("baseline material missing");
                return *id;
            });
    }
};

class SceneEnvironment final
{
    IRhiDevice& m_device;

  public:
    TextureHandle panorama;
    PreparedEnvironment prepared;
    SceneEnvironment(IRhiDevice& device, Assets::AssetManager& assets) : m_device(device)
    {
        const auto handle = assets.Textures().TryFind(Assets::DeriveAssetId("asset://environments/m4-baseline"));
        const auto view = handle ? assets.Textures().TryGet(*handle) : std::nullopt;
        if (!view || view->asset->pixelFormat != Assets::TexturePixelFormat::Rgba16Float)
            throw std::runtime_error("baseline panorama is unavailable or not HDR");
        const auto& texture = *view->asset;
        TextureDesc desc;
        desc.extent = {texture.width, texture.height};
        desc.mipLevels = static_cast<std::uint16_t>(texture.mipCount);
        desc.format = Format::Rgba16Float;
        desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
        desc.debugName = "M6.EnvironmentPanorama";
        panorama = device.CreateTexture(desc);
        try
        {
            std::vector<TextureSubresourceData> levels;
            for (const auto& mip : texture.mips)
                levels.push_back({std::span(texture.pixels).subspan(static_cast<std::size_t>(mip.offset), mip.byteSize),
                                  mip.rowPitch, mip.byteSize});
            device.UploadTexture(panorama, levels);
            std::array<wchar_t, 32768> module{};
            const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
            if (!length || length == module.size())
                throw std::runtime_error("executable path unavailable");
            const auto shaderRoot = fs::path(std::wstring(module.data(), length)).parent_path() / "shaders";
            prepared = PrepareRhiEnvironment(device, panorama, shaderRoot.string(), view->revision);
        }
        catch (...)
        {
            device.Destroy(panorama);
            panorama = {};
            throw;
        }
    }
    ~SceneEnvironment()
    {
        // 失败帧的清理发生在 unwinding 期间：这里必须吞掉次生 destroy 异常，
        // 否则 std::terminate/abort 会掩盖真正的失败原因。
        try
        {
            for (auto texture : prepared.textures)
                if (texture)
                    m_device.Destroy(texture);
            if (panorama)
                m_device.Destroy(panorama);
        }
        catch (const std::exception& error)
        {
            std::cerr << "M6SceneRunner: secondary environment destroy failure: " << error.what() << std::endl;
        }
    }
};

// M7-12：RenderDocScope / PixScope 已抽到共享头（M6/M7 两个运行器复用同一实现）。
} // namespace

// E2：着色器语义哈希给的是**语义身份**，运行包里没有源码树，因此优先取可执行文件旁的
// shaders/（打包时随 EXE 一起携带），只有那里没有 shaders/ 时才退回构建期源码根。
fs::path ShaderSemanticRoot(const std::string_view backend)
{
    std::array<wchar_t, 4096> buffer{};
    const std::uint32_t written =
        static_cast<std::uint32_t>(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    if (written == 0 || written == buffer.size())
        return ProjectRootFallback();
    const fs::path beside = fs::path(buffer.data()).parent_path();
    // 只有 EXE 旁真的带了该后端的 HLSL 源，才算"自足"；否则退回源码树（开发机）。
    for (const auto& entry : fs::directory_iterator(beside / "shaders" / backend, fs::directory_options::skip_permission_denied))
    {
        if (!entry.is_regular_file())
            continue;
        const auto extension = entry.path().extension().string();
        if (extension == ".hlsl" || extension == ".hlsli")
            return beside;
    }
    return ProjectRootFallback();
}

int RunM6Scene(const RhiLaunchOptions& options)
{
    using namespace Rhi;
    using namespace Render;
#ifdef _DEBUG
    if (options.benchmark)
        throw std::invalid_argument("benchmark requires a Release executable");
#endif
    const bool captureToolActive = GetModuleHandleW(L"renderdoc.dll") != nullptr || PIXIsAttachedForGpuCapture();
    if (options.benchmark && captureToolActive)
        throw std::invalid_argument("benchmark cannot run under an injected GPU capture tool");
    const fs::path directory = options.outputDirectory.empty() ? fs::path{} : fs::absolute(options.outputDirectory);
    if (!directory.empty())
        fs::create_directories(directory);
    const fs::path manifest =
        fs::absolute(options.manifest.empty() ? ProjectRootFallback() / "out/m4-09/scene/manifest.json"
                                              : fs::path(options.manifest));
    Scene scene(manifest);
    InputState input;
    WindowDesc windowDesc;
    windowDesc.title = L"MiniEngine M6 Pass Migration";
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
    auto& native = dynamic_cast<NativeDevice&>(*device);
    Extent2D extent{options.width, options.height};
    SwapChainDesc swap;
    swap.extent = extent;
    // D 批次：Present 间隔跟随 --vsync（默认 off，与 C 批次一致）。连续 Demo 的录制/Capture
    // 需要可预期的帧节奏，因此显式打开 --vsync=on 时按显示器刷新率呈现。
    swap.vsync = options.vsync;
    swap.debugName = "M6.BackBuffer";
    auto chain = device->CreateSwapChain(swap);
    auto packages = M604::LoadM604Packages();
    std::vector<ShaderDesc> shaders;
    for (const auto& package : packages)
        shaders.push_back(SelectShader(package, options.backend, package.variants[0].stage));
    World::RenderPacket lastPacket;
    RG::GraphStatistics graphStats{};
    RG::TransientPoolStatistics poolStats{};
    std::optional<TextureReadbackResult> pixels;
    std::string lastTrace;
    std::uint64_t lastCommandCount = 0;
    std::uint32_t rendered = 0, reloadSuccess = 0, reloadRejected = 0, resizeCount = 0;
    bool temporaryResize = false;
    bool measuredWindowForeground = true;
    const auto total = options.benchmark ? options.warmup + options.measure
                       : options.frames  ? options.frames
                                         : options.fixedFrame;
    const bool tour = options.exerciseChanges && !options.benchmark && total >= 6 && !directory.empty();
    // 动作帧：历史协议里 tour 在整数半帧处触发；anchor 帧在它之前一帧（anchor = actionFrame）。
    const std::uint32_t actionFrame = total / 2;
    std::ofstream events;
    if (tour)
    {
        events.open(directory / "tour-events.jsonl", std::ios::binary);
        if (!events)
            throw std::runtime_error("cannot write tour events");
    }
    const auto event = [&](std::string_view marker, std::uint64_t observed, std::uint64_t expected)
    {
        if (!tour)
            return;
        events << "{\"marker\":" << Quoted(marker) << ",\"frame\":" << rendered
               << ",\"observed\":" << observed << ",\"expected\":" << expected << "}\n";
        events.flush();
        if (!events || observed != expected)
            throw std::runtime_error("tour checkpoint failed: " + std::string(marker));
    };
    const auto savePixels = [&](const fs::path& path, const TextureReadbackResult& readback)
    {
        std::ofstream ppm(path, std::ios::binary);
        ppm << "P6\n" << readback.extent.width << ' ' << readback.extent.height << "\n255\n";
        for (std::uint32_t y = 0; y < readback.extent.height; ++y)
            for (std::uint32_t x = 0; x < readback.extent.width; ++x)
                ppm.write(reinterpret_cast<const char*>(readback.bytes.data() + y * readback.rowPitch + x * 4), 3);
        if (!ppm)
            throw std::runtime_error("tour screenshot write failed");
    };
    std::ostringstream rows;
    rows << "frame,measured,prepareMs,buildMs,compileMs,executeMs,presentMs,cpuFrameMs,declared,live,culled,"
            "virtual,physical,physicalTransients,poolBytes,poolHighWaterBytes,logicalTransitions,nativeBarriers,"
            "nativeUnbinds,rhiCommands\n";
    // M6-11 稳定性诊断：每 diagnosticsInterval 帧输出一次 live/cumulative 计数。
    std::ostringstream diagnosticsRows;
    diagnosticsRows
        << "frame,alive,retiring,registrySlots,maxLiveHandles,declared,live,culled,virtualResources,"
           "physicalResources,physicalTransients,transientCreated,transientReused,transientRetired,poolBytes,"
           "poolHighWaterBytes,poolResources,poolHighWaterResources,resourceSets,pipelines,"
           "descriptorRanges,uploadBytes,barriers,unbinds,rhiCommands\n";
    std::uint64_t peakLiveHandles = 0;
    try
    {
        SceneEnvironment environment(*device, scene.assets);
        M6SceneResources resources(*device, scene.assets, shaders, environment.prepared);
        RG::TransientResourcePool pool(*device);
        std::array<BufferHandle, 3> readbacks{};
        std::array<TimestampQueryHandle, 3> begin{}, end{};
        for (std::uint32_t lane = 0; lane < 3; ++lane)
        {
            begin[lane] = device->CreateTimestampQuery("M6.Frame.Begin");
            end[lane] = device->CreateTimestampQuery("M6.Frame.End");
        }
        const auto rebuildReadbacks = [&]
        {
            // 公共 readback 契约：图把截图缓冲声明为 Full coverage，而 RHI 只标记
            // 紧凑像素字节（width*height*4）；两端后端各自持 padding staging。
            // 因此这里必须按紧密尺寸分配，不能用 256 对齐 pitch，否则宽度非 64 像素
            // 对齐时执行期定义性校验会失败。
            const std::uint64_t bytes = static_cast<std::uint64_t>(extent.width) * extent.height * 4;
            // 三个 lane 全部创建成功后再发布，分配失败保留旧读回资源。
            std::array<BufferHandle, 3> candidate{};
            try
            {
                for (auto& handle : candidate)
                    handle = device->CreateBuffer(
                        {bytes, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "M6.ScreenshotReadback"}, {});
            }
            catch (...)
            {
                for (auto handle : candidate)
                    if (handle)
                        device->Destroy(handle);
                throw;
            }
            std::swap(readbacks, candidate);
            for (auto handle : candidate)
                if (handle)
                    device->Destroy(handle);
        };
        rebuildReadbacks();
        // ---- D 批次：连续 Demo 的可读性停留状态机 ----
        // 事件顺序与 C 批次完全一致（挂起 → 临时尺寸 + 同 bytecode 重建 + 拒绝非法候选 → 恢复），
        // 只把"进入下一步"的时机交给停留计数：tourHoldFrames=0 时逐帧推进（历史节奏），
        // >0 时每个状态保持 N 帧，使录屏与 Capture 能辨认。事件本身与检查项不变。
        std::uint32_t holdRemaining = 0;
        std::uint8_t tourStep = 0; // 0 未开始 / 1 已挂起 / 2 已切临时尺寸 / 3 已恢复 / 4 完成
        bool tourSuspended = false;
        const Extent2D temporaryExtent{options.tourTemporaryWidth ? options.tourTemporaryWidth
                                                                  : options.width + 16,
                                       options.tourTemporaryHeight ? options.tourTemporaryHeight
                                                                   : options.height + 8};
        const auto pumpTour = [&]
        {
            if (!tour || rendered < actionFrame || tourStep >= 4)
                return;
            if (holdRemaining && --holdRemaining)
                return;
            switch (tourStep)
            {
            case 0:
            {
                // 1) 程序化挂起：交换链 0×0，不得记录任何帧（不是 OS 最小化）
                device->ResizeSwapChain(chain, {0, 0});
                const auto suspendedSerial = device->BeginFrame(chain).serial;
                event("rhi-suspended", suspendedSerial, 0);
                if (suspendedSerial)
                    throw std::runtime_error("minimized chain recorded a frame");
                tourSuspended = true;
                tourStep = 1;
                holdRemaining = options.tourHoldFrames;
                break;
            }
            case 1:
            {
                // 2) 临时尺寸 + 同一份 bytecode 重建 pipelines + 拒绝非法候选
                tourSuspended = false;
                device->ResizeSwapChain(chain, temporaryExtent);
                extent = temporaryExtent;
                temporaryResize = true;
                pool.Clear();
                rebuildReadbacks();
                resizeCount += 2;
                const auto pipelineCountBefore = resources.PipelineCount();
                resources.ReloadShaders(shaders);
                ++reloadSuccess;
                event("same-bytecode-reload", resources.PipelineCount(), pipelineCountBefore);
                auto broken = shaders;
                broken.front().bytecode = {};
                try
                {
                    resources.ReloadShaders(broken);
                }
                catch (const std::exception&)
                {
                    ++reloadRejected;
                }
                event("invalid-shader-rejected", reloadRejected, 1);
                event("pipeline-count-retained", resources.PipelineCount(), pipelineCountBefore);
                if (reloadRejected != 1)
                    throw std::runtime_error("broken shader candidate was accepted");
                tourStep = 2;
                holdRemaining = options.tourHoldFrames;
                break;
            }
            case 2:
                // 3) 恢复原始尺寸
                device->ResizeSwapChain(chain, {options.width, options.height});
                extent = {options.width, options.height};
                temporaryResize = false;
                pool.Clear();
                rebuildReadbacks();
                ++resizeCount;
                event("original-extent-restored", extent.width, options.width);
                tourStep = 3;
                holdRemaining = options.tourHoldFrames;
                break;
            default:
                tourStep = 4;
                break;
            }
        };
        while (rendered < total && window.PumpMessages())
        {
            if (tourSuspended)
            {
                // 挂起停留：交换链仍为 0×0，每一轮确认没有帧被记录。
                if (device->BeginFrame(chain).serial)
                    throw std::runtime_error("suspended chain recorded a frame");
                window.WaitForMessage(16);
                pumpTour();
                continue;
            }
            if (!options.headless && !temporaryResize)
            {
                Extent2D actual{window.ClientWidth(), window.ClientHeight()};
                if (window.IsMinimized() || !actual.width || !actual.height)
                {
                    if (extent.width)
                    {
                        device->ResizeSwapChain(chain, {0, 0});
                        extent = {};
                    }
                    window.WaitForMessage(20);
                    continue;
                }
                if (actual != extent)
                {
                    device->ResizeSwapChain(chain, actual);
                    extent = actual;
                    pool.Clear();
                    rebuildReadbacks();
                    ++resizeCount;
                }
            }
            if (options.benchmark && rendered >= options.warmup)
                measuredWindowForeground =
                    measuredWindowForeground && GetForegroundWindow() == static_cast<HWND>(window.NativeHandle());
            const auto cpuStart = Clock::now();
            lastPacket = scene.Packet(extent);
            resources.PreparePersistentAssets(lastPacket);
            native.ResetFrameDiagnostics(!options.benchmark && !directory.empty());
            const auto before = native.NativeReport();
            const auto frame = device->BeginFrame(chain);
            if (!frame.serial)
                throw std::runtime_error("unexpected suspended frame");
            auto inputs = resources.Prepare(lastPacket, frame, extent, options.migrationLevel, options.debugView,
                                            options.exposure);
            const auto back = device->QueryTextureState(frame, frame.backBuffer);
            inputs.backBuffer = {frame.backBuffer,
                                 back.descriptor,
                                 back.access,
                                 ResourceAccess::Present,
                                 RG::ContentState::Undefined,
                                 "swapchain",
                                 {}};
            const bool anchorFrame = tour && rendered + 1 == actionFrame;
            inputs.capture = !options.benchmark && (rendered + 1 == total || anchorFrame) && !directory.empty();
            inputs.timestamps = options.migrationLevel >= 8;
            inputs.beginQuery = begin[frame.recycleLane];
            inputs.endQuery = end[frame.recycleLane];
            if (inputs.capture)
            {
                const auto state = device->QueryBufferState(frame, readbacks[frame.recycleLane]);
                inputs.readback = {readbacks[frame.recycleLane],
                                   state.descriptor,
                                   state.access,
                                   ResourceAccess::CopyDestination,
                                   RG::ContentState::Undefined,
                                   "screenshot owner",
                                   {}};
            }
            const auto prepareEnd = Clock::now();
            RG::RenderGraph graph;
            DeclareM6RenderGraph(lastPacket, inputs, graph);
            const auto buildEnd = Clock::now();
            auto plan = graph.Compile();
            const auto compileEnd = Clock::now();
            std::unique_ptr<PixScope> capture;
            std::unique_ptr<RenderDocScope> renderdoc;
            // D 批次：tour 运行的 GPU capture 绑在 anchor 帧（动作之前的静态锚点），
            // 而不是末帧——anchor.json/anchor.ppm 与 capture 因此描述同一个呈现状态。
            // 非 tour 运行沿用末帧捕获。
            const bool captureTargetFrame = tour ? (rendered + 1 == actionFrame) : (rendered + 1 == total);
            if (captureTargetFrame && !options.renderdocCapture.empty())
                renderdoc = std::make_unique<RenderDocScope>(fs::absolute(options.renderdocCapture));
            if (captureTargetFrame && !options.pixCapture.empty())
                capture = std::make_unique<PixScope>(fs::absolute(options.pixCapture));
            const auto executed = plan.TryExecute(*device, frame, pool);
            if (!executed || !executed.presentReady)
                throw std::runtime_error(executed.error ? executed.error->message : "graph cannot present");
            const auto executeEnd = Clock::now();
            device->EndFrame(frame, chain);
            const auto presentEnd = Clock::now();
            if (capture)
                capture->End();
            if (renderdoc)
                renderdoc->End();
            ++rendered;
            graphStats = plan.Statistics();
            poolStats = pool.Statistics();
            lastCommandCount = native.FrameCommandCount();
            const auto after = native.NativeReport();
            if (!options.benchmark)
            {
                const auto objects = device->Diagnostics();
                peakLiveHandles = std::max(peakLiveHandles, static_cast<std::uint64_t>(objects.aliveObjects) +
                                                                static_cast<std::uint64_t>(objects.retiringObjects));
            }
            // 末帧携带截图 readback 的额外 pass/资源，不是可比的稳定采样点。
            if (options.diagnosticsInterval && rendered % options.diagnosticsInterval == 0 && rendered != total)
            {
                const auto objects = device->Diagnostics();
                diagnosticsRows << rendered << ',' << objects.aliveObjects << ',' << objects.retiringObjects << ','
                                << objects.registrySlots << ',' << peakLiveHandles << ',' << graphStats.declaredPasses
                                << ',' << graphStats.livePasses << ',' << graphStats.culledPasses << ','
                                << graphStats.virtualResources << ',' << graphStats.physicalResources << ','
                                << graphStats.physicalTransients << ',' << poolStats.created << ',' << poolStats.reused
                                << ',' << poolStats.retired << ',' << poolStats.bytes << ',' << poolStats.highWaterBytes
                                << ',' << poolStats.resources << ',' << poolStats.highWaterResources << ','
                                << native.FrameResourceSetCount() << ',' << resources.PipelineCount() << ','
                                << after.descriptorRanges << ',' << after.uploadBytes << ',' << after.barriers << ','
                                << after.explicitUnbinds << ',' << lastCommandCount << '\n';
            }
            if (!options.benchmark || rendered > options.warmup)
                rows << rendered << ',' << (options.benchmark ? 1 : 0) << ',' << Ms(cpuStart, prepareEnd) << ','
                     << Ms(prepareEnd, buildEnd) << ',' << Ms(buildEnd, compileEnd) << ',' << Ms(compileEnd, executeEnd)
                     << ',' << Ms(executeEnd, presentEnd) << ',' << Ms(cpuStart, presentEnd) << ','
                     << graphStats.declaredPasses << ',' << graphStats.livePasses << ',' << graphStats.culledPasses
                     << ',' << graphStats.virtualResources << ',' << graphStats.physicalResources << ','
                     << graphStats.physicalTransients << ',' << poolStats.bytes << ',' << poolStats.highWaterBytes
                     << ',' << graphStats.logicalTransitions << ','
                     << (options.backend == RhiBackend::D3D12 ? after.barriers - before.barriers : 0) << ','
                     << after.explicitUnbinds - before.explicitUnbinds << ',' << lastCommandCount << '\n';
            if (anchorFrame)
            {
                // 静态锚点在动作之前独立读回；后续动态状态不与此哈希混用。
                device->WaitIdle();
                const auto anchorPixels = device->TryReadTextureReadback(readbacks[frame.recycleLane]);
                if (!anchorPixels || anchorPixels->unavailable || anchorPixels->frameSerial != frame.serial ||
                    anchorPixels->source != frame.backBuffer || anchorPixels->extent != extent)
                    throw std::runtime_error("tour anchor screenshot missing or stale");
                savePixels(directory / "anchor.ppm", *anchorPixels);
                std::ostringstream anchor;
                anchor << "{\"schemaVersion\":1,\"marker\":\"portfolio-capture\",\"frame\":" << rendered
                       << ",\"backend\":" << Quoted(ToString(options.backend))
                       << ",\"width\":" << extent.width << ",\"height\":" << extent.height
                       << ",\"assetManifestSha256\":"
                       << Quoted(Assets::ToHexDigest(scene.assets.ActiveManifestDigest()))
                       << ",\"environmentArtifactSha256\":" << Quoted(scene.environmentHash)
                       << ",\"shaderSemanticSha256\":"
                       << Quoted(Samples::ShaderSemanticHash(ShaderSemanticRoot(ToString(options.backend)), std::string(ToString(options.backend))))
                       << ",\"visibleSequenceHash\":" << Quoted(PacketHash(lastPacket))
                       << ",\"cameraValues\":" << Quoted(Samples::CameraIdentity(lastPacket))
                       << ",\"lightValues\":" << Quoted(Samples::LightIdentity(lastPacket))
                       << ",\"graphHash\":" << Quoted(Hex(graphStats.planHash))
                       << ",\"commandHash\":" << Quoted(Sha(native.SemanticTrace()))
                       << ",\"warningErrors\":" << after.warningErrors << "}\n";
                Save(directory / "anchor.json", anchor.str());
                event("portfolio-capture", after.warningErrors, 0);
            }
            if (tour && temporaryResize)
                event("temporary-frame-presented", extent.width, temporaryExtent.width);
            if (rendered == total && !directory.empty())
            {
                const auto dumps = plan.Dumps({frame.serial, M610_BUILD_CONFIG, M610_SOURCE_COMMIT});
                Save(directory / "m6-framegraph.json", dumps.frameGraphJson);
                Save(directory / "m6-framegraph.dot", dumps.dot);
                Save(directory / "m6-access-plan.json", dumps.accessPlanJson);
                Save(directory / "m6-transient-plan.json", dumps.transientPlanJson);
                lastTrace = native.SemanticTrace();
                Save(directory / "semantic-trace.txt", lastTrace);
                device->WaitIdle();
                if (inputs.capture)
                {
                    pixels = device->TryReadTextureReadback(readbacks[frame.recycleLane]);
                    if (!pixels || pixels->unavailable || pixels->frameSerial != frame.serial ||
                        pixels->source != frame.backBuffer || pixels->extent != extent)
                        throw std::runtime_error("screenshot missing or stale");
                }
                if (inputs.timestamps)
                {
                    const auto a = device->TryReadTimestamp(begin[frame.recycleLane]);
                    const auto b = device->TryReadTimestamp(end[frame.recycleLane]);
                    if (!a || !b || !TimestampDeltaSeconds(*a, *b))
                        throw std::runtime_error("timestamp pair missing or invalid");
                }
            }
            // 连续 Demo 的下一步由停留状态机推进（tourHoldFrames=0 时逐帧执行）。
            pumpTour();
        }
        if (rendered != total)
            throw std::runtime_error("window closed before requested evidence completed");
        device->WaitIdle();
        pool.Clear();
        for (auto handle : readbacks)
            device->Destroy(handle);
        for (auto handle : begin)
            device->Destroy(handle);
        for (auto handle : end)
            device->Destroy(handle);
    }
    catch (...)
    {
        // 失败进程不再复用 device；先保留首次失败帧证据，再由设备析构回收。
        // 诊断/落盘的次生异常不能覆盖触发失败的原始异常。
        if (!directory.empty())
        {
            try
            {
                Save(directory / "failed-semantic-trace.txt", native.SemanticTrace());
                Save(directory / "failed-native-trace.txt", native.NativeReport().trace);
            }
            catch (...)
            {
            }
        }
        throw;
    }
    device->Destroy(chain);
    device->WaitIdle();
    const auto facts = native.NativeReport();
    const auto capabilities = device->Capabilities();
    native.Shutdown();
    const auto final = native.NativeReport(true);
    if (!directory.empty())
        Save(directory / "native-trace.txt", facts.trace + final.trace);
    if (facts.warningErrors || final.warningErrors || final.liveResources || device->Diagnostics().aliveObjects ||
        device->Diagnostics().retiringObjects)
        throw std::runtime_error("native diagnostic or resource retirement gate failed");
    event("complete-clean-exit", rendered, total);
    if (!directory.empty())
    {
        Save(directory / "frames.csv", rows.str());
        Save(directory / "diagnostics.csv", diagnosticsRows.str());
        if (pixels)
        {
            std::ofstream ppm(directory / "color.ppm", std::ios::binary);
            ppm << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
            for (std::uint32_t y = 0; y < extent.height; ++y)
                for (std::uint32_t x = 0; x < extent.width; ++x)
                    ppm.write(reinterpret_cast<const char*>(pixels->bytes.data() + y * pixels->rowPitch + x * 4), 3);
            if (!ppm)
                throw std::runtime_error("PPM write failed");
        }
        // metadata 写出在下方；所有身份来自当前 packet、实际资产和当前执行结果。
        std::ostringstream metadata;
        metadata << std::boolalpha << std::setprecision(17);
        metadata << "{\n\"schemaVersion\":1,\"renderer\":\"rhi\","
                 << "\"backend\":" << Quoted(ToString(options.backend)) << ','
                 << "\"scene\":\"m4-visual-baseline\",\"iblEnabled\":true,"
                 << "\"buildType\":" << Quoted(M610_BUILD_CONFIG) << ','
                 << "\"sourceCommit\":" << Quoted(M610_SOURCE_COMMIT) << ',' << "\"width\":" << extent.width
                 << ",\"height\":" << extent.height << ',' << "\"resolution\":{\"width\":" << extent.width
                 << ",\"height\":" << extent.height << "},"
                 << "\"exposureEv\":" << options.exposure << ','
                 << "\"toneMapper\":\"reinhard\",\"iblProfile\":\"baseline\","
                 << "\"culling\":{\"main\":true,\"shadow\":true},"
                 << "\"skyboxEnabled\":" << (options.migrationLevel >= 5) << ','
                 << "\"shadow\":{\"resolution\":2048,\"depthBias\":0,\"slopeBias\":0.1,\"pcf\":\"3x3\","
                    "\"casts\":true,\"receives\":true},"
                 << "\"debugView\":" << Quoted(std::to_string(options.debugView)) << ',' << "\"debugViewName\":"
                 << Quoted(std::array<const char*, 11>{"Lit", "BaseColor", "Normal", "Metallic", "Roughness", "AO",
                                                       "Direct", "IBL", "ShadowFactor", "Unknown",
                                                       "SceneLuminance"}[options.debugView])
                 << ',' << "\"fixedTick\":" << rendered << ",\"migrationLevel\":" << options.migrationLevel << ','
                 << "\"assetManifestSha256\":" << Quoted(Assets::ToHexDigest(scene.assets.ActiveManifestDigest()))
                 << ',' << "\"environmentArtifactSha256\":" << Quoted(scene.environmentHash) << ','
                 << "\"shaderSemanticSha256\":"
                 << Quoted(Samples::ShaderSemanticHash(ShaderSemanticRoot(ToString(options.backend)), std::string(ToString(options.backend))))
                 << ',' << "\"visibleSequenceHash\":" << Quoted(PacketHash(lastPacket)) << ','
                 << "\"cameraValues\":" << Quoted(Samples::CameraIdentity(lastPacket)) << ','
                 << "\"lightValues\":" << Quoted(Samples::LightIdentity(lastPacket)) << ','
                 << "\"graphHash\":" << Quoted(Hex(graphStats.planHash)) << ','
                 << "\"commandHash\":" << Quoted(Sha(lastTrace)) << ',' << "\"warningErrors\":" << facts.warningErrors
                 << ",\"fallbackUsed\":false,"
                 << "\"gpu\":" << Quoted(capabilities.adapterName)
                 << ",\"driver\":" << Quoted(capabilities.driverVersion) << ',' << "\"warp\":" << options.warp
                 << ",\"debugLayer\":" << capabilities.debugLayerEnabled << ','
                 << "\"gpuValidation\":" << capabilities.gpuValidationEnabled
                 << ",\"captureToolActive\":" << captureToolActive << ",\"vsync\":" << (options.vsync ? "true" : "false")
                 << ','
                 << "\"windowForeground\":" << measuredWindowForeground << ','
                 << "\"dredRequested\":" << (options.backend == RhiBackend::D3D12 && options.debug) << ','
                 << "\"benchmark\":" << options.benchmark << ",\"warmupFrames\":" << options.warmup
                 << ",\"measuredFrames\":" << (options.benchmark ? options.measure : 0) << ','
                 << "\"visibleCount\":" << lastPacket.stats.mainVisible << ','
                 << "\"opaqueDrawCalls\":" << (options.migrationLevel >= 4 ? lastPacket.stats.opaqueDrawCalls : 0)
                 << ','
                 << "\"shadowDrawCalls\":" << (options.migrationLevel >= 6 ? lastPacket.stats.shadowDrawCalls : 0)
                 << ',' << "\"trianglesSubmitted\":" << lastPacket.stats.trianglesSubmitted << ','
                 << "\"reloadSuccess\":" << reloadSuccess << ",\"reloadRejected\":" << reloadRejected
                 << ",\"resizeCount\":" << resizeCount << ",\"rhiCommandCount\":" << lastCommandCount << "\n}\n";
        Save(directory / "metadata.json", metadata.str());
    }
    std::cout << "{\"status\":\"PASS\",\"frames\":" << rendered << ",\"graphHash\":" << Quoted(Hex(graphStats.planHash))
              << ",\"output\":" << Quoted(directory.string()) << "}\n";
    return 0;
}
} // namespace MiniEngine::Sandbox
