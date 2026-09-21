#pragma once
#include <MiniEngine/RenderGraph/RenderGraphHandle.h>
#include <MiniEngine/Rhi/RhiDescriptors.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniEngine::RenderGraph
{
inline constexpr std::uint32_t kInvalidGraphIndex = std::numeric_limits<std::uint32_t>::max();
enum class ResourceKind : std::uint8_t
{
    Texture,
    Buffer
};
enum class ContentState : std::uint8_t
{
    Undefined,
    Defined
};
enum class GraphPhase : std::uint8_t
{
    Building,
    Setup,
    Compiling,
    Compiled,
    Executing,
    Executed,
    Failed
};
enum class CompileStage : std::uint8_t
{
    ValidateDeclarations,
    CreateNodes,
    CreateDependencies,
    FindRoots,
    Cull,
    TopologicalSort,
    Lifetimes,
    PhysicalSlots,
    AccessTransitions,
    EmitPlan
};
enum class Hazard : std::uint8_t
{
    ReadAfterWrite,
    WriteAfterRead,
    WriteAfterWrite
};
enum class GraphErrorCode : std::uint8_t
{
    InvalidDeclaration,
    StaleHandle,
    UndefinedContent,
    PhaseViolation,
    Cycle,
    InvalidImport,
    ExecutionFailure,
    MissingPresent
};
struct DependencyEdge final
{
    std::uint32_t from = kInvalidGraphIndex;
    std::uint32_t to = kInvalidGraphIndex;
    std::uint32_t resource = kInvalidGraphIndex;
    std::uint32_t version = kInvalidGraphIndex;
    Hazard hazard = Hazard::ReadAfterWrite;
    // RAW/保留旧内容的 WAW 贡献输出；WAR/完整覆盖 WAW 只约束同时存活的 pass。
    bool contributesToOutput = true;
    auto operator<=>(const DependencyEdge&) const = default;
};
struct GraphDiagnostic final
{
    GraphErrorCode code = GraphErrorCode::InvalidDeclaration;
    GraphPhase phase = GraphPhase::Building;
    CompileStage stage = CompileStage::ValidateDeclarations;
    std::uint32_t pass = kInvalidGraphIndex;
    std::uint32_t resource = kInvalidGraphIndex;
    std::uint32_t version = kInvalidGraphIndex;
    std::string passName;
    std::string resourceName;
    std::string message;
    std::vector<DependencyEdge> cycle;
};
class GraphCompileError : public std::logic_error
{
  public:
    explicit GraphCompileError(const std::string& message) : std::logic_error(message)
    {
        m_diagnostic.message = message;
    }
    explicit GraphCompileError(GraphDiagnostic diagnostic)
        : std::logic_error(diagnostic.message), m_diagnostic(std::move(diagnostic))
    {
    }
    [[nodiscard]] const GraphDiagnostic& Diagnostic() const noexcept
    {
        return m_diagnostic;
    }

  private:
    GraphDiagnostic m_diagnostic;
};
class UndefinedContentError final : public GraphCompileError
{
  public:
    using GraphCompileError::GraphCompileError;
};
class StaleGraphHandleError final : public GraphCompileError
{
  public:
    using GraphCompileError::GraphCompileError;
};
class GraphPhaseError final : public GraphCompileError
{
  public:
    using GraphCompileError::GraphCompileError;
};
class GraphCycleError final : public GraphCompileError
{
  public:
    using GraphCompileError::GraphCompileError;
};

// Full 仅用于完整 copy；附件由 Clear/Load/Store 决定，执行仍核对真实内容账本。
enum class WriteCoverage : std::uint8_t
{
    Preserve,
    Full
};
struct TextureImport final
{
    Rhi::TextureHandle physical;
    Rhi::TextureDesc descriptor;
    Rhi::ResourceAccess initialAccess = Rhi::ResourceAccess::None;
    Rhi::ResourceAccess finalAccess = Rhi::ResourceAccess::None;
    ContentState initialContent = ContentState::Undefined;
    std::string owner;
    std::string definitionSource;
};
struct BufferImport final
{
    Rhi::BufferHandle physical;
    Rhi::BufferDesc descriptor;
    Rhi::ResourceAccess initialAccess = Rhi::ResourceAccess::None;
    Rhi::ResourceAccess finalAccess = Rhi::ResourceAccess::None;
    ContentState initialContent = ContentState::Undefined;
    std::string owner;
    std::string definitionSource;
};
struct ResourceUse final
{
    ResourceKind kind = ResourceKind::Texture;
    std::uint32_t resource = 0;
    std::uint32_t version = 0;
    std::uint32_t previousVersion = 0;
    Rhi::ResourceAccess access = Rhi::ResourceAccess::None;
    Rhi::ShaderStage stages = Rhi::ShaderStage::Pixel;
    bool write = false;
    WriteCoverage coverage = WriteCoverage::Preserve;
};
struct LogicalTransition final
{
    std::uint32_t pass = 0; // 原声明 ID；final 批次固定 pass == declaredPasses。
    std::uint32_t resource = 0;
    ResourceKind kind = ResourceKind::Texture;
    Rhi::ResourceAccess before = Rhi::ResourceAccess::None;
    Rhi::ResourceAccess after = Rhi::ResourceAccess::None;
    Rhi::ShaderStage stages = Rhi::ShaderStage::Pixel;
    bool final = false;
    std::uint32_t executionOrder = kInvalidGraphIndex;
    std::uint32_t physicalSlot = kInvalidGraphIndex;
    // 首次使用/owner 切换仅清空内容；before 保留 physical slot 的相邻 access。
    bool resetContent = false;
};
struct LifetimeInterval final
{
    std::uint32_t resource = 0;
    std::uint32_t firstUse = kInvalidGraphIndex;
    std::uint32_t lastUse = 0;
    std::uint32_t physicalSlot = kInvalidGraphIndex;
    bool imported = false;
    // live uses 的 union，位含义按 ResourceKind 对应 TextureUsage/BufferUsage。
    std::uint32_t usageUnion = 0;
};
struct PassPlanInfo final
{
    std::uint32_t declaration = 0;
    std::uint32_t executionOrder = kInvalidGraphIndex;
    bool live = false;
    // 固定原因代码；名称/reason 由声明节点持有并在 dump 中输出。
    std::string_view culledReason = "not_reachable_from_output_roots";
};
struct ResourceVersionInfo final
{
    std::uint32_t resource = 0;
    std::uint32_t version = 0;
    std::uint32_t producer = kInvalidGraphIndex;
    std::uint32_t readersBegin = 0;
    std::uint32_t readersCount = 0;
    bool importedProducer = false;
    bool live = false;
    ContentState content = ContentState::Undefined;
};
struct PhysicalAllocation final
{
    std::uint32_t slot = 0;
    std::uint32_t resource = 0;
    ResourceKind kind = ResourceKind::Texture;
    bool imported = false;
};
struct GraphStatistics final
{
    std::uint32_t declaredPasses = 0;
    std::uint32_t virtualResources = 0;
    std::uint32_t roots = 0;
    std::uint32_t logicalTransitions = 0;
    std::uint32_t livePasses = 0;
    std::uint32_t culledPasses = 0;
    std::uint32_t resourceVersions = 0;
    std::uint32_t dependencyEdges = 0;
    std::uint32_t liveResources = 0;
    std::uint32_t physicalResources = 0;
    std::uint32_t physicalTransients = 0;
    std::uint64_t planHash = 0;
};
struct GraphCapacity final
{
    std::uint32_t passes = 0;
    std::uint32_t resources = 0;
};
// 容量仅用于内存诊断，不参与 canonical hash；完整 allocation/peak 由外部计数器测量。
struct GraphStorageStatistics final
{
    std::size_t declarationBytes = 0;
    std::size_t planBytes = 0;
    std::size_t edgeCapacity = 0;
    std::size_t versionCapacity = 0;
    std::size_t readerCapacity = 0;
};
struct GraphDumpContext final
{
    std::uint64_t frame = 0;
    std::string build;
    std::string commit;
};
struct GraphDumpBundle final
{
    std::string frameGraphJson;
    std::string dot;
    std::string accessPlanJson;
    std::string transientPlanJson;
};
struct GraphExecutionResult final
{
    bool succeeded = false;
    bool workRecorded = false;
    bool presentReady = false;
    std::optional<GraphDiagnostic> error;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return succeeded;
    }
};
[[nodiscard]] std::string_view ToString(CompileStage stage);
[[nodiscard]] std::string_view ToString(Hazard hazard);
[[nodiscard]] std::string_view ToString(GraphErrorCode code);
[[nodiscard]] std::string ToDiagnosticJson(const GraphDiagnostic& diagnostic);
} // namespace MiniEngine::RenderGraph
