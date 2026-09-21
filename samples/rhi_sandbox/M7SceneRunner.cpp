// ============================================================================
// M7SceneRunner.cpp — M7 性能场景的串行基线运行器
// 里程碑：M7-01（性能契约与串行基线）
// 职责：把 m7-cpu-scale / m7-layout / m7-streaming 三个固定场景跑成可复算的 raw
//       证据：固定相机路径 + 固定 seed + 固定分辨率与 VSync off，逐帧采样
//       BenchmarkRun schema 的全量指标，并写出 packet/graph/command/screenshot hash。
//       串行边界：本文件不含 task system、chunk 合并与并行 RenderPacket 构建——
//       那些是 M7-03/05 的范围；这里冻结的是它们之前必须存在的比较基线。
// 关联：docs/architecture/README.md
//       tools/benchmark/include/MiniEngine/Benchmark/BenchmarkSchema.h
//       samples/rhi_sandbox/M6SceneRunner.cpp（同一渲染路径的 M6 版本；本文件不复用
//       其场景代码，避免把 M6 冻结的取证行为卷入性能路径）
// ============================================================================

#include "M7SceneRunner.h"
#include "CaptureScopes.h"
#include "M604ShaderFixtures.h"
#include "NativeDevice.h"
#include "RhiUploadSink.h"
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Assets/AssetUploadCoordinator.h>
#include <MiniEngine/Assets/AsyncAssetLoader.h>
#include <MiniEngine/Assets/CookedFileSource.h>
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Assets/TaskSystemDecodeDispatcher.h>
#include <MiniEngine/Assets/UploadBudget.h>
#include <MiniEngine/Assets/UploadSink.h>
#include <MiniEngine/Benchmark/BenchmarkJson.h>
#include <MiniEngine/Benchmark/BenchmarkSchema.h>
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/ProcessMemoryWin32.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Profiling/Profile.h>
#include <MiniEngine/Render/M6SceneResources.h>
#include <MiniEngine/Render/RenderPacketBuilder.h>
#include <MiniEngine/RenderGraph/TransientResourcePool.h>
#include <MiniEngine/Rhi/RhiFactory.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <MiniEngine/Tasks/TaskSystem.h>
#include <MiniEngine/World/RenderProxyLayouts.h>
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <MiniEngine/World/World.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <pix3.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace MiniEngine::Sandbox
{
namespace
{
using namespace Rhi;
namespace RG = RenderGraph;
namespace BM = Benchmark;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSceneLevel = 9;   // 与 M6 基线相同的 pass 集合（Shadow..ToneMap）
constexpr std::uint32_t kGpuQueryRing = 6; // 每帧一对 timestamp；3 帧后回读，留 3 帧余量
constexpr std::uint32_t kGpuQueryDelay = 3;
// 渲染子集上限（场景定义的一部分，写入 raw JSON 的 renderDrawLimit/shadowDrawLimit）：
// 统计与 packet hash 覆盖全部代理，实际提交给 M6 渲染路径的 draw 取稳定排序前缀。
constexpr std::uint32_t kRenderDrawLimit = 256;
constexpr std::uint32_t kShadowDrawLimit = 64;
// Debug 抽样 serial shadow build（M7-05 第 6 步）：每次并行构建后在主线程跑一次串行
// oracle 并逐字段比对，差异立即抛出（消息含第一个差异的 entityIndex）。profile/Release
// 路径关闭：性能证据不承担 oracle 的额外成本。
#ifdef _DEBUG
constexpr bool kDebugOracleValidation = true;
#else
constexpr bool kDebugOracleValidation = false;
#endif
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

double Ms(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string Hex(std::uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string ToHexLower(std::span<const std::byte> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte value : bytes)
    {
        const auto byte = std::to_integer<unsigned char>(value);
        result += kDigits[byte >> 4];
        result += kDigits[byte & 0x0F];
    }
    return result;
}

std::string Sha256DigestHex(std::span<const std::byte> bytes)
{
    Assets::Sha256Builder builder;
    Assets::Sha256Digest digest{};
    if (!builder.Append(bytes) || !builder.Finish(digest))
        throw std::runtime_error("SHA-256 computation failed");
    return Assets::ToHexDigest(digest);
}

std::vector<std::byte> ReadFileBytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() <= 0)
        throw std::runtime_error("cannot open or empty file: " + path.string());
    const auto size = static_cast<std::size_t>(stream.tellg());
    std::vector<std::byte> bytes(size);
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read file: " + path.string());
    return bytes;
}

std::string Sha256File(const fs::path& path)
{
    const std::vector<std::byte> bytes = ReadFileBytes(path);
    return Sha256DigestHex(bytes);
}

std::string UtcNow()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_s(&utc, &now);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string ExecutablePath()
{
    std::array<wchar_t, 32768> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length == module.size())
        throw std::runtime_error("executable path unavailable");
    return fs::path(std::wstring(module.data(), length)).string();
}

std::string HostName()
{
    wchar_t buffer[256]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameW(buffer, &size))
        return {};
    return fs::path(std::wstring(buffer, size)).string();
}

// 资产身份在 raw JSON 中只用低 64 位十六进制数值表示（跨平台稳定；完整身份见 manifest）。
std::uint64_t AssetIdNumber(const Assets::AssetId& id)
{
    std::uint64_t value = 0;
    std::memcpy(&value, id.bytes.data(), sizeof(value));
    return value;
}

// splitmix64：固定 seed → 固定分布；场景几何不依赖 STL 实现或容器顺序。
std::uint64_t NextRandom(std::uint64_t& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double UnitRandom(std::uint64_t& state)
{
    return static_cast<double>(NextRandom(state) >> 11) * (1.0 / 9007199254740992.0);
}

// ---------------------------------------------------------------------------
// 固定二进制输入：相机路径与流式请求脚本
// 格式定义见 assets/tests/m7/README.md；生成器 tools/assets/gen_m7_scene_inputs.py。
// 读取端做严格长度校验：格式漂移必须显式失败，不能容忍截断或额外字段。
// ---------------------------------------------------------------------------
struct CameraKey final
{
    World::Matrix4 view{};
    World::Matrix4 projection{};
    World::Float3 position{};
};

struct CameraPath final
{
    std::vector<CameraKey> keys;
    std::string sha256;

    [[nodiscard]] const CameraKey& At(std::uint32_t frame) const
    {
        if (keys.empty())
            throw std::runtime_error("camera path has no keys");
        if (keys.size() == 1)
            return keys.front();
        return keys[(frame - 1) % keys.size()];
    }
};

CameraPath LoadCameraPath(const fs::path& path)
{
    const std::vector<std::byte> bytes = ReadFileBytes(path);
    constexpr std::size_t kHeader = 16;
    constexpr std::size_t kKeyBytes = 16 * 4 * 2 + 4 * 4;
    if (bytes.size() < kHeader)
        throw std::runtime_error("camera path header is truncated: " + path.string());
    const auto magic = std::string_view(reinterpret_cast<const char*>(bytes.data()), 8);
    if (magic != std::string_view("MECAM7\0\0", 8))
        throw std::runtime_error("camera path magic mismatch: " + path.string());
    std::uint32_t version = 0, count = 0;
    std::memcpy(&version, bytes.data() + 8, 4);
    std::memcpy(&count, bytes.data() + 12, 4);
    if (version != 1 || count == 0)
        throw std::runtime_error("camera path schema is not v1 or has no keys");
    if (bytes.size() != kHeader + kKeyBytes * count)
        throw std::runtime_error("camera path size does not match key count");

    CameraPath result;
    result.sha256 = Sha256DigestHex(bytes);
    result.keys.resize(count);
    const std::byte* cursor = bytes.data() + kHeader;
    for (CameraKey& key : result.keys)
    {
        std::memcpy(key.view.values.data(), cursor, 16 * 4);
        cursor += 16 * 4;
        std::memcpy(key.projection.values.data(), cursor, 16 * 4);
        cursor += 16 * 4;
        // 只复制 3 个 float；文件里第 4 个 float 是 reserved，必须跳过而不是写进
        // 12 字节的 Float3（曾经因此把 4 字节写到结构外并破坏堆）。
        std::memcpy(&key.position, cursor, 3 * 4);
        cursor += 4 * 4;
    }
    return result;
}

struct StreamRequest final
{
    std::uint32_t frame = 0; // 相对测量窗口的 1 基帧号
    std::uint32_t priority = 0;
    std::uint64_t expectedBytes = 0;
    std::string uri;
};

struct StreamScript final
{
    std::vector<StreamRequest> requests;
    std::string sha256;
};

StreamScript LoadStreamScript(const fs::path& path)
{
    const std::vector<std::byte> bytes = ReadFileBytes(path);
    constexpr std::size_t kHeader = 24;
    if (bytes.size() < kHeader)
        throw std::runtime_error("stream script header is truncated: " + path.string());
    const auto magic = std::string_view(reinterpret_cast<const char*>(bytes.data()), 8);
    if (magic != std::string_view("MESTR7\0\0", 8))
        throw std::runtime_error("stream script magic mismatch: " + path.string());
    std::uint32_t version = 0, count = 0;
    std::memcpy(&version, bytes.data() + 8, 4);
    std::memcpy(&count, bytes.data() + 12, 4);
    if (version != 1 || count == 0)
        throw std::runtime_error("stream script schema is not v1 or has no requests");

    StreamScript result;
    result.sha256 = Sha256DigestHex(bytes);
    std::size_t offset = kHeader;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        if (offset + 20 > bytes.size())
            throw std::runtime_error("stream script request header is truncated");
        StreamRequest request;
        std::memcpy(&request.frame, bytes.data() + offset, 4);
        std::memcpy(&request.priority, bytes.data() + offset + 4, 4);
        std::memcpy(&request.expectedBytes, bytes.data() + offset + 8, 8);
        std::uint32_t uriLength = 0;
        std::memcpy(&uriLength, bytes.data() + offset + 16, 4);
        offset += 20;
        if (uriLength == 0 || offset + uriLength > bytes.size())
            throw std::runtime_error("stream script URI is truncated");
        request.uri.assign(reinterpret_cast<const char*>(bytes.data() + offset), uriLength);
        offset += uriLength;
        result.requests.push_back(std::move(request));
    }
    if (offset != bytes.size())
        throw std::runtime_error("stream script has trailing bytes");
    return result;
}

// ---------------------------------------------------------------------------
// 资产库：M7 场景复用 M4 冻结 manifest 的网格/材质/环境产物。
// 选择规则固定（registry 按 AssetId 字节序，取第一个 mesh/material），因此
// 同一 manifest 在任何机器上得到同一份场景输入。
// ---------------------------------------------------------------------------
class AssetLibrary final
{
  public:
    Assets::AssetManager assets;
    Assets::AssetHandle<Assets::MeshAsset> mesh;
    Assets::AssetHandle<Assets::MaterialAsset> material;
    Assets::AssetId meshId{};
    Assets::AssetId materialId{};
    std::string manifestSha256;

    explicit AssetLibrary(const fs::path& manifest)
    {
        manifestSha256 = Sha256File(manifest);
        std::string error;
        if (!assets.PrepareManifestLoad(manifest, error))
            throw std::runtime_error(error);
        assets.CommitPending();
        assets.ApplyPendingRemovals();
        const auto* registry = assets.Registry();
        if (!registry)
            throw std::runtime_error("manifest registry missing");

        for (std::size_t index = 0; index < registry->EntryCount(); ++index)
        {
            const auto* entry = registry->EntryAt(index);
            if (!entry)
                continue;
            if (entry->kind == Assets::AssetKind::Mesh && !mesh.IsValid())
            {
                meshId = entry->id;
                const auto found = assets.Meshes().TryFind(meshId);
                if (found)
                    mesh = *found;
            }
            else if (entry->kind == Assets::AssetKind::Material && !material.IsValid())
            {
                materialId = entry->id;
                const auto found = assets.Materials().TryFind(materialId);
                if (found)
                    material = *found;
            }
            if (mesh.IsValid() && material.IsValid())
                break;
        }
        if (!mesh.IsValid() || !material.IsValid())
            throw std::runtime_error("M4 manifest does not expose a mesh/material pair");
    }
};

// ---------------------------------------------------------------------------
// IBL 环境：与 M6SceneRunner 的 SceneEnvironment 同一构造顺序（panorama 上传 →
// PrepareRhiEnvironment）。两个运行器各自持有副本，避免 M7 改动触碰 M6 的冻结取证路径。
// ---------------------------------------------------------------------------
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
        desc.debugName = "M7.EnvironmentPanorama";
        panorama = device.CreateTexture(desc);
        try
        {
            std::vector<TextureSubresourceData> levels;
            for (const auto& mip : texture.mips)
                levels.push_back({std::span(texture.pixels).subspan(static_cast<std::size_t>(mip.offset), mip.byteSize),
                                  mip.rowPitch, mip.byteSize});
            device.UploadTexture(panorama, levels);
            const auto shaderRoot = fs::path(ExecutablePath()).parent_path() / "shaders";
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
        // 失败帧的清理在栈展开期间发生：这里吞掉次生 destroy 异常，保留首因。
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
            std::cerr << "M7SceneRunner: secondary environment destroy failure: " << error.what() << std::endl;
        }
    }
};

void RollPacketHash(std::uint64_t& rolling, const World::RenderPacket& packet);

// ---------------------------------------------------------------------------
// 过程化场景：N 个代理共用 manifest 中的一个 mesh + material。
// 位置分布（视轴前方、横向范围与深度成正比）使固定视锥的可见率与深度近似无关；
// `visibleRatioTarget` 只是输入目标，实际可见率由 culling 统计逐帧记录。
// ---------------------------------------------------------------------------
struct BuildTimings final
{
    double fixedUpdateMs = 0.0;
    double worldExtractMs = 0.0;
    double packetBuildMs = 0.0;
    double cullMs = 0.0;
    double packetMergeMs = 0.0; // 并行专属：chunk 合并（串行保持 0）
    double packetWaitMs = 0.0;  // 并行专属：Wait（含 main-help）
    double packetSortMs = 0.0;  // 串行=单次排序；并行=合并后的全局排序
    // M7-08：布局内核（提取 + 分类扫描）耗时；未做布局实验时保持 0 并列入 unavailable。
    double layoutCullMs = 0.0;
};

// 一次 Build 的结果：`renderPacket` 是交给渲染器的**有界子集**（稳定排序后的前缀），
// `fullStats` 是全量代理的 culling 结果。两者必须同时记录，否则"渲染了 256 个物体"
// 会被误读成"只有 256 个物体可见"。
struct BuildResult final
{
    World::RenderPacket renderPacket;
    World::CullingStats fullStats;
    BuildTimings timings;
    // 并行构建的阶段统计（serial 路径保持零值，对应字段列入 unavailableMetrics）。
    Render::RenderPacketBuildStats packetStats;
};

class ProceduralScene final
{
  public:
    struct Config final
    {
        std::uint32_t entityCount = 0;
        float visibleRatioTarget = 0.5F;
        float updateRatio = 1.0F;
        std::uint32_t seed = 0;
        // 渲染子集上限：M6 渲染路径的每 draw 成本（逐 draw 动态常量 + 帧资源集 +
        // 图导入）在 50k 代理规模下会主导整帧，使 culling/packet 无从测量。场景因此
        // 冻结"全量 culling + 有界渲染前缀"：统计与 hash 覆盖全部代理，渲染与命令
        // 证据只取稳定排序后的前 N 个（确定性，可复算）。RHI 侧的逐 draw 扩展
        // 属于 M7-05 的 D3D12 可选实验，不是本基线的被测对象。
        std::uint32_t renderDrawLimit = 256;
        std::uint32_t shadowDrawLimit = 64;
        Assets::AssetHandle<Assets::MeshAsset> mesh;
        Assets::AssetHandle<Assets::MaterialAsset> material;
        Assets::AssetId meshId{};
        Assets::AssetId materialId{};
        // M7-05：非空时走并行路径（chunk 划分 + TaskSystem），空时保持 M7-01 串行路径。
        // 两条路径产出同一 RenderPacket 语义（Debug 下逐帧由串行 oracle 校验）。
        Render::RenderPacketBuilder* packetBuilder = nullptr;
        Render::RenderPacketBuildConfig packetConfig{};
        // M7-08：布局实验（未指定 --layout 时整个布局路径不参与，既有证据不受影响）：
        // 每帧在**同一份 RenderItem 快照**上额外运行指定布局的剔除内核并计时；
        // 生产 packet 构建路径不变，因此跨布局比较是单变量对照。
        bool layoutExperiment = false;
        std::string layout = "aos";
        // Release 下按固定间隔做等价校验（Debug 每帧）；不一致即抛错，不静默继续。
        std::uint32_t layoutCheckInterval = 1;
    };

    ProceduralScene(Assets::AssetManager& assets, const Config& config)
        : m_config(config), m_mesh(config.mesh), m_material(config.material), m_meshId(config.meshId),
          m_materialId(config.materialId)
    {
        if (config.entityCount == 0)
            throw std::runtime_error("procedural scene requires a positive entity count");
        if (!config.mesh.IsValid() || !config.material.IsValid())
            throw std::runtime_error("procedural scene requires valid mesh/material handles");
        const auto view = assets.Meshes().TryGet(m_mesh);
        if (!view)
            throw std::runtime_error("procedural scene mesh payload is unavailable");
        const auto bounds = World::ComputeMeshLocalBounds(*view->asset);
        if (!bounds || bounds->radius <= 0.0F)
            throw std::runtime_error("procedural scene mesh bounds are invalid");
        m_localBounds = *bounds;
        m_indexCount = view->asset->indexCount;
        m_scale = 0.5F / std::max(m_localBounds.radius, 1e-4F);

        m_entities.reserve(m_config.entityCount);
        m_basePositions.reserve(m_config.entityCount);
        std::uint64_t rng = m_config.seed == 0 ? 0x9E3779B97F4A7C15ULL : m_config.seed;
        for (std::uint32_t index = 0; index < m_config.entityCount; ++index)
        {
            const double depth = 12.0 + UnitRandom(rng) * 78.0;
            // 视锥在深度 d 处的半宽为 tan(fovX/2)·d = 1.026·d（fovY=60°、16:9），
            // 半高为 0.577·d；分布用横向半宽 k·d、半高 k·d/1.7778 = 0.5625·k·d，
            // 两轴比例一致，因此可见率 ≈ (1.026/k)²。k=1.45 → 约 50%。
            const double lateral = 1.45 * depth;
            const World::Float3 position{static_cast<float>((UnitRandom(rng) * 2.0 - 1.0) * lateral),
                                         static_cast<float>(3.0 + (UnitRandom(rng) * 2.0 - 1.0) * lateral / 1.7777778),
                                         static_cast<float>(-10.0 + depth)};
            const auto entity = m_world.CreateEntity();
            World::MeshRendererComponent renderer;
            renderer.mesh = m_mesh;
            renderer.material = m_material;
            renderer.visible = true;
            // 2% 代理投影：shadow pass 参与但不淹没主队列（场景定义冻结在 recipe）。
            renderer.castsShadow = (index % 50U) == 0U;
            renderer.receivesShadow = true;
            if (!m_world.SetLocalTrs(entity, position, World::Quaternion{}, {m_scale, m_scale, m_scale}) ||
                !m_world.SetMeshRenderer(entity, renderer))
                throw std::runtime_error("procedural scene entity setup failed");
            m_entities.push_back(entity);
            m_basePositions.push_back(position);
        }
    }

    [[nodiscard]] std::uint32_t EntityCount() const noexcept
    {
        return m_config.entityCount;
    }

    [[nodiscard]] float Scale() const noexcept
    {
        return m_scale;
    }

    // 固定更新：按 updateRatio 旋转窗口更新变换，让 dirty 传播成为可测成本。
    void Advance(std::uint32_t frame)
    {
        if (m_config.updateRatio <= 0.0F)
            return;
        const std::uint32_t count = m_config.entityCount;
        const auto updateCount =
            std::max(1U, static_cast<std::uint32_t>(std::lround(m_config.updateRatio * static_cast<float>(count))));
        const std::uint32_t start =
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(frame) * 2654435761ULL) % count);
        for (std::uint32_t i = 0; i < updateCount; ++i)
        {
            const std::uint32_t index = (start + i) % count;
            const World::Float3& base = m_basePositions[index];
            const float bob = ((frame + index) % 4U) == 0U ? 0.01F : 0.0F;
            if (!m_world.SetLocalTrs(m_entities[index], {base.x, base.y + bob, base.z}, World::Quaternion{},
                                     {m_scale, m_scale, m_scale}))
                throw std::runtime_error("procedural scene advance failed");
        }
    }

    // 全量 culling → 全量 packet hash → 有界渲染子集。全量 packet 只在本函数内存活，
    // 避免每帧复制 50k 级别的 draw 列表。
    BuildResult Build(const CameraKey& camera, std::uint32_t frame, std::uint64_t& rollingPacketHash)
    {
        const auto fixedStart = Clock::now();
        {
            ME_PROFILE_ZONE_NAMED("FixedUpdate");
            Advance(frame);
        }
        const auto fixedEnd = Clock::now();
        // WorldSnapshot：冻结 world→item 快照（UpdateTransforms + 确定性 extract）。
        World::RenderPacket full;
        {
            ME_PROFILE_ZONE_NAMED("WorldSnapshot");
            m_world.UpdateTransforms();
            m_items = m_world.BuildRenderItems();
        }
        const auto extractEnd = Clock::now();
        const World::RenderPacketCamera packetCamera{camera.view, camera.projection, camera.position};
        // 同一份 lookup 供三条路径使用：生产串行/并行与 M7-08 的布局内核。
        const auto meshLookup =
            [this](Assets::AssetHandle<Assets::MeshAsset> handle) -> std::optional<World::MeshDrawInfo>
        {
            if (handle == m_mesh)
                return World::MeshDrawInfo{m_meshId, m_localBounds, m_indexCount};
            return std::nullopt; // 未知句柄保守视为恒可见，不静默丢物体
        };
        const auto materialLookup =
            [this](Assets::AssetHandle<Assets::MaterialAsset> handle) -> std::optional<Assets::AssetId>
        { return handle == m_material ? std::optional<Assets::AssetId>(m_materialId) : std::nullopt; };
        BuildResult result;
        {
            ME_PROFILE_ZONE_NAMED("RenderPacketBuild");
            if (m_config.packetBuilder != nullptr)
            {
                full =
                    m_config.packetBuilder->BuildParallel(m_items, packetCamera, m_light, meshLookup, {},
                                                          materialLookup, m_config.packetConfig, &result.packetStats);
            }
            else
            {
                full = World::BuildRenderPacket(m_items, packetCamera, m_light, meshLookup, {}, materialLookup);
            }
        }
        const auto packetEnd = Clock::now();

        result.fullStats = full.stats;
        result.timings.fixedUpdateMs = Ms(fixedStart, fixedEnd);
        result.timings.worldExtractMs = Ms(fixedEnd, extractEnd);
        result.timings.packetBuildMs = Ms(extractEnd, packetEnd);
        result.timings.cullMs = full.stats.cullingCpuMicroseconds / 1000.0;
        result.timings.packetMergeMs = result.packetStats.mergeCpuMicroseconds / 1000.0;
        result.timings.packetWaitMs = result.packetStats.waitCpuMicroseconds / 1000.0;
        // 并行：只统计合并后的全局排序（chunk 内部排序属于 cull/extract 并行段）；
        // 串行：单次排序，与 M7-01 口径一致。
        result.timings.packetSortMs = m_config.packetBuilder != nullptr
                                          ? result.packetStats.sortCpuMicroseconds / 1000.0
                                          : full.stats.sortCpuMicroseconds / 1000.0;

        // hash 覆盖全量可见集合（含被渲染上限截掉的部分）：culling 或排序的漂移
        // 即使只发生在前缀之外也必须被发现。
        RollPacketHash(rollingPacketHash, full);

        // M7-08 布局实验：同一份 RenderItem 快照 + 同一对 lookup → 规范代理 → 指定布局
        // 的剔除内核。分布/冻结项与生产完全一致（同一 frustum、同一分类规则、同一顺序）。
        if (m_config.layoutExperiment)
        {
            if (!m_layoutReady)
            {
                const std::vector<World::LayoutProxySource> proxies =
                    World::BuildLayoutProxies(m_items, meshLookup, materialLookup);
                m_aos.reserve(proxies.size());
                for (const World::LayoutProxySource& source : proxies)
                {
                    m_aos.push_back(World::ToAoS(source));
                }
                m_hotCold = World::ToHotCold(proxies);
                m_soa = World::ToSoA(proxies);
                m_proxyCount = proxies.size();
                m_proxies = proxies;
                m_layoutReady = true;
            }
            // 逐帧把**当前** RenderItem 快照的可变字段刷进规范代理（身份/包围球是静态的，
            // 只在首次构建时取一次）。这一步在计时区外：内核只承担扫描 + 分类。
            ME_ASSERT(m_proxies.size() == m_items.size(), "layout proxies must track the item snapshot");
            for (std::size_t index = 0; index < m_proxies.size(); ++index)
            {
                m_proxies[index].world = m_items[index].world;
                m_proxies[index].mirrored = m_items[index].mirrored;
                m_proxies[index].castsShadow = m_items[index].castsShadow;
                m_proxies[index].receivesShadow = m_items[index].receivesShadow;
            }
            World::RefreshPerFrameFields(m_proxies, m_hotCold);
            World::RefreshPerFrameFields(m_proxies, m_soa);
            for (std::size_t index = 0; index < m_aos.size(); ++index)
            {
                World::RefreshPerFrameFields(m_proxies[index], m_aos[index]);
            }

            // 直接复用生产 packet 的 viewProjection：布局内核与生产 culling 必须用同一组平面。
            const World::Frustum cameraFrustum = World::Frustum::FromRowVectorDirect3D(full.viewProjection);
            const World::Frustum lightFrustum =
                World::Frustum::FromRowVectorDirect3D(World::BuildLightViewProjection(m_light));
            const World::LayoutCullResult* selected = nullptr;
            World::LayoutCullResult aosResult;
            World::LayoutCullResult hotColdResult;
            World::LayoutCullResult soaResult;
            if (m_config.layout == "aos")
            {
                aosResult = World::CullAoS(m_aos, cameraFrustum, lightFrustum);
                selected = &aosResult;
            }
            else if (m_config.layout == "hot-cold")
            {
                hotColdResult = World::CullHotCold(m_hotCold, cameraFrustum, lightFrustum);
                selected = &hotColdResult;
            }
            else if (m_config.layout == "soa")
            {
                soaResult = World::CullSoA(m_soa, cameraFrustum, lightFrustum);
                selected = &soaResult;
            }
            else
            {
                // M7-LAYOUT-SIMD：批量化 SoA 变体（块级 early-out；结果必须与 soa 逐字节相同，
                // 由同帧的 CompareLayoutResultsAgainstPacket 与等价测试共同守护）。
                soaResult = World::CullSoABatched(m_soa, cameraFrustum, lightFrustum);
                selected = &soaResult;
            }
            result.timings.layoutCullMs = selected->scanCpuMicroseconds / 1000.0;
            m_layoutSemanticHash = selected->semanticHash;

            // 等价校验：与生产 packet 逐项对照（Debug 每帧；Release 按间隔采样）。
            if ((m_layoutCheckCounter++ % m_config.layoutCheckInterval) == 0U)
            {
                const std::string diff = World::CompareLayoutResultsAgainstPacket(*selected, full);
                ++m_layoutCheckFrames;
                if (!diff.empty())
                {
                    ++m_layoutCheckMismatches;
                    throw std::runtime_error("layout kernel differs from production packet: " + diff +
                                             " (layout=" + m_config.layout + ")");
                }
            }
        }

        result.renderPacket.view = full.view;
        result.renderPacket.projection = full.projection;
        result.renderPacket.viewProjection = full.viewProjection;
        result.renderPacket.cameraWorldPosition = full.cameraWorldPosition;
        result.renderPacket.directionalLight = full.directionalLight;
        result.renderPacket.stats = full.stats;
        const auto opaqueCount = std::min<std::size_t>(m_config.renderDrawLimit, full.mainOpaque.size());
        const auto shadowCount = std::min<std::size_t>(m_config.shadowDrawLimit, full.shadowCasters.size());
        result.renderPacket.mainOpaque.assign(full.mainOpaque.begin(), full.mainOpaque.begin() + opaqueCount);
        result.renderPacket.shadowCasters.assign(full.shadowCasters.begin(), full.shadowCasters.begin() + shadowCount);
        return result;
    }

    // ---- M7-08 布局实验的取证（run record 用） ----
    [[nodiscard]] std::string LayoutSemanticHashHex() const
    {
        std::ostringstream stream;
        stream << "0x" << std::uppercase << std::hex << m_layoutSemanticHash;
        return stream.str();
    }
    [[nodiscard]] std::uint32_t LayoutCheckFrames() const noexcept
    {
        return m_layoutCheckFrames;
    }
    [[nodiscard]] std::uint32_t LayoutCheckMismatches() const noexcept
    {
        return m_layoutCheckMismatches;
    }
    [[nodiscard]] std::size_t LayoutProxyCount() const noexcept
    {
        return m_proxyCount;
    }
    // 逐实体字节账（run-notes 记录；不进 schema：它是布局的静态属性，不是逐帧指标）。
    [[nodiscard]] std::size_t LayoutAosBytesPerEntity() const noexcept
    {
        return m_aos.empty() ? 0 : sizeof(World::RenderProxyAoS);
    }
    [[nodiscard]] std::size_t LayoutSoaScanBytesPerEntity() const noexcept
    {
        return m_soa.ScanBytesPerEntity();
    }
    [[nodiscard]] std::size_t LayoutHotColdColdBytesPerEntity() const noexcept
    {
        return m_hotCold.Size() == 0 ? 0 : sizeof(World::RenderProxyCold);
    }

  private:
    Config m_config;
    World::World m_world;
    Assets::AssetHandle<Assets::MeshAsset> m_mesh;
    Assets::AssetHandle<Assets::MaterialAsset> m_material;
    Assets::AssetId m_meshId{};
    Assets::AssetId m_materialId{};
    World::Sphere m_localBounds{};
    std::uint32_t m_indexCount = 0;
    float m_scale = 1.0F;
    std::vector<World::Entity> m_entities;
    std::vector<World::Float3> m_basePositions;
    std::vector<World::RenderItem> m_items;
    World::DirectionalLight m_light{};
    // M7-08：布局集（首次 Build 时按当前身份/包围球构建，之后只刷新逐帧字段）。
    std::vector<World::LayoutProxySource> m_proxies;
    std::vector<World::RenderProxyAoS> m_aos;
    World::RenderProxyHotCold m_hotCold;
    World::RenderProxySoA m_soa;
    std::size_t m_proxyCount = 0;
    bool m_layoutReady = false;
    std::uint64_t m_layoutSemanticHash = 0;
    std::uint32_t m_layoutCheckFrames = 0;
    std::uint32_t m_layoutCheckMismatches = 0;
    std::uint32_t m_layoutCheckCounter = 0;
};

// ---------------------------------------------------------------------------
// 流式请求的串行执行：按脚本帧号在请求帧内完成"阻塞读 + 内容哈希校验"。
// 这是 M7-06 异步流水线（I/O 线程 → decode task → render thread upload）在
// 串行基线下的等价物；loaderMode 记录为 serial-sync-read-validate。
// ---------------------------------------------------------------------------
class SyncStreamingRequests final
{
  public:
    SyncStreamingRequests(const Assets::AssetManager& assets, fs::path outputRoot, const StreamScript& script)
        : m_assets(assets), m_root(std::move(outputRoot))
    {
        for (std::size_t index = 0; index < script.requests.size(); ++index)
            m_byFrame[script.requests[index].frame].push_back(index);
        m_requests = script.requests;
    }

    void ExecuteFrame(std::uint32_t relativeFrame, double& ioMs, double& decodeMs,
                      std::vector<BM::AssetRequestSample>& out)
    {
        const auto found = m_byFrame.find(relativeFrame);
        if (found == m_byFrame.end())
            return;
        const auto* registry = m_assets.Registry();
        if (!registry)
            throw std::runtime_error("streaming requires a committed asset registry");
        for (const std::size_t index : found->second)
        {
            const StreamRequest& request = m_requests[index];
            const Assets::AssetId assetId = Assets::DeriveAssetId(request.uri);
            BM::AssetRequestSample sample;
            sample.requestId = static_cast<std::uint64_t>(index) + 1U;
            sample.assetId = AssetIdNumber(assetId);
            sample.requestFrame = relativeFrame;
            sample.priority = request.priority;
            sample.revision = 1;

            const auto requestStart = Clock::now();
            const auto* entry = registry->Find(assetId);
            if (!entry)
            {
                sample.result = "failed:unknown-asset";
                m_failed = true;
                out.push_back(std::move(sample));
                continue;
            }
            const fs::path artifact = m_root / entry->artifactRelativePath;
            std::vector<std::byte> bytes;
            try
            {
                bytes = ReadFileBytes(artifact);
            }
            catch (const std::exception&)
            {
                sample.result = "failed:read";
                m_failed = true;
                out.push_back(std::move(sample));
                continue;
            }
            // 内存观测只在 allocator 边界记一次：这个 owned 读缓冲就是 M7 流式路径的
            // 分配边界；上层容器不会再重复记账。
            ME_PROFILE_ALLOC(bytes.data(), bytes.size(), "M7.StreamRead");
            const auto readEnd = Clock::now();
            const std::string digest = Sha256DigestHex(bytes);
            const auto decodeEnd = Clock::now();
            const bool hashMatches = digest == ToHexLower(entry->artifactHash.bytes);
            const bool sizeMatches = request.expectedBytes == 0 || bytes.size() == request.expectedBytes;
            sample.bytes = bytes.size();
            sample.readMs = Ms(requestStart, readEnd);
            sample.decodeMs = Ms(readEnd, decodeEnd);
            sample.requestToReadyMs = Ms(requestStart, decodeEnd);
            if (!hashMatches || !sizeMatches)
            {
                sample.result = !hashMatches ? "failed:hash" : "failed:size";
                m_failed = true;
            }
            else
            {
                sample.result = "ready";
                m_bytesRead += bytes.size();
                ++m_ready;
            }
            ioMs += sample.readMs;
            decodeMs += sample.decodeMs;
            ME_PROFILE_FREE(bytes.data(), "M7.StreamRead");
            out.push_back(std::move(sample));
        }
    }

    [[nodiscard]] bool Failed() const noexcept
    {
        return m_failed;
    }
    [[nodiscard]] std::uint64_t BytesRead() const noexcept
    {
        return m_bytesRead;
    }
    [[nodiscard]] std::uint32_t ReadyCount() const noexcept
    {
        return m_ready;
    }

  private:
    const Assets::AssetManager& m_assets;
    fs::path m_root;
    std::vector<StreamRequest> m_requests;
    std::map<std::uint32_t, std::vector<std::size_t>> m_byFrame;
    bool m_failed = false;
    std::uint64_t m_bytesRead = 0;
    std::uint32_t m_ready = 0;
};

// ---------------------------------------------------------------------------
// M7-10（A27）长跑稳定性遥测：逐采样点写 stability.csv，结束后由 Json() 进
// stability-summary.json 与 stdout 摘要，用于"句柄/资源/待办回到基线"的判定。
// ---------------------------------------------------------------------------
struct StabilityTelemetry final
{
    std::uint32_t samplingEvery = 0;
    std::vector<std::string> rows;
    std::uint32_t samples = 0;
    std::uint32_t baselineHandles = 0;
    std::uint32_t peakHandles = 0;
    std::uint32_t finalHandles = 0;
    std::uint32_t baselineAliveObjects = 0;
    std::uint32_t finalAliveObjects = 0;
    std::uint32_t peakAliveObjects = 0;
    std::uint32_t peakRetiringObjects = 0;
    std::uint32_t peakLoaderPending = 0;
    std::uint64_t peakTaskPending = 0;
    std::uint64_t residentFirst = 0;
    std::uint64_t residentLast = 0;
    std::uint64_t residentPeak = 0;
    // M7-SOAK-RSS：进程口径的内存（操作系统看到的占用），与上方 residentBytes（瞬态池字节）并列。
    // 前者才能回答"长跑有没有长内存"；后者只是子系统自报。可用性单独记（读取失败不得当作 0）。
    std::uint64_t workingSetFirst = 0;
    std::uint64_t workingSetLast = 0;
    std::uint64_t workingSetPeak = 0;
    std::uint64_t privateBytesFirst = 0;
    std::uint64_t privateBytesLast = 0;
    std::uint64_t privateBytesPeak = 0;
    bool processMemoryAvailable = false;
    std::uint32_t stressEvents = 0; // 触发的稳定性压力事件（零尺寸挂起 / 不同 extent）
    // M7-RESIZE-PATH：真实 extent 变化的次数（跟随窗口 resize 与 resize 压力共同计入）
    // 与访问过的 extent 列表——证明"拖动改变窗口尺寸"这条路径真的被走过，而不只是"没报错"。
    std::uint32_t resizeEvents = 0;
    std::vector<std::string> extentsVisited;
    // M7-SOAK-RSS：非 benchmark 长跑不写 run-notes.json，上传/loader 侧的计数此前在采集产物里
    // **完全不可见**（只能看 stdout）——这里在收尾时把关键计数并入 stability 证据。
    std::uint64_t finalReloadRequests = 0;
    std::uint64_t finalReloadCommits = 0;
    std::uint64_t finalReloadRejected = 0;
    std::uint64_t finalScriptRejected = 0;
    std::uint64_t finalSupersededSamples = 0;
    std::uint64_t finalUploadsStarted = 0;
    std::uint64_t finalUploadsCommitted = 0;
    std::uint64_t finalUploadsFailed = 0;
    std::uint64_t finalDeferredRetires = 0;
    std::uint64_t finalUploadCpuMicros = 0;
    std::uint64_t finalUploadLiveResources = 0;
    std::uint64_t finalLiveRequests = 0;
    // M7-LOAD-RECORDS：记录保留量与被回收/淘汰的计数（证明"记录不随请求数增长"）。
    std::uint64_t finalRetainedRecords = 0;
    std::uint64_t finalRecordsReleased = 0;
    std::uint64_t finalRecordsEvicted = 0;
    std::uint64_t finalReleaseRejectedLive = 0;

    void Sample(const std::uint32_t frame, const std::uint32_t handles, const std::uint32_t aliveObjects,
                const std::uint32_t retiringObjects, const std::uint32_t loaderPending, const std::uint64_t taskPending,
                const std::uint64_t residentBytes, const Platform::Windows::ProcessMemorySnapshot& processMemory)
    {
        if (samples == 0)
        {
            baselineHandles = handles;
            baselineAliveObjects = aliveObjects;
            residentFirst = residentBytes;
            workingSetFirst = processMemory.workingSetBytes;
            privateBytesFirst = processMemory.privateBytes;
        }
        ++samples;
        finalHandles = handles;
        finalAliveObjects = aliveObjects;
        peakHandles = std::max(peakHandles, handles);
        peakAliveObjects = std::max(peakAliveObjects, aliveObjects);
        peakRetiringObjects = std::max(peakRetiringObjects, retiringObjects);
        peakLoaderPending = std::max(peakLoaderPending, loaderPending);
        peakTaskPending = std::max(peakTaskPending, taskPending);
        residentLast = residentBytes;
        residentPeak = std::max(residentPeak, residentBytes);
        workingSetLast = processMemory.workingSetBytes;
        workingSetPeak = std::max(workingSetPeak, processMemory.workingSetBytes);
        privateBytesLast = processMemory.privateBytes;
        privateBytesPeak = std::max(privateBytesPeak, processMemory.privateBytes);
        processMemoryAvailable = processMemoryAvailable || processMemory.available;
        std::ostringstream row;
        row << frame << ',' << handles << ',' << aliveObjects << ',' << retiringObjects << ',' << loaderPending << ','
            << taskPending << ',' << residentBytes << ',' << processMemory.workingSetBytes << ','
            << processMemory.privateBytes << ',' << (processMemory.available ? 1 : 0);
        rows.push_back(row.str());
    }

    [[nodiscard]] std::string Csv() const
    {
        std::ostringstream out;
        out << "frame,handles,aliveObjects,retiringObjects,loaderPending,taskPending,residentBytes,"
               "workingSetBytes,privateBytes,processMemoryAvailable\n";
        for (const std::string& row : rows)
            out << row << '\n';
        return out.str();
    }

    [[nodiscard]] std::string Json() const
    {
        std::ostringstream json;
        json << "{\"samples\":" << samples << ",\"samplingEvery\":" << samplingEvery
             << ",\"baselineHandles\":" << baselineHandles << ",\"peakHandles\":" << peakHandles
             << ",\"finalHandles\":" << finalHandles << ",\"baselineAliveObjects\":" << baselineAliveObjects
             << ",\"peakAliveObjects\":" << peakAliveObjects << ",\"finalAliveObjects\":" << finalAliveObjects
             << ",\"peakRetiringObjects\":" << peakRetiringObjects << ",\"peakLoaderPending\":" << peakLoaderPending
             << ",\"peakTaskPending\":" << peakTaskPending << ",\"residentFirst\":" << residentFirst
             << ",\"residentLast\":" << residentLast << ",\"residentPeak\":" << residentPeak
             << ",\"workingSetFirst\":" << workingSetFirst << ",\"workingSetLast\":" << workingSetLast
             << ",\"workingSetPeak\":" << workingSetPeak << ",\"privateBytesFirst\":" << privateBytesFirst
             << ",\"privateBytesLast\":" << privateBytesLast << ",\"privateBytesPeak\":" << privateBytesPeak
             << ",\"processMemoryAvailable\":" << (processMemoryAvailable ? "true" : "false")
             << ",\"reloadRequests\":" << finalReloadRequests << ",\"reloadCommits\":" << finalReloadCommits
             << ",\"reloadRejected\":" << finalReloadRejected << ",\"scriptRejected\":" << finalScriptRejected
             << ",\"supersededSamples\":" << finalSupersededSamples
             << ",\"uploadsStarted\":" << finalUploadsStarted << ",\"uploadsCommitted\":" << finalUploadsCommitted
             << ",\"uploadsFailed\":" << finalUploadsFailed << ",\"deferredRetires\":" << finalDeferredRetires
             << ",\"uploadCpuMicros\":" << finalUploadCpuMicros
             << ",\"uploadLiveResources\":" << finalUploadLiveResources
             << ",\"liveRequestsAtEnd\":" << finalLiveRequests
             << ",\"retainedRecordsAtEnd\":" << finalRetainedRecords
             << ",\"recordsReleased\":" << finalRecordsReleased
             << ",\"recordsEvicted\":" << finalRecordsEvicted
             << ",\"releaseRejectedLive\":" << finalReleaseRejectedLive
             << ",\"stressEvents\":" << stressEvents
             << ",\"resizeEvents\":" << resizeEvents << ",\"extentsVisited\":[";
        for (std::size_t index = 0; index < extentsVisited.size(); ++index)
        {
            if (index > 0)
                json << ',';
            json << '\"' << extentsVisited[index] << '\"';
        }
        json << "]}";
        return json.str();
    }
};

// ---------------------------------------------------------------------------
// M7-07 异步流式请求：与 SyncStreamingRequests 消费同一份脚本，但请求交给
// engine/assets/async 的流水线（I/O 线程 → decode task → 预算上传）。loaderMode 记录为
// async-budget-pipeline；本类只负责"按帧提交"与"结束时把 loader 记录转成 raw JSON 样本"。
// ---------------------------------------------------------------------------
class AsyncStreamingRequests final
{
  public:
    struct Entry final
    {
        std::uint32_t frame = 0;
        std::uint32_t priority = 0;
        std::uint64_t expectedBytes = 0;
        std::uint64_t assetIdNumber = 0;
        Assets::AssetId assetId{};
        Assets::RequestId requestId = 0;
        std::uint64_t revision = 1; // M7-10：每次热重载 +1（端到端 revision 链）
        bool submitted = false;
        bool knownAsset = true;
    };

    AsyncStreamingRequests(Assets::AsyncAssetLoader& loader, const Assets::AssetRegistry& registry,
                           const StreamScript& script, const std::uint32_t reloadEvery = 0)
        : m_loader(loader), m_reloadEvery(reloadEvery)
    {
        for (std::size_t index = 0; index < script.requests.size(); ++index)
        {
            const StreamRequest& request = script.requests[index];
            Entry entry;
            entry.frame = request.frame;
            entry.priority = request.priority;
            entry.expectedBytes = request.expectedBytes;
            entry.assetId = Assets::DeriveAssetId(request.uri);
            entry.assetIdNumber = AssetIdNumber(entry.assetId);
            entry.knownAsset = registry.Find(entry.assetId) != nullptr;
            m_entries.push_back(entry);
            m_byFrame[request.frame].push_back(index);
        }
    }

    // 帧点提交：同一帧的多个请求按脚本顺序提交（合并/背压在 loader 内处理），
    // 之后才是热重载 tick（顺序不能反：重载不得吞掉当帧的脚本请求）。
    void SubmitFrame(const std::uint32_t relativeFrame)
    {
        const auto found = m_byFrame.find(relativeFrame);
        if (found != m_byFrame.end())
        {
            for (const std::size_t index : found->second)
            {
                Entry& entry = m_entries[index];
                if (!entry.knownAsset)
                {
                    m_failed = true;
                    continue;
                }
                SubmitEntry(entry, false);
            }
        }
        // M7-10（A27）：热重载 cadence。每 m_reloadEvery 帧把**已提交过**的资产按新 revision
        // 再请求一次：loader 视为更新的 revision，提交时覆盖已发布资源并退休旧资源
        // （reloadCommits / deferredRetires 增长），这就是端到端热重载压力。
        // 重载请求一律用最低优先级（Prefetch）：脚本请求必须排在它前面。
        if (m_reloadEvery > 0 && relativeFrame > 0 && (relativeFrame % m_reloadEvery) == 0)
        {
            for (Entry& entry : m_entries)
            {
                if (!entry.knownAsset || !entry.submitted)
                {
                    continue;
                }
                ++entry.revision;
                SubmitEntry(entry, true);
                ++m_reloadRequests;
            }
        }
    }

    [[nodiscard]] std::uint32_t ReloadRequests() const noexcept
    {
        return m_reloadRequests;
    }

    [[nodiscard]] std::uint32_t ReloadRejected() const noexcept
    {
        return m_reloadRejected;
    }

    [[nodiscard]] std::uint32_t ScriptRejected() const noexcept
    {
        return m_scriptRejected;
    }

    // soak：采样时"最后一次请求"已被更高 revision 取代的条目数（预期，计数不判失败）。
    [[nodiscard]] std::uint32_t SupersededSamples() const noexcept
    {
        return m_supersededSamples;
    }

    // M7-LOAD-RECORDS：把"已被取代且已到终态"的请求记录还给 loader（帧点调用）。
    // 只有终态才回收：在途请求的记录仍被 I/O/decode/上传阶段持有，留在下一帧再试。
    void DrainRetiredRecords()
    {
        std::vector<Assets::RequestId> keep;
        keep.reserve(m_retired.size());
        for (const Assets::RequestId id : m_retired)
        {
            const std::optional<Assets::AssetLoadState> state = m_loader.Query(id);
            if (state.has_value() && !Assets::IsTerminal(*state))
            {
                keep.push_back(id);
                continue;
            }
            m_loader.ReleaseRequest(id);
        }
        m_retired.swap(keep);
    }

    // 当前仍待回收的请求数（诊断/证据：长跑里不应无界增长）。
    [[nodiscard]] std::size_t RetiredPending() const noexcept
    {
        return m_retired.size();
    }

    // 结束时把每个请求的 loader 记录转成 AssetRequestSample（含 queueToUpload/uploadToReady）。
    void CollectSamples(std::vector<BM::AssetRequestSample>& out)
    {
        for (const Entry& entry : m_entries)
        {
            BM::AssetRequestSample sample;
            sample.requestId = entry.requestId;
            sample.assetId = entry.assetIdNumber;
            // M7-10：脚本请求的样本记录**最后一次**请求的 revision（热重载开启时 >1）；
            // 中间各次重载请求按聚合计数进 run-notes（避免样本数量随长跑线性膨胀）。
            sample.revision = entry.revision;
            sample.requestFrame = entry.frame;
            sample.priority = entry.priority;
            if (!entry.submitted)
            {
                sample.result = entry.knownAsset ? "failed:admission" : "failed:unknown-asset";
                out.push_back(std::move(sample));
                continue;
            }
            const std::optional<Assets::AsyncLoadRecord> record = m_loader.Record(entry.requestId);
            if (!record.has_value())
            {
                sample.result = "failed:record-missing";
                out.push_back(std::move(sample));
                continue;
            }
            sample.bytes = record->bytes;
            sample.queueToReadMs = DeltaMs(record->timestamps.enqueueNs, record->timestamps.readStartNs);
            sample.readMs = DeltaMs(record->timestamps.readStartNs, record->timestamps.readEndNs);
            sample.queueToDecodeMs = DeltaMs(record->timestamps.readEndNs, record->timestamps.decodeStartNs);
            sample.decodeMs = DeltaMs(record->timestamps.decodeStartNs, record->timestamps.decodeEndNs);
            sample.queueToUploadMs = DeltaMs(record->timestamps.uploadQueuedNs, record->timestamps.uploadStartNs);
            sample.uploadToReadyMs = DeltaMs(record->timestamps.uploadStartNs, record->timestamps.readyNs);
            sample.requestToReadyMs = DeltaMs(record->timestamps.enqueueNs, record->timestamps.readyNs);
            sample.result = DescribeResult(*record);
            if (record->state != Assets::AssetLoadState::Ready)
            {
                if (m_reloadEvery > 0)
                {
                    // soak 语义：热重载会持续把条目推进到更高 revision，采样时看到的
                    // "最后一次请求"可能是被取代/取消的（Stale/Cancelled）——预期结果，
                    // 计数而不判失败（脚本请求本身在原 revision 上已 Ready）。
                    ++m_supersededSamples;
                }
                else
                {
                    m_failed = true;
                }
            }
            out.push_back(std::move(sample));
        }
    }

    [[nodiscard]] bool Failed() const noexcept
    {
        return m_failed;
    }
    [[nodiscard]] std::size_t SubmittedCount() const noexcept
    {
        std::size_t count = 0;
        for (const Entry& entry : m_entries)
        {
            count += entry.submitted ? 1U : 0U;
        }
        return count;
    }

  private:
    [[nodiscard]] static Assets::LoadPriority ToLoadPriority(const std::uint32_t priority) noexcept
    {
        // 脚本的 0/1/2 与 LoadPriority 的 Critical/Visible/Prefetch 同序（M7-01 冻结）。
        if (priority == 0)
        {
            return Assets::LoadPriority::Critical;
        }
        if (priority == 1)
        {
            return Assets::LoadPriority::Visible;
        }
        return Assets::LoadPriority::Prefetch;
    }

    [[nodiscard]] static double DeltaMs(const std::uint64_t startNs, const std::uint64_t endNs) noexcept
    {
        if (startNs == 0 || endNs == 0 || endNs < startNs)
        {
            return 0.0;
        }
        return static_cast<double>(endNs - startNs) / 1.0e6;
    }

    [[nodiscard]] static std::string DescribeResult(const Assets::AsyncLoadRecord& record)
    {
        switch (record.state)
        {
        case Assets::AssetLoadState::Ready:
            return "ready";
        case Assets::AssetLoadState::Cancelled:
            return "cancelled";
        case Assets::AssetLoadState::Stale:
            return "stale";
        case Assets::AssetLoadState::Failed:
            return record.errorText.empty() ? std::string("failed") : "failed:" + record.errorText;
        default:
            return "incomplete";
        }
    }

    // 单个条目的提交（脚本帧与热重载 tick 共用）。脚本请求被拒 = 证据不完整（失败）；
    // 热重载请求被拒 = 背压的预期行为（计数记录，不判失败）——两者必须区分，
    // 否则长跑 soak 会把"预算内的背压"误报成流式失败。
    void SubmitEntry(Entry& entry, const bool fromReload)
    {
        const Assets::LoadPriority priority =
            fromReload ? Assets::LoadPriority::Prefetch : ToLoadPriority(entry.priority);
        const Assets::RequestId previous = entry.requestId;
        const Assets::RequestResult result = m_loader.Request(entry.assetId, entry.revision, priority, entry.requestId);
        // M7-LOAD-RECORDS：拿到新 RequestId 说明旧请求已被更高 revision 取代。热重载只关心
        // "最新 revision 能否提交"，旧请求的记录不再有人读——登记待回收（帧点统一还给 loader，
        // 在途的留到下一帧，避免删掉还在跑的请求）。
        if (previous != 0 && entry.requestId != previous)
        {
            m_retired.push_back(previous);
        }
        entry.submitted = result == Assets::RequestResult::Accepted || result == Assets::RequestResult::Coalesced ||
                          result == Assets::RequestResult::AlreadyReady;
        if (!entry.submitted)
        {
            if (fromReload)
            {
                ++m_reloadRejected;
            }
            else if (m_reloadEvery > 0)
            {
                // 开启热重载 cadence 的 run 是**故意的过载 soak**：准入/解码背压是预期行为，
                // 计数而不判失败（soak 的判据是"无泄漏/无 validation/遥测稳定/重载有提交"）。
                ++m_scriptRejected;
            }
            else
            {
                m_failed = true;
            }
        }
    }

    Assets::AsyncAssetLoader& m_loader;
    std::vector<Entry> m_entries;
    std::map<std::uint32_t, std::vector<std::size_t>> m_byFrame;
    std::uint32_t m_reloadEvery = 0;
    std::uint32_t m_reloadRequests = 0;
    std::uint32_t m_reloadRejected = 0;
    std::uint32_t m_scriptRejected = 0;
    std::uint32_t m_supersededSamples = 0;
    std::vector<Assets::RequestId> m_retired; // M7-LOAD-RECORDS：待回收的旧请求
    bool m_failed = false;
};

// 上传预算：三约束 + 热重载份额全部来自 CLI（benchmark 下缺一即报错）。
Assets::UploadBudget MakeUploadBudget(const RhiLaunchOptions& options)
{
    Assets::UploadBudget budget;
    budget.maxBytes = static_cast<std::size_t>(options.uploadMiBPerFrame) * 1024u * 1024u;
    budget.maxCpuTime = std::chrono::microseconds(static_cast<std::int64_t>(options.uploadBudgetCpuMs * 1000.0 + 0.5));
    budget.maxRequests = options.uploadBudgetRequests;
    budget.reloadBytesPercent = options.uploadBudgetReloadPercent;
    return budget;
}

// packet 序列 hash：对每帧可见集合的 FNV 滚动值，覆盖整个测量窗口，
// 比只 hash 最后一帧更能发现中途的顺序漂移。字段与 M6 PacketHash 对齐（不含地址）。
void RollPacketHash(std::uint64_t& rolling, const World::RenderPacket& packet)
{
    const auto hashDraws = [](const std::vector<World::RenderDraw>& draws)
    {
        std::uint64_t hash = kFnvOffset;
        const auto bytes = [&](const auto& value)
        {
            for (const std::byte b : std::as_bytes(std::span(&value, 1)))
            {
                hash ^= std::to_integer<unsigned char>(b);
                hash *= kFnvPrime;
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
    for (const std::uint64_t value : {hashDraws(packet.mainOpaque), hashDraws(packet.shadowCasters),
                                      static_cast<std::uint64_t>(packet.stats.trianglesSubmitted)})
        for (std::uint32_t shift = 0; shift < 64; shift += 8)
        {
            rolling ^= (value >> shift) & 255U;
            rolling *= kFnvPrime;
        }
}

std::string PacketHashHex(const World::RenderPacket& packet)
{
    std::uint64_t hash = kFnvOffset;
    RollPacketHash(hash, packet);
    return Hex(hash);
}

void Save(const fs::path& path, std::string_view value)
{
    std::ofstream out(path, std::ios::binary);
    if (!out || !(out << value))
        throw std::runtime_error("cannot write " + path.string());
}

// 每帧任务系统增量采样（M7-05）：把 schema 的 taskQueueDepth/stealAttempts/stealSuccesses/
// activeWorkers 映射到并行 packet 构建的真实执行者数据。串行模式不创建采样器。
struct TaskFrameDelta final
{
    std::uint32_t activeWorkers = 1; // 含主线程；并行时由执行增量推导
    std::uint64_t stealAttempts = 0;
    std::uint64_t stealSuccesses = 0;
    std::uint64_t queueDepth = 0;
};

class TaskFrameSampler final
{
  public:
    explicit TaskFrameSampler(Tasks::TaskSystem* tasks) : m_tasks(tasks)
    {
        if (m_tasks != nullptr)
        {
            // 两个快照缓冲都必须按 workerCount 预分配（Capture 直接下标写入）。
            const std::size_t count = m_tasks->WorkerCount();
            m_countersBefore.resize(count);
            m_countersAfter.resize(count);
        }
    }

    void BeginFrame()
    {
        if (m_tasks == nullptr)
            return;
        m_statsBefore = m_tasks->Statistics();
        Capture(m_countersBefore);
    }

    [[nodiscard]] TaskFrameDelta EndFrame()
    {
        TaskFrameDelta delta;
        if (m_tasks == nullptr)
            return delta;
        const Tasks::TaskSystemStatistics statsAfter = m_tasks->Statistics();
        Capture(m_countersAfter);
        std::uint64_t workersActive = 0;
        std::uint64_t workerExecuted = 0;
        for (std::size_t index = 0; index < m_countersAfter.size(); ++index)
        {
            const std::uint64_t executed = m_countersAfter[index].executed - m_countersBefore[index].executed;
            workerExecuted += executed;
            if (executed > 0)
                ++workersActive;
            delta.stealAttempts += m_countersAfter[index].stealAttempts - m_countersBefore[index].stealAttempts;
            delta.stealSuccesses += m_countersAfter[index].stealSuccesses - m_countersBefore[index].stealSuccesses;
        }
        // 非 worker 执行量 = 主线程 main-help 执行量（本场景只有 main 提交并等待）。
        const std::uint64_t totalExecuted = statsAfter.executed - m_statsBefore.executed;
        delta.activeWorkers = static_cast<std::uint32_t>(workersActive) + (totalExecuted > workerExecuted ? 1U : 0U);
        delta.activeWorkers = std::max(1U, delta.activeWorkers); // 主线程始终是帧执行者之一
        delta.queueDepth = statsAfter.queueDepth + statsAfter.localQueueDepth;
        return delta;
    }

  private:
    void Capture(std::vector<Tasks::WorkerCounters>& counters)
    {
        for (std::uint32_t worker = 0; worker < m_tasks->WorkerCount(); ++worker)
            counters[worker] = m_tasks->WorkerCountersFor(worker);
    }

    Tasks::TaskSystem* m_tasks = nullptr;
    Tasks::TaskSystemStatistics m_statsBefore{};
    std::vector<Tasks::WorkerCounters> m_countersBefore;
    std::vector<Tasks::WorkerCounters> m_countersAfter;
};
} // namespace

int RunM7Scene(const RhiLaunchOptions& options)
{
    using namespace Rhi;
    using namespace Render;
#ifdef _DEBUG
    if (options.benchmark)
        throw std::invalid_argument("benchmark requires an optimized build (windows-msvc-profile / Release)");
#endif
    const bool captureToolActive = GetModuleHandleW(L"renderdoc.dll") != nullptr || PIXIsAttachedForGpuCapture();
    if (options.benchmark && captureToolActive)
        throw std::invalid_argument("benchmark cannot run under an injected GPU capture tool");
#if !ME_ENABLE_TRACY
    if (options.tracyWaitConnectMs > 0)
        throw std::invalid_argument("--tracy-wait-connect requires a Tracy-enabled build "
                                    "(windows-msvc-profile-tracy)");
#endif
    // 观测门面：主线程命名 + 细节级别（粗粒度只保留阶段 zone）。
    ME_PROFILE_THREAD("Main");
    Profiling::SetDetailEnabled(options.profileDetail);

    const fs::path projectRoot = ProjectRootFallback();
    const fs::path cameraPathFile = options.cameraPath.empty() ? projectRoot / "assets/tests/m7/fixed-camera.bin"
                                                               : fs::absolute(options.cameraPath);
    const fs::path streamScriptFile = options.streamScript.empty()
                                          ? projectRoot / "assets/tests/m7/streaming-script.bin"
                                          : fs::absolute(options.streamScript);
    const fs::path sceneManifest = projectRoot / "assets/recipes/m7-performance-scenes.json";
    const fs::path assetManifest = projectRoot / "out/m4-09/scene/manifest.json";
    const bool streaming = options.scene == "m7-streaming";
    if (!fs::exists(assetManifest))
        throw std::runtime_error("M4 frozen asset manifest is missing (run the M4 baseline recipe): " +
                                 assetManifest.string());
    if (!fs::exists(sceneManifest))
        throw std::runtime_error("M7 scene manifest is missing: " + sceneManifest.string());

    const CameraPath camera = LoadCameraPath(cameraPathFile);
    const StreamScript streamScript = streaming ? LoadStreamScript(streamScriptFile) : StreamScript{};

    AssetLibrary library(assetManifest);
    // M7-05：并行路径在组合根显式装配 TaskSystem（worker 数与调度器来自 CLI 控制变量）；
    // 串行路径完全不创建任务系统。声明顺序保证 packetBuilder 引用在 taskSystem 之前销毁。
    const bool parallelPacketBuild = options.packetBuild == "parallel";
    // M7-07：异步上传流水线只在 m7-streaming 上成立（CLI 已校验），decode 需要 compute 执行器。
    const bool asyncAssets = options.asyncAssets && streaming;
    std::unique_ptr<Tasks::TaskSystem> taskSystem;
    std::optional<RenderPacketBuilder> packetBuilder;
    if (parallelPacketBuild || asyncAssets)
    {
        Tasks::TaskSystemConfig taskConfig;
        taskConfig.workerCount = options.workers;
        taskConfig.mode = options.scheduler == "per-worker" ? Tasks::SchedulerMode::PerWorkerDeque
                                                            : Tasks::SchedulerMode::GlobalQueue;
        taskSystem = std::make_unique<Tasks::TaskSystem>(taskConfig);
        packetBuilder.emplace(*taskSystem);
    }
    TaskFrameSampler taskSampler(taskSystem.get());

    ProceduralScene::Config sceneConfig;
    sceneConfig.entityCount = options.entityCount;
    sceneConfig.visibleRatioTarget = options.visibleRatio;
    sceneConfig.updateRatio = options.scene == "m7-layout" ? options.updateRatio : 1.0F;
    sceneConfig.seed = options.seed;
    sceneConfig.renderDrawLimit = kRenderDrawLimit;
    sceneConfig.shadowDrawLimit = kShadowDrawLimit;
    sceneConfig.mesh = library.mesh;
    sceneConfig.material = library.material;
    sceneConfig.meshId = library.meshId;
    sceneConfig.materialId = library.materialId;
    sceneConfig.packetBuilder = packetBuilder ? &*packetBuilder : nullptr;
    sceneConfig.packetConfig.chunkSize = options.chunkSize;
    sceneConfig.packetConfig.validateAgainstSerial = kDebugOracleValidation;
    sceneConfig.packetConfig.reserveChunkCapacity = options.chunkReserve;
    // M7-08：只有显式给出 --layout 时才启用布局实验（既有基线证据完全不受影响）。
    // 校验间隔：Debug 每帧（廉价且最强），Release benchmark 每 256 帧采样
    // （校验本身要排序两组可见集合，放到每帧会污染 hitch 统计）。
    sceneConfig.layoutExperiment = options.layoutEnabled;
    sceneConfig.layout = options.layout;
    sceneConfig.layoutCheckInterval = options.benchmark ? 256U : 1U;
    ProceduralScene scene(library.assets, sceneConfig);
    if (streaming)
    {
        const std::uint32_t windowFrames = options.benchmark ? options.measure : options.frames;
        for (const StreamRequest& request : streamScript.requests)
            if (request.frame == 0 || request.frame > windowFrames)
                throw std::runtime_error("stream script requests a frame outside the measured window: " +
                                         std::to_string(request.frame));
    }

    InputState input;
    WindowDesc windowDesc;
    windowDesc.title = L"MiniEngine M7 Performance Scene";
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
    // M7-RESIZE-PATH：extent 不再 const——非 benchmark 下跟随窗口客户区（拖动 resize 路径）。
    Extent2D extent{options.width, options.height};
    SwapChainDesc swap;
    swap.extent = extent;
    swap.vsync = false; // M7 基线固定 VSync off；CLI 已拒绝 vsync=on
    swap.debugName = "M7.BackBuffer";
    auto chain = device->CreateSwapChain(swap);

    const fs::path metricsFile = options.metricsPath.empty() ? fs::path{} : fs::absolute(options.metricsPath);
    const fs::path artifactDirectory = metricsFile.empty() ? fs::path{} : metricsFile.parent_path();
    if (!artifactDirectory.empty())
        fs::create_directories(artifactDirectory);

    const auto packages = M604::LoadM604Packages();
    std::vector<ShaderDesc> shaders;
    for (const auto& package : packages)
        shaders.push_back(SelectShader(package, options.backend, package.variants[0].stage));

    const std::uint32_t total = options.benchmark ? options.warmup + options.measure : options.frames;
    const std::uint32_t warmup = options.benchmark ? options.warmup : 0;

    std::vector<BM::FrameSample> samples;
    samples.reserve(total - warmup);
    std::vector<BM::AssetRequestSample> requestSamples;
    std::uint64_t packetSequenceHash = kFnvOffset;
    World::RenderPacket lastPacket;
    World::CullingStats lastFullStats{};
    RG::GraphStatistics graphStats{};
    RG::TransientPoolStatistics poolStats{};
    std::string lastTrace;
    // M7-SUBMIT-001：提交路径段级计量的逐帧行（只在 rendered > warmup 时记录）。
    struct SubmitProfileRow final
    {
        std::uint32_t frame = 0;
        double rhiSubmitMs = 0.0;
        std::uint64_t nativeRenderingMicros = 0;
        std::uint64_t nativeBarrierMicros = 0;
        std::uint64_t nativeBindMicros = 0;
        std::uint64_t nativeDrawMicros = 0;
        std::uint64_t nativeOtherMicros = 0;
        std::uint64_t submitMicros = 0;
        std::uint64_t recordedEvents = 0;
    };
    DeviceDiagnostics::SubmitProfile lastSubmitProfile{};
    std::vector<SubmitProfileRow> submitProfileRows;
    std::optional<TextureReadbackResult> pixels;
    std::uint32_t rendered = 0;
    std::uint32_t gpuSamplesReady = 0;
    std::uint32_t measuredFrames = 0;
    std::uint32_t foregroundFrames = 0;
    bool streamFailed = false;
    std::uint64_t streamBytesRead = 0;
    // M7-07 证据快照（loop 结束后从流水线取，避免把中间态混进报告）。
    Assets::UploadCoordinatorStats uploadStats{};
    Assets::AsyncAssetLoaderStats loaderStats{};
    std::uint64_t uploadEventsTotal = 0;
    // M7-UP-QOS：帧点 drain 出来的事件按 kind 计数（此前只记总数，事件被丢弃）。
    std::uint64_t uploadEventsReady = 0;
    std::uint64_t uploadEventsReloaded = 0;
    std::uint64_t uploadEventsFailed = 0;
    std::uint64_t uploadEventsCancelled = 0;
    std::uint64_t uploadEventsStale = 0;
    std::uint64_t uploadCpuTotalMicros = 0;
    std::uint64_t uploadCommittedTotal = 0;
    std::uint64_t uploadDeferredRetireTotal = 0;
    std::size_t uploadPendingHighWater = 0;
    std::size_t residentUploadBytes = 0;
    std::size_t uploadSinkAliveAtEnd = 0;
    // 并行构建的调度证据（run notes）：总 chunk 数与多少走 QueueFull 同步回退。
    std::uint64_t totalChunks = 0;
    std::uint64_t totalSyncFallbacks = 0;
    if (total <= warmup)
        throw std::invalid_argument("M7 scene requires at least one measured frame");

    // M7-10（A27）：稳定性遥测与热重载计数必须在 try 之外声明——帧循环结束后
    // 的收尾/摘要代码在 try/catch 之外，需要读到它们。
    StabilityTelemetry stabilityTelemetry;
    stabilityTelemetry.samplingEvery = options.soakTelemetryEvery;
    std::uint32_t reloadRequestsObserved = 0;
    std::uint32_t reloadRejectedObserved = 0;
    std::uint32_t scriptRejectedObserved = 0;
    std::uint32_t supersededSamplesObserved = 0;

    try
    {
        SceneEnvironment environment(*device, library.assets);
        M6SceneResources resources(*device, library.assets, shaders, environment.prepared);
        RG::TransientResourcePool pool(*device);
        std::array<BufferHandle, 3> readbacks{};
        std::array<std::array<TimestampQueryHandle, 2>, kGpuQueryRing> queries{};
        // M7-RESIZE-PATH：extent 跟随。resize 的失败模式正是"某些尺寸相关资源没跟上"
        //（RHI 会对越界 viewport/scissor 响亮报错），因此跟随必须一次做全：
        //   交换链 → 瞬态池（按 extent 派生的 HDR/depth 纹理住在池里）→ 截图回读缓冲。
        // 每帧的 M6SceneResources::Prepare 是从传入 extent 现场派生描述符的，所以不缓存旧尺寸；
        // 池清空后下一帧会按新尺寸重建，viewport/scissor 也随 data.extent 一起跟随。
        const auto applyExtent = [&](const Extent2D next)
        {
            if (next == extent)
                return;
            device->ResizeSwapChain(chain, next);
            extent = next;
            pool.Clear();
            if (next.width == 0 || next.height == 0)
                return; // 零尺寸是挂起路径：只改链，不重建尺寸相关资源
            const std::string label = std::to_string(next.width) + "x" + std::to_string(next.height);
            if (stabilityTelemetry.extentsVisited.empty() || stabilityTelemetry.extentsVisited.back() != label)
            {
                stabilityTelemetry.extentsVisited.push_back(label);
                ++stabilityTelemetry.resizeEvents;
            }
            for (auto& readback : readbacks)
            {
                if (readback.IsValid())
                    device->Destroy(readback);
                readback = device->CreateBuffer(
                    {static_cast<std::uint64_t>(next.width) * next.height * 4, BufferUsage::CopyDestination,
                     MemoryDomain::GpuToCpu, "M7.ScreenshotReadback"},
                    {});
            }
        };
        for (auto& readback : readbacks)
            readback = device->CreateBuffer(
                {static_cast<std::uint64_t>(extent.width) * extent.height * 4, BufferUsage::CopyDestination,
                 MemoryDomain::GpuToCpu, "M7.ScreenshotReadback"},
                {});
        stabilityTelemetry.extentsVisited.push_back(std::to_string(extent.width) + "x" +
                                                    std::to_string(extent.height));
        for (auto& pair : queries)
        {
            pair[0] = device->CreateTimestampQuery("M7.Frame.Begin");
            pair[1] = device->CreateTimestampQuery("M7.Frame.End");
        }
        const HWND windowHandle = static_cast<HWND>(window.NativeHandle());
        SyncStreamingRequests streamingRequests(library.assets, assetManifest.parent_path(), streamScript);
        // M7-07 异步流水线：真实目录来源 + compute decode + RHI 上传端口 + 预算协调器。
        // 依赖顺序（声明顺序即正确销毁顺序）：coordinator → sink → loader → dispatcher → source。
        std::unique_ptr<Assets::DirectoryCookedFileSource> assetSource;
        std::unique_ptr<Assets::TaskSystemDecodeDispatcher> decodeDispatcher;
        std::unique_ptr<Assets::AsyncAssetLoader> assetLoader;
        std::unique_ptr<RhiUploadSink> uploadSink;
        std::unique_ptr<Assets::AssetUploadCoordinator> uploadCoordinator;
        std::unique_ptr<AsyncStreamingRequests> asyncRequests;
        Assets::UploadBudget uploadBudget{};
        if (asyncAssets)
        {
            const Assets::AssetRegistry* registry = library.assets.Registry();
            if (registry == nullptr || taskSystem == nullptr)
                throw std::runtime_error("async assets require a committed asset registry and a task system");
            assetSource = std::make_unique<Assets::DirectoryCookedFileSource>(assetManifest.parent_path());
            decodeDispatcher = std::make_unique<Assets::TaskSystemDecodeDispatcher>(*taskSystem);
            const Assets::AsyncAssetLoaderConfig loaderConfig{};
            assetLoader = std::make_unique<Assets::AsyncAssetLoader>(loaderConfig, *taskSystem, *assetSource,
                                                                     *decodeDispatcher, *registry);
            uploadSink = std::make_unique<RhiUploadSink>(*device);
            Assets::UploadCoordinatorConfig coordinatorConfig;
            coordinatorConfig.agingThreshold =
                std::chrono::microseconds(static_cast<std::int64_t>(options.uploadAgingMs * 1000.0 + 0.5));
            uploadCoordinator =
                std::make_unique<Assets::AssetUploadCoordinator>(*assetLoader, *uploadSink, coordinatorConfig);
            asyncRequests = std::make_unique<AsyncStreamingRequests>(*assetLoader, *registry, streamScript,
                                                                     options.loaderReloadEvery);
            uploadBudget = MakeUploadBudget(options);
        }
        const bool traceStages = !options.benchmark;
        const auto stage = [traceStages](const char* text)
        {
            if (traceStages)
                std::cerr << "[M7] stage: " << text << std::endl;
        };
        stage("setup-ready");

#if ME_ENABLE_TRACY
        bool captureArmed = false;
        bool captureActive = false;
        std::uint32_t captureFramesRemaining = 0;
#endif
        // M7-10（A27）：稳定性压力（零尺寸挂起 / 不同 extent）与长跑遥测。
        const std::uint32_t stressEvery = options.stabilityStressEvery;
        std::uint32_t stressPhase = 0;
        std::uint32_t resizePhase = 0;
        // 前台就绪只发生在预热前，不暂停计时、不剔除采样帧，也不强抢焦点。
        // Windows 可能拒绝新进程激活；此时给操作者明确的点击窗口机会。
        if (options.benchmark && options.foregroundGate)
        {
            const auto waitStart = Clock::now();
            std::optional<Clock::time_point> foregroundSince;
            const auto writeReadiness = [&](const char* status)
            {
                if (!artifactDirectory.empty())
                {
                    std::ofstream readiness(artifactDirectory / "foreground-readiness.json");
                    readiness << "{\"status\":\"" << status
                              << "\",\"waitMs\":" << Ms(waitStart, Clock::now())
                              << ",\"requiredStableMs\":1000,\"timeoutMs\":60000}\n";
                    if (!readiness)
                        throw std::runtime_error("cannot write foreground readiness evidence");
                }
            };
            std::cerr << "[M7] waiting for foreground (click the test window if necessary; timeout 60s)"
                      << std::endl;
            while (true)
            {
                if (!window.PumpMessages())
                {
                    writeReadiness("CLOSED");
                    throw std::runtime_error("benchmark window closed before foreground readiness");
                }
                const auto now = Clock::now();
                const bool foreground = GetForegroundWindow() == windowHandle && !IsIconic(windowHandle);
                if (!foreground)
                    foregroundSince.reset();
                else if (!foregroundSince)
                    foregroundSince = now;
                if (foregroundSince && now - *foregroundSince >= std::chrono::seconds(1))
                {
                    writeReadiness("READY");
                    break;
                }
                if (now - waitStart >= std::chrono::seconds(60))
                {
                    writeReadiness("TIMEOUT");
                    throw std::runtime_error("benchmark foreground readiness timed out before warmup");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        for (rendered = 1; rendered <= total; ++rendered)
        {
            bool windowAlive = true;
            {
                ME_PROFILE_ZONE_NAMED("PumpMessages");
                windowAlive = window.PumpMessages();
            }
            if (!windowAlive)
                break;
            if (stressEvery > 0 && rendered > warmup && (rendered % stressEvery) == 0)
            {
                // 稳定性压力（与 streaming 同帧发生）：零尺寸挂起 + 立即恢复 —— 这正是
                // 窗口最小化/还原在 D3D12 上走的 suspend/restore 路径（D3D11 为 no-op）。
                // 只走挂起路径、不改 render extent：M7 场景是固定 extent 契约
                // （变 extent 的 resize 会按设计被 RHI 拒绝，见 M7-10 记录与 BACKLOG）。
                switch (stressPhase % 3)
                {
                case 0:
                    device->ResizeSwapChain(chain, {0, 0});
                    // 只有 D3D12 的零尺寸链会"挂起"（BeginFrame 返回 0 帧序号）；D3D11 是 no-op。
                    // 这里用 ToString 而不是枚举名：samples 的非引导文件不得出现后端名称
                    // （check_rhi_boundary.py 的规则）。
                    if (ToString(options.backend) == "d3d12" && device->BeginFrame(chain).serial != 0)
                        throw std::runtime_error("stability stress: zero-size chain did not suspend");
                    device->ResizeSwapChain(chain, extent);
                    ME_PROFILE_MESSAGE("stability stress: suspend + restore");
                    break;
                case 1:
                    // 连续两次挂起/恢复：模拟 minimize → restore → minimize → restore。
                    device->ResizeSwapChain(chain, {0, 0});
                    device->ResizeSwapChain(chain, extent);
                    device->ResizeSwapChain(chain, {0, 0});
                    device->ResizeSwapChain(chain, extent);
                    ME_PROFILE_MESSAGE("stability stress: repeated suspend + restore");
                    break;
                default:
                    // 挂起期间恰好跨过一次显式 wait-idle（等价"最小化时资源被回收"）。
                    device->ResizeSwapChain(chain, {0, 0});
                    device->WaitIdle();
                    device->ResizeSwapChain(chain, extent);
                    ME_PROFILE_MESSAGE("stability stress: suspend + wait-idle + restore");
                    break;
                }
                ++stressPhase;
                ++stabilityTelemetry.stressEvents;
            }
            // M7-RESIZE-PATH：真实窗口 resize 压力（--exercise-resize）。改客户区尺寸 → WM_SIZE，
            // 由下面的"跟随窗口 extent"分支统一走 applyExtent（与用户拖动同一条代码路径）。
            if (options.exerciseResize && stressEvery > 0 && rendered > warmup &&
                (rendered % stressEvery) == (stressEvery / 2) && !options.headless)
            {
                const Extent2D sceneExtent{options.width, options.height};
                switch (resizePhase % 3)
                {
                case 0:
                    window.ResizeClient(std::max(320U, sceneExtent.width * 3 / 5),
                                        std::max(240U, sceneExtent.height * 3 / 5));
                    ME_PROFILE_MESSAGE("resize stress: shrink");
                    break;
                case 1:
                    // 奇数尺寸 + 非整除比例：最容易暴露对齐/奇数宽高假设的地方。
                    window.ResizeClient(sceneExtent.width + 17, sceneExtent.height + 9);
                    ME_PROFILE_MESSAGE("resize stress: odd growth");
                    break;
                default:
                    window.ResizeClient(sceneExtent.width, sceneExtent.height);
                    ME_PROFILE_MESSAGE("resize stress: restore scene extent");
                    break;
                }
                ++resizePhase;
                window.WaitForMessage(50); // 让 WM_SIZE 到位，下一帧的跟随分支立刻生效
            }
            if (!options.headless)
            {
                const Extent2D actual{window.ClientWidth(), window.ClientHeight()};
                if (window.IsMinimized() || actual.width == 0 || actual.height == 0)
                {
                    if (options.benchmark)
                    {
                        // 固定 extent 契约（benchmark）：窗口必须稳定。诊断信息必须包含实际值：
                        // 只有"窗口不稳定"无法区分最小化、尺寸漂移与外部干扰。
                        throw std::runtime_error(
                            "M7 benchmark requires a stable, non-minimized window (scene=" + options.scene +
                            ", frame=" + std::to_string(rendered) + ", minimized=" +
                            (window.IsMinimized() ? "true" : "false") + ", client=" + std::to_string(actual.width) +
                            "x" + std::to_string(actual.height) + ", expected=" + std::to_string(extent.width) + "x" +
                            std::to_string(extent.height) + ")");
                    }
                    // 非 benchmark（交互/长跑）：最小化走挂起路径，等恢复后由同一分支跟随。
                    if (extent.width != 0)
                    {
                        applyExtent({0, 0});
                        ME_PROFILE_MESSAGE("window minimized: chain suspended");
                    }
                    window.WaitForMessage(20);
                }
                else if (actual != extent)
                {
                    // 拖动 resize（M7-RESIZE-PATH）：跟随窗口把交换链与尺寸相关资源搬到新 extent。
                    applyExtent(actual);
                    ME_PROFILE_MESSAGE("window resized: extent followed");
                }
            }
            if (options.benchmark && rendered > warmup)
            {
                ++measuredFrames;
                if (GetForegroundWindow() == windowHandle)
                    ++foregroundFrames;
#if ME_ENABLE_TRACY
                // Capture 协议（M7-02）：预热完成 → 等待 profiler 连接 → capture begin
                // → 规定帧数 → capture end。on-demand 模式下连接前不产生采样数据。
                if (options.tracyWaitConnectMs > 0 && !captureArmed)
                {
                    captureArmed = true;
                    const auto deadline =
                        Clock::now() + std::chrono::milliseconds(static_cast<std::int64_t>(options.tracyWaitConnectMs));
                    while (!ME_PROFILE_IS_CONNECTED() && Clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (!ME_PROFILE_IS_CONNECTED())
                        throw std::runtime_error("profiler did not connect within --tracy-wait-connect");
                    ME_PROFILE_MESSAGE("capture begin");
                    captureActive = true;
                    captureFramesRemaining = options.tracyCaptureFrames;
                }
                if (captureActive && captureFramesRemaining > 0)
                {
                    --captureFramesRemaining;
                    if (captureFramesRemaining == 0)
                    {
                        ME_PROFILE_MESSAGE("capture end");
                        captureActive = false;
                    }
                }
#endif
            }

            const auto frameStart = Clock::now();
            const std::uint32_t slot = (rendered - 1) % kGpuQueryRing;

            // 流式请求：串行基线在请求帧内完成读取与校验（相对测量窗口计帧）；
            // 异步模式只"提交"请求，读取/decode/上传在各自线程与预算内推进。
            double ioMs = 0.0, decodeMs = 0.0;
            if (streaming && rendered > warmup)
            {
                if (asyncAssets)
                    asyncRequests->SubmitFrame(rendered - warmup);
                else
                    streamingRequests.ExecuteFrame(rendered - warmup, ioMs, decodeMs, requestSamples);
            }

            taskSampler.BeginFrame();
            const BuildResult built = scene.Build(camera.At(rendered), rendered, packetSequenceHash);
            const TaskFrameDelta taskDelta = taskSampler.EndFrame();
            totalChunks += built.packetStats.chunkCount;
            totalSyncFallbacks += built.packetStats.syncChunks;
            lastPacket = built.renderPacket;
            lastFullStats = built.fullStats;
            const BuildTimings& timings = built.timings;
            stage("packet-built");
            {
                ME_PROFILE_ZONE_DETAIL();
                resources.PreparePersistentAssets(lastPacket);
            }
            stage("persistent-prepared");
            native.ResetFrameDiagnostics(false);
            // M7-07：预算上传在**帧外**执行——公共 RHI 的 UploadTexture/UploadBuffer 是独立上传
            // （自带提交与 signal），帧内调用会被拒绝（"upload needs ... a frame boundary"）。
            // 提交（换资源 + 换 revision）只在完成判据达标后发生，仍留在 render thread。
            double uploadMs = 0.0;
            std::uint64_t uploadBytesThisFrame = 0;
            std::uint64_t uploadPendingBytes = 0;
            std::uint32_t uploadPendingThisFrame = 0;
            std::uint32_t uploadDeferredThisFrame = 0;
            if (asyncAssets)
            {
                assetLoader->Pump(); // 帧点推进：CpuReady → 上传队列
                asyncRequests->DrainRetiredRecords(); // M7-LOAD-RECORDS：终态记录立刻回收
                const Assets::UploadUsage usage = uploadCoordinator->ProcessFrame(uploadBudget);
                uploadMs = static_cast<double>(usage.cpuTime.count()) / 1000.0;
                uploadBytesThisFrame = usage.bytes;
                const std::vector<Assets::UploadCandidate> pending = assetLoader->PendingUploads();
                uploadPendingThisFrame = static_cast<std::uint32_t>(pending.size());
                uploadPendingHighWater = std::max<std::size_t>(uploadPendingHighWater, pending.size());
                for (const Assets::UploadCandidate& candidate : pending)
                {
                    uploadPendingBytes += candidate.bytes;
                }
                uploadCommittedTotal += uploadCoordinator->RetireCompletedUploads();
                const std::uint64_t deferredTotal = uploadCoordinator->Stats().deferredRetires;
                uploadDeferredThisFrame = static_cast<std::uint32_t>(deferredTotal - uploadDeferredRetireTotal);
                uploadDeferredRetireTotal = deferredTotal;
                std::vector<Assets::UploadEvent> events;
                uploadEventsTotal += uploadCoordinator->DrainEvents(events); // 帧点派发，不回调 gameplay
                // M7-UP-QOS：不再把事件丢掉——**按 kind 派发**（game-runtime 在这里 switch，
                // 而不是逐帧轮询 stats 增量）并逐类计数；收尾与 stats 交叉核对（同一事实两个来源）。
                for (const Assets::UploadEvent& event : events)
                {
                    switch (event.kind)
                    {
                    case Assets::UploadEventKind::Ready:
                        ++uploadEventsReady;
                        break;
                    case Assets::UploadEventKind::Reloaded:
                        ++uploadEventsReloaded;
                        break;
                    case Assets::UploadEventKind::Failed:
                        ++uploadEventsFailed;
                        break;
                    case Assets::UploadEventKind::Cancelled:
                        ++uploadEventsCancelled;
                        break;
                    case Assets::UploadEventKind::Stale:
                        ++uploadEventsStale;
                        break;
                    }
                }
            }
            // M7-DEVICE-LOST-INJECT：在第 N 个渲染帧注入一次设备移除。注入点选在 BeginFrame
            // **之前**：紧接着的这次 BeginFrame 就是"注入后的第一个 RHI 调用"，它必须响亮失败。
            // 不在这里 catch：让异常走到顶层（main 的 catch）→ 非零退出码 + failed-native-trace。
            if (options.injectDeviceLostAfter > 0 && rendered == options.injectDeviceLostAfter)
            {
                ME_PROFILE_MESSAGE("device-lost injection: remove-device");
                std::cerr << "[M7] stage: device-lost-inject (frame=" << rendered << ")" << std::endl;
                native.InjectBackendFaultForTesting("remove-device");
                std::cerr << "[M7] stage: device-lost-injected; next RHI call must fail loudly" << std::endl;
            }
            const auto frame = device->BeginFrame(chain);
            if (!frame.serial)
                throw std::runtime_error("unexpected suspended frame");
            stage("frame-begun");
            M6PipelineResources inputs;
            {
                // 逐 draw 的动态常量/帧资源集准备：detail zone，粗粒度构建里不再细分。
                ME_PROFILE_ZONE_DETAIL();
                inputs = resources.Prepare(lastPacket, frame, extent, kSceneLevel, 0, 0.0F);
            }
            stage("resources-prepared");
            const auto back = device->QueryTextureState(frame, frame.backBuffer);
            inputs.backBuffer = {frame.backBuffer,
                                 back.descriptor,
                                 back.access,
                                 ResourceAccess::Present,
                                 RG::ContentState::Undefined,
                                 "swapchain",
                                 {}};
            inputs.capture = (rendered == total) && !artifactDirectory.empty();
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
            inputs.timestamps = true;
            inputs.beginQuery = queries[slot][0];
            inputs.endQuery = queries[slot][1];
            const auto prepareEnd = Clock::now();

            RG::RenderGraph graph;
            {
                ME_PROFILE_ZONE_NAMED("RenderGraphBuild");
                DeclareM6RenderGraph(lastPacket, inputs, graph);
            }
            const auto buildEnd = Clock::now();
            stage("graph-declared");
            // CompiledRenderGraph 不可默认构造（只有 move），用 optional 承载以便同时
            // 保持 zone 作用域与后续使用。
            std::optional<RG::CompiledRenderGraph> planStorage;
            {
                ME_PROFILE_ZONE_NAMED("RenderGraphCompile");
                planStorage = graph.Compile();
            }
            RG::CompiledRenderGraph& plan = *planStorage;
            const auto compileEnd = Clock::now();
            stage("graph-compiled");
            // M7-12（A25/A26）：末帧 GPU capture（RenderDoc 走 D3D11、PIX 走 D3D12）。
            // 与 M6 的验证方式同源：capture 作用域包住最后一帧的 execute + present，
            // 失败一律抛异常——"没有 capture 文件"不能被当成通过。
            std::unique_ptr<RenderDocScope> renderDocCapture;
            std::unique_ptr<PixScope> pixCapture;
            if (rendered == total && !options.renderdocCapture.empty())
                renderDocCapture = std::make_unique<RenderDocScope>(fs::absolute(options.renderdocCapture));
            if (rendered == total && !options.pixCapture.empty())
                pixCapture = std::make_unique<PixScope>(fs::absolute(options.pixCapture));
            RG::GraphExecutionResult executed;
            {
                ME_PROFILE_ZONE_NAMED("RhiSubmit");
                executed = plan.TryExecute(*device, frame, pool);
            }
            if (!executed || !executed.presentReady)
                throw std::runtime_error(executed.error ? executed.error->message : "graph cannot present");
            const auto executeEnd = Clock::now();
            {
                ME_PROFILE_ZONE_NAMED("Present");
                device->EndFrame(frame, chain);
            }
            const auto presentEnd = Clock::now();
            if (pixCapture)
                pixCapture->End();
            if (renderDocCapture)
                renderDocCapture->End();
            stage("frame-ended");

            graphStats = plan.Statistics();
            poolStats = pool.Statistics();

            // GPU 帧时间延后 3 帧回读：查询槽在 +6 帧才会被重写，3 帧余量足够
            // GPU 完成；读不到就如实记 0（覆盖率在 JSON 中体现，不猜值）。
            if (rendered > kGpuQueryDelay)
            {
                const std::uint32_t target = rendered - kGpuQueryDelay;
                if (target > warmup)
                {
                    const std::uint32_t targetSlot = (target - 1) % kGpuQueryRing;
                    const auto begin = device->TryReadTimestamp(queries[targetSlot][0]);
                    const auto end = device->TryReadTimestamp(queries[targetSlot][1]);
                    if (begin && end)
                    {
                        const auto seconds = TimestampDeltaSeconds(*begin, *end);
                        if (seconds)
                        {
                            samples[target - 1 - warmup].gpuFrameMs = *seconds * 1000.0;
                            ++gpuSamplesReady;
                        }
                    }
                }
            }

            if (rendered > warmup)
            {
                BM::FrameSample sample;
                sample.frameSerial = rendered;
                sample.cpuFrameMs = Ms(frameStart, presentEnd);
                sample.fixedUpdateMs = timings.fixedUpdateMs;
                sample.worldExtractMs = timings.worldExtractMs;
                sample.packetBuildMs = timings.packetBuildMs;
                sample.cullMs = timings.cullMs;
                sample.packetMergeMs = timings.packetMergeMs;
                sample.packetWaitMs = timings.packetWaitMs;
                sample.packetSortMs = timings.packetSortMs;
                sample.layoutCullMs = timings.layoutCullMs;
                sample.renderGraphBuildMs = Ms(prepareEnd, buildEnd);
                sample.renderGraphCompileMs = Ms(buildEnd, compileEnd);
                sample.rhiSubmitMs = Ms(compileEnd, executeEnd);
                sample.presentMs = Ms(executeEnd, presentEnd);
                // M7-SUBMIT-001：提交路径段级计量。每帧在同一位置采样一次累计计数并做差分，
                // 与 sample.rhiSubmitMs 同一时间窗（compileEnd→executeEnd）——只读不改行为。
                {
                    const DeviceDiagnostics diag = device->Diagnostics();
                    const auto& profile = diag.submitProfile;
                    const auto delta = [](const std::uint64_t now, const std::uint64_t before)
                    { return now >= before ? now - before : 0U; };
                    SubmitProfileRow row{};
                    row.frame = rendered;
                    row.rhiSubmitMs = Ms(compileEnd, executeEnd);
                    row.nativeRenderingMicros = delta(profile.nativeRendering.micros, lastSubmitProfile.nativeRendering.micros);
                    row.nativeBarrierMicros = delta(profile.nativeBarrier.micros, lastSubmitProfile.nativeBarrier.micros);
                    row.nativeBindMicros = delta(profile.nativeBind.micros, lastSubmitProfile.nativeBind.micros);
                    row.nativeDrawMicros = delta(profile.nativeDraw.micros, lastSubmitProfile.nativeDraw.micros);
                    row.nativeOtherMicros = delta(profile.nativeOther.micros, lastSubmitProfile.nativeOther.micros);
                    row.submitMicros = delta(profile.submit.micros, lastSubmitProfile.submit.micros);
                    row.recordedEvents = delta(profile.recordedEvents, lastSubmitProfile.recordedEvents);
                    lastSubmitProfile = profile;
                    if (rendered > warmup)
                        submitProfileRows.push_back(row);
                }
                sample.ioReadMs = ioMs;
                sample.decodeMs = decodeMs;
                sample.uploadMs = uploadMs;
                sample.uploadBytes = uploadBytesThisFrame;
                sample.uploadPendingCount = uploadPendingThisFrame;
                sample.uploadDeferredRetires = uploadDeferredThisFrame;
                sample.uploadQueueBytes = uploadPendingBytes;
                sample.uploadsCommitted = uploadCommittedTotal;
                sample.activeWorkers = taskDelta.activeWorkers;
                sample.taskQueueDepth = static_cast<std::uint32_t>(taskDelta.queueDepth);
                sample.stealAttempts = taskDelta.stealAttempts;
                sample.stealSuccesses = taskDelta.stealSuccesses;
                sample.residentBytes = poolStats.bytes;
                sample.visibleCount = lastFullStats.mainVisible;
                sample.drawCount = static_cast<std::uint32_t>(lastPacket.mainOpaque.size());
                samples.push_back(sample);
                // 帧标记与 counter：Tracy 的帧统计与 raw JSON 用同一批数字，便于交叉核对。
                ME_PROFILE_FRAME("M7Frame");
                ME_PROFILE_COUNTER("cpuFrameUs", static_cast<std::int64_t>(sample.cpuFrameMs * 1000.0));
                ME_PROFILE_COUNTER("cullUs", static_cast<std::int64_t>(sample.cullMs * 1000.0));
                ME_PROFILE_COUNTER("packetBuildUs", static_cast<std::int64_t>(sample.packetBuildMs * 1000.0));
                ME_PROFILE_COUNTER("rhiSubmitUs", static_cast<std::int64_t>(sample.rhiSubmitMs * 1000.0));
                ME_PROFILE_COUNTER("visibleProxies", static_cast<std::int64_t>(lastFullStats.mainVisible));
                ME_PROFILE_COUNTER("drawnProxies", static_cast<std::int64_t>(sample.drawCount));
                ME_PROFILE_COUNTER("residentBytes", static_cast<std::int64_t>(sample.residentBytes));
                if (sample.ioReadMs > 0.0)
                    ME_PROFILE_COUNTER("ioReadUs", static_cast<std::int64_t>(sample.ioReadMs * 1000.0));
                if (!options.benchmark)
                {
                    // smoke 模式逐帧打印阶段分解：定位成本必须能一眼看出是哪一段。
                    std::cout << "frame=" << rendered << " cpu=" << sample.cpuFrameMs
                              << " fixed=" << sample.fixedUpdateMs << " extract=" << sample.worldExtractMs
                              << " packet=" << sample.packetBuildMs << " cull=" << sample.cullMs
                              << " merge=" << sample.packetMergeMs << " wait=" << sample.packetWaitMs
                              << " sort=" << sample.packetSortMs << " workers=" << sample.activeWorkers
                              << " declare=" << sample.renderGraphBuildMs << " compile=" << sample.renderGraphCompileMs
                              << " submit=" << sample.rhiSubmitMs << " present=" << sample.presentMs
                              << " visible=" << sample.visibleCount << " culled=" << lastFullStats.mainCulled
                              << " drawn=" << sample.drawCount << " shadowDrawn=" << lastPacket.shadowCasters.size()
                              << " shadowVisible=" << lastFullStats.shadowVisible << " io=" << sample.ioReadMs
                              << " decode=" << sample.decodeMs << '\n';
                }
            }
            // M7-10（A27）：长跑遥测采样（句柄数 / 设备驻留对象 / 待办 / 常驻内存）。
            if (stabilityTelemetry.samplingEvery > 0 && rendered > warmup &&
                (rendered % stabilityTelemetry.samplingEvery) == 0)
            {
                std::uint32_t handles = 0;
                if (::GetProcessHandleCount(::GetCurrentProcess(), reinterpret_cast<DWORD*>(&handles)) == 0)
                    handles = 0;
                const DeviceDiagnostics diagnostics = device->Diagnostics();
                // M7-SOAK-RSS：同时采进程口径内存（工作集/私有提交），与瞬态池字节并列记录。
                const Platform::Windows::ProcessMemorySnapshot processMemory =
                    Platform::Windows::QueryProcessMemory();
                stabilityTelemetry.Sample(
                    rendered, handles, static_cast<std::uint32_t>(diagnostics.aliveObjects),
                    static_cast<std::uint32_t>(diagnostics.retiringObjects),
                    asyncAssets ? static_cast<std::uint32_t>(assetLoader->LiveRequestCount()) : 0U,
                    taskSystem->Statistics().pending, static_cast<std::uint64_t>(poolStats.bytes), processMemory);
            }
            // 末帧截图：readback 必须在等待 GPU 完成后读取，且校验帧身份。
            if (rendered == total && inputs.capture)
            {
                device->WaitIdle();
                pixels = device->TryReadTextureReadback(readbacks[frame.recycleLane]);
                if (!pixels || pixels->unavailable || pixels->frameSerial != frame.serial ||
                    pixels->source != frame.backBuffer || pixels->extent != extent)
                    throw std::runtime_error("screenshot missing or stale");
            }
        }
        if (rendered <= total)
            throw std::runtime_error("window closed before the requested frames completed");
        // M7-RESIZE-PATH：压力/交互可能在非整数相位结束（窗口停在缩小或奇数尺寸），
        // 收尾时把窗口与 extent 拉回场景尺寸——报告里的 width/height 必须与场景定义一致，
        // resize 次数与访问过的 extent 在遥测里单列（stability.resizeEvents / extentsVisited）。
        if (!options.headless && (extent.width != options.width || extent.height != options.height))
        {
            window.ResizeClient(options.width, options.height);
            window.WaitForMessage(50);
            const Extent2D actual{window.ClientWidth(), window.ClientHeight()};
            if (actual.width != 0 && actual.height != 0)
                applyExtent(actual);
        }
        device->WaitIdle();
        if (asyncAssets)
        {
            // 流水线关闭前采样：每个请求的阶段时序（含 queueToUpload/uploadToReady）。
            uploadStats = uploadCoordinator->Stats();
            loaderStats = assetLoader->Stats();
            uploadCpuTotalMicros = uploadStats.uploadCpuMicros;
            residentUploadBytes = uploadStats.residentBytes;
            asyncRequests->CollectSamples(requestSamples);
            reloadRequestsObserved = asyncRequests->ReloadRequests(); // M7-10：端到端热重载请求数
            reloadRejectedObserved = asyncRequests->ReloadRejected();
            scriptRejectedObserved = asyncRequests->ScriptRejected();
            supersededSamplesObserved = asyncRequests->SupersededSamples();
            // M7-SOAK-RSS：把上传/loader 计数并入 stability 证据——非 benchmark 长跑不写 run-notes.json，
            // 这些计数此前在采集产物里完全不可见（只能看 stdout）。采样必须在 Shutdown 之前。
            stabilityTelemetry.finalUploadsStarted = uploadStats.uploadsStarted;
            stabilityTelemetry.finalUploadsCommitted = uploadStats.uploadsCommitted;
            stabilityTelemetry.finalUploadsFailed = uploadStats.uploadsFailed;
            stabilityTelemetry.finalDeferredRetires = uploadStats.deferredRetires;
            stabilityTelemetry.finalUploadCpuMicros = uploadStats.uploadCpuMicros;
            stabilityTelemetry.finalReloadRequests = reloadRequestsObserved;
            stabilityTelemetry.finalReloadCommits = uploadStats.reloadCommits;
            stabilityTelemetry.finalReloadRejected = reloadRejectedObserved;
            stabilityTelemetry.finalScriptRejected = scriptRejectedObserved;
            stabilityTelemetry.finalSupersededSamples = supersededSamplesObserved;
            stabilityTelemetry.finalLiveRequests = assetLoader->LiveRequestCount();
            stabilityTelemetry.finalRetainedRecords = assetLoader->RetainedRecordCount();
            stabilityTelemetry.finalRecordsReleased = loaderStats.recordsReleased;
            stabilityTelemetry.finalRecordsEvicted = loaderStats.recordsEvicted;
            stabilityTelemetry.finalReleaseRejectedLive = loaderStats.releaseRejectedLive;
            streamFailed = asyncRequests->Failed();
            streamBytesRead = loaderStats.bytesRead;
            uploadCoordinator->Shutdown(); // 未提交的在飞全部取消 + 临时资源退休
            assetLoader->Shutdown(true);
            // 关闭后不得残留任何上传资源（不做半提交，也不泄漏临时资源）。
            uploadSinkAliveAtEnd = uploadSink->LiveResourceCount();
            if (uploadSinkAliveAtEnd != 0)
                throw std::runtime_error("async upload sink still holds live resources after shutdown");
            stabilityTelemetry.finalUploadLiveResources = uploadSinkAliveAtEnd;
            stage("async-pipeline-stopped");
        }
        else
        {
            if (streaming && rendered > 0 && !streamScript.requests.empty())
            {
                // 每个请求必须恰好执行一次：数量对不上说明脚本帧号或窗口映射有问题。
                if (requestSamples.size() != streamScript.requests.size())
                    throw std::runtime_error("streaming requests did not execute exactly once");
            }
            streamFailed = streaming && streamingRequests.Failed();
            streamBytesRead = streamingRequests.BytesRead();
        }
        pool.Clear();
        for (auto handle : readbacks)
            device->Destroy(handle);
        for (auto& pair : queries)
        {
            device->Destroy(pair[0]);
            device->Destroy(pair[1]);
        }
    }
    catch (...)
    {
        if (!artifactDirectory.empty())
        {
            try
            {
                Save(artifactDirectory / "failed-native-trace.txt", native.NativeReport().trace);
            }
            catch (...)
            {
            }
        }
        throw;
    }

    // 显式关闭：drain 已接收任务并 join worker；异常路径由 TaskSystem 析构兜底 drain。
    if (taskSystem)
        taskSystem->Shutdown(true);

    device->Destroy(chain);
    device->WaitIdle();
    const auto facts = native.NativeReport();
    lastTrace = native.SemanticTrace();
    const auto capabilities = device->Capabilities();
    native.Shutdown();
    const auto final = native.NativeReport(true);
    const bool diagnosticsClean = !facts.warningErrors && !final.warningErrors && !final.liveResources &&
                                  !device->Diagnostics().aliveObjects && !device->Diagnostics().retiringObjects;

    // 前台比例：性能数字只在窗口保持前台时可比（后台窗口会被 DWM 节流）。
    // 允许极少量丢失（例如启动瞬间的焦点切换），低于 90% 即判 FAIL 并记录实际比例。
    const double foregroundRatio =
        measuredFrames == 0 ? 0.0 : static_cast<double>(foregroundFrames) / static_cast<double>(measuredFrames);
    // 前台 gate 是 M7-01 的"可比性"条件（后台窗口会被 DWM 节流）；自动化会话无法稳定
    // 保持前台时用 --foreground-gate=off 显式关闭，状态写入 raw JSON 与 run notes。
    const bool foregroundOk = !options.benchmark || !options.foregroundGate || foregroundRatio >= 0.9;
    const bool correctnessPass = diagnosticsClean && foregroundOk && !streamFailed;
    const Profiling::EventCounters profilingCounters = Profiling::Counters();

    // 末帧截图：哈希入 correctness，像素落在 metrics 同目录（大文件不入 Git）。
    std::string screenshotHash;
    if (pixels && !artifactDirectory.empty())
    {
        screenshotHash = Sha256DigestHex(pixels->bytes);
        std::ofstream raw(artifactDirectory / "screenshot.rgba", std::ios::binary);
        raw.write(reinterpret_cast<const char*>(pixels->bytes.data()),
                  static_cast<std::streamsize>(pixels->bytes.size()));
        if (!raw)
            throw std::runtime_error("cannot write screenshot bytes");
        std::ofstream ppm(artifactDirectory / "screenshot.ppm", std::ios::binary);
        ppm << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
        for (std::uint32_t y = 0; y < extent.height; ++y)
            for (std::uint32_t x = 0; x < extent.width; ++x)
                ppm.write(reinterpret_cast<const char*>(pixels->bytes.data() + y * pixels->rowPitch + x * 4), 3);
        if (!ppm)
            throw std::runtime_error("PPM write failed");
    }

    if (options.benchmark && !metricsFile.empty())
    {
        BM::BenchmarkRun run;
        run.experimentId = options.experimentId;
        run.variant = options.variant;
        // M7-09：交错采集的顺序记录（driver 传入；raw JSON 里按 run 可追溯）。
        run.runOrderNote = options.runOrderNote;
        run.sceneName = options.scene;
        run.sceneManifestPath = fs::relative(sceneManifest, projectRoot).generic_string();
        run.sceneManifestSha256 = Sha256File(sceneManifest);
        run.cameraPathSha256 = camera.sha256;
        run.streamScriptSha256 = streaming ? streamScript.sha256 : std::string{};
        run.machineManifestPath = options.machineManifest;
        run.machineManifestSha256 =
            options.machineManifest.empty() ? std::string{} : Sha256File(fs::absolute(options.machineManifest));
        run.sourceCommit = M610_SOURCE_COMMIT;
        run.executablePath = ExecutablePath();
        run.executableSha256 = Sha256File(run.executablePath);
        run.rhi = std::string(ToString(options.backend));
        run.workers = options.workers;
        run.chunkSize = options.chunkSize;
        run.packetBuildMode = options.packetBuild;
        run.schedulerMode = options.scheduler;
        run.chunkReserve = options.chunkReserve;
        run.foregroundGate = options.foregroundGate;
        run.seed = options.seed;
        run.runIndex = options.runIndex;
        run.width = extent.width;
        run.height = extent.height;
        run.renderDrawLimit = kRenderDrawLimit;
        run.shadowDrawLimit = kShadowDrawLimit;
        run.vsync = false;
        run.warmupFrames = warmup;
        run.measuredFrames = static_cast<std::uint32_t>(samples.size());
        run.uploadMiBPerFrame = options.uploadMiBPerFrame;
        run.uploadBudgetCpuMs = asyncAssets ? options.uploadBudgetCpuMs : 0.0;
        run.uploadBudgetRequests = asyncAssets ? options.uploadBudgetRequests : 0U;
        run.uploadBudgetReloadPercent = asyncAssets ? options.uploadBudgetReloadPercent : 0U;
        run.uploadAgingThresholdMs = asyncAssets ? options.uploadAgingMs : 0.0;
        run.loaderMode =
            asyncAssets ? "async-budget-pipeline" : (streaming ? "serial-sync-read-validate" : "serial-sync-scene");
        // M7-08：布局变体是控制变量（未指定时记录默认值，并把 layoutCullMs 列入未观测）。
        run.layoutVariant = options.layout;
        run.defaultsUsed = [](const RhiLaunchOptions& opts)
        {
            std::string used;
            const auto add = [&used](std::string_view name)
            {
                if (!used.empty())
                    used += ',';
                used += name;
            };
            if (!opts.widthSpecified)
                add("width");
            if (!opts.heightSpecified)
                add("height");
            if (!opts.cameraPath.empty() ? false : true)
                add("cameraPath");
            if (opts.streamScript.empty())
                add("streamScript");
            return used;
        }(options);
        // 本场景未观测的字段必须显式列出：0 不等于"已测量且为零"。
        run.unavailableMetrics = {"allocationCount", "allocatedBytes"};
        if (!parallelPacketBuild)
        {
            // 串行路径没有 chunk 合并/任务等待，也不经过任务系统。
            run.unavailableMetrics.push_back("packetMergeMs");
            run.unavailableMetrics.push_back("packetWaitMs");
            run.unavailableMetrics.push_back("taskQueueDepth");
            run.unavailableMetrics.push_back("stealAttempts");
            run.unavailableMetrics.push_back("stealSuccesses");
        }
        if (gpuSamplesReady == 0)
            run.unavailableMetrics.push_back("gpuFrameMs");
        if (!asyncAssets)
        {
            // 串行基线的上传阶段只有"写进本帧"的语义，没有预算/队列/提交（M7-07 起由异步模式填充）。
            run.unavailableMetrics.push_back("uploadMs");
            run.unavailableMetrics.push_back("uploadQueueBytes");
            run.unavailableMetrics.push_back("uploadsCommitted");
            run.unavailableMetrics.push_back("uploadBytes");
            run.unavailableMetrics.push_back("uploadPendingCount");
            run.unavailableMetrics.push_back("uploadDeferredRetires");
        }
        if (!options.layoutEnabled)
        {
            // M7-08：只有显式给出 --layout 才运行布局内核（既有基线证据不受影响）。
            run.unavailableMetrics.push_back("layoutCullMs");
        }
        if (!streaming)
        {
            run.unavailableMetrics.push_back("ioReadMs");
            run.unavailableMetrics.push_back("decodeMs");
        }

        run.environment.hostName = HostName();
        run.environment.osVersion.clear(); // 机器级信息由环境 manifest 承载（见 schema 注释）
        run.environment.buildType = M610_BUILD_CONFIG;
        run.environment.sourceCommit = M610_SOURCE_COMMIT;
        run.environment.gpuAdapter = capabilities.adapterName;
        run.environment.gpuDriver = capabilities.driverVersion;
        run.environment.adapterLuid = capabilities.adapterLuid;
        run.environment.displayMode = std::to_string(extent.width) + "x" + std::to_string(extent.height);
        run.environment.debugLayer = capabilities.debugLayerEnabled;
        run.environment.gpuValidation = capabilities.gpuValidationEnabled;
        run.environment.warp = options.warp;
        run.environment.captureToolActive = captureToolActive;
        run.environment.tracyConnected = Profiling::IsConnected(); // M7-02 的 profiler 连接状态在此接入
        run.environment.capturedUtc = UtcNow();

        run.correctness.status = correctnessPass ? "PASS" : "FAIL";
        run.correctness.packetSequenceHash = Hex(packetSequenceHash);
        run.correctness.graphHash = Hex(graphStats.planHash);
        run.correctness.commandHash =
            lastTrace.empty() ? std::string{}
                              : Sha256DigestHex(std::as_bytes(std::span(lastTrace.data(), lastTrace.size())));
        run.correctness.screenshotHash = screenshotHash;
        run.correctness.visibleCount = lastFullStats.mainVisible;
        run.correctness.drawCount = static_cast<std::uint32_t>(lastPacket.mainOpaque.size());
        run.correctness.trianglesSubmitted = lastFullStats.trianglesSubmitted;
        run.correctness.validationMessages = static_cast<std::uint32_t>(facts.warningErrors + final.warningErrors);
        // M7-08：布局内核的语义 hash（只有真的跑过布局实验才有值；空串表示未观测）。
        run.correctness.layoutSemanticHash = options.layoutEnabled ? scene.LayoutSemanticHashHex() : std::string{};

        run.statistics = BM::ComputeStatistics(samples);
        if (asyncAssets)
        {
            // M7-07：上传预算/优先级/提交/退休的整轮证据（非 async 模式保持 0）。
            run.statistics.totalUploadsCommitted = uploadStats.uploadsCommitted;
            run.statistics.totalUploadsStarted = uploadStats.uploadsStarted;
            run.statistics.totalUploadsFailed = uploadStats.uploadsFailed;
            run.statistics.totalReloadCommits = uploadStats.reloadCommits;
            run.statistics.totalDeferredRetires = uploadStats.deferredRetires;
            run.statistics.totalFairnessHolds = uploadStats.fairnessHolds;
            run.statistics.totalAgingPromotions = uploadStats.agingPromotions;
            run.statistics.totalUploadEvents = uploadEventsTotal;
            run.statistics.uploadBytesEstimated = uploadStats.estimatedBytes;
            run.statistics.uploadBytesActual = uploadStats.actualBytes;
            run.statistics.uploadEstimatedErrorBytes = uploadStats.estimatedErrorBytes;
            run.statistics.residentUploadBytes = uploadStats.residentBytes;
            run.statistics.uploadBytesReleased = uploadStats.releasedBytes;
            run.statistics.cancelWasteBytes = loaderStats.cancelWasteBytes;
            run.statistics.uploadPendingHighWater = uploadStats.pendingUploadHighWater;
            run.statistics.uploadInFlightHighWater = uploadStats.inFlightHighWater;
        }
        if (options.layoutEnabled)
        {
            // M7-08：布局内核与生产 packet 的等价校验证据（采样计数 + 不一致数）。
            run.statistics.layoutCheckFrames = scene.LayoutCheckFrames();
            run.statistics.layoutCheckMismatches = scene.LayoutCheckMismatches();
        }
        run.samples = samples;
        run.assetRequests = requestSamples;

        std::string validationError;
        const bool valid = BM::Validate(run, validationError);
        if (!valid && correctnessPass)
            throw std::runtime_error("benchmark run failed self-validation: " + validationError);
        Save(metricsFile, BM::SerializeRun(run));
        std::ofstream notes(artifactDirectory / "run-notes.json", std::ios::binary);
        notes << std::setprecision(6) << "{\"streamingRequestsFailed\":" << (streamFailed ? "true" : "false")
              << ",\"streamBytesRead\":" << streamBytesRead << ",\"gpuFrameSamples\":" << gpuSamplesReady
              << ",\"measuredFrames\":" << measuredFrames << ",\"foregroundFrames\":" << foregroundFrames
              << ",\"foregroundRatio\":" << foregroundRatio << ",\"foregroundOk\":" << (foregroundOk ? "true" : "false")
              << ",\"foregroundGate\":" << (options.foregroundGate ? "true" : "false")
              << ",\"profileDetail\":" << (options.profileDetail ? "true" : "false") << ",\"tracyConnected\":"
              << (Profiling::IsConnected() ? "true" : "false")
              // M7-LAYOUT-VISIBILITY：`--visible-ratio` 是**输入目标**，实际可见率由固定分布与视锥决定
              //（实测差得远：目标 10% 的数据集实测也能到 ~52%）。两个值都落盘，读者不必自己推。
              << ",\"visibleRatioRequested\":" << options.visibleRatio
              << ",\"updateRatioRequested\":" << options.updateRatio
              << ",\"entityCountMeasured\":" << options.entityCount
              << ",\"visibleCountMeasured\":" << lastFullStats.mainVisible
              << ",\"visibleRatioMeasured\":"
              << (options.entityCount > 0
                      ? static_cast<double>(lastFullStats.mainVisible) / static_cast<double>(options.entityCount)
                      : 0.0)
              // 事件计数器：M7-A04 的观测覆盖证据（zone/frame/plot/message/memory/lock）。
              << ",\"profileCounters\":{\"zones\":" << profilingCounters.zones
              << ",\"frames\":" << profilingCounters.frames << ",\"plots\":" << profilingCounters.plots
              << ",\"messages\":" << profilingCounters.messages << ",\"allocations\":" << profilingCounters.allocations
              << ",\"frees\":" << profilingCounters.frees << ",\"lockAcquires\":" << profilingCounters.lockAcquires
              << '}'
              // M7-05：并行构建的调度证据（chunk 总数、QueueFull 同步回退数、调度器模式）。
              << ",\"packetBuildMode\":" << '"' << options.packetBuild << '"' << ",\"schedulerMode\":" << '"'
              << options.scheduler << '"' << ",\"packetChunks\":" << totalChunks
              << ",\"packetSyncFallbacks\":" << totalSyncFallbacks << ",\"taskWorkerCount\":"
              << (taskSystem ? taskSystem->WorkerCount() : 0U)
              // M7-07：上传预算控制变量与流水线证据（非 async 模式全部为 0/空）。
              << ",\"loaderMode\":" << '"' << (asyncAssets ? "async-budget-pipeline" : "serial-sync-read-validate")
              << '"' << ",\"uploadBudgetMiB\":" << options.uploadMiBPerFrame
              << ",\"uploadBudgetCpuMs\":" << options.uploadBudgetCpuMs
              << ",\"uploadBudgetRequests\":" << options.uploadBudgetRequests
              << ",\"uploadBudgetReloadPercent\":" << options.uploadBudgetReloadPercent
              << ",\"uploadAgingMs\":" << options.uploadAgingMs << ",\"uploadsStarted\":" << uploadStats.uploadsStarted
              << ",\"uploadsCommitted\":" << uploadStats.uploadsCommitted
              << ",\"uploadsFailed\":" << uploadStats.uploadsFailed
              << ",\"reloadCommits\":" << uploadStats.reloadCommits
              // M7-LOAD-RELOAD-E2E：端到端热重载的请求/拒绝/替换计数与延迟退休总量（此前只在
              // stdout 摘要里，采集产物里缺——热重载证据要求它们随 raw 一起落盘）。
              << ",\"reloadRequests\":" << reloadRequestsObserved
              << ",\"reloadRejected\":" << reloadRejectedObserved
              << ",\"scriptRejected\":" << scriptRejectedObserved
              << ",\"supersededSamples\":" << supersededSamplesObserved
              << ",\"deferredRetires\":" << uploadStats.deferredRetires
              << ",\"deferredRetiresTotal\":" << uploadDeferredRetireTotal
              << ",\"fairnessHolds\":" << uploadStats.fairnessHolds
              // M7-ASYNC-FRAME-COST：把"预算为什么没兜住帧内开销"的证据一并落盘——
              // budgetStops（预算是真的会停）与 singleRequestOverruns（大资产被允许单次超预算，
              // 反饿死设计）：帧 p95 的 +1.15 ms 上传开销就来自后者的准入形态。
              << ",\"budgetStops\":" << uploadStats.budgetStops
              << ",\"singleRequestOverruns\":" << uploadStats.singleRequestOverruns
              << ",\"commitsRejected\":" << uploadStats.commitsRejected
              << ",\"selectionMisses\":" << uploadStats.selectionMisses
              << ",\"agingPromotions\":" << uploadStats.agingPromotions << ",\"uploadEvents\":" << uploadEventsTotal
              // M7-UP-QOS：帧点派发的事件分类计数 + 与 stats 的交叉核对（事件流是"派发"口径，
              // stats 是"记账"口径；两者对不上说明有事件被丢或重复派发）。
              << ",\"uploadEventsReady\":" << uploadEventsReady
              << ",\"uploadEventsReloaded\":" << uploadEventsReloaded
              << ",\"uploadEventsFailed\":" << uploadEventsFailed
              << ",\"uploadEventsCancelled\":" << uploadEventsCancelled
              << ",\"uploadEventsStale\":" << uploadEventsStale
              << ",\"uploadEventsCrossCheckOk\":"
              << ((uploadEventsReady + uploadEventsReloaded == uploadStats.uploadsCommitted &&
                   uploadEventsFailed == uploadStats.uploadsFailed &&
                   uploadEventsReloaded == uploadStats.reloadCommits)
                      ? "true"
                      : "false")
              << ",\"uploadCpuMicros\":" << uploadCpuTotalMicros
              << ",\"uploadPendingHighWater\":" << uploadStats.pendingUploadHighWater
              << ",\"uploadInFlightHighWater\":" << uploadStats.inFlightHighWater
              << ",\"residentUploadBytes\":" << uploadStats.residentBytes
              << ",\"uploadBytesActual\":" << uploadStats.actualBytes
              << ",\"uploadEstimatedBytes\":" << uploadStats.estimatedBytes
              << ",\"uploadEstimatedErrorBytes\":" << uploadStats.estimatedErrorBytes << ",\"uploadLiveResources\":"
              << (asyncAssets ? uploadSinkAliveAtEnd : 0U)
              // M7-08：布局实验的控制变量、等价校验与逐实体字节账（run-notes 承载非逐帧证据）。
              << ",\"layoutVariant\":" << '"' << (options.layoutEnabled ? options.layout : std::string("none")) << '"'
              << ",\"layoutProxies\":" << (options.layoutEnabled ? scene.LayoutProxyCount() : 0U)
              << ",\"layoutCheckFrames\":" << (options.layoutEnabled ? scene.LayoutCheckFrames() : 0U)
              << ",\"layoutCheckMismatches\":" << (options.layoutEnabled ? scene.LayoutCheckMismatches() : 0U)
              << ",\"layoutSemanticHash\":" << '"'
              << (options.layoutEnabled ? scene.LayoutSemanticHashHex() : std::string{}) << '"'
              << ",\"layoutBytes\":{\"aosPerEntity\":" << (options.layoutEnabled ? scene.LayoutAosBytesPerEntity() : 0U)
              << ",\"soaScanPerEntity\":" << (options.layoutEnabled ? scene.LayoutSoaScanBytesPerEntity() : 0U)
              << ",\"hotColdPerEntity\":" << (options.layoutEnabled ? scene.LayoutHotColdColdBytesPerEntity() : 0U)
              << '}' << ",\"assetManifestSha256\":" << '"' << library.manifestSha256 << '"' << "}\n";
    }

    // M7-10（A27）：长跑遥测落盘（stability.csv + stability-summary.json）与 stdout 摘要。
    if (stabilityTelemetry.samples > 0 && !artifactDirectory.empty())
    {
        std::ofstream csv(artifactDirectory / "stability.csv", std::ios::binary);
        csv << stabilityTelemetry.Csv();
        std::ofstream stability(artifactDirectory / "stability-summary.json", std::ios::binary);
        stability << stabilityTelemetry.Json() << '\n';
    }

    // M7-SUBMIT-001：提交路径段级计量落盘（逐帧 CSV + 段中位数/IQR 汇总）。
    // 只读段计数器，不进入 raw schema（避免 schemaVersion 抖动）：作为 artifact 侧文件发布。
    if (!submitProfileRows.empty() && !artifactDirectory.empty())
    {
        std::ofstream csv(artifactDirectory / "submit-profile.csv", std::ios::binary);
        csv << "frame,rhiSubmitMs,nativeTotalMicros,nativeRenderingMicros,nativeBarrierMicros,"
               "nativeBindMicros,nativeDrawMicros,nativeOtherMicros,submitMicros,recordedEvents\n";
        for (const auto& row : submitProfileRows)
        {
            const std::uint64_t nativeTotal = row.nativeRenderingMicros + row.nativeBarrierMicros +
                                              row.nativeBindMicros + row.nativeDrawMicros + row.nativeOtherMicros;
            csv << row.frame << ',' << std::setprecision(6) << row.rhiSubmitMs << ',' << nativeTotal << ','
                << row.nativeRenderingMicros << ',' << row.nativeBarrierMicros << ',' << row.nativeBindMicros << ','
                << row.nativeDrawMicros << ',' << row.nativeOtherMicros << ',' << row.submitMicros << ','
                << row.recordedEvents << '\n';
        }
        const auto medianOf = [&](auto pick)
        {
            std::vector<std::uint64_t> values;
            values.reserve(submitProfileRows.size());
            for (const auto& row : submitProfileRows)
                values.push_back(pick(row));
            std::sort(values.begin(), values.end());
            return values.empty() ? 0ULL : values[values.size() / 2];
        };
        const auto medianMsOf = [&]()
        {
            std::vector<double> values;
            values.reserve(submitProfileRows.size());
            for (const auto& row : submitProfileRows)
                values.push_back(row.rhiSubmitMs);
            std::sort(values.begin(), values.end());
            return values.empty() ? 0.0 : values[values.size() / 2];
        };
        const std::uint64_t rendering = medianOf([](const SubmitProfileRow& r) { return r.nativeRenderingMicros; });
        const std::uint64_t barrier = medianOf([](const SubmitProfileRow& r) { return r.nativeBarrierMicros; });
        const std::uint64_t bind = medianOf([](const SubmitProfileRow& r) { return r.nativeBindMicros; });
        const std::uint64_t draw = medianOf([](const SubmitProfileRow& r) { return r.nativeDrawMicros; });
        const std::uint64_t other = medianOf([](const SubmitProfileRow& r) { return r.nativeOtherMicros; });
        const std::uint64_t nativeTotal = rendering + barrier + bind + draw + other;
        const std::uint64_t submit = medianOf([](const SubmitProfileRow& r) { return r.submitMicros; });
        const std::uint64_t events = medianOf([](const SubmitProfileRow& r) { return r.recordedEvents; });
        const double submitMs = medianMsOf();
        const double submitMicros = submitMs * 1000.0;
        const auto share = [](const std::uint64_t part, const std::uint64_t whole)
        { return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole); };
        std::ofstream summary(artifactDirectory / "submit-profile-summary.json", std::ios::binary);
        summary << std::setprecision(6) << "{\"frames\":" << submitProfileRows.size() << ",\"rhiSubmitMs\":" << submitMs
                << ",\"nativeMicros\":" << nativeTotal << ",\"nativeSharePercent\":"
                << (submitMicros > 0.0 ? share(nativeTotal, static_cast<std::uint64_t>(submitMicros)) : 0.0)
                << ",\"bookkeepingMicros\":" << (submitMicros > static_cast<double>(nativeTotal)
                                                     ? static_cast<std::uint64_t>(submitMicros - nativeTotal)
                                                     : 0U)
                << ",\"submitMicros\":" << submit << ",\"recordedEvents\":" << events
                << ",\"categories\":{\"renderingMicros\":" << rendering << ",\"barrierMicros\":" << barrier
                << ",\"bindMicros\":" << bind << ",\"drawMicros\":" << draw << ",\"otherMicros\":" << other
                << "},\"categoryShareOfNative\":{\"rendering\":" << share(rendering, nativeTotal)
                << ",\"barrier\":" << share(barrier, nativeTotal) << ",\"bind\":" << share(bind, nativeTotal)
                << ",\"draw\":" << share(draw, nativeTotal) << ",\"other\":" << share(other, nativeTotal) << "}}\n";
    }

    // Win32 路径含反斜杠：直接拼进 JSON 会产生非法转义序列（消费方 ConvertFrom-Json 报
    // "Unrecognized escape sequence"）。统一转成正斜杠形式（JSON 合法且跨工具可读）。
    const std::string metricsPathJson = metricsFile.generic_string();
    std::ostringstream summary;
    summary << "{\"status\":" << (correctnessPass ? "\"PASS\"" : "\"FAIL\"") << ",\"scene\":" << '"' << options.scene
            << '"' << ",\"rhi\":" << '"' << ToString(options.backend) << '"' << ",\"measuredFrames\":" << samples.size()
            << ",\"gpuFrameSamples\":" << gpuSamplesReady << ",\"graphHash\":" << '"' << Hex(graphStats.planHash) << '"'
            << ",\"packetSequenceHash\":" << '"' << Hex(packetSequenceHash)
            << '"'
            // M7-10：Debug/GBV 矩阵与稳定性判定直接消费这两个字段（无需读 raw JSON）。
            << ",\"streamFailed\":" << (streamFailed ? "true" : "false") << ",\"foregroundRatio\":" << foregroundRatio
            << ",\"validationMessages\":" << (facts.warningErrors + final.warningErrors)
            << ",\"liveResources\":" << final.liveResources
            << ",\"aliveObjects\":" << device->Diagnostics().aliveObjects
            << ",\"retiringObjects\":" << device->Diagnostics().retiringObjects
            << ",\"reloadRequests\":" << reloadRequestsObserved << ",\"reloadRejected\":" << reloadRejectedObserved
            << ",\"scriptRejected\":" << scriptRejectedObserved
            << ",\"supersededSamples\":" << supersededSamplesObserved
            << ",\"reloadCommits\":" << (asyncAssets ? uploadStats.reloadCommits : 0U)
            << ",\"stability\":" << (stabilityTelemetry.samples > 0 ? stabilityTelemetry.Json() : std::string("null"))
            << ",\"metrics\":" << '"' << metricsPathJson << '"' << "}\n";
    std::cout << summary.str();
    return correctnessPass ? 0 : 3;
}
} // namespace MiniEngine::Sandbox
