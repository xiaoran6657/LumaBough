// ============================================================================
// D3D12Renderer.h — 最小 clear/present 帧循环与安全点 Resize（04 篇）
// 里程碑：M5（04 篇 Command List、SwapChain 与 Resize；手抄清单第 2 条）
// 职责：把 M5-03 的提交时间线与 M5-04 的交换链组成一帧：
//       BeginFrame（取 SwapChain 的 current index）→ back buffer
//       PRESENT→RENDER_TARGET → OMSetRenderTargets + viewport/scissor →
//       ClearRenderTargetView → RENDER_TARGET→PRESENT → Close/Execute/Signal →
//       Present。每帧 Reset 后**显式**重设状态（D3D12 direct list 不继承上次
//       记录状态）；heap/RootSignature/PSO 随 05/08 篇加入同一处基线设置。
// 内部性说明：后端内部类型（src/），由 Sandbox 组合根（经公共门面）与设备级
//       测试消费；本篇尚未提供公共渲染器头——真正的公共面随 09 篇落地。
// 关联：docs/architecture/README.md（最小 clear/present、Resize 八步）
//       docs/architecture/DECISIONS.md（决策 1/2/4）
// ============================================================================
#pragma once

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <MiniEngine/Rhi/D3D12/D3D12RootBindings.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "D3D12AssetCache.h"
#include "D3D12Constants.h"
#include "D3D12DepthBuffer.h"
#include "D3D12DescriptorHeap.h"
#include "D3D12FrameReadback.h"
#include "D3D12PsoFactory.h"
#include "D3D12Queue.h"
#include "D3D12ResourceStateTracker.h"
#include "D3D12SwapChain.h"
#include "D3D12UploadManager.h"
#include "D3D12UploadRing.h"

// 前置声明：渲染器只在帧内**借用**这两个对象（09 篇第 2 步的帧输入），
// 因此公共/内部头都不需要拉进 Assets 与 World 的完整定义。
namespace MiniEngine::Assets
{
class AssetManager;
}
namespace MiniEngine::World
{
struct RenderPacket;
}

namespace MiniEngine::Rhi::D3D12
{
struct D3D12IblSet;

// 渲染器创建选项（Sandbox CLI 与测试共用）。
struct D3D12RendererOptions final
{
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool vsync = true;
    // 与 M4 D3D11 基线一致的背景色：M5-09 parity 时两侧先比"纯 clear"这一帧，
    // 该值必须逐位相同（0.04, 0.08, 0.14）。
    float clearColor[4] = {0.04F, 0.08F, 0.14F, 1.0F};
};

// 退休计数不包含仍在使用的 active 资源；shutdown flush 后所有项应归零。
struct D3D12RetirementCounts final
{
    std::size_t assets = 0, ibl = 0, pendingIbl = 0, psos = 0, descriptors = 0, uploads = 0;
    std::uint64_t constantBytes = 0, uploadBytes = 0;
};

class D3D12Renderer final
{
  public:
    // 构造/析构都 out-of-line：本类按值持有 D3D12AssetCache（PImpl），而 Impl 的完整
    // 定义只存在于 D3D12AssetCache.cpp——内联默认构造/析构会要求每个消费者 TU 都能
    // 实例化 unique_ptr<Impl> 的删除器（MSVC C2027/C2338）。
    D3D12Renderer();
    ~D3D12Renderer();
    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    // 创建 Queue（M5-03）与 SwapChain（M5-04），并准备好 viewport/scissor。
    // nativeWindow 为 WindowsWindow::NativeHandle() 的不透明句柄。
    //
    // 失败：HRESULT 失败抛 HResultError；尺寸为 0 或句柄为空抛 std::invalid_argument。
    void Initialize(D3D12Device& device, void* nativeWindow, const D3D12RendererOptions& options);

    // 渲染并呈现一帧。返回 false 表示本帧未呈现（挂起或窗口被遮挡）。
    //
    // 失败：未初始化、resize 失败状态、Present 的 device removed 等抛异常
    //   （上层走 fatal 路径，不再继续渲染）。
    [[nodiscard]] bool RenderFrame();

    // WM_SIZE 路径：只记录 pending 尺寸（0×0 = minimized）。
    void RequestResize(std::uint32_t width, std::uint32_t height);

    // 安全点：若有 pending resize 则执行完整重建（flush → 释放 → ResizeBuffers →
    // 重建 RTV → 复位状态 → 更新 viewport/scissor）。
    // 返回：是否执行了 resize。
    bool ApplyPendingResizeIfNeeded();

    // 帧循环收尾/重配置用（与 swap chain 的 vsync 保持一致）。
    void SetVsync(bool vsync) noexcept;

    // 显式 flush（只允许 resize/shutdown/一次性初始化边界；reason 进 trace）。
    void FlushGpu(std::string_view reason);

    // InfoQueue 全量取走（Sandbox 每轮 Gate 终点调用）。
    [[nodiscard]] ValidationReport DrainInfoQueue();

    [[nodiscard]] const SwapChainConfig& SwapChainFacts() const noexcept;
    [[nodiscard]] std::uint64_t PresentedFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t SkippedFrameCount() const noexcept;
    [[nodiscard]] bool IsSuspended() const noexcept;
    [[nodiscard]] D3D12Queue& Queue() noexcept;
    [[nodiscard]] D3D12SwapChain& SwapChain() noexcept;

    // ---- 09 篇迁移顺序第 1 步：固定 triangle ----
    // 打开后，每帧在 clear 之后额外录一段 TriangleSmoke pass：绑定 root signature +
    // 该 pass 的 PSO + 两个根 CBV（b0 FrameConstants / b1 ObjectConstants，走常量上传环）
    // + 主深度 DSV + 顶点缓冲，然后 DrawInstanced(3,1,0,0)。
    //
    // 为什么需要它：它证明"root signature + PSO + 根 CBV + 主深度 + draw"这条链路成立，
    // 且是 09 篇「不要一次复制完整 D3D11 renderer 后看黑屏」的第一个可见里程碑。
    // 关闭时（默认）帧循环与 04 篇的 clear/present 逐位相同，既有取证不受影响。
    void SetTriangleEnabled(bool enabled) noexcept;
    [[nodiscard]] bool TriangleEnabled() const noexcept;
    // 累计录制的三角形 draw 次数与顶点数（进 metadata：证明 draw 真的发了）。
    [[nodiscard]] std::uint64_t TriangleDrawCount() const noexcept;
    [[nodiscard]] std::uint64_t TriangleVertexCount() const noexcept;
    // TriangleSmoke 专用写入字节数（b0 + b1 每次录制各一次，不含其他 pass）。
    [[nodiscard]] std::uint64_t TriangleConstantUploadBytes() const noexcept;
    // 全部 pass 累计经常量上传环写入的结构体字节数，不含对齐开销。
    [[nodiscard]] std::uint64_t ConstantUploadBytes() const noexcept;
    [[nodiscard]] const D3D12DepthBuffer& DepthBuffer() const noexcept;

    // ---- 09 篇第 2 步：descriptor 表与 RenderPacket 消费 ----
    // 绑定 shader-visible 的 CBV_SRV_UAV heap（沙盒创建并提供，与 root signature 同款：
    // 渲染器只持非拥有引用）。绑定后渲染器会从它分配：
    //   3 个材质表（每 FrameContext 一个，各 5 槽，避免"CPU 改写 descriptor 时 GPU
    //   还在读上一帧的表"）与 1 个 global 表（t5—t8，内容稳定）；
    //   另建一个**非 shader-visible** 的 staging heap 承载纹理 SRV 与 5 个材质 fallback
    //   （M5-05 实测：拷贝描述符的源不能是 shader-visible heap）。
    // 失败：重复绑定、容量不足（Allocate 抛）→ 异常。
    void BindDescriptorHeap(D3D12DescriptorHeap& srvHeap);

    // 设置本帧要绘制的 RenderPacket 与资产来源（**非拥有**，只在 RenderFrame 期间使用）。
    // 传 nullptr 表示本帧不画 forward pass（与 04 篇 clear/present 行为一致）。
    void SetFrameInput(const World::RenderPacket* packet, const Assets::AssetManager* assets) noexcept;

    // 调试视图编号（写入 b0.w；与 M4 一致：1=baseColor、2=normal、3=metallic、
    // 4=roughness、5=AO、6=direct、8=shadow、0=最终着色）。**不进 PSO key**。
    void SetDebugView(std::uint32_t debugView) noexcept;
    [[nodiscard]] std::uint32_t DebugView() const noexcept;

    [[nodiscard]] std::uint64_t ForwardDrawCount() const noexcept;
    [[nodiscard]] std::uint64_t ForwardIndexCount() const noexcept;
    [[nodiscard]] std::uint64_t ForwardSkippedDrawCount() const noexcept;
    [[nodiscard]] std::uint32_t UploadedMeshCount() const noexcept;
    [[nodiscard]] std::uint32_t UploadedTextureCount() const noexcept;
    [[nodiscard]] std::uint32_t MaterialTableCount() const noexcept;
    // 表在 shader-visible heap 里的起始槽位（进 metadata；沙盒不再自己分配这两张表）。
    [[nodiscard]] std::uint32_t MaterialTableBase() const noexcept;
    [[nodiscard]] std::uint32_t GlobalTableBase() const noexcept;
    [[nodiscard]] bool DescriptorHeapBound() const noexcept;
    // 资产上传的累计字节数与失败/占位计数（进 metadata：证明"贴图与网格真的上去了"，
    // 且失败没有被静默吞掉——02 篇失败语义要求失败可见）。
    [[nodiscard]] std::uint64_t UploadedVertexBytes() const noexcept;
    [[nodiscard]] std::uint64_t UploadedIndexBytes() const noexcept;
    [[nodiscard]] std::uint64_t UploadedTextureBytes() const noexcept;
    [[nodiscard]] std::uint32_t PlaceholderAssetCount() const noexcept;
    [[nodiscard]] std::uint32_t RejectedAssetUploadCount() const noexcept;
    // 上一帧 forward pass 结束时（本帧的 Ensure* 全部跑完）仍"CPU revision 领先于 GPU"
    // 的条目数（与 D3D11 的 gpuRevisionLagCount 同义）。恒 0 才说明帧内把该传的都传了；
    // 取值时机是**每帧重算**（不是累计），因此必须帧内全部上传完成后取。
    [[nodiscard]] std::uint32_t MeshRevisionLag() const noexcept;
    [[nodiscard]] std::uint32_t TextureRevisionLag() const noexcept;
    [[nodiscard]] std::uint64_t ToneMapDrawCount() const noexcept;

    // 开关用于逐 pass parity，默认完整渲染；环境无效时保留明确的黑色 fallback。
    void SetLightingEnabled(bool shadows, bool environment, bool skybox) noexcept;
    void SetExposure(float exposureEv) noexcept;
    // 环境句柄由沙盒按 manifest URI 解析；revision 变化才生成替换集。
    void SetEnvironment(Assets::AssetHandle<Assets::TextureAsset> panorama) noexcept;
    // 只有生成提交的 fence 完成且回读 finite/非零检查通过，环境才 Ready。
    [[nodiscard]] bool IblReady() const noexcept;
    [[nodiscard]] std::uint64_t ShadowDrawCount() const noexcept;
    [[nodiscard]] std::uint64_t SkyboxDrawCount() const noexcept;

    // 截图只排队到下一次真实录制；调用者等待 IBL Ready 后再请求。
    void RequestScreenshot();
    [[nodiscard]] std::optional<D3D12Rgba8Image> TakeScreenshot();
    [[nodiscard]] std::vector<D3D12FrameTiming> TakeGpuTimings();
    [[nodiscard]] const D3D12UploadRing& AssetUploadRing() const noexcept
    {
        return m_assetUploadRing;
    }
    [[nodiscard]] const D3D12UploadRing& ConstantRing() const noexcept
    {
        return m_constantRing;
    }
    [[nodiscard]] const UploadManagerStats& AssetUploadStats() const noexcept
    {
        return m_assetUploadManager.Stats();
    }
    [[nodiscard]] std::size_t RetiredAssetCount() const noexcept
    {
        return m_assetCache.RetiredResourceCount();
    }
    [[nodiscard]] double LastPresentMilliseconds() const noexcept
    {
        return m_lastPresentMilliseconds;
    }
    [[nodiscard]] std::uint64_t SubmittedFrameCount() const noexcept
    {
        return m_submittedFrameCount;
    }

    // ---- 08 篇：Root Signature 与 PSO ----
    // root signature 由 05 篇的 CreateM5RootSignature 创建后绑定进来（PSO 创建时
    // runtime 会校验两者兼容）。
    void BindRootSignature(ID3D12RootSignature& rootSignature, std::uint64_t rootSignatureRevision);
    // init 时创建全部 baseline PSO（4 个 pass + PBR/Skybox 的 mirrored 变体 = 6 个），
    // shader 字节码来自 tools/shader_compiler 产出的 .dxil（**不在帧内编译/创建**）。
    // 失败：产物缺失即抛——不接受"缺一个 PSO 就跳过"的静默降级。
    void CreateBaselinePsoSet(const std::filesystem::path& shaderDirectory);

    // 热重载事务（08 篇「PSO lifecycle」）：**先完整创建替换集**，再在 frame
    // boundary 一次性 Commit。
    //   Begin  —— 把 shaderRevision 提升到 nextShaderRevision 并创建全部新 PSO
    //             （进 pending 集，旧集不受影响）；
    //   Commit —— 发布 pending 集，旧 PSO 挂 retireFenceValue 按 fence 延迟释放；
    //   Abort  —— 丢弃 pending 集（创建阶段任一失败都走这里，旧集保持可用）。
    // 失败：未创建 baseline 集 / 事务状态不对 → std::logic_error。
    void BeginPsoHotReload(std::uint64_t nextShaderRevision);
    void CommitPsoHotReload(std::uint64_t retireFenceValue);
    void AbortPsoHotReload() noexcept;
    // 回收已完成 fence 对应的旧 PSO（帧边界调用，判据唯一：completed >= retireFence）。
    void ReclaimRetiredPsos(std::uint64_t completedFenceValue);

    [[nodiscard]] std::size_t BaselinePsoCount() const noexcept;
    [[nodiscard]] std::uint64_t PsoCreationMicroseconds() const noexcept;
    [[nodiscard]] std::size_t PendingPsoCount() const noexcept;
    // PSO 创建/缓存统计（"帧内不创建 PSO"的取证来源：lookups = cacheHits + created）。
    [[nodiscard]] const PsoFactoryStats& PsoStats() const noexcept;
    [[nodiscard]] std::size_t RetiredPsoCount() const noexcept;
    [[nodiscard]] std::uint64_t ShaderRevision() const noexcept;

    // ---- 07 篇：状态跟踪取证字段 ----
    [[nodiscard]] const D3D12ResourceStateTracker& StateTracker() const noexcept;
    // 本帧 barrier trace 的滚动哈希（每帧 BeginRecording 重置）。
    //
    // "跨 run 稳定"的**适用条件**（M5-08 审查 P3-4，203 帧实验证实）：trace 的最后一行
    // 是最后一帧的 back buffer transition，而那一帧落在哪个后备缓冲上由
    // `(presentedFrames - 1) mod bufferCount` 决定——**帧数不同则该位可能不同**。
    // 因此跨 run 比较必须固定帧数（或按帧数换算）；M5-07 的 0xeb90…（200 帧）与
    // M5-08 的 0x422f…（60/300 帧）差异即源于此，不是行为变化。
    [[nodiscard]] std::uint64_t LastFrameBarrierTraceHash() const noexcept;
    [[nodiscard]] std::uint64_t TotalBarrierCount() const noexcept;
    // M5-11：全进程峰值，包含初始化/重载，不冒称测量区间计数。
    [[nodiscard]] std::uint64_t BarrierFrameHighWater() const noexcept
    {
        return m_barrierFrameHighWater;
    }
    [[nodiscard]] std::uint64_t PsoHighWater() const noexcept
    {
        return m_psoHighWater;
    }
    // 运行期累计的 barrier trace 链（每帧的 trace hash 链入），用于跨配置对比。
    [[nodiscard]] std::uint64_t BarrierTraceChainHash() const noexcept;

    // M5-10：只按实际 completed fence 回收；调用方决定是否先 flush。
    void ReclaimCompletedWork();
    [[nodiscard]] D3D12RetirementCounts RetirementCounts() const noexcept;
    [[nodiscard]] std::uint64_t IblFailureCount() const noexcept
    {
        return m_iblFailureCount;
    }
    [[nodiscard]] const std::string& IblLastError() const noexcept
    {
        return m_iblLastError;
    }
    void RetryEnvironment() noexcept;

  private:
    friend struct D3D12StabilityAccess; // 仅专用测试访问注入点，生产 sample 无触发参数。
    [[nodiscard]] bool HasUsableIbl() const noexcept;
    D3D12FrameReadback m_frameReadback;
    bool m_screenshotRequested = false;
    std::uint64_t m_submittedFrameCount = 0;
    double m_lastPresentMilliseconds = 0;
    // Reset 之后的显式状态基线（D3D12 不继承）：本篇 = viewport/scissor/RTV；
    // descriptor heap / Root Signature / PSO 随 05/08 篇加入此处。
    void ApplyCommandListBaselineState(ID3D12GraphicsCommandList& commandList, std::uint32_t backBufferIndex);

    // 按给定 shader revision 创建（或命中）整套 baseline PSO：init 与热重载共用
    // 同一段代码，"创建集合"这件事只有一处定义。
    void CreateBaselineSetForRevision(std::uint64_t shaderRevision);

    // 创建 fallback 资源与描述符（5 个材质 fallback + 4 个 global fallback），
    // 并把 global 表的 4 个槽位一次性写好（内容稳定，不需要按帧重用）。
    //
    // **必须在录制的帧内首次调用**：D3D12 禁止在 UPLOAD/READBACK 堆上创建纹理
    // （调试层实测 `CreateCommittedResource: A texture resource cannot be created on
    // a D3D12_HEAP_TYPE_UPLOAD...`），因此 fallback 纹理只能走
    // DEFAULT + COPY_DEST → CopyTextureRegion → transition 的常规上传路径，
    // 而这条路径需要 command list。
    void CreateFallbackResources(ID3D12Device& device, ID3D12GraphicsCommandList& commandList);
    // 录制 forward pass：b0/b1/b2/b3 + 两张 descriptor 表 + 逐 draw 绘制。
    void RecordForwardPass(ID3D12GraphicsCommandList& commandList);
    // HDR 中间目标（RGBA16F，与 M4 的 profile 一致）：PbrOpaque/Skybox 的 RTV 按
    // 09 篇格式表被冻结为 RGBA16F，因此它们**不能**直写后备缓冲——必须先写 HDR，
    // 再由 tone map pass 显式 Linear→sRGB 编码到 UNORM 后备缓冲。
    void CreateHdrTarget(ID3D12Device& device);
    void ReleaseHdrTarget();
    // tone map pass：全屏三角形（SV_VertexID）+ HDR SRV（材质表槽 0）+ PostProcessConstants。
    void RecordToneMapPass(ID3D12GraphicsCommandList& commandList);
    // 把本帧的 5 个材质 SRV 写进当前 FrameContext 的材质表（从 staging heap 拷）。
    void PublishMaterialTable(ID3D12Device& device, std::uint32_t frameIndex,
                              const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaterialSrvCount>& sources);

    // 完整场景 pass 的资源与录制，实现集中在 D3D12RendererPasses.cpp。
    void CreateSceneResources(ID3D12Device& device);
    void RecordShadowPass(ID3D12GraphicsCommandList& commandList);
    void RecordSkyboxPass(ID3D12GraphicsCommandList& commandList);
    void PrepareEnvironment(ID3D12GraphicsCommandList& commandList);
    void PollEnvironment(std::uint64_t completedFence);
    void ReleaseIblSet(std::unique_ptr<D3D12IblSet>& set);
    void PublishGlobalTable(ID3D12Device& device, std::uint32_t frameIndex);
    // 单位立方体与 M4 共享方向约定，拥有自己的静态顶点和索引资源。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeGeometry;
    D3D12_VERTEX_BUFFER_VIEW m_cubeVertices{};
    D3D12_INDEX_BUFFER_VIEW m_cubeIndices{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    ResourceKey m_shadowKey{};
    std::unique_ptr<D3D12DescriptorHeap> m_shadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_shadowSrv{};
    std::array<DescriptorRange, kD3D12FrameContextCount> m_frameGlobalTables{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kGlobalSrvCount> m_globalFallbacks{};
    // 失败身份抑制无限重试；只有输入 revision 改变或显式 Retry 才重新生成。
    Assets::AssetHandle<Assets::TextureAsset> m_failedIblSource{};
    std::uint64_t m_failedIblRevision = 0, m_failedIblShaderRevision = 0, m_iblFailureCount = 0;
    std::string m_iblLastError;
    std::uint32_t m_iblFailureAfterDraws = 0;
    std::unique_ptr<D3D12IblSet> m_ibl;
    std::unique_ptr<D3D12IblSet> m_pendingIbl;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<D3D12IblSet>>> m_retiredIbl;
    Assets::AssetHandle<Assets::TextureAsset> m_environmentHandle{};
    std::uint64_t m_shadowDrawCount = 0;
    std::uint64_t m_skyboxDrawCount = 0;
    bool m_shadowsEnabled = true;
    bool m_environmentEnabled = true;
    bool m_skyboxEnabled = true;
    float m_exposureEv = 0.0F;

    // 固定三角形的顶点流：3 个顶点（position + color，stride 24）放进一个小的
    // committed UPLOAD 缓冲。静态数据只上传一次，因此不占常量上传环的预算。
    void CreateTriangleGeometry(ID3D12Device& device);
    // 录制 TriangleSmoke pass（只在该 pass 启用时调用）。
    void RecordTrianglePass(ID3D12GraphicsCommandList& commandList);

    // 非拥有指针：Device 由组合根持有且生命周期长于渲染器；InfoQueue 通道
    // （零容忍 Gate）经它访问，避免渲染器自己再查一次 ID3D12InfoQueue。
    D3D12Device* m_device = nullptr;
    D3D12Queue m_queue;
    D3D12ResourceStateTracker m_stateTracker;
    D3D12SwapChain m_swapChain;
    // 声明顺序即构造顺序：m_stateTracker 在两者之前，因此 tracker 的生命周期更长
    // （两者都持有 tracker 的非拥有指针）。m_depthBuffer 在 Resize 里先 Unregister 再释放。
    D3D12DepthBuffer m_depthBuffer;
    // 常量上传环：b0/b1 每帧一次，容量按"3 帧 × 两个常量 × 256B 对齐"留足余量。
    D3D12UploadRing m_constantRing;
    // 资产上传（09 篇第 2 步）：渲染器自持一条环与策略层，与沙盒的 06 篇压力环互不干扰。
    D3D12UploadRing m_assetUploadRing;
    D3D12UploadManager m_assetUploadManager;
    D3D12PsoFactory m_psoFactory;
    ID3D12RootSignature* m_rootSignature = nullptr;
    std::uint64_t m_rootSignatureRevision = 0;
    std::size_t m_baselinePsoCount = 0;
    // baseline 字节码按 pass 保存：热重载重编译后需按同一结构重建 PSO 集。
    std::map<PassKind, PsoShaderSet> m_baselineShaders;
    // 08 篇：当前生效 / 待提交的 shader revision（PSO key 的一个维度）。
    std::uint64_t m_shaderRevision = 0;
    std::uint64_t m_pendingShaderRevision = 0;
    bool m_psoBaselineReady = false;
    D3D12RendererOptions m_options;
    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissor{};
    std::uint64_t m_presentedFrames = 0;
    std::uint64_t m_skippedFrames = 0;
    std::uint64_t m_lastFrameBarrierTraceHash = 0;
    std::uint64_t m_totalBarrierCount = 0;
    std::uint64_t m_barrierFrameHighWater = 0;
    std::uint64_t m_psoHighWater = 0;
    std::uint64_t m_barrierTraceChainHash = 0;
    // 09 篇第 1 步（固定三角形）的状态与取证计数。
    bool m_triangleEnabled = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_triangleVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_triangleVertexBufferView{};
    std::uint64_t m_triangleDrawCount = 0;
    std::uint64_t m_triangleVertexCount = 0;
    std::uint64_t m_triangleConstantUploadBytes = 0;
    std::uint64_t m_constantUploadBytes = 0; // 全部 pass 的总计数。
    // ---- 09 篇第 2 步：descriptor 表、fallback 与帧输入 ----
    D3D12DescriptorHeap* m_srvHeap = nullptr;           // shader-visible（非拥有，沙盒提供）
    std::unique_ptr<D3D12DescriptorHeap> m_stagingHeap; // 非 shader-visible（本对象拥有）
    D3D12AssetCache m_assetCache;
    // 3 个材质表（按 FrameContext 轮转）+ 1 个 global 表（内容稳定）。
    std::array<DescriptorRange, kD3D12FrameContextCount> m_materialTables{};
    // 每个 draw 独占快照；BeginFrame 已等待该帧 fence，只有此时才能从槽 0 复用。
    std::array<std::vector<DescriptorRange>, kD3D12FrameContextCount> m_drawTables;
    std::uint32_t m_drawTableCursor = 0;
    std::uint32_t m_lastMaterialTableBase = 0;

    DescriptorRange m_globalTable{};
    // fallback 资源：0–4 = 材质五角色（白色 sRGB / +Z linear / G=B=1 / 白 / 黑 sRGB），
    // 5–6 = 黑 cube（irradiance、prefilter），7 = 中性 LUT (1,0)，8 = shadow 1×1 = 1.0。
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 9> m_fallbackResources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaterialSrvCount> m_materialFallbacks{};
    const World::RenderPacket* m_packet = nullptr;
    const Assets::AssetManager* m_assets = nullptr;
    std::uint32_t m_debugView = 0;
    std::uint64_t m_forwardDrawCount = 0;
    std::uint64_t m_forwardIndexCount = 0;
    std::uint64_t m_forwardSkippedDrawCount = 0;
    // 上一帧 forward 结束时的 revision lag（取证字段；每帧覆盖，不累计）。
    std::uint32_t m_meshRevisionLag = 0;
    std::uint32_t m_textureRevisionLag = 0;
    // ---- HDR 中间目标（09 篇第 2 步的必然配套，见 CreateHdrTarget 注释）----
    std::unique_ptr<D3D12DescriptorHeap> m_rtvHeap; // 本对象拥有（HDR 的 RTV）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hdrTarget;
    ResourceKey m_hdrKey{};
    DescriptorRange m_hdrRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_hdrSrv{};
    std::uint32_t m_hdrGeneration = 0U;
    std::uint64_t m_toneMapDrawCount = 0;
    bool m_descriptorHeapBound = false;
    bool m_fallbacksReady = false;
    bool m_initialized = false;
};
} // namespace MiniEngine::Rhi::D3D12
