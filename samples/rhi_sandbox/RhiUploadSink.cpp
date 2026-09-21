// ============================================================================
// RhiUploadSink.cpp — UploadSink 的真实实现（M7-07）
// 关联：samples/rhi_sandbox/RhiUploadSink.h
//       engine/render/src/M6SceneResources.cpp（Mesh/Texture 上传与格式映射的既有惯例）
// ============================================================================

#include "RhiUploadSink.h"

#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Assets/TextureFormatV2.h>
#include <MiniEngine/Profiling/Profile.h>

#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace MiniEngine::Sandbox
{
namespace
{
using namespace MiniEngine::Assets;
using namespace MiniEngine::Rhi;

constexpr std::uint32_t kVertexStride = static_cast<std::uint32_t>(sizeof(PbrVertex));
constexpr std::uint32_t kIndexStride = static_cast<std::uint32_t>(sizeof(std::uint32_t));

// 与 M6SceneResources 的 TextureFormat 同源：仅支持 M4-02 的 RGBA8 角色与环境的 Rgba16Float。
[[nodiscard]] Format TextureFormat(const TextureAsset& asset)
{
    if (asset.pixelFormat == TexturePixelFormat::Rgba16Float)
    {
        return Format::Rgba16Float;
    }
    if (asset.pixelFormat == TexturePixelFormat::Rgba8Unorm)
    {
        return asset.colorSpace == TextureColorSpace::Srgb ? Format::Rgba8UnormSrgb : Format::Rgba8Unorm;
    }
    throw std::invalid_argument("unsupported texture pixel format for upload");
}

[[nodiscard]] TextureDesc TextureDescriptor(const TextureAsset& asset, const std::string& debugName)
{
    const auto header =
        TextureHeaderV2{asset.width, asset.height, asset.mipCount, asset.pixelFormat, asset.colorSpace, asset.usage};
    if (!IsValid(header) || asset.mips.size() != asset.mipCount || asset.mipCount == 0)
    {
        throw std::invalid_argument("invalid texture asset for upload");
    }
    TextureDesc desc;
    desc.dimension = TextureDimension::Texture2D;
    desc.extent = {asset.width, asset.height};
    desc.mipLevels = static_cast<std::uint16_t>(asset.mipCount);
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = TextureFormat(asset);
    // Assets::TextureUsage 与 Rhi::TextureUsage 同名：这里必须显式限定 RHI 的那一个。
    desc.usage = Rhi::TextureUsage::Sampled | Rhi::TextureUsage::CopyDestination;
    desc.debugName = debugName;
    return desc;
}

[[nodiscard]] std::string ResourceName(const UploadRequestDescription& description, const char* suffix)
{
    return "M7.Stream." + std::string(suffix) + "." + std::to_string(description.revision);
}
} // namespace

UploadResult RhiUploadSink::CreateAndUpload(const UploadRequestDescription& description)
{
    AssertOwnerThread("upload sink called from a non-render thread"); // M7-10（A13）
    ME_PROFILE_ZONE_NAMED("RhiUploadCreate");
    UploadResult result;
    if (description.payload == nullptr)
    {
        result.errorText = "upload description has no payload";
        ++m_failures;
        return result;
    }
    const CpuAssetPayload& payload = *description.payload;
    UploadedResource resource;
    resource.token = m_nextToken++;
    resource.assetId = description.assetId;
    resource.kind = description.kind;
    resource.revision = description.revision;
    resource.bytes = payload.ByteSize();

    try
    {
        switch (payload.kind)
        {
        case AssetPayloadKind::Mesh:
        {
            const MeshAsset& mesh = payload.mesh;
            if (mesh.vertexStride != kVertexStride || mesh.indexStride != kIndexStride || mesh.vertexCount == 0 ||
                mesh.indexCount == 0 ||
                mesh.vertexData.size() != static_cast<std::size_t>(mesh.vertexCount) * kVertexStride ||
                mesh.indexData.size() != static_cast<std::size_t>(mesh.indexCount) * kIndexStride)
            {
                throw std::invalid_argument("mesh payload is not a valid PbrVertex/uint32 asset");
            }
            const auto vertexBytes = std::span<const std::byte>(mesh.vertexData.data(), mesh.vertexData.size());
            const auto indexBytes = std::span<const std::byte>(mesh.indexData.data(), mesh.indexData.size());
            resource.vertex = m_device.CreateBuffer(
                {vertexBytes.size(), BufferUsage::Vertex, MemoryDomain::GpuOnly, ResourceName(description, "VB")},
                vertexBytes);
            resource.index = m_device.CreateBuffer(
                {indexBytes.size(), BufferUsage::Index, MemoryDomain::GpuOnly, ResourceName(description, "IB")},
                indexBytes);
            resource.hasGpuObject = true;
            break;
        }
        case AssetPayloadKind::Texture:
        {
            const TextureAsset& texture = payload.texture;
            const TextureDesc desc = TextureDescriptor(texture, ResourceName(description, "Tex"));
            resource.texture = m_device.CreateTexture(desc);
            std::vector<TextureSubresourceData> levels;
            levels.reserve(texture.mips.size());
            const auto bytes = std::span<const std::byte>(texture.pixels.data(), texture.pixels.size());
            for (const TextureMipInfo& mip : texture.mips)
            {
                if (mip.offset > bytes.size() || mip.byteSize > bytes.size() - static_cast<std::size_t>(mip.offset))
                {
                    throw std::invalid_argument("texture mip is outside payload");
                }
                levels.push_back(
                    {bytes.subspan(static_cast<std::size_t>(mip.offset), mip.byteSize), mip.rowPitch, mip.byteSize});
            }
            m_device.UploadTexture(resource.texture, levels);
            resource.hasGpuObject = true;
            break;
        }
        case AssetPayloadKind::Material:
        case AssetPayloadKind::RawBytes:
            // 无 GPU 对象：材质是逐 draw 常量，World 字节由 engine/world 实例化。
            // 仍然分配 token：提交事务、事件与记账对四种 kind 一视同仁。
            resource.hasGpuObject = false;
            break;
        case AssetPayloadKind::None:
        default:
            throw std::invalid_argument("payload has no decoded content");
        }
    }
    catch (const std::exception& exception)
    {
        // 失败必须清理已创建的部分对象（CreateBuffer 第二个失败时不能漏掉第一个）。
        if (resource.vertex)
        {
            m_device.Destroy(resource.vertex);
        }
        if (resource.index)
        {
            m_device.Destroy(resource.index);
        }
        if (resource.texture)
        {
            m_device.Destroy(resource.texture);
        }
        result.success = false;
        result.errorText = exception.what();
        ++m_failures;
        return result;
    }

    resource.alive = true;
    m_resources.push_back(resource);
    result.success = true;
    result.resource = resource.token;
    // 独立上传自带提交与 signal：用"最近一次提交的 serial"作为完成判据，
    // completedSerial 达标后才允许提交（无 GPU 对象的 kind 立即可用，fence=0）。
    result.completionFence = resource.hasGpuObject ? m_device.Diagnostics().lastSubmittedSerial : 0U;
    result.actualBytes = resource.bytes;
    return result;
}

bool RhiUploadSink::IsFenceComplete(const std::uint64_t fence) const noexcept
{
    if (fence == 0)
    {
        return true; // 同步后端（D3D11 immediate 路径）语义
    }
    return m_device.Diagnostics().completedSerial >= fence;
}

void RhiUploadSink::DeferDestroy(const Assets::UploadResourceToken resource) noexcept
{
    AssertOwnerThread("upload sink retire called from a non-render thread"); // M7-10（A13）
    for (UploadedResource& entry : m_resources)
    {
        if (entry.token != resource || !entry.alive)
        {
            continue;
        }
        entry.alive = false;
        ++m_destroyed;
        // IRhiDevice::Destroy = 立即撤销逻辑可用性 + 底层对象待最后一次 GPU 使用完成后退休；
        // 这就是"旧 revision 资源进延迟退休"的真实落点。
        if (entry.vertex)
        {
            m_device.Destroy(entry.vertex);
            entry.vertex = {};
        }
        if (entry.index)
        {
            m_device.Destroy(entry.index);
            entry.index = {};
        }
        if (entry.texture)
        {
            m_device.Destroy(entry.texture);
            entry.texture = {};
        }
        return;
    }
}

void RhiUploadSink::ReleaseAll() noexcept
{
    for (UploadedResource& entry : m_resources)
    {
        if (!entry.alive)
        {
            continue;
        }
        entry.alive = false;
        ++m_destroyed;
        if (entry.vertex)
        {
            m_device.Destroy(entry.vertex);
            entry.vertex = {};
        }
        if (entry.index)
        {
            m_device.Destroy(entry.index);
            entry.index = {};
        }
        if (entry.texture)
        {
            m_device.Destroy(entry.texture);
            entry.texture = {};
        }
    }
}

std::size_t RhiUploadSink::LiveResourceCount() const noexcept
{
    std::size_t live = 0;
    for (const UploadedResource& entry : m_resources)
    {
        if (entry.alive)
        {
            ++live;
        }
    }
    return live;
}
} // namespace MiniEngine::Sandbox
