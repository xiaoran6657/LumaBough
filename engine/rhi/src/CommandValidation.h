#pragma once
#include "DeviceLifetime.h"

namespace MiniEngine::Rhi
{
// M6-04 绑定/draw 前置验证器；不是 M6-05 的完整命令列表或第三个图形后端。
// adapter 先通过验证，再调用 native。此对象仅属于构造时的当前 FrameToken。
class CommandValidation final
{
  public:
    CommandValidation(DeviceLifetime& device, FrameToken frame);
    void BeginRendering(const RenderingInfo& rendering);
    void EndRendering();
    void SetPipeline(GraphicsPipelineHandle pipeline);
    void BindResourceSet(std::uint32_t index, ResourceSetHandle set, std::span<const std::uint32_t> dynamicOffsets);
    void BindVertexBuffer(std::uint32_t slot, BufferView view, std::uint32_t stride);
    void BindIndexBuffer(BufferView view, IndexType type);
    void Draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1, std::uint32_t firstVertex = 0,
              std::uint32_t firstInstance = 0);
    // 当前 M6 profile 仅接受 vertexOffset=0；mesh owner 保证 index 值落在顶点 view 内。
    // validator 检查 index view 区间，不隐式回读或猜测 GPU index 内容。
    void DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1, std::uint32_t firstIndex = 0,
                     std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0);
    bool HasSet(std::uint32_t index) const;

  private:
    struct BoundSet
    {
        ResourceSetHandle handle;
        std::vector<std::uint32_t> offsets;
    };
    struct VertexBuffer
    {
        BufferView view;
        std::uint32_t stride;
    };
    void ValidateDraw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex,
                      std::uint32_t firstInstance, bool indexed);
    void ValidateView(BufferView view, BufferUsage usage) const;
    [[noreturn]] static void Fail(const char* message);
    DeviceLifetime& m_device;
    FrameToken m_frame;
    GraphicsPipelineHandle m_pipeline;
    std::string m_layoutKey;
    std::array<std::optional<BoundSet>, 3> m_sets;
    std::array<std::optional<VertexBuffer>, 16> m_vertices;
    std::optional<BufferView> m_indices;
    IndexType m_indexType = IndexType::UInt16;
    bool m_rendering = false;
    std::vector<TextureHandle> m_attachments;
    std::array<Format, 4> m_colors{};
    std::uint8_t m_colorCount = 0;
    Format m_depth = Format::Unknown;
};
} // namespace MiniEngine::Rhi
