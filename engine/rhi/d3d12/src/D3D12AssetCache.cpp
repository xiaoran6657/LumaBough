// ============================================================================
// D3D12AssetCache.cpp — 资产上传实现（staging → Copy*Region → 显式 transition）
// 里程碑：M5（09 篇迁移顺序第 2 步「baked mesh + texture + depth」）
// 职责：实现 D3D12AssetCache.h。两条上传路径共用同一套顺序契约：
//   创建 DEFAULT 资源（实际初始状态 COPY_DEST）→ Register(COPY_DEST) →
//   从上传分配里逐行/整块写字节 → CopyBufferRegion / CopyTextureRegion →
//   Transition(目标状态) + FlushBarriersTo（**在依赖它的 draw 之前**，07 篇要求）。
// 失败语义：任何一步失败都在**替换 entry 之前**返回 false —— 旧 entry（可能仍被在飞
//   帧引用）保持不动，调用方退回 fallback SRV 或跳过本 draw（02 篇失败语义）。
// 关联：docs/architecture/README.md（M4 resource profile 保持）
//       engine/rhi/d3d12/src/D3D12TextureUpload.h（mip 逐级打包与拷贝的唯一实现）
//       engine/assets/include/MiniEngine/Assets/{MeshAsset,TextureAsset}.h（载荷契约）
// ============================================================================
#include "D3D12AssetCache.h"
#include "D3D12Events.h"

#include "D3D12DescriptorHeap.h"
#include "D3D12ResourceStateTracker.h"
#include "D3D12TextureUpload.h"
#include "D3D12UploadManager.h"

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Assets/TextureAsset.h>

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// Handle → map key：index 与 generation 都要参与，否则"槽位复用后世代不同"的旧句柄
// 会命中新资产的 entry（正是 AssetHandle 的世代机制要防的事）。
std::uint64_t KeyOf(const std::uint32_t index, const std::uint32_t generation) noexcept
{
    return (static_cast<std::uint64_t>(generation) << 32U) | static_cast<std::uint64_t>(index);
}

// 顶点流 stride：`.memesh` v2 的顶点就是 PbrVertex（48B），与 D3D12 的 PbrOpaque
// 输入布局逐字段对应（position 0 / normal 12 / tangent 24 / uv0 40）。
constexpr std::uint64_t kPbrVertexStride = sizeof(Assets::PbrVertex);

std::wstring DebugName(const wchar_t* prefix, const std::uint32_t index)
{
    return std::wstring{prefix} + std::to_wstring(index);
}

// 创建一个 DEFAULT heap 资源，实际初始状态 = COPY_DEST（上传前的唯一合法状态）。
Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device& device, const std::uint64_t byteSize,
                                                           const D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
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
    description.Flags = flags;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
                  "ID3D12Device::CreateCommittedResource(asset buffer)");
    return resource;
}

// 把一块连续字节写进 upload 分配并发出 CopyBufferRegion（缓冲上传的共用尾段）。
void UploadBytes(const AssetUploadContext& context, ID3D12Resource& destination, const std::byte* data,
                 const std::uint64_t byteSize, const std::uint64_t destinationOffset,
                 const D3D12_RESOURCE_STATES finalState, const std::string_view tag)
{
    const UploadAllocation allocation = context.uploadManager.Allocate(byteSize, 512U, tag);
    if (!allocation)
    {
        throw std::runtime_error{"upload allocation failed for " + std::string{tag}};
    }
    std::memcpy(allocation.cpu, data, static_cast<std::size_t>(byteSize));

    // Copy 的源用 GPUVA 即可（缓冲对缓冲）；目标区间的绝对偏移 = 资源内偏移。
    context.commandList.CopyBufferRegion(&destination, destinationOffset, allocation.source, allocation.offset,
                                         byteSize);
    static_cast<void>(finalState); // 状态转换由调用方按资源粒度统一发出（见下方 Register/Transition）
}
} // namespace

struct D3D12AssetCache::Impl final
{
    // 已上传网格：资源 + 视图 + 上传时的 CPU revision（revision 驱动判据）。
    struct MeshEntry final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        ResourceKey vertexKey{};
        ResourceKey indexKey{};
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        D3D12_INDEX_BUFFER_VIEW indexView{};
        std::uint32_t indexCount = 0;
        std::uint64_t revision = 0;
        bool usable = false;     // false：占位 payload（0 顶点/0 索引），无可绘制视图
        bool registered = false; // 是否已在 tracker 里注册（ReleaseAll/Prune 依赖它）
    };

    struct TextureEntry final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        ResourceKey key{};
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        std::uint32_t descriptorBase = 0;
        DescriptorRange descriptorRange{};
        std::uint64_t revision = 0;
        bool usable = false;
        bool registered = false;
    };

    D3D12DescriptorHeap* srvStaging = nullptr;
    D3D12ResourceStateTracker* tracker = nullptr;
    std::unordered_map<std::uint64_t, MeshEntry> meshes;
    std::unordered_map<std::uint64_t, TextureEntry> textures;
    // 被 revision 替换/PruneStale 淘汰的旧资源。**只在安全点释放**（ReleaseAll，
    // 调用方须已证明 GPU 不再引用）：替换发生在帧内，旧 VB/IB/纹理可能仍被在飞
    // 命令列表引用，立即析构是 use-after-free；保持存活只多占内存，不产生正确性问题。
    // tracker 的注销在替换时立即做（注销只是记账，不等于释放；07 篇"释放前注销"的
    // 顺序由"先注销、后（安全点）释放"保持）。
    struct RetiredResource final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::uint64_t fenceValue = 0;
    };

    std::vector<RetiredResource> retired;
    std::uint32_t nextDebugIndex = 0;
    std::uint32_t placeholders = 0;
    std::uint32_t rejections = 0;
    std::uint64_t vertexBytes = 0;
    std::uint64_t indexBytes = 0;
    std::uint64_t textureBytes = 0;

    void ReclaimRetired(const std::uint64_t completedFenceValue)
    {
        for (auto it = retired.begin(); it != retired.end();)
        {
            if (it->fenceValue != 0U && it->fenceValue <= completedFenceValue)
            {
                it = retired.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void CommitRetired(const std::uint64_t submittedFenceValue)
    {
        for (auto& item : retired)
        {
            if (item.fenceValue == 0U)
            {
                item.fenceValue = submittedFenceValue;
            }
        }
    }
};

D3D12AssetCache::D3D12AssetCache() = default;
D3D12AssetCache::~D3D12AssetCache() = default;

void D3D12AssetCache::Initialize(D3D12DescriptorHeap& srvStagingHeap, D3D12ResourceStateTracker& tracker)
{
    if (m_impl != nullptr)
    {
        throw std::logic_error{"D3D12AssetCache::Initialize called twice"};
    }
    m_impl = std::make_unique<Impl>();
    m_impl->srvStaging = &srvStagingHeap;
    m_impl->tracker = &tracker;
}

bool D3D12AssetCache::EnsureMeshUploaded(const AssetUploadContext& context, const Assets::AssetManager& assets,
                                         const Assets::AssetHandle<Assets::MeshAsset> handle)
{
    if (m_impl == nullptr || !handle.IsValid())
    {
        return false;
    }
    const std::optional<Assets::AssetPool<Assets::MeshAsset>::View> view = assets.Meshes().TryGet(handle);
    if (!view.has_value())
    {
        return false; // stale handle / 未就绪：不创建也不改动 entry
    }

    const std::uint64_t key = KeyOf(handle.Index(), handle.Generation());
    const Impl::MeshEntry* const existing = [&]() -> const Impl::MeshEntry*
    {
        const auto found = m_impl->meshes.find(key);
        return found == m_impl->meshes.end() ? nullptr : &found->second;
    }();
    if (existing != nullptr && existing->revision == view->revision)
    {
        return true; // 已是最新：no-op（D3D11 同款语义）
    }

    const Assets::MeshAsset& mesh = *view->asset;
    const std::uint64_t vertexBytes = mesh.vertexData.size();
    const std::uint64_t indexBytes = mesh.indexData.size();

    // 占位 payload（0 顶点/0 索引）：只记录 revision，不创建资源（D3D11 同款）。
    // 记录 revision 的意义在于"下次热重载 revision 变化时会重试"。
    if (vertexBytes == 0U || indexBytes == 0U || vertexBytes % kPbrVertexStride != 0U)
    {
        Impl::MeshEntry placeholder;
        placeholder.revision = view->revision;
        placeholder.usable = false;
        placeholder.indexCount = 0;
        // 旧 entry 若仍可用则不移除其资源：这里直接替换会释放旧 VB/IB，而在飞帧可能
        // 仍引用它。M5 的资产热重载由渲染端在帧边界驱动（PruneStale），因此这里
        // 只做"标记不可用"，把释放留给安全点。
        if (existing != nullptr)
        {
            auto& entry = m_impl->meshes[key];
            entry.revision = view->revision;
            entry.usable = false;
            entry.indexCount = 0;
        }
        else
        {
            m_impl->meshes.emplace(key, std::move(placeholder));
        }
        ++m_impl->placeholders;
        return true; // "成功记录"但没有可绘制视图：调用方 TryGetMeshView 会得到 nullopt
    }

    try
    {
        const auto* assetId = assets.Meshes().TryGetAssetId(handle);
        const std::string identity = assetId ? assetId->ToHexString() : "unresolved";
        PIXScopedEvent(&context.commandList, PIX_COLOR_DEFAULT, "Upload:%s mesh revision=%llu", identity.c_str(),
                       static_cast<unsigned long long>(view->revision));
        Impl::MeshEntry entry;
        entry.vertexBuffer = CreateDefaultBuffer(context.device, vertexBytes);
        entry.indexBuffer = CreateDefaultBuffer(context.device, indexBytes);
        entry.revision = view->revision;
        entry.indexCount = static_cast<std::uint32_t>(indexBytes / sizeof(std::uint32_t));
        entry.usable = true;

        // 注册必须在写入之前：Register 记的是"此刻的真实状态"（COPY_DEST）。
        const std::uint32_t nameIndex = m_impl->nextDebugIndex++;
        entry.vertexKey = context.tracker.Register(*entry.vertexBuffer.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   DebugName(L"M5.Asset.Mesh.VB", nameIndex).c_str());
        entry.indexKey = context.tracker.Register(*entry.indexBuffer.Get(), 1U, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  DebugName(L"M5.Asset.Mesh.IB", nameIndex).c_str());
        entry.registered = true;

        UploadBytes(context, *entry.vertexBuffer.Get(), mesh.vertexData.data(), vertexBytes, 0U,
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "mesh-vertices");
        UploadBytes(context, *entry.indexBuffer.Get(), mesh.indexData.data(), indexBytes, 0U,
                    D3D12_RESOURCE_STATE_INDEX_BUFFER, "mesh-indices");

        context.tracker.Transition(entry.vertexKey, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        context.tracker.Transition(entry.indexKey, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        // 07 篇：barrier 必须在**依赖新状态的第一个 draw 之前**发出去。
        static_cast<void>(context.tracker.FlushBarriersTo(context.commandList));

        entry.vertexView.BufferLocation = entry.vertexBuffer->GetGPUVirtualAddress();
        entry.vertexView.SizeInBytes = static_cast<UINT>(vertexBytes);
        entry.vertexView.StrideInBytes = static_cast<UINT>(kPbrVertexStride);
        entry.indexView.BufferLocation = entry.indexBuffer->GetGPUVirtualAddress();
        entry.indexView.SizeInBytes = static_cast<UINT>(indexBytes);
        entry.indexView.Format = DXGI_FORMAT_R32_UINT;

        m_impl->vertexBytes += vertexBytes;
        m_impl->indexBytes += indexBytes;

        // 成功之后才替换：失败路径上旧 entry 原封不动。
        if (existing != nullptr)
        {
            // 旧资源：先注销 tracker（记账立即清），本体挂进 retired 在安全点释放。
            // 直接覆盖 map 条目会立即析构旧 ComPtr——而旧 VB/IB 可能仍被在飞命令列表
            // 引用（07 篇顺序契约）；M5-08 的 PSO 覆盖路径修过同一型问题。
            if (existing->registered)
            {
                m_impl->tracker->Unregister(existing->vertexKey);
                m_impl->tracker->Unregister(existing->indexKey);
            }
            if (existing->vertexBuffer != nullptr)
            {
                m_impl->retired.push_back(Impl::RetiredResource{std::move(existing->vertexBuffer), 0U});
            }
            if (existing->indexBuffer != nullptr)
            {
                m_impl->retired.push_back(Impl::RetiredResource{std::move(existing->indexBuffer), 0U});
            }
            m_impl->meshes[key] = std::move(entry);
        }
        else
        {
            m_impl->meshes.emplace(key, std::move(entry));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        ++m_impl->rejections;
        MiniEngine::WriteLog(MiniEngine::LogLevel::Warning, std::string{"mesh upload failed: "} + exception.what());
        return false;
    }
}

std::optional<D3D12AssetCache::GpuMeshView> D3D12AssetCache::TryGetMeshView(
    const Assets::AssetHandle<Assets::MeshAsset> handle) const
{
    if (m_impl == nullptr || !handle.IsValid())
    {
        return std::nullopt;
    }
    const auto found = m_impl->meshes.find(KeyOf(handle.Index(), handle.Generation()));
    if (found == m_impl->meshes.end() || !found->second.usable)
    {
        return std::nullopt;
    }
    GpuMeshView view;
    view.vertexBuffer = found->second.vertexView;
    view.indexBuffer = found->second.indexView;
    view.indexCount = found->second.indexCount;
    return view;
}

bool D3D12AssetCache::EnsureTextureUploaded(const AssetUploadContext& context, const Assets::AssetManager& assets,
                                            const Assets::AssetHandle<Assets::TextureAsset> handle)
{
    if (m_impl == nullptr || !handle.IsValid())
    {
        return false;
    }
    const std::optional<Assets::AssetPool<Assets::TextureAsset>::View> view = assets.Textures().TryGet(handle);
    if (!view.has_value())
    {
        return false;
    }

    const std::uint64_t key = KeyOf(handle.Index(), handle.Generation());
    const auto existingIt = m_impl->textures.find(key);
    if (existingIt != m_impl->textures.end() && existingIt->second.revision == view->revision)
    {
        return true;
    }

    const Assets::TextureAsset& texture = *view->asset;
    const bool placeholder = texture.width == 0U || texture.height == 0U || texture.pixels.empty() ||
                             texture.mips.size() != texture.mipCount;
    if (placeholder)
    {
        if (existingIt == m_impl->textures.end())
        {
            Impl::TextureEntry entry;
            entry.revision = view->revision;
            m_impl->textures.emplace(key, std::move(entry));
        }
        else
        {
            existingIt->second.revision = view->revision;
            existingIt->second.usable = false;
        }
        ++m_impl->placeholders;
        return true;
    }

    const DescriptorRange descriptor = m_impl->srvStaging->Allocate(1U);
    try
    {
        // 格式口径（与 M4 的**采样语义**一致，资源格式不同）：
        //   Rgba8 + Srgb   → R8G8B8A8_UNORM_SRGB（硬件解码）
        //   Rgba8 + Linear → R8G8B8A8_UNORM
        //   Rgba16Float    → R16G16B16A16_FLOAT
        // 为什么不用 M4 那种 `R8G8B8A8_TYPELESS` + 类型化视图：D3D12 的
        // `GetCopyableFootprints`（上传计划的唯一来源）**拒绝 TYPELESS 资源**——
        // 它算不出元素大小。资源格式与视图格式因此在这里取同一个具体格式；
        // 像素字节与采样解码结果与 M4 完全相同。
        const auto* assetId = assets.Textures().TryGetAssetId(handle);
        const std::string identity = assetId ? assetId->ToHexString() : "unresolved";
        PIXScopedEvent(&context.commandList, PIX_COLOR_DEFAULT, "Upload:%s texture revision=%llu", identity.c_str(),
                       static_cast<unsigned long long>(view->revision));
        const bool hdr = texture.pixelFormat == Assets::TexturePixelFormat::Rgba16Float;
        const DXGI_FORMAT srvFormat =
            hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                : (texture.colorSpace == Assets::TextureColorSpace::Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                         : DXGI_FORMAT_R8G8B8A8_UNORM);
        const DXGI_FORMAT resourceFormat = srvFormat;

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProperties.CreationNodeMask = 1U;
        heapProperties.VisibleNodeMask = 1U;

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = texture.width;
        description.Height = texture.height;
        description.DepthOrArraySize = 1U;
        description.MipLevels = static_cast<UINT16>(texture.mipCount);
        description.Format = resourceFormat;
        description.SampleDesc.Count = 1U;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        Microsoft::WRL::ComPtr<ID3D12Resource> newTexture;
        ThrowIfFailed(context.device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&newTexture)),
                      "ID3D12Device::CreateCommittedResource(asset texture)");

        // 先按 baseOffset=0 求出总字节，再按**真实分配偏移**重新计划：
        // placed footprint 的 Offset 是"缓冲内绝对偏移"，必须把分配的起点算进去。
        const TextureUploadPlan sizing = PlanTextureUpload(context.device, description, 0U);
        const UploadAllocation allocation = context.uploadManager.Allocate(sizing.totalBytes, 512U, "asset-texture");
        if (!allocation)
        {
            throw std::runtime_error{"texture upload allocation failed"};
        }
        const TextureUploadPlan plan =
            allocation.offset == 0U ? sizing : PlanTextureUpload(context.device, description, allocation.offset);

        // PackTextureRows 的 uploadBase 是**缓冲起点**、uploadByteSize 是缓冲大小：
        // 分配只给出起点与长度，因此用 (cpu - offset) 还原起点，并把
        // (offset + size) 作为缓冲大小的保守下界（越界检查依然有效）。
        std::byte* const bufferBase = allocation.cpu - allocation.offset;
        const std::uint64_t bufferSizeLowerBound = allocation.offset + allocation.size;
        for (std::uint32_t level = 0U; level < texture.mipCount; ++level)
        {
            const Assets::TextureMipInfo& mip = texture.mips[level];
            if (static_cast<std::uint64_t>(mip.offset) + mip.byteSize > texture.pixels.size())
            {
                throw std::runtime_error{"texture mip range exceeds payload"};
            }
            const TextureUploadSource source{texture.pixels.data() + mip.offset, mip.rowPitch, mip.byteSize};
            PackTextureRows(plan, level, bufferBase, bufferSizeLowerBound, source);
        }

        const std::uint32_t nameIndex = m_impl->nextDebugIndex++;
        const ResourceKey resourceKey =
            context.tracker.Register(*newTexture.Get(), texture.mipCount, D3D12_RESOURCE_STATE_COPY_DEST,
                                     DebugName(L"M5.Asset.Texture", nameIndex).c_str());
        RecordTextureCopies(context.commandList, *newTexture.Get(), plan, *allocation.source);
        context.tracker.Transition(resourceKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        static_cast<void>(context.tracker.FlushBarriersTo(context.commandList));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
        srvDescription.Format = srvFormat;
        srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDescription.Texture2D.MostDetailedMip = 0U;
        srvDescription.Texture2D.MipLevels = texture.mipCount;
        srvDescription.Texture2D.PlaneSlice = 0U;
        srvDescription.Texture2D.ResourceMinLODClamp = 0.0F;
        context.device.CreateShaderResourceView(newTexture.Get(), &srvDescription, m_impl->srvStaging->Cpu(descriptor));

        Impl::TextureEntry entry;
        entry.texture = std::move(newTexture);
        entry.key = resourceKey;
        entry.srv = m_impl->srvStaging->Cpu(descriptor);
        entry.descriptorBase = descriptor.base;
        entry.descriptorRange = descriptor;
        entry.revision = view->revision;
        entry.usable = true;
        entry.registered = true;

        m_impl->textureBytes += texture.pixels.size();
        if (existingIt != m_impl->textures.end())
        {
            // 旧纹理：与网格同款——注销 tracker、本体挂 retired（安全点释放）；
            // descriptor 槽位不回收（句柄可能仍被在飞帧的材质表引用，05 篇）。
            if (existingIt->second.registered)
            {
                m_impl->tracker->Unregister(existingIt->second.key);
            }
            if (existingIt->second.texture != nullptr)
            {
                m_impl->retired.push_back(Impl::RetiredResource{std::move(existingIt->second.texture), 0U});
            }
            if (existingIt->second.descriptorRange)
            {
                m_impl->srvStaging->Free(existingIt->second.descriptorRange);
            }
            existingIt->second = std::move(entry);
        }
        else
        {
            m_impl->textures.emplace(key, std::move(entry));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        m_impl->srvStaging->Free(descriptor);
        ++m_impl->rejections;
        MiniEngine::WriteLog(MiniEngine::LogLevel::Warning, std::string{"texture upload failed: "} + exception.what());
        return false;
    }
}

bool D3D12AssetCache::TryGetTextureSrv(const Assets::AssetHandle<Assets::TextureAsset> handle,
                                       D3D12_CPU_DESCRIPTOR_HANDLE& outSrv) const
{
    if (m_impl == nullptr || !handle.IsValid())
    {
        return false;
    }
    const auto found = m_impl->textures.find(KeyOf(handle.Index(), handle.Generation()));
    if (found == m_impl->textures.end() || !found->second.usable)
    {
        return false;
    }
    outSrv = found->second.srv;
    return true;
}

std::uint32_t D3D12AssetCache::RevisionLagCount(const Assets::AssetManager& assets) const
{
    if (m_impl == nullptr)
    {
        return 0U;
    }
    std::uint32_t lag = 0U;
    for (const auto& [key, entry] : m_impl->meshes)
    {
        const Assets::AssetHandle<Assets::MeshAsset> handle{static_cast<std::uint32_t>(key & 0xFFFFFFFFU),
                                                            static_cast<std::uint32_t>(key >> 32U)};
        const auto view = assets.Meshes().TryGet(handle);
        if (view.has_value() && view->revision > entry.revision)
        {
            ++lag;
        }
    }
    return lag;
}

std::uint32_t D3D12AssetCache::TextureRevisionLagCount(const Assets::AssetManager& assets) const
{
    if (m_impl == nullptr)
    {
        return 0U;
    }
    std::uint32_t lag = 0U;
    for (const auto& [key, entry] : m_impl->textures)
    {
        const Assets::AssetHandle<Assets::TextureAsset> handle{static_cast<std::uint32_t>(key & 0xFFFFFFFFU),
                                                               static_cast<std::uint32_t>(key >> 32U)};
        const auto view = assets.Textures().TryGet(handle);
        if (view.has_value() && view->revision > entry.revision)
        {
            ++lag;
        }
    }
    return lag;
}

void D3D12AssetCache::PruneStale(const Assets::AssetManager& assets)
{
    if (m_impl == nullptr)
    {
        return;
    }
    // 只淘汰"已不可解析"的条目（Removed/Unload）：先 Unregister 再释放（07 篇顺序契约）。
    for (auto it = m_impl->meshes.begin(); it != m_impl->meshes.end();)
    {
        const Assets::AssetHandle<Assets::MeshAsset> handle{static_cast<std::uint32_t>(it->first & 0xFFFFFFFFU),
                                                            static_cast<std::uint32_t>(it->first >> 32U)};
        if (assets.Meshes().TryGet(handle).has_value())
        {
            ++it;
            continue;
        }
        if (it->second.registered)
        {
            m_impl->tracker->Unregister(it->second.vertexKey);
            m_impl->tracker->Unregister(it->second.indexKey);
        }
        // 本体不在这里析构：与替换路径同一理由（可能仍被在飞帧引用），挂 retired。
        if (it->second.vertexBuffer != nullptr)
        {
            m_impl->retired.push_back(Impl::RetiredResource{std::move(it->second.vertexBuffer), 0U});
        }
        if (it->second.indexBuffer != nullptr)
        {
            m_impl->retired.push_back(Impl::RetiredResource{std::move(it->second.indexBuffer), 0U});
        }
        it = m_impl->meshes.erase(it);
    }
    for (auto it = m_impl->textures.begin(); it != m_impl->textures.end();)
    {
        const Assets::AssetHandle<Assets::TextureAsset> handle{static_cast<std::uint32_t>(it->first & 0xFFFFFFFFU),
                                                               static_cast<std::uint32_t>(it->first >> 32U)};
        if (assets.Textures().TryGet(handle).has_value())
        {
            ++it;
            continue;
        }
        if (it->second.registered)
        {
            m_impl->tracker->Unregister(it->second.key);
        }
        if (it->second.texture != nullptr)
        {
            m_impl->retired.push_back(Impl::RetiredResource{std::move(it->second.texture), 0U});
        }
        if (it->second.descriptorRange)
        {
            m_impl->srvStaging->Free(it->second.descriptorRange);
        }
        it = m_impl->textures.erase(it);
    }
}

void D3D12AssetCache::BeginFrame(const std::uint64_t completedFenceValue)
{
    if (m_impl != nullptr)
    {
        m_impl->ReclaimRetired(completedFenceValue);
        m_impl->srvStaging->Reclaim(completedFenceValue);
    }
}

void D3D12AssetCache::CommitFrame(const std::uint64_t submittedFenceValue)
{
    if (m_impl == nullptr)
    {
        return;
    }
    if (submittedFenceValue == 0U)
    {
        throw std::invalid_argument{"D3D12AssetCache::CommitFrame requires a submitted fence"};
    }
    m_impl->CommitRetired(submittedFenceValue);
}

void D3D12AssetCache::ReleaseAll()
{
    if (m_impl == nullptr)
    {
        return;
    }
    for (auto& [key, entry] : m_impl->meshes)
    {
        if (entry.registered)
        {
            m_impl->tracker->Unregister(entry.vertexKey);
            m_impl->tracker->Unregister(entry.indexKey);
        }
    }
    for (auto& [key, entry] : m_impl->textures)
    {
        if (entry.registered)
        {
            m_impl->tracker->Unregister(entry.key);
        }
        if (entry.descriptorRange)
        {
            m_impl->srvStaging->Free(entry.descriptorRange);
        }
    }
    m_impl->meshes.clear();
    m_impl->textures.clear();
    // 安全点：前置契约是"调用方已证明 GPU 不再引用"（FlushGpu 之后），这里才真正释放
    // retired 队列里的旧资源（替换/Prune 路径只挂账不析构，见 Impl::retired 注释）。
    m_impl->retired.clear();
}

std::uint32_t D3D12AssetCache::UploadedMeshCount() const noexcept
{
    if (m_impl == nullptr)
    {
        return 0U;
    }
    std::uint32_t count = 0U;
    for (const auto& [key, entry] : m_impl->meshes)
    {
        count += entry.usable ? 1U : 0U;
    }
    return count;
}

std::uint32_t D3D12AssetCache::UploadedTextureCount() const noexcept
{
    if (m_impl == nullptr)
    {
        return 0U;
    }
    std::uint32_t count = 0U;
    for (const auto& [key, entry] : m_impl->textures)
    {
        count += entry.usable ? 1U : 0U;
    }
    return count;
}

std::uint64_t D3D12AssetCache::UploadedVertexBytes() const noexcept
{
    return m_impl == nullptr ? 0U : m_impl->vertexBytes;
}

std::uint64_t D3D12AssetCache::UploadedIndexBytes() const noexcept
{
    return m_impl == nullptr ? 0U : m_impl->indexBytes;
}

std::uint64_t D3D12AssetCache::UploadedTextureBytes() const noexcept
{
    return m_impl == nullptr ? 0U : m_impl->textureBytes;
}

std::uint32_t D3D12AssetCache::PlaceholderCount() const noexcept
{
    return m_impl == nullptr ? 0U : m_impl->placeholders;
}

std::uint32_t D3D12AssetCache::RejectedUploadCount() const noexcept
{
    return m_impl == nullptr ? 0U : m_impl->rejections;
}

std::uint32_t D3D12AssetCache::RetiredResourceCount() const noexcept
{
    return m_impl == nullptr ? 0U : static_cast<std::uint32_t>(m_impl->retired.size());
}
} // namespace MiniEngine::Rhi::D3D12
