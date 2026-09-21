// ============================================================================
// D3D12AssetCache.h — 资产 → GPU 资源缓存（网格 VB/IB 与纹理 SRV）
// 里程碑：M5（09 篇迁移顺序第 2 步「baked mesh + texture + depth」）
// 职责：把 CPU 侧的 `.memesh` / `.metex` 载荷按 **revision 驱动**上传成 DEFAULT heap
//       资源，并产出可直接绑定的 D3D12_VERTEX_BUFFER_VIEW / INDEX_BUFFER_VIEW 与
//       纹理 SRV 描述符。与 D3D11AssetCache 的语义一一对应（map key = 类型化 Handle、
//       entry 不存 Handle、失败保留旧 entry），差异只在"怎么上传"：
//       D3D11 用 immutable 资源 + 驱动管理生命周期；D3D12 必须自己 staging →
//       CopyBufferRegion/CopyTextureRegion → 显式 transition。
// 上传必须录进 command list（06 篇"上传批"）：所有 Ensure* 调用都要求调用方在
//       BeginFrame 之后、draw 之前调用，并把本帧的上传/转换与帧一起提交。
// 失败语义（与 02 篇 fallback 契约一致）：stale handle、payload 非法、创建/分配失败
//       都返回 false 并**保留旧 entry**——渲染端继续用上一个可用版本或 fallback，
//       绝不因为一个资产失败丢掉整帧；lag 由 RevisionLagCount 暴露。
// 内部性说明：后端内部类型（src/），直接暴露 ID3D12Resource 与视图结构。
// 关联：docs/architecture/README.md（M4 resource profile 保持）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11AssetCache.h（语义对照）
//       engine/rhi/d3d12/src/D3D12TextureUpload.h（mip 逐级上传的唯一实现）
// ============================================================================
#pragma once

#include <MiniEngine/Assets/AssetHandle.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace MiniEngine::Assets
{
class AssetManager;
struct MeshAsset;
struct TextureAsset;
} // namespace MiniEngine::Assets

namespace MiniEngine::Rhi::D3D12
{
class D3D12DescriptorHeap;
class D3D12ResourceStateTracker;
class D3D12UploadManager;

// 一次上传所需的全部设备侧句柄。刻意做成显式结构体而不是"缓存持有它们的指针"：
// 上传只发生在帧内（command list 有效），缓存因此不需要长期持有 list/manager。
struct AssetUploadContext final
{
    ID3D12Device& device;
    ID3D12GraphicsCommandList& commandList;
    D3D12UploadManager& uploadManager;
    D3D12ResourceStateTracker& tracker;
};

class D3D12AssetCache final
{
  public:
    // 已上传网格的绘制视图：视图结构按值返回，指向的资源所有权仍在缓存内。
    struct GpuMeshView final
    {
        D3D12_VERTEX_BUFFER_VIEW vertexBuffer{};
        D3D12_INDEX_BUFFER_VIEW indexBuffer{};
        std::uint32_t indexCount = 0;
    };

    // 构造/析构都 out-of-line：Impl 的完整定义只在 .cpp 里，内联实现会让每个消费者
    // TU 实例化 unique_ptr<Impl> 的删除器（MSVC C2027/C2338）。
    D3D12AssetCache();
    ~D3D12AssetCache();
    D3D12AssetCache(const D3D12AssetCache&) = delete;
    D3D12AssetCache& operator=(const D3D12AssetCache&) = delete;

    // 绑定 SRV 描述符的落脚点与状态跟踪器。
    //
    // 为什么必须是非 shader-visible 的 staging heap：材质表（shader-visible）的 5 个槽位
    // 要按 draw 重写，而 `CopyDescriptorsSimple`/`CopyDescriptors` 的**源不能是
    // shader-visible heap**（M5-05 实测约束）。纹理 SRV 因此先落在 staging heap，
    // 再由渲染端按 draw 拷进表。
    // tracker 在 Initialize 时绑定（而不是每次上传传入）：PruneStale/ReleaseAll 需要它
    // 做 Unregister，而这两个入口不接收上传上下文（07 篇：释放前必须先注销）。
    void Initialize(D3D12DescriptorHeap& srvStagingHeap, D3D12ResourceStateTracker& tracker);

    // revision 驱动上传网格：已是最新 → no-op true。
    // 失败：stale handle、payload 为空（占位）、创建/分配失败 → false（保留旧 entry）。
    [[nodiscard]] bool EnsureMeshUploaded(const AssetUploadContext& context, const Assets::AssetManager& assets,
                                          Assets::AssetHandle<Assets::MeshAsset> handle);

    // 仅当已上传且含可绘制 VB/IB 时返回视图。
    [[nodiscard]] std::optional<GpuMeshView> TryGetMeshView(Assets::AssetHandle<Assets::MeshAsset> handle) const;

    // revision 驱动上传纹理（完整 mip 链），并把 SRV 写进 staging heap。
    //
    // 格式口径与 M4 逐项一致：Rgba8 → 资源 `R8G8B8A8_TYPELESS`，SRV 按 colorSpace 取
    // `UNORM_SRGB`（sRGB 硬件解码）或 `UNORM`；Rgba16Float → `R16G16B16A16_FLOAT`。
    // 失败：stale handle、0×0/空像素、mip 越界、创建失败 → false（保留旧 entry）。
    [[nodiscard]] bool EnsureTextureUploaded(const AssetUploadContext& context, const Assets::AssetManager& assets,
                                             Assets::AssetHandle<Assets::TextureAsset> handle);

    // 已上传纹理的 SRV CPU 句柄（staging heap 内、位置稳定）：渲染端按 draw 把它
    // 拷进材质表的对应槽位。未上传/占位返回 false。
    [[nodiscard]] bool TryGetTextureSrv(Assets::AssetHandle<Assets::TextureAsset> handle,
                                        D3D12_CPU_DESCRIPTOR_HANDLE& outSrv) const;

    // CPU revision 领先于已上传 revision 的条目数（与 D3D11 的 gpuRevisionLagCount 同义）。
    [[nodiscard]] std::uint32_t RevisionLagCount(const Assets::AssetManager& assets) const;
    [[nodiscard]] std::uint32_t TextureRevisionLagCount(const Assets::AssetManager& assets) const;

    // 淘汰已不可解析（Removed/Unload）的 entry：先 Unregister 再释放（07 篇顺序契约）。
    void PruneStale(const Assets::AssetManager& assets);

    // 帧开始时只回收已提交且 fence 已完成的旧资源。
    void BeginFrame(std::uint64_t completedFenceValue);

    // 帧提交后把本帧产生的 pending 旧资源绑定到 submittedFenceValue。
    void CommitFrame(std::uint64_t submittedFenceValue);

    // 释放全部 GPU 资源（先 Unregister 再释放；需要 tracker 仍有效）。
    // 前置：调用方已证明 GPU 不再引用（渲染器的 FlushGpu 安全点）。
    void ReleaseAll();

    // ---- 取证字段（进 metadata）----
    [[nodiscard]] std::uint32_t UploadedMeshCount() const noexcept;
    [[nodiscard]] std::uint32_t UploadedTextureCount() const noexcept;
    [[nodiscard]] std::uint64_t UploadedVertexBytes() const noexcept;
    [[nodiscard]] std::uint64_t UploadedIndexBytes() const noexcept;
    [[nodiscard]] std::uint64_t UploadedTextureBytes() const noexcept;
    [[nodiscard]] std::uint32_t PlaceholderCount() const noexcept; // 占位 payload（0 顶点/0×0）
    [[nodiscard]] std::uint32_t RejectedUploadCount() const noexcept;
    // 已被替换/淘汰但仍在 retired 队列里等安全点释放的资源数（VB+IB 按两个计）。
    // 判据：替换/Prune 后 > 0（证明旧本体没被立即析构）；ReleaseAll（GPU idle 安全点）后 = 0。
    [[nodiscard]] std::uint32_t RetiredResourceCount() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Rhi::D3D12
