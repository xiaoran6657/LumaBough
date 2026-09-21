#pragma once
#include <MiniEngine/RenderGraph/RenderGraphHandle.h>
#include <MiniEngine/Rhi/RhiDescriptors.h>
#include <MiniEngine/Rhi/RhiHandle.h>
#include <memory>

namespace MiniEngine::RenderGraph
{
class CompiledRenderGraph;
class RgResources final
{
  public:
    // 返回借用的 RHI handle；只在当前 callback 内、且 exact version 已声明时可解析。
    // 保存本 resolver 的副本不会延长 pass/graph/generation 的有效期。
    [[nodiscard]] Rhi::TextureHandle Get(RgTexture texture) const;
    [[nodiscard]] Rhi::BufferHandle Get(RgBuffer buffer) const;
    // 仅在当前 execute callback 内创建帧专属集合；graph 资源必须由本 pass 以兼容的读访问声明。
    // 外部 immutable 资源仍交由 RHI 的既有 ResourceSet 校验；返回集合由当前帧自动退休。
    [[nodiscard]] Rhi::ResourceSetHandle CreateResourceSet(const Rhi::ResourceSetDesc& desc) const;

  private:
    friend class CompiledRenderGraph;
    RgResources(const std::shared_ptr<Detail::GraphState>& state, std::uint32_t pass, std::uint64_t generation);
    std::weak_ptr<Detail::GraphState> m_state;
    std::uint32_t m_pass = 0;
    std::uint64_t m_generation = 0;
};
} // namespace MiniEngine::RenderGraph
