#pragma once
#include <MiniEngine/RenderGraph/RenderGraphTypes.h>
#include <array>
#include <memory>
#include <string_view>

namespace MiniEngine::RenderGraph
{
class RenderGraph;
class RgBuilder final
{
  public:
    RgTexture CreateTexture(std::string_view name, const Rhi::TextureDesc& descriptor);
    RgBuffer CreateBuffer(std::string_view name, const Rhi::BufferDesc& descriptor);
    RgTexture ImportTexture(std::string_view name, const TextureImport& imported);
    RgBuffer ImportBuffer(std::string_view name, const BufferImport& imported);
    RgTexture Read(RgTexture texture, Rhi::ResourceAccess access, Rhi::ShaderStage stages = Rhi::ShaderStage::Pixel);
    RgBuffer Read(RgBuffer buffer, Rhi::ResourceAccess access, Rhi::ShaderStage stages = Rhi::ShaderStage::Vertex);
    [[nodiscard]] RgTexture Write(RgTexture texture, Rhi::ResourceAccess access,
                                  WriteCoverage coverage = WriteCoverage::Preserve);
    [[nodiscard]] RgBuffer Write(RgBuffer buffer, Rhi::ResourceAccess access,
                                 WriteCoverage coverage = WriteCoverage::Preserve);
    void SetColorAttachment(RgTexture texture, Rhi::LoadOp load, Rhi::StoreOp store,
                            std::array<float, 4> clearColor = {0, 0, 0, 1});
    void SetDepthAttachment(RgTexture texture, Rhi::LoadOp load, Rhi::StoreOp store, float clearDepth = 1.0F,
                            std::uint8_t clearStencil = 0);
    void Present(RgTexture texture);
    void Export(RgTexture texture);
    void Export(RgBuffer buffer);
    void SideEffect(std::string_view reason);

  private:
    friend class RenderGraph;
    RgBuilder(const std::shared_ptr<Detail::GraphState>& state, std::uint32_t pass);
    std::shared_ptr<Detail::GraphState> Lock() const;
    std::weak_ptr<Detail::GraphState> m_state;
    std::uint32_t m_pass = 0;
    std::uint64_t m_generation = 0;
};
} // namespace MiniEngine::RenderGraph
