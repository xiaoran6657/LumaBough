// ============================================================================
// D3D12PsoFactory.h — PSO 创建、缓存与热重载事务（后端内部）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 3 条）
// 职责：把 08 篇的 PSO 生命周期落成一处实现：
//   - init 时创建全部 baseline PSO（帧循环绝不创建 PSO、绝不编译 shader）；
//   - cache key = 08 篇的 PsoKey（pass/mirrored/shader 版本/root signature 版本）；
//   - formats/depth/raster/blend 由本文件内的**格式表**显式给出（08 篇表格），
//     不依赖任何隐式默认值；
//   - 热重载是事务：先创建完整替换集（BeginHotReload → 逐个创建 → Commit 或 Abort），
//     Commit 时把旧 PSO 挂到"最后引用它的 fence"延迟释放（复用 06 篇的 DeferredRelease）；
//   - 失败保留 HRESULT 与调试层诊断（D3D12 没有 PSO 序列化错误 blob，细节在 InfoQueue）。
// M5 不做 Pipeline Library / disk cache（08 篇：M7 再评估）。
// 内部性说明：后端内部类型（src/），直接暴露 ID3D12PipelineState。
// 关联：docs/architecture/README.md（PSO key / lifecycle / format 表）
//       engine/rhi/d3d12/include/MiniEngine/Rhi/D3D12/D3D12PsoKey.h（键）
//       engine/rhi/d3d12/src/D3D12DeferredRelease.h（旧 PSO 的延迟释放）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "D3D12DeferredRelease.h"
#include "MiniEngine/Rhi/D3D12/D3D12PsoKey.h"

namespace MiniEngine::Rhi::D3D12
{
// 一个 pass 的 shader 集（VS 必需；depth-only pass 的 PS 为空）。
struct PsoShaderSet final
{
    std::vector<std::byte> vertexShader;
    std::vector<std::byte> pixelShader; // 空 = depth-only（PSO 的 PS 为空）
};

// 08 篇格式表（RTV/DSV per pass）的机读形态：测试直接对照它断言 PSO desc。
struct PassFormatEntry final
{
    std::uint32_t renderTargetCount = 1;
    DXGI_FORMAT renderTargetFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthStencilFormat = DXGI_FORMAT_UNKNOWN;
};

// 主深度格式（PBR/Skybox 用；09 篇主 pass 与之保持一致）。
inline constexpr DXGI_FORMAT kMainDepthFormat = DXGI_FORMAT_D32_FLOAT;

// 格式表查询（纯函数：与 PsoFactory 的 desc 构造同源，测试可独立断言）。
[[nodiscard]] PassFormatEntry PassFormats(PassKind pass) noexcept;

// 光栅化绕序契约（正面 = CCW 还是 CW）。
//
// **这是 09 篇第 1 步实测抓出的结构性缺陷**：工厂原实现写的是
// `FrontCounterClockwise = mirrored ? TRUE : FALSE`（非镜像 = CW 正面），而 M4/D3D11
// 侧是 `FrontCounterClockwise = TRUE` + 镜像取反（`D3D11Renderer.cpp` 的
// "M3 烘焙网格正面为 CCW，故 CCW 为正面"）。两者相反 → D3D12 端每个网格都被背面剔除，
// 现象是"draw 发了、像素是空的"（三角形 smoke 首轮实测：导出 RT 全是 clear 色）。
// 结构性差异不会在指标里表现为"轻微色差"，而是整片消失，因此这里把它提成**可断言的契约**：
// 非镜像 = CCW 正面（与 M4 一致），镜像 = CW 正面。
[[nodiscard]] bool FrontCounterClockwiseIsFront(bool mirrored) noexcept;

// 剔除模式契约：哪些 pass 的几何**不携带绕序语义**，必须与绕序约定解耦。
//
// **这是 09 篇第 2 步实测抓出的结构性缺陷**（与第 1 步的绕序缺陷同类）：ToneMap 的
// 超屏三角形（FullscreenTriangle.hlsli，CW）与 Skybox 的"相机位于立方体内表面"都不
// 携带绕序语义，D3D11 侧用独立的 noCull 光栅化（`D3D11Renderer.cpp` 的
// `noCullDesc.CullMode = D3D11_CULL_NONE`，注释明说"skybox 与 tone map 共用"）；
// D3D12 工厂原实现给**所有** pass 都上 CULL_BACK → tone map 全屏三角形被背面剔除，
// 现象是"31 个 draw 全部发出、后备缓冲只剩 clear 色"（RenderDoc pixel history 实测
// `passed=no flags=backfaceCulled`）。因此：ToneMap/Skybox = CULL_NONE，
// 其余（Shadow/PbrOpaque/TriangleSmoke）= CULL_BACK + 上面两条绕序契约。
[[nodiscard]] D3D12_CULL_MODE CullModeForPass(PassKind pass) noexcept;

// PSO 创建/缓存统计（进 metadata：08 篇要求记录 PSO creation 时间）。
struct PsoFactoryStats final
{
    std::uint64_t created = 0;              // 累计真正创建的 PSO 数（cache miss）
    std::uint64_t cacheHits = 0;            // 命中缓存的次数
    std::uint64_t creationMicroseconds = 0; // 累计创建耗时
    std::uint64_t failedCreations = 0;
};

// M6 public payload 持有强引用；缓存只持弱引用，实际退役继续由 public owner 的 fence 决定。
struct AdaptedPso final
{
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
};
class D3D12PsoFactory final
{
  public:
    D3D12PsoFactory() = default;
    ~D3D12PsoFactory() = default;
    D3D12PsoFactory(const D3D12PsoFactory&) = delete;
    D3D12PsoFactory& operator=(const D3D12PsoFactory&) = delete;

    // 绑定 device 与 root signature（PSO 创建时 runtime 会校验两者兼容）。
    // 失败：device/rootSignature 为空、重复 Initialize → 抛异常。
    void Initialize(ID3D12Device& device, ID3D12RootSignature& rootSignature, std::uint64_t rootSignatureRevision);

    // 取得（必要时创建）一个 PSO。帧循环只应调用 Get（命中缓存），创建发生在 init
    // 与热重载事务内。
    //
    // 失败：未初始化、PSO 创建失败 → std::runtime_error（文本含 pass 名、mirrored、
    //   输入布局名与 HRESULT；细节见调试层输出）。
    ID3D12PipelineState& GetOrCreate(const PsoKey& key, const PsoShaderSet& shaders);

    // 已完成 public descriptor lowering 的入口；key 必须包含 shader/layout 身份与全部静态状态。
    // 同一存活 revision 命中缓存；owner 释放最后一个 payload 后缓存不能延长 PSO 寿命。
    std::shared_ptr<AdaptedPso> AcquireAdapted(std::string key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

    // ---- 热重载事务 ----
    // BeginHotReload 之后的 GetOrCreate 把新 PSO 放进 pending 集，旧集保持不变。
    // 事务契约：pending 必须是**完整**替换集（生产路径由 CreateBaselineSetForRevision 保证）。
    void BeginHotReload(std::uint64_t nextShaderRevision);
    // Commit：把 pending 集发布为当前集；旧 PSO 交给 deferred（按"最后引用 fence"释放）。
    // 语义是**整体替换**——Commit 后 `CachedPsoCount() == PendingPsoCount()`（提交前），
    // 旧集不留在缓存里（否则会以 pipelineState==nullptr 的空条目形式累积，见 .cpp 注释）。
    // 失败：没有 pending 集 → std::logic_error；pending 集**为空** → std::logic_error
    //   （空替换集会把当前集清空，而替换集永远不该为空）。两种失败都不改变事务状态，
    //   调用方可补齐替换集后重试，或 AbortHotReload。
    void CommitHotReload(std::uint64_t retireFenceValue);
    // Abort：丢弃 pending 集（旧集保持可用，符合"失败保留旧 set"）。
    void AbortHotReload() noexcept;

    [[nodiscard]] bool IsHotReloadPending() const noexcept;
    [[nodiscard]] std::size_t PendingPsoCount() const noexcept;
    [[nodiscard]] std::size_t CachedPsoCount() const noexcept;
    // 已进入"按 fence 延迟释放"队列但尚未回收的旧 PSO 数（08 篇：旧 PSO 必须由
    // 最后引用它的 fence 兜住，因此该数在 fence 完成前不应回到 0）。
    [[nodiscard]] std::size_t RetiredPendingCount() const noexcept;
    [[nodiscard]] const PsoFactoryStats& Stats() const noexcept;
    // 旧 PSO 的延迟释放队列（Commit 后由调用方在帧边界 Reclaim）。
    [[nodiscard]] D3D12DeferredRelease& DeferredRelease() noexcept;
    // 把本工厂持有的全部 PSO 描述写到日志（取证：PIX 之外的机读副本）。
    void LogPsoInventory() const;

  private:
    struct Entry final
    {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
        PassFormatEntry formats;
        std::string debugName;
    };

    [[nodiscard]] static std::string MakePsoName(const PsoKey& key);
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12PipelineState> CreatePipelineState(const PsoKey& key,
                                                                                  const PsoShaderSet& shaders,
                                                                                  std::string& outDebugName);

    std::map<std::string, std::weak_ptr<AdaptedPso>> m_adapted;
    ID3D12Device* m_device = nullptr;
    ID3D12RootSignature* m_rootSignature = nullptr;
    std::uint64_t m_rootSignatureRevision = 0;
    std::map<PsoKey, Entry> m_current; // 当前生效集
    std::map<PsoKey, Entry> m_pending; // 热重载候选集
    bool m_hotReloadPending = false;
    PsoFactoryStats m_stats;
    D3D12DeferredRelease m_deferred;
};
} // namespace MiniEngine::Rhi::D3D12
