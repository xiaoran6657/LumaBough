#pragma once
#include <MiniEngine/RenderGraph/RenderGraphTypes.h>
#include <MiniEngine/Rhi/IRhiDevice.h>
#include <memory>
#include <span>

namespace MiniEngine::RenderGraph
{
class RenderGraph;
class TransientResourcePool;
class CompiledRenderGraph final
{
  public:
    CompiledRenderGraph(CompiledRenderGraph&&) noexcept = default;
    CompiledRenderGraph& operator=(CompiledRenderGraph&&) noexcept = default;
    CompiledRenderGraph(const CompiledRenderGraph&) = delete;
    CompiledRenderGraph& operator=(const CompiledRenderGraph&) = delete;

    // 只执行 live topo order；Compile 纯 CPU，Execute 才获取 lane physical resources。
    // 此调用拥有 Begin/EndGraphics；外部 owner 拥有 Begin/EndFrame 与 imported 资源。
    // 每个 compiled generation 仅能 Execute 一次。失败进入 Failed，由 owner 恢复 frame。
    void Execute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame);
    void Execute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame, TransientResourcePool& pool);
    // Release/应用入口使用结构化结果；失败永不授权 owner EndFrame/Present。
    // 全裁剪空图成功但 workRecorded/presentReady 为 false；非空完整帧需 live final Present。
    [[nodiscard]] GraphExecutionResult TryExecute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame);
    [[nodiscard]] GraphExecutionResult TryExecute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame,
                                                  TransientResourcePool& pool);
    [[nodiscard]] GraphStatistics Statistics() const;
    [[nodiscard]] GraphStorageStatistics StorageStatistics() const;
    // 以下 view 在 Reset/owner 销毁前有效；不能跨 graph generation 保存。
    [[nodiscard]] std::span<const std::uint32_t> ExecutionOrder() const;
    [[nodiscard]] std::span<const PassPlanInfo> Passes() const;
    [[nodiscard]] std::span<const ResourceVersionInfo> ResourceVersions() const;
    [[nodiscard]] std::span<const std::uint32_t> VersionReaders() const;
    [[nodiscard]] std::span<const DependencyEdge> Dependencies() const;
    [[nodiscard]] std::span<const LifetimeInterval> Lifetimes() const;
    [[nodiscard]] std::span<const PhysicalAllocation> PhysicalAllocations() const;
    [[nodiscard]] std::span<const CompileStage> CompilationStages() const;
    [[nodiscard]] GraphDumpBundle Dumps(const GraphDumpContext& context = {}) const;
    [[nodiscard]] std::span<const LogicalTransition> Transitions() const;

  private:
    friend class RenderGraph;
    CompiledRenderGraph(const std::shared_ptr<Detail::GraphState>& state, std::uint64_t generation);
    std::shared_ptr<Detail::GraphState> Lock() const;
    std::shared_ptr<Detail::GraphState> m_state;
    std::uint64_t m_generation = 0;
};
struct GraphCompileResult final
{
    std::optional<CompiledRenderGraph> plan;
    std::optional<GraphDiagnostic> error;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return plan.has_value();
    }
};
} // namespace MiniEngine::RenderGraph
