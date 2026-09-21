#pragma once

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>
#include <MiniEngine/Render/M6RenderPipeline.h>
#include <MiniEngine/Rhi/RhiFactory.h>
#include <MiniEngine/Rhi/RhiShaderPackage.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace MiniEngine::Assets
{
class AssetManager;
}

namespace MiniEngine::Render
{
class M6SceneResources final
{
  public:
    M6SceneResources(Rhi::IRhiDevice& device, Assets::AssetManager& assets, std::span<const Rhi::ShaderDesc> shaders,
                     const Rhi::PreparedEnvironment& environment);
    M6SceneResources(Rhi::IRhiDevice& device, Assets::AssetManager& assets, std::span<const Rhi::ShaderDesc> shaders);
    ~M6SceneResources();

    void PreparePersistentAssets(const World::RenderPacket& packet);

    [[nodiscard]] M6PipelineResources Prepare(const World::RenderPacket& packet, const Rhi::FrameToken& frame,
                                              Rhi::Extent2D extent, std::uint32_t level, std::uint32_t debugView,
                                              float exposure) const;

    void ReloadShaders(std::span<const Rhi::ShaderDesc> shaders);

    // 当前 revision 的 live GraphicsPipeline 数；稳定性诊断使用。
    [[nodiscard]] std::size_t PipelineCount() const
    {
        return m_revision.pipelines.size();
    }

  private:
    struct GpuMesh final
    {
        Assets::AssetHandle<Assets::MeshAsset> source;
        std::uint64_t revision = 0;
        Rhi::BufferHandle vertex;
        Rhi::BufferHandle index;
        Rhi::BufferView vertexView;
        Rhi::BufferView indexView;
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
    };

    struct GpuTexture final
    {
        Assets::AssetHandle<Assets::TextureAsset> source;
        std::uint64_t revision = 0;
        Rhi::TextureHandle texture;
        Rhi::TextureDesc descriptor;
    };

    struct PipelineSet final
    {
        std::array<Rhi::ResourceSetLayoutHandle, 3> layouts{};
        std::uint8_t setCount = 0;
        Rhi::PipelineLayoutHandle layout;
        Rhi::GraphicsPipelineHandle regular;
        Rhi::GraphicsPipelineHandle mirrored;
    };

    struct ShaderRevision final
    {
        std::vector<Rhi::ShaderHandle> shaders;
        std::vector<Rhi::ResourceSetLayoutHandle> setLayouts;
        std::vector<Rhi::PipelineLayoutHandle> pipelineLayouts;
        std::vector<Rhi::GraphicsPipelineHandle> pipelines;
        PipelineSet pbr;
        PipelineSet shadow;
        PipelineSet sky;
        PipelineSet tone;
        Rhi::GraphicsPipelineHandle triangle;
    };

    static void DestroyRevision(Rhi::IRhiDevice& device, ShaderRevision& revision) noexcept;
    void ReleaseAll() noexcept;
    void EnsureMesh(const Assets::AssetId& id, Assets::AssetHandle<Assets::MeshAsset> handle);
    void EnsureTexture(const Assets::AssetId& id, Assets::AssetHandle<Assets::TextureAsset> handle,
                       Assets::TextureUsage expectedUsage);
    void EnsureMaterialTextures(Assets::AssetHandle<Assets::MaterialAsset> handle);

    [[nodiscard]] const GpuMesh& FindMesh(const Assets::AssetId& id) const;
    [[nodiscard]] const GpuTexture& FindTexture(const Assets::AssetId& id, Assets::TextureUsage usage) const;
    [[nodiscard]] Rhi::TextureHandle MaterialTexture(const Assets::AssetId& id, Assets::TextureUsage usage) const;

    [[nodiscard]] Rhi::BufferView WriteConstants(const Rhi::FrameToken& frame, std::span<const std::byte> bytes,
                                                 Rhi::ShaderStage stages, std::vector<M6BufferInput>& imports,
                                                 std::string_view name) const;
    void AddBufferImport(const Rhi::FrameToken& frame, Rhi::BufferHandle handle, Rhi::ResourceAccess finalAccess,
                         Rhi::ShaderStage stages, std::vector<M6BufferInput>& imports, std::string_view name) const;
    void AddTextureImport(const Rhi::FrameToken& frame, Rhi::TextureHandle handle, Rhi::ResourceAccess finalAccess,
                          Rhi::ShaderStage stages, std::vector<M6TextureInput>& imports, std::string_view name) const;

    [[nodiscard]] M6Draw MakeOpaqueDraw(const World::RenderDraw& draw, const Rhi::BufferView& frameConstants,
                                        const Rhi::BufferView& iblConstants, const Rhi::BufferView& objectConstants,
                                        const Rhi::BufferView& materialConstants) const;
    [[nodiscard]] M6Draw MakeShadowDraw(const World::RenderDraw& draw, const Rhi::BufferView& objectConstants) const;
    [[nodiscard]] Rhi::ResourceSetDesc EmptySet(const PipelineSet& pipeline, std::uint8_t set) const;

    Rhi::IRhiDevice& m_device;
    Assets::AssetManager& m_assets;
    Rhi::PreparedEnvironment m_environment{};
    Rhi::TextureHandle m_toneSource;
    ShaderRevision m_revision;
    std::unordered_map<Assets::AssetId, GpuMesh, Assets::AssetIdHasher> m_meshes;
    std::unordered_map<Assets::AssetId, GpuTexture, Assets::AssetIdHasher> m_textures;
    std::array<Rhi::TextureHandle, 5> m_fallbackTextures{};
    Rhi::BufferHandle m_skyVertexBuffer;
    Rhi::BufferHandle m_skyIndexBuffer;
    Rhi::BufferView m_skyVertexView;
    Rhi::BufferView m_skyIndexView;
    std::array<Rhi::SamplerHandle, 3> m_samplers{};
};
} // namespace MiniEngine::Render
