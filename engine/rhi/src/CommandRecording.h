#pragma once

#include "CommandValidation.h"
#include <functional>
#include <map>
#include <set>

namespace MiniEngine::Rhi
{
// 设备所有的 whole-texture / buffer byte-range 内容与访问状态；跨帧保存。
// Graph 的编译期定义性证明不能替代这里的执行期校验。
struct ResourceContent final
{
    ResourceAccess access = ResourceAccess::None;
    bool defined = false;
    std::uint64_t declaredFrame = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    bool Contains(std::uint64_t offset, std::uint64_t size) const;
    void Define(std::uint64_t offset, std::uint64_t size);
};
using CommandResourceStates = std::map<ResourceIdentity, ResourceContent>;

// 验证后才送往 backend/trace；不包含任何 native 对象。
struct CommandEvent final
{
    std::string operation;
    std::vector<ResourceIdentity> resources;
    std::vector<std::uint64_t> integers;
    std::vector<float> scalars;
    std::string text;
};

// 单个 frame 专属；关闭后不 reset 为下一帧，避免旧 C++ 引用获得新 token 的权利。
// 仅 adapter 和 graph executor 可见此实现，pass 只接收 IRhiCommandList。
class CommandRecording final : public IRhiCommandList, public IRhiGraphCommandSink
{
  public:
    enum class State
    {
        Idle,
        Recording,
        Rendering,
        Closed
    };
    using ValidateImport = std::function<void(ResourceIdentity)>;
    using DeclareImports = std::function<void(std::span<const ResourceIdentity>)>;
    using Emit = std::function<void(const CommandEvent&)>;
    CommandRecording(DeviceLifetime& device, FrameToken frame, CommandResourceStates& resources, Emit emit,
                     ValidateImport imports, DeclareImports declareImports = {});
    void Close();
    bool HasActiveAttachment(ResourceIdentity handle) const;
    State CurrentState() const
    {
        return m_state;
    }
    std::uint64_t CommandIndex() const
    {
        return m_commandIndex;
    }
    void BeginLabel(std::string_view name) override;
    void EndLabel() override;
    void BeginRendering(const RenderingInfo& info) override;
    void EndRendering() override;
    void SetPipeline(GraphicsPipelineHandle pipeline) override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissor(const Rect& scissor) override;
    void BindVertexBuffer(std::uint32_t slot, const BufferView& view, std::uint32_t stride) override;
    void BindIndexBuffer(const BufferView& view, IndexType type) override;
    void BindResourceSet(std::uint32_t set, ResourceSetHandle resources,
                         std::span<const std::uint32_t> offsets) override;
    void Draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex,
              std::uint32_t firstInstance) override;
    void DrawIndexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t firstIndex,
                     std::int32_t vertexOffset, std::uint32_t firstInstance) override;
    void CopyBuffer(const BufferView& source, const BufferView& destination, std::uint64_t size) override;
    void CopyTextureForReadback(TextureHandle source, BufferHandle readback, Extent2D extent) override;
    void WriteTimestamp(TimestampQueryHandle query) override;
    void ImportResources(std::span<const GraphResourceImport> resources) override;
    void ResetTransientContents(std::span<const GraphResourceImport> resources) override;
    void ApplyTransitions(std::span<const AccessTransition> transitions) override;

  private:
    // 失败保留底层 error code/handle 名，追加当前 frame/pass/command 上下文。
    template <class Function> void Run(CommandEvent event, Function validate)
    {
        ++m_commandIndex;
        try
        {
            m_device.ValidateFrameToken(m_frame);
            if (m_state != State::Recording && m_state != State::Rendering)
                Fail("command list is closed");
            for (const auto& h : event.resources)
                m_device.ValidateAlive(h);
            validate();
        }
        catch (const RhiException& exception)
        {
            auto error = exception.Error();
            error.operation = event.operation;
            if (error.backend.empty())
                error.backend = std::string(ToString(m_device.Backend()));
            if (error.objectName.empty() && !event.resources.empty())
            {
                try
                {
                    error.objectName = m_device.DebugName(event.resources.front());
                }
                catch (const RhiException&)
                {
                }
            }
            error.message =
                "frame=" + std::to_string(m_frame.serial) + " pass=" + (m_labels.empty() ? "<none>" : m_labels.back()) +
                " command=" + std::to_string(m_commandIndex) + " handle=" + error.objectName + ": " + error.message;
            throw RhiValidationError(std::move(error));
        }
        m_emit(event);
    }
    [[noreturn]] static void Fail(const char* message);
    [[noreturn]] void FailResource(ResourceIdentity handle, const char* message) const;
    void OutsideRendering() const;
    void RequireAccess(ResourceIdentity handle, ResourceAccess access) const;
    void RequireDefined(TextureHandle texture) const;
    void RequireDefined(BufferView view) const;
    void ValidateDrawState();
    void ValidateTransition(const AccessTransition& transition, CommandResourceStates& candidate) const;
    DeviceLifetime& m_device;
    const FrameToken m_frame;
    CommandResourceStates& m_resources;
    Emit m_emit;
    ValidateImport m_validateImport;
    DeclareImports m_declareImports;
    CommandValidation m_bindings;
    State m_state = State::Idle;
    std::uint64_t m_commandIndex = 0;
    std::vector<std::string> m_labels;
    std::optional<Viewport> m_viewport;
    std::optional<Rect> m_scissor;
    std::optional<GraphicsPipelineHandle> m_pipeline;
    std::array<std::optional<std::pair<ResourceSetHandle, std::vector<std::uint32_t>>>, 3> m_sets;
    std::map<std::uint32_t, BufferView> m_vertices;
    std::optional<BufferView> m_indices;
    Extent2D m_extent;
    std::vector<std::pair<TextureHandle, StoreOp>> m_attachments;
    std::optional<TextureHandle> m_depth;
    std::set<TimestampQueryHandle> m_writtenQueries;
};
} // namespace MiniEngine::Rhi
