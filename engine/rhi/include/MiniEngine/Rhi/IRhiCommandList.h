#pragma once

// M6-01 公共契约草稿；运行时实现与验证在后续篇目完成。
// 所有调用限渲染线程；句柄不拥有对象，span/string_view 仅在调用期间借用。

#include <MiniEngine/Rhi/RhiDescriptors.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace MiniEngine::Rhi
{
struct ColorAttachment final
{
    TextureHandle texture;
    LoadOp load = LoadOp::DontCare;
    StoreOp store = StoreOp::Store;
    std::array<float, 4> clearColor{};
};

struct DepthAttachment final
{
    TextureHandle texture;
    LoadOp load = LoadOp::DontCare;
    StoreOp store = StoreOp::DontCare;
    float clearDepth = 1.0F;
    std::uint8_t clearStencil = 0;
};

struct RenderingInfo final
{
    std::span<const ColorAttachment> colors;
    const DepthAttachment* depth = nullptr;
    Extent2D extent;
};

struct Viewport final
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float minDepth = 0.0F;
    float maxDepth = 1.0F;
};

struct Rect final
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class IRhiCommandList
{
  public:
    virtual ~IRhiCommandList() = default;

    virtual void BeginLabel(std::string_view name) = 0;
    virtual void EndLabel() = 0;
    virtual void BeginRendering(const RenderingInfo& info) = 0;
    virtual void EndRendering() = 0;
    virtual void SetPipeline(GraphicsPipelineHandle pipeline) = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const Rect& scissor) = 0;
    virtual void BindVertexBuffer(std::uint32_t slot, const BufferView& view, std::uint32_t stride) = 0;
    virtual void BindIndexBuffer(const BufferView& view, IndexType type) = 0;
    virtual void BindResourceSet(std::uint32_t set, ResourceSetHandle resources,
                                 std::span<const std::uint32_t> dynamicOffsets) = 0;
    virtual void Draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                      std::uint32_t firstInstance) = 0;
    // 当前 M6 profile 仅接受 vertexOffset=0；mesh owner 保证 index 值落在顶点 view 内。
    // validator 检查 index view 区间，不隐式回读或猜测 GPU index 内容。
    virtual void DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                             std::int32_t vertexOffset, std::uint32_t firstInstance) = 0;
    virtual void CopyBuffer(const BufferView& source, const BufferView& destination, std::uint64_t size) = 0;
    // RGBA8 全帧读回：readback 缓冲必须正好是 extent.width*extent.height*4 字节。
    // 后端自行承担 256B 行对齐 staging；若图把该缓冲声明为 Full coverage，
    // 缓冲比紧凑尺寸偏大时执行期定义性校验会失败（不能按对齐 pitch 补大）。
    virtual void CopyTextureForReadback(TextureHandle source, BufferHandle readback, Extent2D extent) = 0;
    virtual void WriteTimestamp(TimestampQueryHandle query) = 0;
};
} // namespace MiniEngine::Rhi
