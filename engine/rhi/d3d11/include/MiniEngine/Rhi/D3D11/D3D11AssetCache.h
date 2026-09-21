#pragma once

// 7-D：D3D11 concrete GPU 上传缓存（07 篇 D3D11 concrete reload）。
//
// - map key = typed AssetHandle，entry 不重复存 handle（snippet 契约）；
// - entry.uploadedRevision 与 CPU pool revision 比较驱动热替换；
// - 07 篇规则：创建全新 immutable 资源成功后才替换 entry，绝不原地修改旧
//   resource；失败保留旧 entry/uploadedRevision，lag 通过 RevisionLagCount 暴露；
// - 旧 ComPtr 释放交给 D3D11 runtime（outstanding GPU 引用生命周期由其管理）。
// D3D11 类型只出现在本 target；Assets/World 头不含 D3D11。
// Deferral：debug name（Asset URI/revision）与 texture 缓存随真实 payload 篇落地。

#include <MiniEngine/Assets/AssetHandle.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace MiniEngine::Assets
{
class AssetManager;
// AssetHandle<T> 作为模板实参只需类型声明（_handle 体只存 index/generation）。
struct MeshAsset;
struct TextureAsset;
} // namespace MiniEngine::Assets

namespace MiniEngine::Rhi::D3D11
{
class D3D11AssetCache final
{
  public:
    // 已上传网格的绘制视图：裸指针仅当次绘制有效，所有权仍在缓存内。
    struct GpuMeshView final
    {
        ID3D11Buffer* vertexBuffer{};
        ID3D11Buffer* indexBuffer{};
        std::uint32_t indexCount{};
    };

    // 已上传贴图的采样视图：同上，指针不跨帧持有。
    struct GpuTextureView final
    {
        ID3D11ShaderResourceView* srv{};
    };

    D3D11AssetCache();
    ~D3D11AssetCache();
    D3D11AssetCache(const D3D11AssetCache&) = delete;
    D3D11AssetCache& operator=(const D3D11AssetCache&) = delete;

    // uploadedRevision == CPU revision → no-op 返回 true；否则创建全新 immutable
    // VB/IB 并替换 entry。stale handle 或创建失败返回 false（保留旧 entry）。
    // 占位 payload（0 顶点/0 索引）：记录 revision、不创建缓冲，无可绘制视图。
    [[nodiscard]] bool EnsureUploaded(ID3D11Device& device, const Assets::AssetManager& assets,
                                      const Assets::AssetHandle<Assets::MeshAsset> handle);

    // 仅当已上传且含可绘制 VB/IB 时返回视图；供 draw 前绑定。
    [[nodiscard]] std::optional<GpuMeshView> TryGetUploadedView(
        const Assets::AssetHandle<Assets::MeshAsset> handle) const;

    // TextureAsset（RGBA8，可选 sRGB）→ immutable Texture2D + SRV；revision 驱动替换。
    [[nodiscard]] bool EnsureTextureUploaded(ID3D11Device& device, const Assets::AssetManager& assets,
                                             const Assets::AssetHandle<Assets::TextureAsset> handle);

    // 已上传且有 SRV 时返回视图；占位 payload（0×0）不创建资源。
    [[nodiscard]] std::optional<GpuTextureView> TryGetTextureView(
        const Assets::AssetHandle<Assets::TextureAsset> handle) const;

    // CPU revision 领先于已上传 revision 的条目数（07 篇 gpuRevisionLagCount）。
    [[nodiscard]] std::uint32_t RevisionLagCount(const Assets::AssetManager& assets) const;
    [[nodiscard]] std::uint32_t TextureRevisionLagCount(const Assets::AssetManager& assets) const;

    // 淘汰不再可解析（已 Removed/Unload）的 GPU entry（P1-2：watch 长会话下 map 不得只增不减）。
    // 每帧由 Renderer 在 Render 前调用一次；条数少，全扫成本可忽略。
    void PruneStale(const Assets::AssetManager& assets);

    // 释放全部已上传的 GPU 资源（配合 Renderer 的 device 释放顺序）。
    void ReleaseAll();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Rhi::D3D11
