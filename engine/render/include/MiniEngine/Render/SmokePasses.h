#pragma once
#include <MiniEngine/Rhi/IRhiCommandList.h>

namespace MiniEngine::Render
{
// 本篇只提供 adapter smoke 的实际 pass；图声明/帧同步由 composition executor 负责。
struct SmokeBindings final
{
    Rhi::GraphicsPipelineHandle pipeline;
    std::array<Rhi::ResourceSetHandle, 3> sets{};
    std::array<std::vector<std::uint32_t>, 3> dynamicOffsets;
    std::uint8_t setCount = 0;
};
void ClearPass(Rhi::IRhiCommandList& commands, Rhi::TextureHandle color, Rhi::Extent2D extent,
               const std::array<float, 4>& clear);
void FullscreenPass(Rhi::IRhiCommandList& commands, Rhi::TextureHandle color, Rhi::Extent2D extent,
                    const SmokeBindings& bindings);
void DepthMeshPass(Rhi::IRhiCommandList& commands, Rhi::TextureHandle depth, Rhi::Extent2D extent,
                   const SmokeBindings& bindings, Rhi::BufferView vertices, Rhi::BufferView indices);
} // namespace MiniEngine::Render
