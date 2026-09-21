// ============================================================================
// D3D11AssetCache.cpp — GPU 上传缓存与"新建后替换"失败语义的实现
// 里程碑：M3（7-D）
// 职责：把 CPU 池中的 MeshAsset / TextureAsset 上传为 immutable VB/IB/Texture2D/SRV。
//       每个条目记录 uploadedRevision，与 CPU pool 的 revision 比较驱动热替换；
//       全部走"先建全新资源到临时 ComPtr、成功才替换 entry"的路径，
//       失败时旧 GPU 资源原样保留，lag 由 RevisionLagCount 暴露。
// 关联：docs/architecture/DECISIONS.md §5
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11AssetCache.h（对外契约）
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11AssetCache.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MiniEngine::Rhi::D3D11
{
using Microsoft::WRL::ComPtr;

namespace
{
// 创建 immutable 缓冲（顶点/索引共用）。数据为空或宽度为 0 返回空 ComPtr，
// 由调用方按"占位 payload 不建资源"处理；创建失败同样返回空而非抛异常，
// 让上层走"保留旧 entry、暴露 lag"的失败语义。
ComPtr<ID3D11Buffer> CreateImmutableBuffer(ID3D11Device& device, const std::byte* data, const std::uint32_t byteWidth,
                                           const D3D11_BIND_FLAG bindFlag)
{
    if (data == nullptr || byteWidth == 0)
    {
        return {};
    }

    D3D11_BUFFER_DESC description{};
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.ByteWidth = byteWidth;
    description.BindFlags = bindFlag;
    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = data;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device.CreateBuffer(&description, &initialData, &buffer)))
    {
        return {};
    }
    return buffer;
}

// .metex v2（完整 mip 链）→ immutable Texture2D + SRV。
// 资源格式按 pixelFormat 分派：RGBA8 走 R8G8B8A8_TYPELESS（视图格式由 colorSpace
// 决定：Srgb → UNORM_SRGB 硬件解码，Linear → UNORM）；Rgba16Float（04 篇 HDR
// environment）走 R16G16B16A16_FLOAT，线性、无 sRGB 变体。MipLevels = Cooker
// 确定性生成的完整链长（禁止 GPU GenerateMips 进入正式资产）。
bool CreateImmutableTexture(ID3D11Device& device, const Assets::TextureAsset& asset, ComPtr<ID3D11Texture2D>& texture,
                            ComPtr<ID3D11ShaderResourceView>& view)
{
    if (asset.width == 0 || asset.height == 0 || asset.pixels.empty() || asset.mips.size() != asset.mipCount)
    {
        return false; // 占位 payload：不创建资源（revision 仍会记录）
    }

    // 每级 mip 一个 subresource 描述（级联布局，offset 相对 pixels 起点）。
    std::vector<D3D11_SUBRESOURCE_DATA> initialData(asset.mipCount);
    for (std::uint32_t level = 0; level < asset.mipCount; ++level)
    {
        const Assets::TextureMipInfo& mip = asset.mips[level];
        if (static_cast<std::uint64_t>(mip.offset) + mip.byteSize > asset.pixels.size())
        {
            return false;
        }
        initialData[level].pSysMem = asset.pixels.data() + mip.offset;
        initialData[level].SysMemPitch = mip.rowPitch;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = asset.width;
    description.Height = asset.height;
    description.MipLevels = asset.mipCount;
    description.ArraySize = 1;
    description.Format = asset.pixelFormat == Assets::TexturePixelFormat::Rgba16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                                      : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> newTexture;
    if (FAILED(device.CreateTexture2D(&description, initialData.data(), &newTexture)))
    {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    // 视图格式：RGBA8 按 colorSpace 选 sRGB 变体；Rgba16Float 天然线性（FLOAT 无
    // sRGB 变体，数据按字节原样上传、采样时硬件转换 float）。
    viewDescription.Format = asset.pixelFormat == Assets::TexturePixelFormat::Rgba16Float
                                 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                             : asset.colorSpace == Assets::TextureColorSpace::Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                                   : DXGI_FORMAT_R8G8B8A8_UNORM;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = asset.mipCount;

    ComPtr<ID3D11ShaderResourceView> newView;
    if (FAILED(device.CreateShaderResourceView(newTexture.Get(), &viewDescription, &newView)))
    {
        return false;
    }

    texture = std::move(newTexture);
    view = std::move(newView);
    return true;
}
} // namespace

struct D3D11AssetCache::Impl final
{
    // handle 由 map key 承载而非 entry 字段（snippet 契约）。
    struct GpuMeshEntry final
    {
        std::uint64_t uploadedRevision{};
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
        std::uint32_t indexCount{};
    };

    struct GpuTextureEntry final
    {
        std::uint64_t uploadedRevision{};
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
    };

    std::unordered_map<Assets::AssetHandle<Assets::MeshAsset>, GpuMeshEntry,
                       Assets::AssetHandleHasher<Assets::MeshAsset>>
        meshes;
    std::unordered_map<Assets::AssetHandle<Assets::TextureAsset>, GpuTextureEntry,
                       Assets::AssetHandleHasher<Assets::TextureAsset>>
        textures;
};

// 不能在头文件里用 make_unique 初始化（Impl 在那里是不完整类型）；
// 必须在 Impl 定义之后于本文件构造，否则 m_impl 为 nullptr。
D3D11AssetCache::D3D11AssetCache() : m_impl{std::make_unique<Impl>()}
{
}

D3D11AssetCache::~D3D11AssetCache() = default;

bool D3D11AssetCache::EnsureUploaded(ID3D11Device& device, const Assets::AssetManager& assets,
                                     const Assets::AssetHandle<Assets::MeshAsset> handle)
{
    // P1-2：先验证 CPU payload 存在再触碰 map——stale handle 不得在表里插入空 entry。
    const auto cpuView = assets.Meshes().TryGet(handle);
    if (!cpuView.has_value())
    {
        return false; // stale handle：无 CPU payload 可上传，也不残留空 entry
    }
    auto found = m_impl->meshes.find(handle);
    if (found == m_impl->meshes.end())
    {
        found = m_impl->meshes.emplace(handle, Impl::GpuMeshEntry{}).first;
    }
    // GpuMeshEntry 是 Impl 的嵌套类型：在本类的成员函数里必须加 Impl:: 限定。
    Impl::GpuMeshEntry& entry = found->second;
    if (entry.uploadedRevision == cpuView->revision)
    {
        return true; // 已是最新：no-op
    }

    // 07 篇：先建全新 immutable 资源到临时 ComPtr，成功才替换 entry；
    // 失败时旧 entry/uploadedRevision 原样保留（gpuRevisionLag 可见）。
    // MeshAsset 位于 MiniEngine::Assets，本文件在 Rhi::D3D11 命名空间内，必须限定。
    const Assets::MeshAsset& asset = *cpuView->asset;
    ComPtr<ID3D11Buffer> newVertexBuffer;
    ComPtr<ID3D11Buffer> newIndexBuffer;
    if (asset.vertexCount > 0)
    {
        newVertexBuffer =
            CreateImmutableBuffer(device, asset.vertexData.data(), static_cast<std::uint32_t>(asset.vertexData.size()),
                                  D3D11_BIND_VERTEX_BUFFER);
        if (!newVertexBuffer)
        {
            return false;
        }
    }
    if (asset.indexCount > 0)
    {
        newIndexBuffer =
            CreateImmutableBuffer(device, asset.indexData.data(), static_cast<std::uint32_t>(asset.indexData.size()),
                                  D3D11_BIND_INDEX_BUFFER);
        if (!newIndexBuffer)
        {
            return false;
        }
    }

    entry.vertexBuffer = std::move(newVertexBuffer);
    entry.indexBuffer = std::move(newIndexBuffer);
    entry.indexCount = asset.indexCount;
    entry.uploadedRevision = cpuView->revision;
    return true;
}

std::optional<D3D11AssetCache::GpuMeshView> D3D11AssetCache::TryGetUploadedView(
    const Assets::AssetHandle<Assets::MeshAsset> handle) const
{
    const auto found = m_impl->meshes.find(handle);
    if (found == m_impl->meshes.end() || found->second.indexCount == 0 || !found->second.vertexBuffer ||
        !found->second.indexBuffer)
    {
        return std::nullopt;
    }
    return GpuMeshView{found->second.vertexBuffer.Get(), found->second.indexBuffer.Get(), found->second.indexCount};
}

std::uint32_t D3D11AssetCache::RevisionLagCount(const Assets::AssetManager& assets) const
{
    std::uint32_t lag = 0;
    for (const auto& entry : m_impl->meshes)
    {
        const auto cpuView = assets.Meshes().TryGet(entry.first);
        if (cpuView.has_value() && cpuView->revision > entry.second.uploadedRevision)
        {
            ++lag;
        }
    }
    return lag;
}

bool D3D11AssetCache::EnsureTextureUploaded(ID3D11Device& device, const Assets::AssetManager& assets,
                                            const Assets::AssetHandle<Assets::TextureAsset> handle)
{
    // P1-2：先验证 CPU payload 存在再触碰 map（stale 不插空 entry）。
    const auto cpuView = assets.Textures().TryGet(handle);
    if (!cpuView.has_value())
    {
        return false; // stale handle
    }
    auto found = m_impl->textures.find(handle);
    if (found == m_impl->textures.end())
    {
        found = m_impl->textures.emplace(handle, Impl::GpuTextureEntry{}).first;
    }
    Impl::GpuTextureEntry& entry = found->second;
    if (entry.uploadedRevision == cpuView->revision)
    {
        return true; // no-op
    }

    // 07 篇：先建全新 immutable 资源成功后才替换 entry；失败保留旧资源/uploadedRevision。
    const Assets::TextureAsset& asset = *cpuView->asset;
    ComPtr<ID3D11Texture2D> newTexture;
    ComPtr<ID3D11ShaderResourceView> newView;
    const bool created = asset.width == 0 || asset.height == 0 || asset.pixels.empty()
                             ? true // 占位：无资源可建，仍推进 revision（绘制时绑定 fallback）
                             : CreateImmutableTexture(device, asset, newTexture, newView);
    if (!created)
    {
        return false;
    }
    entry.texture = std::move(newTexture);
    entry.view = std::move(newView);
    entry.uploadedRevision = cpuView->revision;
    return true;
}

std::optional<D3D11AssetCache::GpuTextureView> D3D11AssetCache::TryGetTextureView(
    const Assets::AssetHandle<Assets::TextureAsset> handle) const
{
    const auto found = m_impl->textures.find(handle);
    if (found == m_impl->textures.end() || !found->second.view)
    {
        return std::nullopt;
    }
    return GpuTextureView{found->second.view.Get()};
}

std::uint32_t D3D11AssetCache::TextureRevisionLagCount(const Assets::AssetManager& assets) const
{
    std::uint32_t lag = 0;
    for (const auto& entry : m_impl->textures)
    {
        const auto cpuView = assets.Textures().TryGet(entry.first);
        if (cpuView.has_value() && cpuView->revision > entry.second.uploadedRevision)
        {
            ++lag;
        }
    }
    return lag;
}

void D3D11AssetCache::PruneStale(const Assets::AssetManager& assets)
{
    for (auto iterator = m_impl->meshes.begin(); iterator != m_impl->meshes.end();)
    {
        if (!assets.Meshes().TryGet(iterator->first).has_value())
        {
            iterator = m_impl->meshes.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    for (auto iterator = m_impl->textures.begin(); iterator != m_impl->textures.end();)
    {
        if (!assets.Textures().TryGet(iterator->first).has_value())
        {
            iterator = m_impl->textures.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

void D3D11AssetCache::ReleaseAll()
{
    m_impl->meshes.clear();
    m_impl->textures.clear();
}
} // namespace MiniEngine::Rhi::D3D11
