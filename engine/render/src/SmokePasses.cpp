#include <MiniEngine/Render/SmokePasses.h>
namespace MiniEngine::Render
{
namespace
{
void Bind(Rhi::IRhiCommandList& commands, Rhi::Extent2D extent, const SmokeBindings& bindings)
{
    commands.SetPipeline(bindings.pipeline);
    commands.SetViewport({0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1});
    commands.SetScissor({0, 0, extent.width, extent.height});
    for (std::uint32_t i = 0; i < bindings.setCount; ++i)
        commands.BindResourceSet(i, bindings.sets[i], bindings.dynamicOffsets[i]);
}
} // namespace
void ClearPass(Rhi::IRhiCommandList& commands, Rhi::TextureHandle color, Rhi::Extent2D extent,
               const std::array<float, 4>& clear)
{
    commands.BeginLabel("M6.Clear");
    const Rhi::ColorAttachment attachment{color, Rhi::LoadOp::Clear, Rhi::StoreOp::Store, clear};
    commands.BeginRendering({std::span(&attachment, 1), nullptr, extent});
    commands.EndRendering();
    commands.EndLabel();
}
void FullscreenPass(Rhi::IRhiCommandList& commands, Rhi::TextureHandle color, Rhi::Extent2D extent,
                    const SmokeBindings& bindings)
{
    commands.BeginLabel("M6.ToneMap");
    const Rhi::ColorAttachment attachment{color, Rhi::LoadOp::Clear, Rhi::StoreOp::Store, {0, 0, 0, 1}};
    commands.BeginRendering({std::span(&attachment, 1), nullptr, extent});
    Bind(commands, extent, bindings);
    commands.Draw(3, 1, 0, 0);
    commands.EndRendering();
    commands.EndLabel();
}
void DepthMeshPass(Rhi::IRhiCommandList& commands, Rhi::TextureHandle depth, Rhi::Extent2D extent,
                   const SmokeBindings& bindings, Rhi::BufferView vertices, Rhi::BufferView indices)
{
    commands.BeginLabel("M6.DepthMesh");
    const Rhi::DepthAttachment attachment{depth, Rhi::LoadOp::Clear, Rhi::StoreOp::Store, 1.0F, 0};
    commands.BeginRendering({{}, &attachment, extent});
    Bind(commands, extent, bindings);
    commands.BindVertexBuffer(0, vertices, 12);
    commands.BindIndexBuffer(indices, Rhi::IndexType::UInt16);
    commands.DrawIndexed(3, 1, 0, 0, 0);
    commands.EndRendering();
    commands.EndLabel();
}
} // namespace MiniEngine::Render
