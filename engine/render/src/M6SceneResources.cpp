#include <MiniEngine/Render/M6SceneResources.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/World/RenderQueueBuilder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniEngine::Render
{
namespace
{
using namespace Rhi;
using namespace RenderGraph;

constexpr std::uint32_t kShadowSize = 2048;
constexpr std::uint32_t kVertexStride = 48;
constexpr std::uint32_t kIndexStride = 4;
constexpr std::uint32_t kDebugSceneLuminance = 10;

struct alignas(16) FrameConstants final
{
    std::array<float, 16> viewProjection{};
    std::array<float, 4> cameraPositionAndDebugMode{};
    std::array<float, 4> directionAndIntensity{};
    std::array<float, 4> lightColorAndPadding{};
    std::array<float, 4> shadowMapSizeAndPadding{};
};
static_assert(sizeof(FrameConstants) == 128);

struct alignas(16) ObjectConstants final
{
    std::array<float, 16> world{};
    std::array<float, 16> normalMatrix{};
    std::array<float, 16> lightWorldViewProjection{};
    std::array<float, 4> handednessAndReceivesShadow{};
};
static_assert(sizeof(ObjectConstants) == 208);

struct alignas(16) MaterialConstants final
{
    std::array<float, 4> baseColorFactor{};
    std::array<float, 4> emissiveAndMetallic{};
    std::array<float, 4> roughnessNormalOcclusionFlags{};
};
static_assert(sizeof(MaterialConstants) == 48);

struct alignas(16) IblConstants final
{
    std::array<float, 4> prefilterMipCountAndFlags{};
};
static_assert(sizeof(IblConstants) == 16);

struct alignas(16) SkyboxConstants final
{
    std::array<float, 16> viewProjectionWithoutTranslation{};
};
static_assert(sizeof(SkyboxConstants) == 64);

struct alignas(16) PostProcessConstants final
{
    float exposureEv = 0.0F;
    std::uint32_t debugHdr = 0;
    float inverseWidth = 0.0F;
    float inverseHeight = 0.0F;
};
static_assert(sizeof(PostProcessConstants) == 16);

template <typename T> std::span<const std::byte> Bytes(const T& value)
{
    return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}

std::array<float, 16> Transpose(const World::Matrix4& source)
{
    std::array<float, 16> result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result[row * 4 + column] = source.values[column * 4 + row];
    return result;
}

std::array<float, 16> Multiply(const std::array<float, 16>& left, const std::array<float, 16>& right)
{
    std::array<float, 16> result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t k = 0; k < 4; ++k)
                result[row * 4 + column] += left[row * 4 + k] * right[k * 4 + column];
    return result;
}

std::array<float, 16> SkyViewProjection(const World::Matrix4& view, const World::Matrix4& projection)
{
    World::Matrix4 viewWithoutTranslation = view;
    viewWithoutTranslation.values[12] = 0.0F;
    viewWithoutTranslation.values[13] = 0.0F;
    viewWithoutTranslation.values[14] = 0.0F;
    const auto rowMajor = Multiply(viewWithoutTranslation.values, projection.values);
    World::Matrix4 result;
    result.values = rowMajor;
    return Transpose(result);
}

std::uint16_t VertexOffset(VertexSemantic semantic)
{
    switch (semantic)
    {
    case VertexSemantic::Position:
        return static_cast<std::uint16_t>(offsetof(Assets::PbrVertex, position));
    case VertexSemantic::Normal:
        return static_cast<std::uint16_t>(offsetof(Assets::PbrVertex, normal));
    case VertexSemantic::Tangent:
        return static_cast<std::uint16_t>(offsetof(Assets::PbrVertex, tangent));
    case VertexSemantic::TexCoord:
        return static_cast<std::uint16_t>(offsetof(Assets::PbrVertex, uv0));
    case VertexSemantic::Color:
        return static_cast<std::uint16_t>(kVertexStride);
    }
    throw std::invalid_argument("unknown M6 vertex semantic");
}

std::size_t FallbackIndex(Assets::TextureUsage usage)
{
    switch (usage)
    {
    case Assets::TextureUsage::BaseColor:
        return 0;
    case Assets::TextureUsage::Normal:
        return 1;
    case Assets::TextureUsage::MetallicRoughness:
        return 2;
    case Assets::TextureUsage::Occlusion:
        return 3;
    case Assets::TextureUsage::Emissive:
        return 4;
    default:
        throw std::invalid_argument("HDR environment cannot use material fallback");
    }
}

Format TextureFormat(const Assets::TextureAsset& asset)
{
    if (asset.pixelFormat == Assets::TexturePixelFormat::Rgba16Float)
        return Format::Rgba16Float;
    if (asset.pixelFormat == Assets::TexturePixelFormat::Rgba8Unorm)
        return asset.colorSpace == Assets::TextureColorSpace::Srgb ? Format::Rgba8UnormSrgb : Format::Rgba8Unorm;
    throw std::invalid_argument("unsupported M6 texture pixel format");
}

TextureDesc MaterialTextureDesc(const Assets::TextureAsset& asset, std::string name)
{
    const auto header = Assets::TextureHeaderV2{asset.width,       asset.height,     asset.mipCount,
                                                asset.pixelFormat, asset.colorSpace, asset.usage};
    if (!Assets::IsValid(header) || asset.mips.size() != asset.mipCount)
        throw std::invalid_argument("invalid M6 texture asset");
    TextureDesc desc;
    desc.dimension = TextureDimension::Texture2D;
    desc.extent = {asset.width, asset.height};
    desc.mipLevels = static_cast<std::uint16_t>(asset.mipCount);
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = TextureFormat(asset);
    desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
    desc.debugName = std::move(name);
    return desc;
}

void ValidateEnvironment(const PreparedEnvironment& environment)
{
    for (std::size_t i = 0; i < environment.textures.size(); ++i)
        if (!environment.textures[i] || environment.descriptors[i].format == Format::Unknown)
            throw std::invalid_argument("M6 PreparedEnvironment is incomplete");
    if (environment.descriptors[0].dimension != TextureDimension::TextureCube ||
        environment.descriptors[1].dimension != TextureDimension::TextureCube ||
        environment.descriptors[2].dimension != TextureDimension::TextureCube ||
        environment.descriptors[3].dimension != TextureDimension::Texture2D)
        throw std::invalid_argument("M6 PreparedEnvironment dimensions are invalid");
}

ResourceBinding UniformBinding(std::uint16_t binding, const BufferView& view)
{
    ResourceBinding result;
    result.binding = binding;
    result.type = BindingType::UniformBuffer;
    result.buffer = view;
    return result;
}
ResourceBinding TextureBinding(std::uint16_t binding, TextureHandle texture)
{
    ResourceBinding result;
    result.binding = binding;
    result.type = BindingType::SampledTexture;
    result.texture = texture;
    return result;
}
ResourceBinding SamplerBinding(std::uint16_t binding, SamplerHandle sampler)
{
    ResourceBinding result;
    result.binding = binding;
    result.type = BindingType::Sampler;
    result.sampler = sampler;
    return result;
}

void AddRequirement(std::vector<ResourceSetLayoutDesc>& layouts, const ShaderDesc& shader)
{
    for (const auto& need : shader.manifest.bindings)
    {
        while (layouts.size() <= need.set)
        {
            ResourceSetLayoutDesc layout;
            layout.set = static_cast<std::uint8_t>(layouts.size());
            layouts.push_back(std::move(layout));
        }
        auto& entries = layouts[need.set].entries;
        const auto found = std::find_if(entries.begin(), entries.end(),
                                        [&](const auto& entry) { return entry.binding == need.binding; });
        if (found != entries.end())
        {
            if (found->type != need.type || found->count != need.count || found->uniformBytes != need.uniformBytes ||
                found->textureDimension != need.textureDimension || found->comparisonSampler != need.comparisonSampler)
                throw std::invalid_argument("M6 shader reflection binding mismatch");
            found->visibility = found->visibility | shader.stage;
            continue;
        }
        const bool dynamic = need.type == BindingType::UniformBuffer &&
                             ((need.set == 0 && need.binding == 0) || (need.set == 2 && need.binding == 0));
        entries.push_back({need.binding, need.type, need.count, shader.stage, dynamic, need.uniformBytes,
                           need.textureDimension, need.comparisonSampler});
    }
}

} // namespace
M6SceneResources::M6SceneResources(IRhiDevice& device, Assets::AssetManager& assets,
                                   std::span<const ShaderDesc> shaders)
    : M6SceneResources(device, assets, shaders, PreparedEnvironment{})
{
}

M6SceneResources::M6SceneResources(IRhiDevice& device, Assets::AssetManager& assets,
                                   std::span<const ShaderDesc> shaders, const PreparedEnvironment& environment)
    : m_device(device), m_assets(assets), m_environment(environment)
{
    try
    {
        SamplerDesc material;
        material.minMagFilter = Filter::Linear;
        material.mipFilter = Filter::Linear;
        material.addressU = AddressMode::Repeat;
        material.addressV = AddressMode::Repeat;
        material.addressW = AddressMode::Repeat;
        material.debugName = "M6.MaterialSampler";
        m_samplers[0] = m_device.CreateSampler(material);

        SamplerDesc ibl = material;
        ibl.addressU = AddressMode::Clamp;
        ibl.addressV = AddressMode::Clamp;
        ibl.addressW = AddressMode::Clamp;
        ibl.debugName = "M6.IblSampler";
        m_samplers[1] = m_device.CreateSampler(ibl);

        SamplerDesc shadow = ibl;
        shadow.comparisonEnabled = true;
        shadow.comparison = CompareOp::LessEqual;
        shadow.mipFilter = Filter::Nearest;
        shadow.addressU = AddressMode::Border;
        shadow.addressV = AddressMode::Border;
        shadow.addressW = AddressMode::Border;
        shadow.borderColor = {1.0F, 1.0F, 1.0F, 1.0F};
        shadow.maxLod = 0.0F;
        shadow.debugName = "M6.ShadowCompareSampler";
        m_samplers[2] = m_device.CreateSampler(shadow);

        const std::array<std::array<std::byte, 4>, 5> pixels{
            std::array<std::byte, 4>{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}},
            std::array<std::byte, 4>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}},
            std::array<std::byte, 4>{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}},
            std::array<std::byte, 4>{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}},
            std::array<std::byte, 4>{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}}};
        const std::array<Format, 5> formats{Format::Rgba8UnormSrgb, Format::Rgba8Unorm, Format::Rgba8Unorm,
                                            Format::Rgba8Unorm, Format::Rgba8UnormSrgb};
        for (std::size_t i = 0; i < m_fallbackTextures.size(); ++i)
        {
            TextureDesc desc;
            desc.dimension = TextureDimension::Texture2D;
            desc.extent = {1, 1};
            desc.mipLevels = 1;
            desc.format = formats[i];
            desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
            desc.debugName = "M6.MaterialFallback." + std::to_string(i);
            m_fallbackTextures[i] = m_device.CreateTexture(desc);
            TextureSubresourceData level{pixels[i], 4, 4};
            m_device.UploadTexture(m_fallbackTextures[i], std::span(&level, 1));
        }

        constexpr std::array<float, 72> cubeVertices{
            -1, -1, -1, -1, 1,  -1, 1,  1, -1, 1,  -1, -1, -1, -1, 1,  1, -1, 1,  1, 1,  1, -1, 1,  1,
            -1, 1,  -1, -1, 1,  1,  1,  1, 1,  1,  1,  -1, -1, -1, -1, 1, -1, -1, 1, -1, 1, -1, -1, 1,
            -1, -1, -1, -1, -1, 1,  -1, 1, 1,  -1, 1,  -1, 1,  -1, -1, 1, 1,  -1, 1, 1,  1, 1,  -1, 1};
        constexpr std::array<std::uint16_t, 36> cubeIndices{0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,
                                                            8,  9,  10, 8,  10, 11, 12, 13, 14, 12, 14, 15,
                                                            16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};
        const auto vertexBytes = std::as_bytes(std::span(cubeVertices));
        const auto indexBytes = std::as_bytes(std::span(cubeIndices));
        m_skyVertexBuffer = m_device.CreateBuffer(
            {vertexBytes.size(), BufferUsage::Vertex, MemoryDomain::GpuOnly, "M6.SkyboxVertices"}, vertexBytes);
        m_skyIndexBuffer = m_device.CreateBuffer(
            {indexBytes.size(), BufferUsage::Index, MemoryDomain::GpuOnly, "M6.SkyboxIndices"}, indexBytes);
        m_skyVertexView = {m_skyVertexBuffer, 0, vertexBytes.size()};
        m_skyIndexView = {m_skyIndexBuffer, 0, indexBytes.size()};

        TextureDesc toneSource;
        toneSource.extent = {1, 1};
        toneSource.format = Format::Rgba16Float;
        toneSource.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
        toneSource.debugName = "M6.ToneSourceFixture";
        m_toneSource = m_device.CreateTexture(toneSource);
        const std::array<std::uint16_t, 4> hdr{0x4400, 0x4000, 0x3C00, 0x3C00};
        const TextureSubresourceData hdrUpload{std::as_bytes(std::span(hdr)), 8, 8};
        m_device.UploadTexture(m_toneSource, std::span(&hdrUpload, 1));

        ReloadShaders(shaders);
    }
    catch (...)
    {
        ReleaseAll();
        throw;
    }
}

M6SceneResources::~M6SceneResources()
{
    ReleaseAll();
}

void M6SceneResources::DestroyRevision(IRhiDevice& device, ShaderRevision& revision) noexcept
{
    try
    {
        for (const auto handle : revision.pipelines)
            if (handle)
                device.Destroy(handle);
    }
    catch (...)
    {
    }
    try
    {
        for (const auto handle : revision.pipelineLayouts)
            if (handle)
                device.Destroy(handle);
    }
    catch (...)
    {
    }
    try
    {
        for (const auto handle : revision.setLayouts)
            if (handle)
                device.Destroy(handle);
    }
    catch (...)
    {
    }
    try
    {
        for (const auto handle : revision.shaders)
            if (handle)
                device.Destroy(handle);
    }
    catch (...)
    {
    }
    revision = {};
}

void M6SceneResources::ReleaseAll() noexcept
{
    DestroyRevision(m_device, m_revision);
    for (auto& [id, mesh] : m_meshes)
    {
        if (mesh.vertex)
        {
            try
            {
                m_device.Destroy(mesh.vertex);
            }
            catch (...)
            {
            }
        }
        if (mesh.index)
        {
            try
            {
                m_device.Destroy(mesh.index);
            }
            catch (...)
            {
            }
        }
    }
    m_meshes.clear();
    for (auto& [id, texture] : m_textures)
        if (texture.texture)
        {
            try
            {
                m_device.Destroy(texture.texture);
            }
            catch (...)
            {
            }
        }
    m_textures.clear();
    if (m_skyVertexBuffer)
    {
        try
        {
            m_device.Destroy(m_skyVertexBuffer);
        }
        catch (...)
        {
        }
    }
    if (m_skyIndexBuffer)
    {
        try
        {
            m_device.Destroy(m_skyIndexBuffer);
        }
        catch (...)
        {
        }
    }
    m_skyVertexBuffer = {};
    m_skyIndexBuffer = {};
    if (m_toneSource)
    {
        try
        {
            m_device.Destroy(m_toneSource);
        }
        catch (...)
        {
        }
    }
    m_toneSource = {};
    for (auto& texture : m_fallbackTextures)
    {
        if (texture)
        {
            try
            {
                m_device.Destroy(texture);
            }
            catch (...)
            {
            }
        }
        texture = {};
    }
    for (auto& sampler : m_samplers)
    {
        if (sampler)
        {
            try
            {
                m_device.Destroy(sampler);
            }
            catch (...)
            {
            }
        }
        sampler = {};
    }
}

void M6SceneResources::EnsureMesh(const Assets::AssetId& id, Assets::AssetHandle<Assets::MeshAsset> handle)
{
    if (!id.IsValid())
        throw std::invalid_argument("M6 mesh id is invalid");
    const auto assetId = m_assets.Meshes().TryGetAssetId(handle);
    const auto view = m_assets.Meshes().TryGet(handle);
    if (!assetId || *assetId != id || !view || !view->asset)
        throw std::invalid_argument("M6 mesh handle/id is stale");
    const auto& asset = *view->asset;
    if (asset.vertexStride != kVertexStride || asset.indexStride != kIndexStride || asset.vertexCount == 0 ||
        asset.indexCount == 0 || asset.vertexData.size() != std::size_t(asset.vertexCount) * kVertexStride ||
        asset.indexData.size() != std::size_t(asset.indexCount) * kIndexStride)
        throw std::invalid_argument("M6 mesh is not a valid PbrVertex/uint32 asset");

    const auto found = m_meshes.find(id);
    if (found != m_meshes.end() && found->second.source == handle && found->second.revision == view->revision)
        return;

    const auto vertexBytes = std::span<const std::byte>(asset.vertexData.data(), asset.vertexData.size());
    const auto indexBytes = std::span<const std::byte>(asset.indexData.data(), asset.indexData.size());
    BufferHandle vertex;
    BufferHandle index;
    try
    {
        vertex = m_device.CreateBuffer(
            {vertexBytes.size(), BufferUsage::Vertex, MemoryDomain::GpuOnly, "M6.Mesh." + id.ToHexString() + ".VB"},
            vertexBytes);
        index = m_device.CreateBuffer(
            {indexBytes.size(), BufferUsage::Index, MemoryDomain::GpuOnly, "M6.Mesh." + id.ToHexString() + ".IB"},
            indexBytes);
    }
    catch (...)
    {
        if (vertex)
        {
            try
            {
                m_device.Destroy(vertex);
            }
            catch (...)
            {
            }
        }
        if (index)
        {
            try
            {
                m_device.Destroy(index);
            }
            catch (...)
            {
            }
        }
        throw;
    }

    GpuMesh replacement;
    replacement.source = handle;
    replacement.revision = view->revision;
    replacement.vertex = vertex;
    replacement.index = index;
    replacement.vertexView = {vertex, 0, vertexBytes.size()};
    replacement.indexView = {index, 0, indexBytes.size()};
    replacement.vertexCount = asset.vertexCount;
    replacement.indexCount = asset.indexCount;
    if (found != m_meshes.end())
    {
        const auto oldVertex = found->second.vertex;
        const auto oldIndex = found->second.index;
        found->second = replacement;
        if (oldVertex)
            m_device.Destroy(oldVertex);
        if (oldIndex)
            m_device.Destroy(oldIndex);
    }
    else
        m_meshes.emplace(id, std::move(replacement));
}

void M6SceneResources::EnsureTexture(const Assets::AssetId& id, Assets::AssetHandle<Assets::TextureAsset> handle,
                                     Assets::TextureUsage expectedUsage)
{
    if (!id.IsValid())
        throw std::invalid_argument("M6 texture id is invalid");
    const auto assetId = m_assets.Textures().TryGetAssetId(handle);
    const auto view = m_assets.Textures().TryGet(handle);
    if (!assetId || *assetId != id || !view || !view->asset || view->asset->usage != expectedUsage)
        throw std::invalid_argument("M6 material texture handle/usage is invalid");
    const auto& asset = *view->asset;
    const auto desc = MaterialTextureDesc(asset, "M6.Texture." + id.ToHexString());
    const auto found = m_textures.find(id);
    if (found != m_textures.end() && found->second.source == handle && found->second.revision == view->revision)
        return;

    TextureHandle texture;
    try
    {
        texture = m_device.CreateTexture(desc);
        std::vector<TextureSubresourceData> levels;
        levels.reserve(asset.mips.size());
        const auto bytes = std::span<const std::byte>(asset.pixels.data(), asset.pixels.size());
        for (const auto& mip : asset.mips)
        {
            if (mip.offset > bytes.size() || mip.byteSize > bytes.size() - mip.offset)
                throw std::invalid_argument("M6 texture mip is outside payload");
            levels.push_back(
                {bytes.subspan(static_cast<std::size_t>(mip.offset), mip.byteSize), mip.rowPitch, mip.byteSize});
        }
        m_device.UploadTexture(texture, levels);
    }
    catch (...)
    {
        if (texture)
        {
            try
            {
                m_device.Destroy(texture);
            }
            catch (...)
            {
            }
        }
        throw;
    }

    GpuTexture replacement;
    replacement.source = handle;
    replacement.revision = view->revision;
    replacement.texture = texture;
    replacement.descriptor = desc;
    if (found != m_textures.end())
    {
        const auto old = found->second.texture;
        found->second = replacement;
        if (old)
            m_device.Destroy(old);
    }
    else
        m_textures.emplace(id, std::move(replacement));
}
void M6SceneResources::EnsureMaterialTextures(Assets::AssetHandle<Assets::MaterialAsset> handle)
{
    const auto view = m_assets.Materials().TryGet(handle);
    if (!view || !view->asset)
        throw std::invalid_argument("M6 material handle is stale");
    const auto ensure = [&](const Assets::AssetId& id, Assets::TextureUsage usage)
    {
        if (!id.IsValid())
            return;
        const auto textureHandle = m_assets.Textures().TryFind(id);
        if (!textureHandle)
            throw std::invalid_argument("M6 material references a missing texture");
        EnsureTexture(id, *textureHandle, usage);
    };
    ensure(view->asset->baseColorTexture, Assets::TextureUsage::BaseColor);
    ensure(view->asset->normalTexture, Assets::TextureUsage::Normal);
    ensure(view->asset->metallicRoughnessTexture, Assets::TextureUsage::MetallicRoughness);
    ensure(view->asset->occlusionTexture, Assets::TextureUsage::Occlusion);
    ensure(view->asset->emissiveTexture, Assets::TextureUsage::Emissive);
}

void M6SceneResources::PreparePersistentAssets(const World::RenderPacket& packet)
{
    const auto prepare = [&](const World::RenderDraw& draw)
    {
        EnsureMesh(draw.meshId, draw.mesh);
        EnsureMaterialTextures(draw.material);
    };
    for (const auto& draw : packet.mainOpaque)
        prepare(draw);
    for (const auto& draw : packet.shadowCasters)
        prepare(draw);
}

const M6SceneResources::GpuMesh& M6SceneResources::FindMesh(const Assets::AssetId& id) const
{
    const auto found = m_meshes.find(id);
    if (found == m_meshes.end())
        throw std::invalid_argument("M6 persistent mesh is not prepared");
    return found->second;
}

const M6SceneResources::GpuTexture& M6SceneResources::FindTexture(const Assets::AssetId& id,
                                                                  Assets::TextureUsage usage) const
{
    const auto found = m_textures.find(id);
    if (found == m_textures.end())
        throw std::invalid_argument("M6 persistent texture is not prepared");
    const auto view = m_assets.Textures().TryGet(found->second.source);
    if (!view || !view->asset || view->asset->usage != usage || view->revision != found->second.revision)
        throw std::invalid_argument("M6 persistent texture revision is stale");
    return found->second;
}

TextureHandle M6SceneResources::MaterialTexture(const Assets::AssetId& id, Assets::TextureUsage usage) const
{
    if (!id.IsValid())
        return m_fallbackTextures[FallbackIndex(usage)];
    return FindTexture(id, usage).texture;
}

void M6SceneResources::AddBufferImport(const FrameToken& frame, BufferHandle handle, ResourceAccess finalAccess,
                                       ShaderStage stages, std::vector<M6BufferInput>& imports,
                                       std::string_view name) const
{
    if (!handle)
        throw std::invalid_argument("M6 draw references invalid buffer");
    const auto found = std::find_if(imports.begin(), imports.end(),
                                    [&](const auto& entry) { return entry.imported.physical == handle; });
    if (found != imports.end())
        return;
    const auto state = m_device.QueryBufferState(frame, handle);
    M6BufferInput input;
    input.name = std::string(name);
    input.imported = {handle,
                      state.descriptor,
                      state.access,
                      finalAccess,
                      state.fullyDefined ? ContentState::Defined : ContentState::Undefined,
                      "M6SceneResources",
                      std::string(name)};
    input.access = finalAccess;
    input.stages = stages;
    imports.push_back(std::move(input));
}

void M6SceneResources::AddTextureImport(const FrameToken& frame, TextureHandle handle, ResourceAccess finalAccess,
                                        ShaderStage stages, std::vector<M6TextureInput>& imports,
                                        std::string_view name) const
{
    if (stages != ShaderStage::Pixel)
        throw std::invalid_argument("M6 sampled textures require pixel-stage reads");
    if (!handle)
        throw std::invalid_argument("M6 draw references invalid texture");
    const auto found = std::find_if(imports.begin(), imports.end(),
                                    [&](const auto& entry) { return entry.imported.physical == handle; });
    if (found != imports.end())
        return;
    const auto state = m_device.QueryTextureState(frame, handle);
    M6TextureInput input;
    input.name = std::string(name);
    input.imported = {handle,
                      state.descriptor,
                      state.access,
                      finalAccess,
                      state.fullyDefined ? ContentState::Defined : ContentState::Undefined,
                      "M6SceneResources",
                      std::string(name)};
    imports.push_back(std::move(input));
}

BufferView M6SceneResources::WriteConstants(const FrameToken& frame, std::span<const std::byte> bytes,
                                            ShaderStage stages, std::vector<M6BufferInput>& imports,
                                            std::string_view name) const
{
    if (bytes.empty() || (bytes.size() % 16) != 0)
        throw std::invalid_argument("M6 constant size must be a nonzero 16-byte multiple");
    auto alignment = m_device.Capabilities().uniformBufferOffsetAlignment;
    if (alignment == 0)
        alignment = 16;
    const auto slice = m_device.WriteDynamicBuffer(frame, bytes, alignment);
    AddBufferImport(frame, slice.view.buffer, ResourceAccess::UniformRead, stages, imports, name);
    return slice.view;
}

ResourceSetDesc M6SceneResources::EmptySet(const PipelineSet& pipeline, std::uint8_t set) const
{
    if (set >= pipeline.setCount || !pipeline.layouts[set])
        throw std::invalid_argument("M6 empty set index is invalid");
    return {pipeline.layouts[set], {}, "M6.EmptySet." + std::to_string(set)};
}

M6Draw M6SceneResources::MakeOpaqueDraw(const World::RenderDraw& draw, const BufferView& frameConstants,
                                        const BufferView& iblConstants, const BufferView& objectConstants,
                                        const BufferView& materialConstants) const
{
    const auto& mesh = FindMesh(draw.meshId);
    const auto material = m_assets.Materials().TryGet(draw.material);
    if (!material || !material->asset)
        throw std::invalid_argument("M6 material is stale");
    M6Draw result;
    result.stableId = "M6.Opaque." + std::to_string(draw.entityIndex) + "." + draw.meshId.ToHexString();
    result.pipeline = draw.mirrored ? m_revision.pbr.mirrored : m_revision.pbr.regular;
    result.setCount = 3;
    result.vertices = mesh.vertexView;
    result.indices = mesh.indexView;
    result.stride = kVertexStride;
    result.vertexCount = mesh.vertexCount;
    result.indexCount = mesh.indexCount;
    result.indexType = IndexType::UInt32;
    result.sets[0] = {m_revision.pbr.layouts[0],
                      {UniformBinding(0, frameConstants), UniformBinding(1, iblConstants),
                       TextureBinding(2, m_environment.textures[1]), TextureBinding(3, m_environment.textures[2]),
                       TextureBinding(4, m_environment.textures[3]), TextureBinding(5, {}),
                       SamplerBinding(6, m_samplers[0]), SamplerBinding(7, m_samplers[1]),
                       SamplerBinding(8, m_samplers[2])},
                      "M6.Pbr.SceneSet"};
    result.sets[1] = {
        m_revision.pbr.layouts[1],
        {UniformBinding(0, materialConstants),
         TextureBinding(1, MaterialTexture(material->asset->baseColorTexture, Assets::TextureUsage::BaseColor)),
         TextureBinding(2, MaterialTexture(material->asset->normalTexture, Assets::TextureUsage::Normal)),
         TextureBinding(
             3, MaterialTexture(material->asset->metallicRoughnessTexture, Assets::TextureUsage::MetallicRoughness)),
         TextureBinding(4, MaterialTexture(material->asset->occlusionTexture, Assets::TextureUsage::Occlusion)),
         TextureBinding(5, MaterialTexture(material->asset->emissiveTexture, Assets::TextureUsage::Emissive))},
        "M6.Pbr.MaterialSet"};
    result.sets[2] = {m_revision.pbr.layouts[2], {UniformBinding(0, objectConstants)}, "M6.Pbr.ObjectSet"};
    result.dynamicOffsets[0] = {0};
    result.dynamicOffsets[2] = {0};
    return result;
}

M6Draw M6SceneResources::MakeShadowDraw(const World::RenderDraw& draw, const BufferView& objectConstants) const
{
    const auto& mesh = FindMesh(draw.meshId);
    M6Draw result;
    result.stableId = "M6.Shadow." + std::to_string(draw.entityIndex) + "." + draw.meshId.ToHexString();
    result.pipeline = draw.mirrored ? m_revision.shadow.mirrored : m_revision.shadow.regular;
    result.setCount = 3;
    result.vertices = mesh.vertexView;
    result.indices = mesh.indexView;
    result.stride = kVertexStride;
    result.vertexCount = mesh.vertexCount;
    result.indexCount = mesh.indexCount;
    result.indexType = IndexType::UInt32;
    result.sets[0] = EmptySet(m_revision.shadow, 0);
    result.sets[1] = EmptySet(m_revision.shadow, 1);
    result.sets[2] = {m_revision.shadow.layouts[2], {UniformBinding(0, objectConstants)}, "M6.Shadow.ObjectSet"};
    result.dynamicOffsets[2] = {0};
    return result;
}
M6PipelineResources M6SceneResources::Prepare(const World::RenderPacket& packet, const FrameToken& frame,
                                              Extent2D extent, std::uint32_t level, std::uint32_t debugView,
                                              float exposure) const
{
    if (!frame.serial || extent.width == 0 || extent.height == 0 || level < 1 || level > 9)
        throw std::invalid_argument("invalid M6 frame/extent/level");
    M6PipelineResources result;
    result.extent = extent;
    result.level = level;
    result.shadowDesc = {TextureDimension::Texture2D,
                         {kShadowSize, kShadowSize},
                         1,
                         1,
                         1,
                         Format::D32Float,
                         TextureUsage::DepthStencil | TextureUsage::Sampled,
                         "M6.ShadowMap",
                         {0, 0, 0, 1},
                         1.0F,
                         0};
    result.depthDesc = {TextureDimension::Texture2D,
                        extent,
                        1,
                        1,
                        1,
                        Format::D32Float,
                        TextureUsage::DepthStencil,
                        "M6.SceneDepth",
                        {0, 0, 0, 1},
                        1.0F,
                        0};
    result.hdrDesc = {TextureDimension::Texture2D,
                      extent,
                      1,
                      1,
                      1,
                      Format::Rgba16Float,
                      TextureUsage::ColorAttachment | TextureUsage::Sampled,
                      "M6.HdrColor",
                      {0, 0, 0, 1},
                      0.0F};
    result.triangle.pipeline = m_revision.triangle;
    result.triangle.stableId = "M6.Triangle";
    result.triangle.vertexCount = 3;
    result.triangle.indexCount = 0;
    if (level < 3)
        return result;

    if (!std::isfinite(exposure))
        throw std::invalid_argument("M6 exposure must be finite");
    PostProcessConstants post;
    post.exposureEv = exposure;
    post.debugHdr = debugView == kDebugSceneLuminance ? 1U : 0U;
    post.inverseWidth = 1.0F / static_cast<float>(extent.width);
    post.inverseHeight = 1.0F / static_cast<float>(extent.height);
    const auto postBuffer =
        WriteConstants(frame, Bytes(post), ShaderStage::Pixel, result.buffers, "M6.PostProcessConstants");
    result.tone.pipeline = m_revision.tone.regular;
    result.tone.stableId = "M6.ToneMap";
    result.tone.setCount = 1;
    result.tone.vertexCount = 3;
    result.tone.sets[0] = {m_revision.tone.layouts[0],
                           {SamplerBinding(7, m_samplers[1]), UniformBinding(9, postBuffer), TextureBinding(10, {})},
                           "M6.Tone.Set"};

    if (level < 4)
    {
        const auto state = m_device.QueryTextureState(frame, m_toneSource);
        result.toneSource = {
            m_toneSource,          state.descriptor,   state.access,          ResourceAccess::SampledRead,
            ContentState::Defined, "M6SceneResources", "M6.ToneSourceFixture"};
        return result;
    }

    ValidateEnvironment(m_environment);
    for (const auto& draw : packet.mainOpaque)
    {
        (void)FindMesh(draw.meshId);
        if (!m_assets.Materials().TryGet(draw.material))
            throw std::invalid_argument("M6 material is not prepared");
    }
    const FrameConstants frameData{
        Transpose(packet.viewProjection),
        {packet.cameraWorldPosition.x, packet.cameraWorldPosition.y, packet.cameraWorldPosition.z,
         static_cast<float>(debugView)},
        {packet.directionalLight.directionToLight[0], packet.directionalLight.directionToLight[1],
         packet.directionalLight.directionToLight[2], packet.directionalLight.illuminanceScale},
        {packet.directionalLight.colorLinear[0], packet.directionalLight.colorLinear[1],
         packet.directionalLight.colorLinear[2], 0.0F},
        {static_cast<float>(kShadowSize), static_cast<float>(kShadowSize), 0.0F, 0.0F}};
    const auto frameBuffer = WriteConstants(frame, Bytes(frameData), ShaderStage::Vertex | ShaderStage::Pixel,
                                            result.buffers, "M6.FrameConstants");
    const auto prefilterMips = static_cast<float>(m_environment.descriptors[2].mipLevels);
    if (prefilterMips <= 0.0F)
        throw std::invalid_argument("M6 environment prefilter has no mip chain");
    const IblConstants iblData{{prefilterMips, 0.0F, 0.0F, 0.0F}};
    const auto iblBuffer = WriteConstants(frame, Bytes(iblData), ShaderStage::Pixel, result.buffers, "M6.IblConstants");

    for (std::size_t i = 0; i < m_environment.textures.size(); ++i)
        AddTextureImport(frame, m_environment.textures[i], ResourceAccess::SampledRead, ShaderStage::Pixel,
                         result.textures, "M6.Environment." + std::to_string(i));
    for (const auto& [id, texture] : m_textures)
        AddTextureImport(frame, texture.texture, ResourceAccess::SampledRead, ShaderStage::Pixel, result.textures,
                         "M6.MaterialTexture." + id.ToHexString());
    for (std::size_t i = 0; i < m_fallbackTextures.size(); ++i)
        AddTextureImport(frame, m_fallbackTextures[i], ResourceAccess::SampledRead, ShaderStage::Pixel, result.textures,
                         "M6.MaterialFallback." + std::to_string(i));

    for (const auto& draw : packet.mainOpaque)
    {
        const auto& mesh = FindMesh(draw.meshId);
        const auto material = m_assets.Materials().TryGet(draw.material);
        if (!material || !material->asset)
            throw std::invalid_argument("M6 material is stale");
        const MaterialConstants materialData{
            material->asset->baseColorFactor,
            {material->asset->emissiveFactor[0], material->asset->emissiveFactor[1], material->asset->emissiveFactor[2],
             material->asset->metallicFactor},
            {material->asset->roughnessFactor, material->asset->normalScale, material->asset->occlusionStrength, 0.0F}};
        const auto materialBuffer = WriteConstants(frame, Bytes(materialData), ShaderStage::Pixel, result.buffers,
                                                   "M6.MaterialConstants." + std::to_string(draw.entityIndex));
        const ObjectConstants objectData{Transpose(draw.world),
                                         Transpose(draw.normal),
                                         Transpose(World::BuildLightViewProjection(packet.directionalLight)),
                                         {draw.mirrored ? -1.0F : 1.0F, draw.receivesShadow ? 1.0F : 0.0F, 0.0F, 0.0F}};
        const auto objectBuffer =
            WriteConstants(frame, Bytes(objectData), ShaderStage::Vertex | ShaderStage::Pixel, result.buffers,
                           "M6.ObjectConstants." + std::to_string(draw.entityIndex));
        result.opaque.push_back(MakeOpaqueDraw(draw, frameBuffer, iblBuffer, objectBuffer, materialBuffer));
        AddBufferImport(frame, mesh.vertex, ResourceAccess::VertexRead, ShaderStage::Vertex, result.buffers,
                        "M6.Mesh.Vertex." + draw.meshId.ToHexString());
        AddBufferImport(frame, mesh.index, ResourceAccess::IndexRead, ShaderStage::Vertex, result.buffers,
                        "M6.Mesh.Index." + draw.meshId.ToHexString());
    }

    if (level >= 5)
    {
        const SkyboxConstants skyData{SkyViewProjection(packet.view, packet.projection)};
        const auto skyBuffer =
            WriteConstants(frame, Bytes(skyData), ShaderStage::Vertex, result.buffers, "M6.SkyboxConstants");
        result.sky.pipeline = m_revision.sky.regular;
        result.sky.stableId = "M6.Skybox";
        result.sky.setCount = 1;
        result.sky.vertices = m_skyVertexView;
        result.sky.indices = m_skyIndexView;
        result.sky.stride = 12;
        result.sky.vertexCount = 24;
        result.sky.indexCount = 36;
        result.sky.indexType = IndexType::UInt16;
        result.sky.sets[0] = {m_revision.sky.layouts[0],
                              {SamplerBinding(7, m_samplers[1]), UniformBinding(11, skyBuffer),
                               TextureBinding(12, m_environment.textures[0])},
                              "M6.Skybox.Set"};
        AddBufferImport(frame, m_skyVertexBuffer, ResourceAccess::VertexRead, ShaderStage::Vertex, result.buffers,
                        "M6.Skybox.Vertex");
        AddBufferImport(frame, m_skyIndexBuffer, ResourceAccess::IndexRead, ShaderStage::Vertex, result.buffers,
                        "M6.Skybox.Index");
    }

    if (level >= 6)
    {
        for (const auto& draw : packet.shadowCasters)
        {
            const auto& mesh = FindMesh(draw.meshId);
            const ObjectConstants objectData{
                Transpose(draw.world),
                Transpose(draw.normal),
                Transpose(World::BuildLightViewProjection(packet.directionalLight)),
                {draw.mirrored ? -1.0F : 1.0F, draw.receivesShadow ? 1.0F : 0.0F, 0.0F, 0.0F}};
            const auto objectBuffer = WriteConstants(frame, Bytes(objectData), ShaderStage::Vertex, result.buffers,
                                                     "M6.Shadow.ObjectConstants." + std::to_string(draw.entityIndex));
            result.shadow.push_back(MakeShadowDraw(draw, objectBuffer));
            AddBufferImport(frame, mesh.vertex, ResourceAccess::VertexRead, ShaderStage::Vertex, result.buffers,
                            "M6.Mesh.Vertex." + draw.meshId.ToHexString());
            AddBufferImport(frame, mesh.index, ResourceAccess::IndexRead, ShaderStage::Vertex, result.buffers,
                            "M6.Mesh.Index." + draw.meshId.ToHexString());
        }
    }
    return result;
}
void M6SceneResources::ReloadShaders(std::span<const ShaderDesc> shaders)
{
    if (shaders.empty())
        throw std::invalid_argument("M6 shader revision is empty");
    ShaderRevision candidate;
    try
    {
        const auto findShader = [&](std::string_view name, ShaderStage stage) -> const ShaderDesc&
        {
            const auto found = std::find_if(shaders.begin(), shaders.end(), [&](const auto& shader)
                                            { return shader.debugName == name && shader.stage == stage; });
            if (found == shaders.end())
                throw std::invalid_argument("M6 shader revision missing " + std::string(name));
            return *found;
        };
        const auto& pbrVs = findShader("PbrForwardVSMain", ShaderStage::Vertex);
        const auto& pbrPs = findShader("PbrForwardPSMain", ShaderStage::Pixel);
        const auto& shadowVs = findShader("ShadowDepthVSMain", ShaderStage::Vertex);
        const auto& skyVs = findShader("SkyboxVSMain", ShaderStage::Vertex);
        const auto& skyPs = findShader("SkyboxPSMain", ShaderStage::Pixel);
        const auto& toneVs = findShader("ToneMapVSMain", ShaderStage::Vertex);
        const auto& tonePs = findShader("ToneMapPSMain", ShaderStage::Pixel);
        const auto& triangleVs = findShader("GraphTriangleVSMain", ShaderStage::Vertex);
        const auto& trianglePs = findShader("GraphTrianglePSMain", ShaderStage::Pixel);

        std::vector<std::pair<std::string, ShaderHandle>> created;
        for (const auto& shader : shaders)
        {
            if (shader.debugName.empty())
                throw std::invalid_argument("M6 shader debugName is empty");
            if (std::find_if(created.begin(), created.end(),
                             [&](const auto& value) { return value.first == shader.debugName; }) != created.end())
                throw std::invalid_argument("duplicate M6 shader debugName");
            created.emplace_back(shader.debugName, m_device.CreateShader(shader));
            candidate.shaders.push_back(created.back().second);
        }
        const auto shaderHandle = [&](const ShaderDesc& desc)
        {
            const auto found = std::find_if(created.begin(), created.end(),
                                            [&](const auto& value) { return value.first == desc.debugName; });
            if (found == created.end())
                throw std::logic_error("M6 shader handle creation failed");
            return found->second;
        };

        const auto makePipelineSet =
            [&](const ShaderDesc& vs, const ShaderDesc* ps, std::string name, bool mirroredPipelines)
        {
            std::vector<ResourceSetLayoutDesc> layouts;
            AddRequirement(layouts, vs);
            if (ps)
                AddRequirement(layouts, *ps);
            PipelineSet result;
            result.setCount = static_cast<std::uint8_t>(layouts.size());
            for (auto& layout : layouts)
            {
                std::sort(layout.entries.begin(), layout.entries.end(),
                          [](const auto& a, const auto& b) { return a.binding < b.binding; });
                layout.debugName = name + ".Set" + std::to_string(layout.set);
                result.layouts[layout.set] = m_device.CreateResourceSetLayout(layout);
                candidate.setLayouts.push_back(result.layouts[layout.set]);
            }
            PipelineLayoutDesc pipelineLayout;
            pipelineLayout.sets = result.layouts;
            pipelineLayout.setCount = result.setCount;
            pipelineLayout.debugName = name + ".PipelineLayout";
            result.layout = m_device.CreatePipelineLayout(pipelineLayout);
            candidate.pipelineLayouts.push_back(result.layout);

            const auto attributes = [&]
            {
                std::vector<VertexAttributeDesc> resultAttributes;
                for (const auto& input : vs.manifest.vertexInputs)
                    resultAttributes.push_back(
                        {input.semantic, input.format, VertexOffset(input.semantic), 0, 0, input.semanticIndex});
                return resultAttributes;
            };
            const auto makeDesc = [&](GraphicsPipelineDesc desc, FrontFace frontFace, std::string debugName)
            {
                desc.vertexShader = shaderHandle(vs);
                desc.pixelShader = ps ? shaderHandle(*ps) : ShaderHandle{};
                desc.layout = result.layout;
                desc.frontFace = frontFace;
                desc.debugName = std::move(debugName);
                const auto handle = m_device.CreateGraphicsPipeline(desc);
                candidate.pipelines.push_back(handle);
                return handle;
            };

            GraphicsPipelineDesc base;
            base.vertexAttributes = attributes();
            base.topology = PrimitiveTopology::TriangleList;
            if (name == "M6.Pbr")
            {
                base.colorAttachmentCount = 1;
                base.colorFormats[0] = Format::Rgba16Float;
                base.depthFormat = Format::D32Float;
                base.cullMode = CullMode::Back;
                base.depthTest = true;
                base.depthWrite = true;
                base.depthCompare = CompareOp::LessEqual;
            }
            else if (name == "M6.Shadow")
            {
                base.depthFormat = Format::D32Float;
                base.cullMode = CullMode::Back;
                base.depthTest = true;
                base.depthWrite = true;
                base.depthCompare = CompareOp::LessEqual;
                base.slopeScaledDepthBias = 0.1F;
            }
            else if (name == "M6.Sky")
            {
                base.colorAttachmentCount = 1;
                base.colorFormats[0] = Format::Rgba16Float;
                base.depthFormat = Format::D32Float;
                base.cullMode = CullMode::None;
                base.depthTest = true;
                base.depthWrite = false;
                base.depthCompare = CompareOp::LessEqual;
            }
            else
            {
                base.colorAttachmentCount = 1;
                base.colorFormats[0] = Format::Rgba8Unorm;
                base.depthFormat = Format::Unknown;
                base.cullMode = CullMode::None;
                base.depthTest = false;
                base.depthWrite = false;
            }
            result.regular = makeDesc(base, FrontFace::CounterClockwise, name + ".Pipeline");
            result.mirrored =
                mirroredPipelines ? makeDesc(base, FrontFace::Clockwise, name + ".MirroredPipeline") : result.regular;
            return result;
        };

        candidate.pbr = makePipelineSet(pbrVs, &pbrPs, "M6.Pbr", true);
        candidate.shadow = makePipelineSet(shadowVs, nullptr, "M6.Shadow", true);
        candidate.sky = makePipelineSet(skyVs, &skyPs, "M6.Sky", false);
        candidate.tone = makePipelineSet(toneVs, &tonePs, "M6.Tone", false);

        PipelineLayoutDesc triangleLayout;
        triangleLayout.setCount = 0;
        triangleLayout.debugName = "M6.Triangle.PipelineLayout";
        const auto trianglePipelineLayout = m_device.CreatePipelineLayout(triangleLayout);
        candidate.pipelineLayouts.push_back(trianglePipelineLayout);
        GraphicsPipelineDesc triangle;
        triangle.vertexShader = shaderHandle(triangleVs);
        triangle.pixelShader = shaderHandle(trianglePs);
        triangle.layout = trianglePipelineLayout;
        triangle.colorAttachmentCount = 1;
        triangle.colorFormats[0] = Format::Rgba8Unorm;
        triangle.cullMode = CullMode::None;
        triangle.depthTest = false;
        triangle.depthWrite = false;
        triangle.frontFace = FrontFace::CounterClockwise;
        triangle.debugName = "M6.Triangle.Pipeline";
        candidate.triangle = m_device.CreateGraphicsPipeline(triangle);
        candidate.pipelines.push_back(candidate.triangle);

        ShaderRevision old = std::move(m_revision);
        m_revision = std::move(candidate);
        DestroyRevision(m_device, old);
    }
    catch (...)
    {
        DestroyRevision(m_device, candidate);
        throw;
    }
}

} // namespace MiniEngine::Render
