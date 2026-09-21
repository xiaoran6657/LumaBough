// ============================================================================
// D3D12Renderer.cpp — 最小 clear/present 帧循环与安全点 Resize 实现
// 里程碑：M5（04 篇 Command List、SwapChain 与 Resize；手抄清单第 2 条）
// 职责：实现 D3D12Renderer.h。本篇只做 clear/present——先证明"三帧轮转 +
//       resize + 零调试层消息"，任何 draw 都留到后续篇目。
// 顺序契约（04 篇「最小 clear/present」）：
//   BeginFrame → barrier PRESENT→RENDER_TARGET → OMSetRenderTargets →
//   ClearRenderTargetView → barrier RENDER_TARGET→PRESENT → Close/Execute/Signal
//   → Present。Signal 在 Present 之前，fence 覆盖本帧全部 GPU 引用（03 篇口径）。
// 关联：docs/architecture/README.md（Resize 八步/失败策略）
// ============================================================================
#include "D3D12Renderer.h"

#include <MiniEngine/Rhi/D3D12/D3D12RootBindings.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>
#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/World/DirectionalLight.h>
#include <MiniEngine/World/RenderPacket.h>
#include <MiniEngine/World/RenderQueueBuilder.h>

#include "D3D12Diagnostics.h"
#include "D3D12Events.h"
#include "D3D12IblSet.h"
#include "D3D12TextureUpload.h"

#include <array>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// 常量上传环容量。09 篇第 2 步起每帧的常量不再只有 b0/b1：每个 draw 还要写
// b1（208B→256B 对齐）+ b2（48B→256B 对齐）= 512B/draw，因此容量按
// "3 帧在飞 × 1000 draw × 512B ≈ 1.5MB" 留到 4MB（固定场景只有 ~30 draw，
// 这个上界是给 --culling-scene=1000 那类压力场景的）。
constexpr std::uint64_t kConstantRingCapacity = 4U * 1024U * 1024U;

// 资产上传环：网格 + 纹理（含完整 mip 链）的 staging。16MB 足以容纳固定场景的
// 全部资产一次性上传；超过 ring/4 的单次请求由 06 篇策略层走 dedicated。
constexpr std::uint64_t kAssetUploadRingCapacity = 16U * 1024U * 1024U;

// staging 描述符堆容量：5 个材质 fallback + 每个已上传纹理 1 个 SRV。
constexpr std::uint32_t kSrvStagingCapacity = 4096U;

// 固定三角形的顶点（position + color，stride 24），**NDC 空间**：第 1 步的 b0
// ViewProjection 是单位矩阵，因此顶点坐标即 clip 坐标。z 取 0.5（D3D clip 是 [0,1]，
// 0 恰好压在近平面上，0.5 让它明确落在视锥内部）。三个顶点三种颜色：这样"顶点流真的
// 被读到了"在像素上可见——若顶点缓冲/输入布局接错，颜色会立刻不对而不是"看起来还行"。
struct TriangleVertex final
{
    float position[3];
    float color[3];
};

constexpr std::array<TriangleVertex, 3> kTriangleVertices{{
    {{-0.5F, -0.5F, 0.5F}, {1.0F, 0.0F, 0.0F}}, // 左下：红
    {{0.5F, -0.5F, 0.5F}, {0.0F, 1.0F, 0.0F}},  // 右下：绿
    {{0.0F, 0.5F, 0.5F}, {0.0F, 0.0F, 1.0F}},   // 顶部：蓝
}};

// viewport/scissor 由当前交换链尺寸派生（resize 后必须同步更新，04 篇第 7 步）。
D3D12_VIEWPORT BuildViewport(const std::uint32_t width, const std::uint32_t height)
{
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0F;
    viewport.TopLeftY = 0.0F;
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    return viewport;
}

D3D12_RECT BuildScissor(const std::uint32_t width, const std::uint32_t height)
{
    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);
    return scissor;
}
} // namespace

D3D12Renderer::D3D12Renderer() = default;
D3D12Renderer::~D3D12Renderer()
{
    if (m_initialized)
    {
        // 析构体先等待，再释放成员；Queue 的析构晚于资源，不能依靠它兜底。
        try
        {
            m_queue.FlushGpu("shutdown-renderer");
        }
        catch (...)
        {
        }
        ReleaseIblSet(m_pendingIbl);
        ReleaseIblSet(m_ibl);
        for (auto& retired : m_retiredIbl)
        {
            ReleaseIblSet(retired.second);
        }
    }
}

void D3D12Renderer::Initialize(D3D12Device& device, void* nativeWindow, const D3D12RendererOptions& options)
{
    if (m_initialized)
    {
        throw std::logic_error{"D3D12Renderer::Initialize called twice"};
    }
    if (nativeWindow == nullptr)
    {
        throw std::invalid_argument{"D3D12Renderer::Initialize requires a valid window handle"};
    }
    if (IsSuspendedSize(options.width, options.height))
    {
        throw std::invalid_argument{"D3D12Renderer::Initialize requires non-zero window size"};
    }

    m_options = options;
    m_device = &device;

    // 句柄 → 类型：公共头只给不透明 void*（边界约定），类型还原在后端内部完成。
    auto* const nativeDevice = static_cast<ID3D12Device*>(device.NativeDeviceHandle());
    auto* const nativeFactory = static_cast<IDXGIFactory7*>(device.NativeFactoryHandle());

    // Queue 先于 SwapChain：CreateSwapChainForHwnd 的 device 参数就是 Direct Queue。
    m_queue.Initialize(*nativeDevice);
    m_frameReadback.Initialize(*nativeDevice, m_queue.NativeQueue());
    // 07 篇：tracker 先于交换链存在（交换链把 back buffer 注册进来）；两者生命周期
    // 都由本渲染器持有，tracker 不延长资源寿命（非拥有指针 + 显式注销）。
    m_swapChain.Initialize(*nativeDevice, *nativeFactory, m_queue.NativeQueue(), m_stateTracker, nativeWindow,
                           options.width, options.height, options.vsync);

    m_viewport = BuildViewport(m_swapChain.Config().width, m_swapChain.Config().height);
    m_scissor = BuildScissor(m_swapChain.Config().width, m_swapChain.Config().height);

    // 09 篇第 1 步：主深度（缺口③的 Depth 部分）与常量上传环。
    // 深度按交换链尺寸创建；resize 时随 ApplyPendingResizeIfNeeded 一起重建。
    m_depthBuffer.Initialize(*nativeDevice, m_swapChain.Config().width, m_swapChain.Config().height, m_stateTracker);
    // 常量环容量：b0(128B) + b1(208B) 各按 256B 对齐 → 每帧 512B；3 个 frame context
    // 轮转，因此稳态占用 ~1.5KB。取 64KiB 是给"某帧多上传几个对象常量"留余量，而不是
    // 现在的实际需求——容量偏大只会多占一点上传堆，不会掩盖"环回绕"这类缺陷。
    m_constantRing.Initialize(*nativeDevice, kConstantRingCapacity);
    // 资产上传环 + 策略层（09 篇第 2 步）：等待走 03 篇的**定向等待**（不是全量 flush）。
    m_assetUploadRing.Initialize(*nativeDevice, kAssetUploadRingCapacity);
    m_assetUploadManager.Initialize(
        nativeDevice, m_assetUploadRing, [this](const std::uint64_t value)
        { m_queue.WaitForSubmittedFence(value, "asset-upload"); }, kAssetUploadRingCapacity);
    CreateTriangleGeometry(*nativeDevice);

    m_initialized = true;
}

void D3D12Renderer::ApplyCommandListBaselineState(ID3D12GraphicsCommandList& commandList,
                                                  const std::uint32_t backBufferIndex)
{
    // D3D12 direct list 不继承上次记录状态：每次 Reset 后必须显式重设。
    // 本篇只有 viewport/scissor/RTV；descriptor heap、Root Signature、PSO
    // 随 05/08 篇加入（同一处，避免"依赖上一帧设置"的隐性耦合）。
    // descriptor heap 也是"Reset 后不继承"的状态：设置了 root descriptor table 却没设
    // heap 会被调试层判为无效用法（实测 ERROR：No CBV_SRV_UAV descriptor heap is
    // currently set on the command list）。
    if (m_srvHeap != nullptr)
    {
        ID3D12DescriptorHeap* const heaps[1]{&m_srvHeap->Native()};
        commandList.SetDescriptorHeaps(1U, heaps);
    }

    commandList.RSSetViewports(1, &m_viewport);
    commandList.RSSetScissorRects(1, &m_scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain.RtvHandle(backBufferIndex);
    commandList.OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // 08 篇：Root Signature 与 PSO 同属"Reset 后必须显式重设"的状态，因此一起放在
    // 基线里。此处只做**查找**（命中缓存），帧循环绝不创建 PSO、更不编译 shader。
    //
    // 当前写后备缓冲的只有 ToneMap（RTV = R8G8B8A8_UNORM、无 DSV），因此它是这一帧
    // 的基线 PSO；09 篇接入真实 pass 后这里换成对应 pass 的 key，查找语义不变。
    // 这段查找是"帧内零创建"这条证据不退化成空断言的前提：每帧都真的问了工厂。
    if (m_psoBaselineReady)
    {
        commandList.SetGraphicsRootSignature(m_rootSignature);

        PsoKey key;
        key.pass = PassKind::ToneMap;
        key.mirrored = false;
        key.shaderRevision = m_shaderRevision;
        key.rootSignatureRevision = m_rootSignatureRevision;

        ID3D12PipelineState& pipelineState = m_psoFactory.GetOrCreate(key, m_baselineShaders.at(PassKind::ToneMap));
        commandList.SetPipelineState(&pipelineState);
    }
}

bool D3D12Renderer::RenderFrame()
{
    struct RemovalReportGuard final
    {
        D3D12Device* device;
        int exceptionCount = std::uncaught_exceptions();
        ~RemovalReportGuard() noexcept
        {
            if (device && std::uncaught_exceptions() > exceptionCount)
            {
                try
                {
                    device->ReportDeviceRemoved();
                }
                catch (...)
                {
                }
            }
        }
    } removalGuard{m_device};
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12Renderer::RenderFrame before Initialize"};
    }
    if (m_swapChain.IsFailed())
    {
        // resize 失败后状态明确：不再渲染，不混用新旧资源（04 篇失败策略）。
        throw std::logic_error{"D3D12Renderer::RenderFrame called in failed resize state"};
    }
    if (m_swapChain.IsSuspended())
    {
        ++m_skippedFrames; // 0×0（minimized）：不渲染、不 ResizeBuffers
        return false;
    }

    // current index 只来自 SwapChain（04 篇契约）。
    const std::uint32_t backBufferIndex = m_swapChain.CurrentBackBufferIndex();

    if (m_device->Metadata().dredActive)
    {
        const auto nextFence = m_queue.NextFenceValue();
        m_device->UpdateRemovalContext(nextFence, nextFence - 1U, m_queue.CompletedValue());
        std::string revisions;
        if (m_assets && m_packet)
        {
            for (const auto& draw : m_packet->mainOpaque)
            {
                const auto material = m_assets->Materials().TryGet(draw.material);
                revisions +=
                    draw.materialId.ToHexString() + ":" + std::to_string(material ? material->revision : 0U) + ";";
            }
            const auto environment = m_assets->Textures().TryGet(m_environmentHandle);
            revisions += "environment:" + std::to_string(environment ? environment->revision : 0U);
        }
        m_device->UpdateDiagnosticRevisions(revisions, std::to_string(m_shaderRevision));
        m_device->RecordDiagnosticEvent("Frame.Begin");
    }
    D3D12FrameContext& frame = m_queue.BeginFrame(backBufferIndex);
    ID3D12GraphicsCommandList& commandList = m_queue.CommandList();

    // 07 篇：一帧 = 一段 recording。状态请求先登记，barrier 批在依赖它的访问之前发出。
    m_stateTracker.BeginRecording();
    m_drawTableCursor = 0;
    // 06 篇：先按已完成 fence 回收上传环，再录制本帧的常量/资产（回收不依赖本帧提交）。
    const std::uint64_t completedFence = m_queue.CompletedValue();
    PollEnvironment(completedFence);
    m_frameReadback.BeginFrame(backBufferIndex, completedFence, m_submittedFrameCount + 1);
    m_frameReadback.BeginPass(commandList, backBufferIndex, 4);
    m_constantRing.Reclaim(completedFence);
    m_assetUploadManager.Reclaim(completedFence);
    if (m_descriptorHeapBound)
    {
        m_assetCache.BeginFrame(completedFence);
        if (m_assets)
            m_assetCache.PruneStale(*m_assets);
    }
    try
    {
        const std::wstring frameName = L"M5.Frame " + std::to_wstring(m_submittedFrameCount + 1U);
        EventScope frameEvent(commandList, frameName.c_str());
        // pass 入口（clear pass）：PRESENT → RENDER_TARGET，随即 flush 这一小批 barrier，
        // 保证 clear 之前状态已到位（07 篇：不能把整帧 barrier 留到末尾统一发）。
        m_swapChain.TrackTransition(backBufferIndex, D3D12_RESOURCE_STATE_RENDER_TARGET);
        static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));

        ApplyCommandListBaselineState(commandList, backBufferIndex);
        commandList.ClearRenderTargetView(m_swapChain.RtvHandle(backBufferIndex), m_options.clearColor, 0, nullptr);

        // 09 篇第 2 步：消费 RenderPacket 的 forward pass（在 clear 之后、出口 transition 之前）。
        if (m_packet != nullptr && m_assets != nullptr && m_descriptorHeapBound)
        {
            RecordForwardPass(commandList);
        }

        // 09 篇第 1 步：固定三角形（在 clear 之后、出口 transition 之前录进同一段）。
        if (m_triangleEnabled)
        {
            RecordTrianglePass(commandList);
        }

        m_frameReadback.EndPass(commandList, backBufferIndex, 4);
        if (m_screenshotRequested)
        {
            m_device->RecordDiagnosticEvent("M5.Screenshot.Copy");
            EventScope screenshotEvent(commandList, L"ScreenshotCopy");
            m_swapChain.TrackTransition(backBufferIndex, D3D12_RESOURCE_STATE_COPY_SOURCE);
            static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));
            m_frameReadback.RecordScreenshot(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), commandList,
                                             m_swapChain.BackBuffer(backBufferIndex), m_swapChain.Config().width,
                                             m_swapChain.Config().height);
        }
        {
            EventScope resolveEvent(commandList, L"TimestampResolve");
            m_frameReadback.Resolve(commandList, backBufferIndex);
        }
        // pass 出口：RENDER_TARGET → PRESENT，再 flush（Present 之前状态必须到位）。
        m_swapChain.TrackTransition(backBufferIndex, D3D12_RESOURCE_STATE_PRESENT);
        static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));

        // Close/Execute/Signal：Signal 成功后才写回 context（03 篇单一写点）。
        frameEvent.End();
        const std::uint64_t submittedFence = m_queue.ExecuteAndSignal(frame);
        if (m_device->Metadata().dredActive)
            m_device->UpdateRemovalContext(submittedFence, submittedFence, m_queue.CompletedValue());
        m_frameReadback.Commit(backBufferIndex, submittedFence);
        if (m_screenshotRequested)
        {
            m_frameReadback.CommitScreenshot(submittedFence);
            m_screenshotRequested = false;
        }
        ++m_submittedFrameCount;
        // 提交成功 → pending 状态成为 committed（07 篇：Execute 之后才 commit）。
        m_stateTracker.CommitExecuted();
        // 常量环与资产上传环的 span 都绑定到本帧 fence：在它完成之前绝不复用（06 篇）。
        m_constantRing.CommitFrame(submittedFence);
        m_assetUploadManager.CommitFrame(submittedFence);
        if (m_descriptorHeapBound)
        {
            m_assetCache.CommitFrame(submittedFence);
        }
        if (m_pendingIbl && m_pendingIbl->fence == 0)
        {
            m_pendingIbl->fence = submittedFence;
        }
    }
    catch (...)
    {
        // Close/Execute 失败：丢弃 pending，committed 不变（07 篇：失败不污染全局状态）。
        m_stateTracker.Rollback();
        throw;
    }

    // 取证：本帧 trace + 累计链（跨 run 应稳定，用于"barrier trace hash 稳定"的验证）。
    m_lastFrameBarrierTraceHash = m_stateTracker.TraceHash();
    m_totalBarrierCount += m_stateTracker.BarrierCount();
    m_barrierFrameHighWater = std::max(m_barrierFrameHighWater, m_stateTracker.BarrierCount());
    m_barrierTraceChainHash ^= m_lastFrameBarrierTraceHash;
    m_barrierTraceChainHash *= 1099511628211ULL; // FNV-1a 素数（与 06/01 篇同口径）

    // Present 在 Signal 之后：fence 覆盖本帧全部 GPU 引用（03 篇固定顺序）。
    const auto presentStart = std::chrono::steady_clock::now();
    const bool presented = m_swapChain.Present();
    m_lastPresentMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
    if (presented)
    {
        ++m_presentedFrames;
    }
    else
    {
        ++m_skippedFrames; // 被遮挡：上层低频探测待机
    }
    return presented;
}

void D3D12Renderer::RequestScreenshot()
{
    if (m_screenshotRequested || !m_frameReadback.CanRecordScreenshot())
        throw std::logic_error{"screenshot already pending"};
    m_screenshotRequested = true;
}
std::optional<D3D12Rgba8Image> D3D12Renderer::TakeScreenshot()
{
    return m_frameReadback.TryReadScreenshot(m_queue.CompletedValue());
}
std::vector<D3D12FrameTiming> D3D12Renderer::TakeGpuTimings()
{
    m_frameReadback.Poll(m_queue.CompletedValue());
    return m_frameReadback.TakeTimings();
}

void D3D12Renderer::RequestResize(const std::uint32_t width, const std::uint32_t height)
{
    m_swapChain.RequestResize(width, height);
}

bool D3D12Renderer::ApplyPendingResizeIfNeeded()
{
    if (!m_swapChain.HasPendingResize())
    {
        return false;
    }
    const std::uint32_t before = m_swapChain.Config().resizeCount;
    m_swapChain.ApplyPendingResize(m_queue);

    // 第 7 步：更新 viewport/scissor（camera aspect 随 06/09 篇的相机接线一起做）。
    m_viewport = BuildViewport(m_swapChain.Config().width, m_swapChain.Config().height);
    m_scissor = BuildScissor(m_swapChain.Config().width, m_swapChain.Config().height);

    // 主深度必须与后备缓冲同尺寸：交换链的 ApplyPendingResize 已在内部 FlushGpu，
    // 因此此刻 GPU 不再引用旧深度，可以安全地"先 Unregister 再释放再重建"。
    m_depthBuffer.Resize(m_swapChain.Config().width, m_swapChain.Config().height);
    // HDR 目标同理（同一个安全点）。
    if (m_descriptorHeapBound)
    {
        CreateHdrTarget(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()));
    }
    return m_swapChain.Config().resizeCount != before;
}

void D3D12Renderer::SetVsync(const bool vsync) noexcept
{
    m_options.vsync = vsync;
    m_swapChain.SetVsync(vsync);
}

void D3D12Renderer::FlushGpu(const std::string_view reason)
{
    m_queue.FlushGpu(reason);
}

void D3D12Renderer::ReclaimCompletedWork()
{
    if (!m_initialized)
        return;
    const auto completed = m_queue.CompletedValue();
    m_constantRing.Reclaim(completed);
    m_assetUploadManager.Reclaim(completed);
    m_assetCache.BeginFrame(completed);
    ReclaimRetiredPsos(completed);
    PollEnvironment(completed);
    // 发布候选可能刚退休旧集；同一个已完成 fence 下立即完成第二次回收。
    PollEnvironment(completed);
    if (m_srvHeap)
        m_srvHeap->Reclaim(completed);
    if (m_stagingHeap)
        m_stagingHeap->Reclaim(completed);
}

D3D12RetirementCounts D3D12Renderer::RetirementCounts() const noexcept
{
    D3D12RetirementCounts counts{};
    if (!m_initialized)
        return counts;
    counts.assets = m_assetCache.RetiredResourceCount();
    counts.ibl = m_retiredIbl.size();
    counts.pendingIbl = m_pendingIbl ? 1U : 0U;
    counts.psos = RetiredPsoCount();
    counts.descriptors =
        (m_srvHeap ? m_srvHeap->RetiredRangeCount() : 0U) + (m_stagingHeap ? m_stagingHeap->RetiredRangeCount() : 0U);
    counts.uploads = m_assetUploadManager.PendingDedicatedCount() + m_assetUploadManager.RetiredDedicatedCount();
    counts.constantBytes = m_constantRing.Allocator().OccupiedBytes();
    counts.uploadBytes = m_assetUploadRing.Allocator().OccupiedBytes();
    return counts;
}

ValidationReport D3D12Renderer::DrainInfoQueue()
{
    // 直接复用 Device 的 InfoQueue 通道：渲染器与设备共享同一条调试消息流。
    return m_device == nullptr ? ValidationReport{} : m_device->DrainInfoQueue();
}

const SwapChainConfig& D3D12Renderer::SwapChainFacts() const noexcept
{
    return m_swapChain.Config();
}

std::uint64_t D3D12Renderer::PresentedFrameCount() const noexcept
{
    return m_presentedFrames;
}

std::uint64_t D3D12Renderer::SkippedFrameCount() const noexcept
{
    return m_skippedFrames;
}

bool D3D12Renderer::IsSuspended() const noexcept
{
    return m_swapChain.IsSuspended();
}

D3D12Queue& D3D12Renderer::Queue() noexcept
{
    return m_queue;
}

D3D12SwapChain& D3D12Renderer::SwapChain() noexcept
{
    return m_swapChain;
}

// ---- 09 篇迁移顺序第 1 步：固定 triangle ----

void D3D12Renderer::SetTriangleEnabled(const bool enabled) noexcept
{
    m_triangleEnabled = enabled;
}

bool D3D12Renderer::TriangleEnabled() const noexcept
{
    return m_triangleEnabled;
}

std::uint64_t D3D12Renderer::TriangleDrawCount() const noexcept
{
    return m_triangleDrawCount;
}

std::uint64_t D3D12Renderer::TriangleVertexCount() const noexcept
{
    return m_triangleVertexCount;
}

std::uint64_t D3D12Renderer::TriangleConstantUploadBytes() const noexcept
{
    return m_triangleConstantUploadBytes;
}

std::uint64_t D3D12Renderer::ConstantUploadBytes() const noexcept
{
    return m_constantUploadBytes;
}

const D3D12DepthBuffer& D3D12Renderer::DepthBuffer() const noexcept
{
    return m_depthBuffer;
}

void D3D12Renderer::CreateTriangleGeometry(ID3D12Device& device)
{
    // 静态顶点流：只上传一次。用 committed UPLOAD 资源而不是常量环——几何不随帧变化，
    // 每帧重传既浪费环预算，又让"顶点缓冲地址稳定"这一可断言性质消失。
    const std::uint64_t byteSize = sizeof(kTriangleVertices);

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = byteSize;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;

    // 上传堆资源创建即处于 GENERIC_READ，命令列表只能从该状态读；因此不注册进
    // tracker（它不参与任何 transition，注册反而会引入一条永远不会变化的记账）。
    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&m_triangleVertexBuffer)),
                  "ID3D12Device::CreateCommittedResource(triangle vertices)");
    Internal::SetDebugName(m_triangleVertexBuffer.Get(), L"M5.TriangleVertices");

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0U, 0U}; // 只写不读 → 空读区间
    ThrowIfFailed(m_triangleVertexBuffer->Map(0U, &readRange, &mapped), "ID3D12Resource::Map(triangle vertices)");
    std::memcpy(mapped, kTriangleVertices.data(), static_cast<std::size_t>(byteSize));
    m_triangleVertexBuffer->Unmap(0U, nullptr);

    m_triangleVertexBufferView.BufferLocation = m_triangleVertexBuffer->GetGPUVirtualAddress();
    m_triangleVertexBufferView.SizeInBytes = static_cast<UINT>(byteSize);
    m_triangleVertexBufferView.StrideInBytes = static_cast<UINT>(sizeof(TriangleVertex));
}

void D3D12Renderer::RecordTrianglePass(ID3D12GraphicsCommandList& commandList)
{
    // 前置：root signature 与 PSO 集已在 init 就绪（未就绪时 SetTriangleEnabled 打开也
    // 不能画——此时显式失败，而不是画一个"什么都没发生"的空帧）。
    if (!m_psoBaselineReady)
    {
        throw std::logic_error{"triangle pass requires CreateBaselinePsoSet first"};
    }

    // 1) 常量：b0 FrameConstants（128B）+ b1 ObjectConstants（208B），各按 256B 对齐。
    //    第 1 步的 ViewProjection / World 都是**单位矩阵**（顶点已是 NDC 坐标），
    //    因此这里上传的是"结构体正确、数值中性"的一份——它验证的是布局与绑定链路，
    //    不是相机数学（相机数学由 D3D12CameraParityTests 独立锁定）。
    const UploadAllocation frameConstants = m_constantRing.TryAllocateConstant(sizeof(FrameConstants));
    const UploadAllocation objectConstants = m_constantRing.TryAllocateConstant(sizeof(ObjectConstants));
    if (!frameConstants || !objectConstants)
    {
        // 06 篇：上传失败必须显式失败，不能静默少画一帧。
        throw std::runtime_error{"constant upload ring exhausted while recording the triangle pass"};
    }

    FrameConstants frame{};
    frame.viewProjection = DirectX::XMFLOAT4X4{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                               0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    frame.cameraPositionAndDebugMode = DirectX::XMFLOAT4{0.0F, 0.0F, 0.0F, 0.0F};
    frame.directionAndIntensity = DirectX::XMFLOAT4{0.0F, 0.0F, 1.0F, 0.0F};
    frame.lightColorAndPadding = DirectX::XMFLOAT4{0.0F, 0.0F, 0.0F, 0.0F};
    frame.shadowMapSizeAndPadding = DirectX::XMFLOAT4{0.0F, 0.0F, 0.0F, 0.0F};
    std::memcpy(frameConstants.cpu, &frame, sizeof(frame));

    ObjectConstants object{};
    object.world = frame.viewProjection;
    object.normalMatrix = frame.viewProjection;
    object.lightWorldViewProjection = frame.viewProjection;
    object.handednessAndReceivesShadow = DirectX::XMFLOAT4{1.0F, 0.0F, 0.0F, 0.0F};
    std::memcpy(objectConstants.cpu, &object, sizeof(object));
    m_triangleConstantUploadBytes += sizeof(frame) + sizeof(object);
    m_constantUploadBytes += sizeof(frame) + sizeof(object);

    // 2) 主深度：本 pass 要清深度并作为 DSV 绑定。深度资源注册时即 DEPTH_WRITE，
    //    这里显式再请求一次——同状态不产生 barrier（07 篇"只在不同状态时生成 barrier"），
    //    但把"我依赖它处于 DEPTH_WRITE"写进了 trace，而不是靠隐含假设。
    m_stateTracker.Transition(m_depthBuffer.Key(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));

    // 3) 目标：后备缓冲 RTV + 主深度 DSV。基线里绑的是"无 DSV"（04 篇 clear/present），
    //    因此本 pass 必须显式重绑带 DSV 的版本。
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain.RtvHandle(m_swapChain.CurrentBackBufferIndex());
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depthBuffer.DsvHandle();
    commandList.OMSetRenderTargets(1U, &rtv, FALSE, &dsv);
    commandList.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);

    // 4) 状态与绘制：PSO + 两个根 CBV + 顶点流 + 拓扑 + DrawInstanced。
    PsoKey key;
    key.pass = PassKind::TriangleSmoke;
    key.mirrored = false;
    key.shaderRevision = m_shaderRevision;
    key.rootSignatureRevision = m_rootSignatureRevision;
    ID3D12PipelineState& pipelineState = m_psoFactory.GetOrCreate(key, m_baselineShaders.at(PassKind::TriangleSmoke));
    commandList.SetPipelineState(&pipelineState);
    commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::FrameCbv), frameConstants.gpu);
    commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::ObjectCbv), objectConstants.gpu);

    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.IASetVertexBuffers(0U, 1U, &m_triangleVertexBufferView);
    commandList.DrawInstanced(static_cast<UINT>(kTriangleVertices.size()), 1U, 0U, 0U);

    ++m_triangleDrawCount;
    m_triangleVertexCount += kTriangleVertices.size();
}

// ===========================================================================
// 09 篇第 2 步：descriptor 表、fallback 资源与 RenderPacket 消费
// ===========================================================================

void D3D12Renderer::BindDescriptorHeap(D3D12DescriptorHeap& srvHeap)
{
    if (m_descriptorHeapBound)
    {
        throw std::logic_error{"BindDescriptorHeap called twice"};
    }
    if (m_device == nullptr)
    {
        throw std::logic_error{"BindDescriptorHeap before Initialize"};
    }

    auto* const device = static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    m_srvHeap = &srvHeap;

    // staging heap：纹理 SRV 与 5 个材质 fallback 住在这里（拷贝描述符的源）。
    m_stagingHeap = std::make_unique<D3D12DescriptorHeap>();
    m_stagingHeap->Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvStagingCapacity, false,
                              L"M5.D3D12.SrvStagingHeap");
    // 自持的 RTV heap（HDR 目标的 RTV；交换链的 RTV heap 由它自己管理）。
    m_rtvHeap = std::make_unique<D3D12DescriptorHeap>();
    m_rtvHeap->Initialize(*device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8U, false, L"M5.D3D12.RendererRtvHeap");

    // 3 个材质表（按 FrameContext 轮转）+ 1 个 global 表。
    for (DescriptorRange& table : m_materialTables)
    {
        table = srvHeap.Allocate(kMaterialSrvCount);
    }
    m_globalTable = srvHeap.Allocate(kGlobalSrvCount);

    for (auto& table : m_frameGlobalTables)
    {
        table = srvHeap.Allocate(kGlobalSrvCount);
    }
    m_assetCache.Initialize(*m_stagingHeap, m_stateTracker);
    CreateSceneResources(*device);
    // HDR 中间目标：RTV/SRV 创建不需要 command list，因此在这里建（resize 时重建）。
    CreateHdrTarget(*device);
    // fallback 资源延迟到首次录制的帧内创建（纹理不能在 UPLOAD 堆上建，需要 command list）。
    m_fallbacksReady = false;
    m_descriptorHeapBound = true;

    MiniEngine::WriteLog(MiniEngine::LogLevel::Info,
                         "d3d12 descriptor tables: material[0].base=" + std::to_string(m_materialTables[0].base) +
                             " global.base=" + std::to_string(m_globalTable.base) +
                             " stagingCapacity=" + std::to_string(m_stagingHeap->Capacity()));
}

void D3D12Renderer::CreateFallbackResources(ID3D12Device& device, ID3D12GraphicsCommandList& commandList)
{
    static_cast<void>(commandList); // 见下方 createTexture：上传路径需要它

    // 09 篇第 2 步的 fallback 语义与 M4 逐项一致：缺省贴图槽位必须绑定**真实**资源，
    // 禁止留空后依赖上一材质的绑定残留。IBL/shadow 尚未落地（第 6/7 步），因此这里
    // 用黑 cube ×2 + 中性 LUT + 全亮 shadow 让 indirect 退化为 0、shadow 退化为 1。
    struct FallbackSpec final
    {
        const wchar_t* name;
        std::uint8_t rgba[4];
        DXGI_FORMAT format;
        bool cube;
    };
    constexpr std::array<FallbackSpec, 5> kMaterialFallbacks{{
        {L"M5.Fallback.BaseColor", {255U, 255U, 255U, 255U}, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false},
        {L"M5.Fallback.Normal", {128U, 128U, 255U, 255U}, DXGI_FORMAT_R8G8B8A8_UNORM, false},
        {L"M5.Fallback.MetallicRoughness", {255U, 255U, 255U, 255U}, DXGI_FORMAT_R8G8B8A8_UNORM, false},
        {L"M5.Fallback.Occlusion", {255U, 255U, 255U, 255U}, DXGI_FORMAT_R8G8B8A8_UNORM, false},
        {L"M5.Fallback.Emissive", {0U, 0U, 0U, 255U}, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false},
    }};

    // fallback 纹理：DEFAULT + COPY_DEST → 常规上传路径（D3D12 **禁止**在 UPLOAD 堆上
    // 创建纹理——调试层实测报 "A texture resource cannot be created on a
    // D3D12_HEAP_TYPE_UPLOAD ..."，因此这里必须走与资产纹理同一条上传链）。
    const auto createTexture = [&device, &commandList,
                                this](const FallbackSpec& spec, const std::uint32_t arraySize,
                                      const std::byte* const pixels,
                                      const std::uint32_t bytesPerPixel) -> Microsoft::WRL::ComPtr<ID3D12Resource>
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProperties.CreationNodeMask = 1U;
        heapProperties.VisibleNodeMask = 1U;

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = 1U;
        description.Height = 1U;
        description.DepthOrArraySize = static_cast<UINT16>(arraySize);
        description.MipLevels = 1U;
        description.Format = spec.format;
        description.SampleDesc.Count = 1U;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
                      "ID3D12Device::CreateCommittedResource(fallback texture)");
        Internal::SetDebugName(resource.Get(), spec.name);

        const TextureUploadPlan sizing = PlanTextureUpload(device, description, 0U);
        const UploadAllocation allocation = m_assetUploadManager.Allocate(sizing.totalBytes, 512U, "fallback-texture");
        if (!allocation)
        {
            throw std::runtime_error{"fallback texture upload allocation failed"};
        }
        const TextureUploadPlan plan =
            allocation.offset == 0U ? sizing : PlanTextureUpload(device, description, allocation.offset);
        std::byte* const bufferBase = allocation.cpu - allocation.offset;
        const std::uint64_t bufferSize = allocation.offset + allocation.size;
        // 高度 1：每个 subresource 就是一行，源 pitch = 单个像素的字节数。
        for (std::uint32_t slice = 0U; slice < arraySize; ++slice)
        {
            const TextureUploadSource source{pixels + static_cast<std::size_t>(slice) * bytesPerPixel, bytesPerPixel,
                                             bytesPerPixel};
            PackTextureRows(plan, slice, bufferBase, bufferSize, source);
        }

        const ResourceKey key =
            m_stateTracker.Register(*resource.Get(), arraySize, D3D12_RESOURCE_STATE_COPY_DEST, spec.name);
        RecordTextureCopies(commandList, *resource.Get(), plan, *allocation.source);
        m_stateTracker.Transition(key, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));
        return resource;
    };

    // ---- 材质五角色（写进 staging heap，作为按 draw 拷贝的源）----
    for (std::size_t index = 0U; index < kMaterialFallbacks.size(); ++index)
    {
        const FallbackSpec& spec = kMaterialFallbacks[index];
        std::byte pixel[4]{};
        std::memcpy(pixel, spec.rgba, sizeof(pixel));
        const DescriptorRange slot = m_stagingHeap->Allocate(1U);
        m_fallbackResources[index] = createTexture(spec, 1U, pixel, 4U);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = spec.format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1U;
        device.CreateShaderResourceView(m_fallbackResources[index].Get(), &srv, m_stagingHeap->Cpu(slot));
        m_materialFallbacks[index] = m_stagingHeap->Cpu(slot);
    }

    // ---- global 表四槽（内容稳定：一次写好，不参与按帧重用）----
    std::byte neutralLut[8]{};
    {
        // 中性 LUT： (scale, bias) = (1, 0) → 即使 prefiltered 非零也不会把间接光算歪。
        const float lut[2]{1.0F, 0.0F};
        std::memcpy(neutralLut, lut, sizeof(lut));
    }
    std::byte shadowWhite[4]{};
    {
        const float one = 1.0F;
        std::memcpy(shadowWhite, &one, sizeof(one));
    }

    const FallbackSpec cubeSpec{L"M5.Fallback.IblCube", {0U, 0U, 0U, 255U}, DXGI_FORMAT_R16G16B16A16_FLOAT, true};
    // 黑 cube：6 面 × 8 字节（RGBA16F 每面 1 个像素）= 48 字节，全零。
    // 这两份黑 cube 让 IBL 的第 7 步落地前 indirect 项恒为 0（而不是 NaN）。
    std::byte cubePixels[6U * 8U]{};
    m_fallbackResources[5] = createTexture(cubeSpec, 6U, cubePixels, 8U);
    m_fallbackResources[6] = createTexture(cubeSpec, 6U, cubePixels, 8U);

    const FallbackSpec lutSpec{L"M5.Fallback.BrdfLut", {0U, 0U, 0U, 255U}, DXGI_FORMAT_R16G16_FLOAT, false};
    m_fallbackResources[7] = createTexture(lutSpec, 1U, neutralLut, 4U);

    const FallbackSpec shadowSpec{L"M5.Fallback.ShadowMap", {0U, 0U, 0U, 255U}, DXGI_FORMAT_R32_FLOAT, false};
    m_fallbackResources[8] = createTexture(shadowSpec, 1U, shadowWhite, 4U);

    const auto writeGlobal = [&device, this](const std::uint32_t slotOffset, ID3D12Resource& resource,
                                             const D3D12_SRV_DIMENSION dimension, const DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = dimension;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (dimension == D3D12_SRV_DIMENSION_TEXTURECUBE)
        {
            srv.TextureCube.MipLevels = 1U;
        }
        else
        {
            srv.Texture2D.MipLevels = 1U;
        }
        const auto stagingSlot = m_stagingHeap->Allocate(1U);
        m_globalFallbacks[slotOffset] = m_stagingHeap->Cpu(stagingSlot);
        device.CreateShaderResourceView(&resource, &srv, m_globalFallbacks[slotOffset]);
        device.CopyDescriptorsSimple(1U, m_srvHeap->Cpu(m_globalTable.base + slotOffset), m_globalFallbacks[slotOffset],
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    };

    writeGlobal(0U, *m_fallbackResources[5].Get(), D3D12_SRV_DIMENSION_TEXTURECUBE, DXGI_FORMAT_R16G16B16A16_FLOAT);
    writeGlobal(1U, *m_fallbackResources[6].Get(), D3D12_SRV_DIMENSION_TEXTURECUBE, DXGI_FORMAT_R16G16B16A16_FLOAT);
    writeGlobal(2U, *m_fallbackResources[7].Get(), D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R16G16_FLOAT);
    writeGlobal(3U, *m_fallbackResources[8].Get(), D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT);

    MiniEngine::WriteLog(MiniEngine::LogLevel::Info,
                         "d3d12 fallback resources ready: materialSrvs=" + std::to_string(kMaterialSrvCount) +
                             " globalSrvs=" + std::to_string(kGlobalSrvCount));
}

void D3D12Renderer::PublishMaterialTable(ID3D12Device& device, const std::uint32_t frameIndex,
                                         const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaterialSrvCount>& sources)
{
    // 一次 CopyDescriptors 把 5 个来自 staging heap 的描述符写进本帧的材质表：
    // 源是 5 个单槽区间（不同纹理的 SRV 在 staging heap 里不连续），目标是 1 个 5 槽区间。
    // 为什么不能用 CopyDescriptorsSimple：它要求源连续。
    const UINT destSizes[1]{kMaterialSrvCount};
    auto& tables = m_drawTables.at(frameIndex);
    if (m_drawTableCursor == tables.size())
    {
        tables.push_back(m_srvHeap->Allocate(kMaterialSrvCount));
    }
    m_lastMaterialTableBase = tables.at(m_drawTableCursor++).base;
    const D3D12_CPU_DESCRIPTOR_HANDLE destStart = m_srvHeap->Cpu(m_lastMaterialTableBase);
    const UINT sourceSizes[kMaterialSrvCount]{1U, 1U, 1U, 1U, 1U};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaterialSrvCount> sourceStarts = sources;
    device.CopyDescriptors(1U, &destStart, destSizes, kMaterialSrvCount, sourceStarts.data(), sourceSizes,
                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12Renderer::SetFrameInput(const World::RenderPacket* packet, const Assets::AssetManager* assets) noexcept
{
    m_packet = packet;
    m_assets = assets;
}

void D3D12Renderer::SetDebugView(const std::uint32_t debugView) noexcept
{
    m_debugView = debugView;
}

std::uint32_t D3D12Renderer::DebugView() const noexcept
{
    return m_debugView;
}

std::uint64_t D3D12Renderer::ForwardDrawCount() const noexcept
{
    return m_forwardDrawCount;
}

std::uint64_t D3D12Renderer::ForwardIndexCount() const noexcept
{
    return m_forwardIndexCount;
}

std::uint64_t D3D12Renderer::ForwardSkippedDrawCount() const noexcept
{
    return m_forwardSkippedDrawCount;
}

std::uint32_t D3D12Renderer::UploadedMeshCount() const noexcept
{
    return m_assetCache.UploadedMeshCount();
}

std::uint32_t D3D12Renderer::UploadedTextureCount() const noexcept
{
    return m_assetCache.UploadedTextureCount();
}

std::uint32_t D3D12Renderer::MaterialTableCount() const noexcept
{
    return static_cast<std::uint32_t>(m_materialTables.size());
}

std::uint32_t D3D12Renderer::MaterialTableBase() const noexcept
{
    return m_materialTables[0].base;
}

std::uint32_t D3D12Renderer::GlobalTableBase() const noexcept
{
    return m_globalTable.base;
}

bool D3D12Renderer::DescriptorHeapBound() const noexcept
{
    return m_descriptorHeapBound;
}

std::uint64_t D3D12Renderer::UploadedVertexBytes() const noexcept
{
    return m_assetCache.UploadedVertexBytes();
}

std::uint64_t D3D12Renderer::UploadedIndexBytes() const noexcept
{
    return m_assetCache.UploadedIndexBytes();
}

std::uint64_t D3D12Renderer::UploadedTextureBytes() const noexcept
{
    return m_assetCache.UploadedTextureBytes();
}

std::uint32_t D3D12Renderer::PlaceholderAssetCount() const noexcept
{
    return m_assetCache.PlaceholderCount();
}

std::uint32_t D3D12Renderer::RejectedAssetUploadCount() const noexcept
{
    return m_assetCache.RejectedUploadCount();
}

std::uint32_t D3D12Renderer::MeshRevisionLag() const noexcept
{
    return m_meshRevisionLag;
}

std::uint32_t D3D12Renderer::TextureRevisionLag() const noexcept
{
    return m_textureRevisionLag;
}

std::uint64_t D3D12Renderer::ToneMapDrawCount() const noexcept
{
    return m_toneMapDrawCount;
}

void D3D12Renderer::RecordForwardPass(ID3D12GraphicsCommandList& commandList)
{
    if (!m_psoBaselineReady)
    {
        throw std::logic_error{"forward pass requires CreateBaselinePsoSet first"};
    }
    const World::RenderPacket& packet = *m_packet;
    const Assets::AssetManager& assets = *m_assets;

    // 首次进入时创建 fallback 资源（需要 command list，见 CreateFallbackResources 注释）。
    if (!m_fallbacksReady)
    {
        CreateFallbackResources(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), commandList);
        m_fallbacksReady = true;
    }

    PrepareEnvironment(commandList);
    PublishGlobalTable(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()),
                       m_swapChain.CurrentBackBufferIndex());
    RecordShadowPass(commandList);
    commandList.RSSetViewports(1U, &m_viewport);
    commandList.RSSetScissorRects(1U, &m_scissor);

    m_frameReadback.BeginPass(commandList, m_swapChain.CurrentBackBufferIndex(), 1);
    m_device->RecordDiagnosticEvent("M5.PBR.HDR");
    EventScope pbrEvent(commandList, L"PbrOpaque");
    // ---- 目标：HDR（RGBA16F）+ 主深度 DSV ----
    // 09 篇格式表把 PbrOpaque 的 RTV 冻结为 RGBA16F，因此 forward pass 写 HDR 中间目标，
    // 再由 tone map pass 编码到 UNORM 后备缓冲（M4 同序）。HDR 的 clear 恒 0：
    // 背景 radiance 由 skybox 提供，不在 HDR 阶段预置"像显示色"的值（04/05 篇契约）。
    if (m_hdrTarget == nullptr)
    {
        throw std::logic_error{"forward pass requires an HDR target"};
    }
    m_stateTracker.Transition(m_hdrKey, D3D12_RESOURCE_STATE_RENDER_TARGET);
    static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));

    const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = m_rtvHeap->Cpu(m_hdrRtv);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depthBuffer.DsvHandle();
    commandList.OMSetRenderTargets(1U, &hdrRtv, FALSE, &dsv);
    const float hdrClear[4]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(hdrRtv, hdrClear, 0U, nullptr);
    commandList.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);

    // ---- b0：相机/光照/debug view（每帧一次）----
    const UploadAllocation frameConstants = m_constantRing.TryAllocateConstant(sizeof(FrameConstants));
    if (!frameConstants)
    {
        throw std::runtime_error{"constant upload ring exhausted while recording the forward pass"};
    }
    FrameConstants frame{};
    frame.viewProjection = TransposedForHlsl(packet.viewProjection);
    frame.cameraPositionAndDebugMode = DirectX::XMFLOAT4{packet.cameraWorldPosition.x, packet.cameraWorldPosition.y,
                                                         packet.cameraWorldPosition.z, static_cast<float>(m_debugView)};
    const World::DirectionalLight& light = packet.directionalLight;
    frame.directionAndIntensity = DirectX::XMFLOAT4{light.directionToLight[0], light.directionToLight[1],
                                                    light.directionToLight[2], light.illuminanceScale};
    frame.lightColorAndPadding =
        DirectX::XMFLOAT4{light.colorLinear[0], light.colorLinear[1], light.colorLinear[2], 0.0F};
    // shadow map 尺寸 = fallback 的 1×1（第 6 步换成 2048²）。3×3 PCF 全部命中同一个
    // texel=1.0，shadow factor 因此恒为 1（全亮），与"shadow 尚未落地"一致。
    frame.shadowMapSizeAndPadding = DirectX::XMFLOAT4{2048.0F, 2048.0F, 0.0F, 0.0F};
    std::memcpy(frameConstants.cpu, &frame, sizeof(frame));
    m_constantUploadBytes += sizeof(frame);

    // ---- b3：prefiltered mip 数 = fallback cube 的 1 ----
    const UploadAllocation iblConstants = m_constantRing.TryAllocateConstant(sizeof(IblConstants));
    if (!iblConstants)
    {
        throw std::runtime_error{"constant upload ring exhausted while recording the forward pass"};
    }
    IblConstants ibl{};
    ibl.prefilterMipCountAndFlags = DirectX::XMFLOAT4{HasUsableIbl() ? 8.0F : 1.0F, 0.0F, 0.0F, 0.0F};
    std::memcpy(iblConstants.cpu, &ibl, sizeof(ibl));
    m_constantUploadBytes += sizeof(ibl);

    commandList.SetGraphicsRootSignature(m_rootSignature);
    commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::FrameCbv), frameConstants.gpu);
    commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::IblCbv), iblConstants.gpu);
    commandList.SetGraphicsRootDescriptorTable(
        RootIndex(RootParameter::GlobalSrvs),
        m_srvHeap->Gpu(m_frameGlobalTables[m_swapChain.CurrentBackBufferIndex()]));

    // ---- 光空间 VP：固定 shadow volume（与 M4 的 BuildLightViewProjection 同源）----
    const World::Matrix4 lightViewProjection = World::BuildLightViewProjection(light);
    const DirectX::XMFLOAT4X4 lightMatrix = TransposedForHlsl(lightViewProjection);

    const std::uint32_t frameIndex = m_swapChain.CurrentBackBufferIndex();
    const std::uint32_t objectSlices = 0U; // 保留：object 常量按 draw 逐个分配

    for (const World::RenderDraw& draw : packet.mainOpaque)
    {
        if (!m_assetCache.EnsureMeshUploaded(
                AssetUploadContext{*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), commandList,
                                   m_assetUploadManager, m_stateTracker},
                assets, draw.mesh))
        {
            ++m_forwardSkippedDrawCount;
            continue;
        }
        const std::optional<D3D12AssetCache::GpuMeshView> meshView = m_assetCache.TryGetMeshView(draw.mesh);
        if (!meshView.has_value())
        {
            ++m_forwardSkippedDrawCount;
            continue;
        }

        // 材质：句柄失效时退回默认值（与 D3D11 的 kDefaultMaterial 同语义：不丢 draw）。
        Assets::MaterialAsset defaultMaterial;
        defaultMaterial.baseColorFactor = {1.0F, 1.0F, 1.0F, 1.0F};
        const Assets::MaterialAsset* material = &defaultMaterial;
        if (draw.material.IsValid())
        {
            const auto materialView = assets.Materials().TryGet(draw.material);
            if (materialView.has_value())
            {
                material = &*materialView->asset;
            }
        }

        // ---- 材质表：5 个角色槽位，未引用/失效的槽位绑真实 fallback ----
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaterialSrvCount> sources = m_materialFallbacks;
        const Assets::AssetId* const slotIds[kMaterialSrvCount]{
            &material->baseColorTexture, &material->normalTexture,   &material->metallicRoughnessTexture,
            &material->occlusionTexture, &material->emissiveTexture,
        };
        for (std::uint32_t slot = 0U; slot < kMaterialSrvCount; ++slot)
        {
            const Assets::AssetId& id = *slotIds[slot];
            if (!id.IsValid())
            {
                continue; // 保持 fallback
            }
            const auto textureHandle = assets.Textures().TryFind(id);
            if (!textureHandle.has_value() ||
                !m_assetCache.EnsureTextureUploaded(
                    AssetUploadContext{*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), commandList,
                                       m_assetUploadManager, m_stateTracker},
                    assets, *textureHandle))
            {
                continue;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE textureSrv{};
            if (m_assetCache.TryGetTextureSrv(*textureHandle, textureSrv))
            {
                sources[slot] = textureSrv;
            }
        }
        PublishMaterialTable(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), frameIndex, sources);

        // ---- b1：世界/法线/光 VP/手性（每 draw 一次）----
        const UploadAllocation objectConstants = m_constantRing.TryAllocateConstant(sizeof(ObjectConstants));
        if (!objectConstants)
        {
            throw std::runtime_error{"constant upload ring exhausted while recording the forward pass"};
        }
        ObjectConstants object{};
        object.world = TransposedForHlsl(draw.world);
        object.normalMatrix = TransposedForHlsl(draw.normal);
        object.lightWorldViewProjection = lightMatrix;
        object.handednessAndReceivesShadow = DirectX::XMFLOAT4{
            draw.mirrored ? -1.0F : 1.0F, draw.receivesShadow && m_shadowsEnabled ? 1.0F : 0.0F, 0.0F, 0.0F};
        std::memcpy(objectConstants.cpu, &object, sizeof(object));
        m_constantUploadBytes += sizeof(object);

        // ---- b2：材质因子（每 draw 一次）----
        const UploadAllocation materialConstants = m_constantRing.TryAllocateConstant(sizeof(MaterialConstants));
        if (!materialConstants)
        {
            throw std::runtime_error{"constant upload ring exhausted while recording the forward pass"};
        }
        MaterialConstants materialData{};
        materialData.baseColorFactor = DirectX::XMFLOAT4{material->baseColorFactor[0], material->baseColorFactor[1],
                                                         material->baseColorFactor[2], material->baseColorFactor[3]};
        materialData.emissiveAndMetallic = DirectX::XMFLOAT4{material->emissiveFactor[0], material->emissiveFactor[1],
                                                             material->emissiveFactor[2], material->metallicFactor};
        materialData.roughnessNormalOcclusionFlags =
            DirectX::XMFLOAT4{material->roughnessFactor, material->normalScale, material->occlusionStrength, 0.0F};
        std::memcpy(materialConstants.cpu, &materialData, sizeof(materialData));
        m_constantUploadBytes += sizeof(materialData);

        // ---- PSO：镜像实例用另一个 winding 的 PSO（08 篇）----
        PsoKey key;
        key.pass = PassKind::PbrOpaque;
        key.mirrored = draw.mirrored;
        key.shaderRevision = m_shaderRevision;
        key.rootSignatureRevision = m_rootSignatureRevision;
        ID3D12PipelineState& pipelineState = m_psoFactory.GetOrCreate(key, m_baselineShaders.at(PassKind::PbrOpaque));
        commandList.SetPipelineState(&pipelineState);

        commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::ObjectCbv), objectConstants.gpu);
        commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::MaterialCbv), materialConstants.gpu);
        commandList.SetGraphicsRootDescriptorTable(RootIndex(RootParameter::MaterialSrvs),
                                                   m_srvHeap->Gpu(m_lastMaterialTableBase));

        commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList.IASetVertexBuffers(0U, 1U, &meshView->vertexBuffer);
        commandList.IASetIndexBuffer(&meshView->indexBuffer);
        MarkDraw(commandList, draw);
        commandList.DrawIndexedInstanced(meshView->indexCount, 1U, 0U, 0, 0U);

        ++m_forwardDrawCount;
        m_forwardIndexCount += meshView->indexCount;
    }
    static_cast<void>(objectSlices);

    // 取证：本帧的上传全部做完之后再算 revision lag（>0 = 到帧尾仍有条目没跟上）。
    // 放在这里而不是沙盒侧：lag 依赖"本帧真的跑过 Ensure*"，只有渲染器知道那个时点。
    m_meshRevisionLag = m_assetCache.RevisionLagCount(assets);
    m_textureRevisionLag = m_assetCache.TextureRevisionLagCount(assets);

    // forward 结束后立刻 tone map：HDR → 后备缓冲（09 篇每帧录制的固定顺序）。
    pbrEvent.End();
    m_frameReadback.EndPass(commandList, m_swapChain.CurrentBackBufferIndex(), 1);
    RecordSkyboxPass(commandList);
    RecordToneMapPass(commandList);
}

void D3D12Renderer::CreateHdrTarget(ID3D12Device& device)
{
    const std::uint32_t width = m_swapChain.Config().width;
    const std::uint32_t height = m_swapChain.Config().height;
    if (width == 0U || height == 0U)
    {
        return;
    }

    // 重建（含 resize）时：先 Unregister 再释放（07 篇顺序契约），并换新的 generation。
    if (m_hdrTarget != nullptr)
    {
        m_stateTracker.Unregister(m_hdrKey);
        m_hdrTarget.Reset();
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1U;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // 必须给优化的 clear 值：每帧都会 ClearRenderTargetView，不给会被调试层判为
    // EXECUTION WARNING #820（CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE）。
    D3D12_CLEAR_VALUE optimizedClear{};
    optimizedClear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    optimizedClear.Color[0] = 0.0F;
    optimizedClear.Color[1] = 0.0F;
    optimizedClear.Color[2] = 0.0F;
    optimizedClear.Color[3] = 1.0F;

    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &optimizedClear,
                                                 IID_PPV_ARGS(&m_hdrTarget)),
                  "ID3D12Device::CreateCommittedResource(hdr target)");
    Internal::SetDebugName(m_hdrTarget.Get(), L"M5.Hdr.SceneColor");

    ++m_hdrGeneration;
    m_hdrKey = m_stateTracker.Register(*m_hdrTarget.Get(), 1U, D3D12_RESOURCE_STATE_RENDER_TARGET, L"M5.Hdr.SceneColor",
                                       m_hdrGeneration);

    if (!m_hdrRtv)
    {
        m_hdrRtv = m_rtvHeap->Allocate(1U);
    }
    device.CreateRenderTargetView(m_hdrTarget.Get(), nullptr, m_rtvHeap->Cpu(m_hdrRtv));

    // SRV 每次重建都换一个新 staging 槽位：旧描述符可能仍被在飞帧的材质表引用，
    // 覆盖它等于"改别人正在读的描述符"（05 篇 copy-on-write 语义）。
    if (m_hdrSrv.ptr == 0)
    {
        m_hdrSrv = m_stagingHeap->Cpu(m_stagingHeap->Allocate(1U));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    device.CreateShaderResourceView(m_hdrTarget.Get(), &srv, m_hdrSrv);
}

void D3D12Renderer::ReleaseHdrTarget()
{
    if (m_hdrTarget == nullptr)
    {
        return;
    }
    m_stateTracker.Unregister(m_hdrKey);
    m_hdrTarget.Reset();
}

void D3D12Renderer::RecordToneMapPass(ID3D12GraphicsCommandList& commandList)
{
    m_device->RecordDiagnosticEvent("M5.ToneMap");
    EventScope event(commandList, L"ToneMap");
    m_frameReadback.BeginPass(commandList, m_swapChain.CurrentBackBufferIndex(), 3);
    auto* const device = static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    const std::uint32_t frameIndex = m_swapChain.CurrentBackBufferIndex();

    // HDR 在 forward 期间是 RENDER_TARGET，tone map 要采样它 → 转 PS_RESOURCE，之后转回。
    m_stateTracker.Transition(m_hdrKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain.RtvHandle(frameIndex);
    commandList.OMSetRenderTargets(1U, &rtv, FALSE, nullptr); // tone map 不用深度

    // ToneMap.hlsl 的 HdrScene 在 t0（= 材质表槽 0），其余槽位保持 fallback。
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaterialSrvCount> sources = m_materialFallbacks;
    sources[0] = m_hdrSrv;
    PublishMaterialTable(*device, frameIndex, sources);

    const UploadAllocation postConstants = m_constantRing.TryAllocateConstant(sizeof(PostProcessConstants));
    if (!postConstants)
    {
        throw std::runtime_error{"constant upload ring exhausted while recording the tone map pass"};
    }
    PostProcessConstants postProcess{};
    postProcess.exposureEv = m_exposureEv;
    postProcess.debugHdr = m_debugView == 10U ? 1U : 0U;
    postProcess.inverseOutputSize = DirectX::XMFLOAT2{1.0F / static_cast<float>(m_swapChain.Config().width),
                                                      1.0F / static_cast<float>(m_swapChain.Config().height)};
    std::memcpy(postConstants.cpu, &postProcess, sizeof(postProcess));
    m_constantUploadBytes += sizeof(postProcess);

    PsoKey key;
    key.pass = PassKind::ToneMap;
    key.mirrored = false;
    key.shaderRevision = m_shaderRevision;
    key.rootSignatureRevision = m_rootSignatureRevision;
    commandList.SetPipelineState(&m_psoFactory.GetOrCreate(key, m_baselineShaders.at(PassKind::ToneMap)));
    commandList.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::FrameCbv), postConstants.gpu);
    commandList.SetGraphicsRootDescriptorTable(RootIndex(RootParameter::MaterialSrvs),
                                               m_srvHeap->Gpu(m_lastMaterialTableBase));
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.DrawInstanced(3U, 1U, 0U, 0U); // 全屏三角形：无顶点缓冲（SV_VertexID）
    ++m_toneMapDrawCount;
    m_frameReadback.EndPass(commandList, m_swapChain.CurrentBackBufferIndex(), 3);

    // 还原：下一帧 forward 仍以 RENDER_TARGET 写入 HDR。
    m_stateTracker.Transition(m_hdrKey, D3D12_RESOURCE_STATE_RENDER_TARGET);
    static_cast<void>(m_stateTracker.FlushBarriersTo(commandList));
}

void D3D12Renderer::BindRootSignature(ID3D12RootSignature& rootSignature, const std::uint64_t rootSignatureRevision)
{
    m_rootSignature = &rootSignature;
    m_rootSignatureRevision = rootSignatureRevision;
    // PSO 工厂与 root signature 绑定：PSO 创建时 runtime 校验两者兼容，因此 revision
    // 必须一起带进去（热重载换 root signature 时 key 也会变）。
    m_psoFactory.Initialize(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), rootSignature,
                            rootSignatureRevision);
}

void D3D12Renderer::CreateBaselinePsoSet(const std::filesystem::path& shaderDirectory)
{
    if (m_rootSignature == nullptr)
    {
        throw std::logic_error{"CreateBaselinePsoSet requires BindRootSignature first"};
    }

    // 产物文件名约定来自 tools/shader_compiler：<stem>.<entry>.<stage>.dxil。
    const auto loadShader =
        [&shaderDirectory](const std::string& stem, const std::string& entry, const std::string& stage)
    {
        const std::filesystem::path path = shaderDirectory / (stem + "." + entry + "." + stage + ".dxil");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            // 产物缺失必须显式失败：静默跳过会让"少一个 PSO"变成运行期才暴露的问题。
            throw std::runtime_error{"missing shader artifact: " + path.string() +
                                     " (run MiniEngineShaderCompiler first)"};
        }
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (size <= 0 || !file.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            throw std::runtime_error{"cannot read shader artifact: " + path.string()};
        }
        return bytes;
    };

    // 四个 pass 的 shader 集（Shadow 无 PS = depth-only）。
    m_baselineShaders.clear();
    m_baselineShaders[PassKind::Shadow] = PsoShaderSet{loadShader("ShadowDepth", "VSMain", "vs"), {}};
    m_baselineShaders[PassKind::PbrOpaque] =
        PsoShaderSet{loadShader("PbrForward", "VSMain", "vs"), loadShader("PbrForward", "PSMain", "ps")};
    m_baselineShaders[PassKind::Skybox] =
        PsoShaderSet{loadShader("Skybox", "VSMain", "vs"), loadShader("Skybox", "PSMain", "ps")};
    m_baselineShaders[PassKind::ToneMap] =
        PsoShaderSet{loadShader("ToneMap", "VSMain", "vs"), loadShader("ToneMap", "PSMain", "ps")};
    // 09 篇第 1 步：固定三角形的 shader 集（09 篇「迁移顺序」第 1 条）。
    m_baselineShaders[PassKind::TriangleSmoke] =
        PsoShaderSet{loadShader("TriangleSmoke", "VSMain", "vs"), loadShader("TriangleSmoke", "PSMain", "ps")};

    m_baselineShaders[PassKind::EquirectToCube] =
        PsoShaderSet{loadShader("EquirectToCube", "VSMain", "vs"), loadShader("EquirectToCube", "PSMain", "ps")};
    m_baselineShaders[PassKind::Irradiance] = PsoShaderSet{loadShader("IrradianceConvolution", "VSMain", "vs"),
                                                           loadShader("IrradianceConvolution", "PSMain", "ps")};
    m_baselineShaders[PassKind::EnvironmentDownsample] = PsoShaderSet{
        loadShader("IrradianceConvolution", "VSMain", "vs"), loadShader("IrradianceConvolution", "PSDownsample", "ps")};
    m_baselineShaders[PassKind::Prefilter] = PsoShaderSet{loadShader("PrefilterEnvironment", "VSMain", "vs"),
                                                          loadShader("PrefilterEnvironment", "PSMain", "ps")};
    m_baselineShaders[PassKind::BrdfLut] =
        PsoShaderSet{loadShader("IntegrateBrdf", "VSMain", "vs"), loadShader("IntegrateBrdf", "PSMain", "ps")};

    // 08 篇：init 时创建**全部** baseline PSO（帧循环之后只查找，不再创建）。
    CreateBaselineSetForRevision(m_shaderRevision);
    m_psoBaselineReady = true;

    m_psoFactory.LogPsoInventory();
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info,
                         "d3d12 baseline pso set: count=" + std::to_string(m_baselinePsoCount) +
                             " shaderRevision=" + std::to_string(m_shaderRevision) +
                             " creationUs=" + std::to_string(m_psoFactory.Stats().creationMicroseconds) +
                             " pending=" + std::to_string(m_psoFactory.PendingPsoCount()) +
                             " failed=" + std::to_string(m_psoFactory.Stats().failedCreations));
}

void D3D12Renderer::CreateBaselineSetForRevision(const std::uint64_t shaderRevision)
{
    // baseline 集：Shadow / PbrOpaque(+mirrored) / Skybox(+mirrored) / ToneMap / TriangleSmoke
    // = 7 个 PSO。键的四个维度中 pass 与 mirrored 在此枚举，另两个（shader / root signature
    // revision）由调用方给出——热重载因此只需要换 revision 就能得到一整套新 PSO。
    //
    // TriangleSmoke 必须在这里（09 篇第 1 步）：它每帧被 RecordTrianglePass 查一次，
    // 若不在 init 建好，第一次查就会**在帧内创建 PSO**——违反 08 篇硬约束，并被沙盒的
    // `unexpectedCreations` 判据判为退出码 3（这正是本节实现期自查发现的缺口）。
    const std::array<std::pair<PassKind, bool>, 13> baselineKeys{{
        {PassKind::Shadow, false},
        {PassKind::Shadow, true},
        {PassKind::EquirectToCube, false},
        {PassKind::Irradiance, false},
        {PassKind::EnvironmentDownsample, false},
        {PassKind::Prefilter, false},
        {PassKind::BrdfLut, false},
        {PassKind::PbrOpaque, false},
        {PassKind::PbrOpaque, true},
        {PassKind::Skybox, false},
        {PassKind::Skybox, true},
        {PassKind::ToneMap, false},
        {PassKind::TriangleSmoke, false},
    }};

    std::size_t createdInThisCall = 0U;
    for (const auto& [pass, mirrored] : baselineKeys)
    {
        const PsoShaderSet& shaders = m_baselineShaders.at(pass);

        PsoKey key;
        key.pass = pass;
        key.mirrored = mirrored;
        key.shaderRevision = shaderRevision;
        key.rootSignatureRevision = m_rootSignatureRevision;

        static_cast<void>(m_psoFactory.GetOrCreate(key, shaders));
        ++createdInThisCall;
        m_psoHighWater = std::max(m_psoHighWater, static_cast<std::uint64_t>(m_psoFactory.CachedPsoCount() +
                                                                             m_psoFactory.PendingPsoCount() +
                                                                             m_psoFactory.RetiredPendingCount()));
    }
    m_baselinePsoCount = createdInThisCall;
}

void D3D12Renderer::BeginPsoHotReload(const std::uint64_t nextShaderRevision)
{
    if (!m_psoBaselineReady)
    {
        throw std::logic_error{"BeginPsoHotReload requires CreateBaselinePsoSet first"};
    }
    // 事务第一步：创建**完整**替换集（pending）。此时旧集仍然完全可用——
    // 任一 PSO 创建失败都由调用方 Abort，不会留下半套 PSO（08 篇硬要求）。
    m_psoFactory.BeginHotReload(nextShaderRevision);
    m_pendingShaderRevision = nextShaderRevision;
    CreateBaselineSetForRevision(nextShaderRevision);
}

void D3D12Renderer::CommitPsoHotReload(const std::uint64_t retireFenceValue)
{
    m_psoFactory.CommitHotReload(retireFenceValue);
    m_shaderRevision = m_pendingShaderRevision;
}

void D3D12Renderer::AbortPsoHotReload() noexcept
{
    m_psoFactory.AbortHotReload();
}

void D3D12Renderer::ReclaimRetiredPsos(const std::uint64_t completedFenceValue)
{
    static_cast<void>(m_psoFactory.DeferredRelease().Reclaim(completedFenceValue));
}

std::size_t D3D12Renderer::BaselinePsoCount() const noexcept
{
    return m_baselinePsoCount;
}

std::uint64_t D3D12Renderer::PsoCreationMicroseconds() const noexcept
{
    return m_psoFactory.Stats().creationMicroseconds;
}

std::size_t D3D12Renderer::PendingPsoCount() const noexcept
{
    return m_psoFactory.PendingPsoCount();
}

const PsoFactoryStats& D3D12Renderer::PsoStats() const noexcept
{
    return m_psoFactory.Stats();
}

std::size_t D3D12Renderer::RetiredPsoCount() const noexcept
{
    return m_psoFactory.RetiredPendingCount();
}

std::uint64_t D3D12Renderer::ShaderRevision() const noexcept
{
    return m_shaderRevision;
}

const D3D12ResourceStateTracker& D3D12Renderer::StateTracker() const noexcept
{
    return m_stateTracker;
}

std::uint64_t D3D12Renderer::LastFrameBarrierTraceHash() const noexcept
{
    return m_lastFrameBarrierTraceHash;
}

std::uint64_t D3D12Renderer::TotalBarrierCount() const noexcept
{
    return m_totalBarrierCount;
}

std::uint64_t D3D12Renderer::BarrierTraceChainHash() const noexcept
{
    return m_barrierTraceChainHash;
}

} // namespace MiniEngine::Rhi::D3D12
