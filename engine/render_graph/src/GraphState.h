#pragma once
#include "DependencyAlgorithms.h"
#include <MiniEngine/RenderGraph/RenderGraph.h>
#include <MiniEngine/Rhi/RhiValidation.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <type_traits>
#include <vector>

namespace MiniEngine::RenderGraph::Detail
{
inline constexpr std::uint32_t kNoPass = std::numeric_limits<std::uint32_t>::max();
struct VersionRecord final
{
    std::uint32_t writer = kNoPass;
    ContentState content = ContentState::Undefined;
};
struct ResourceRecord final
{
    std::string name;
    ResourceKind kind = ResourceKind::Texture;
    bool imported = false;
    bool rooted = false;
    Rhi::TextureHandle texture;
    Rhi::BufferHandle buffer;
    Rhi::TextureDesc textureDesc;
    Rhi::BufferDesc bufferDesc;
    Rhi::ResourceAccess initialAccess = Rhi::ResourceAccess::None;
    Rhi::ResourceAccess finalAccess = Rhi::ResourceAccess::None;
    ContentState initialContent = ContentState::Undefined;
    std::string owner;
    std::string definitionSource;
    std::vector<VersionRecord> versions{{}};
};
struct AttachmentRecord final
{
    std::uint32_t resource = 0;
    std::uint32_t version = 0;
    Rhi::LoadOp load = Rhi::LoadOp::DontCare;
    Rhi::StoreOp store = Rhi::StoreOp::Store;
    bool depth = false;
    std::array<float, 4> clearColor{};
    float clearDepth = 1.0F;
    std::uint8_t clearStencil = 0;
};
struct PassNode final
{
    std::string name;
    std::any data;
    std::function<void(const std::any&, const RgResources&, Rhi::IRhiCommandList&)> invoke;
    std::vector<ResourceUse> uses;
    std::vector<AttachmentRecord> attachments;
    std::string sideEffect;
};
struct CallbackResolution final
{
    ResourceKind kind = ResourceKind::Texture;
    std::uint32_t resource = kNoPass;
    std::uint32_t version = kNoPass;
    Rhi::TextureHandle texture;
    Rhi::BufferHandle buffer;
};
struct RootRecord final
{
    std::uint32_t resource = 0;
    std::uint32_t version = 0;
    bool present = false;
};
struct GraphState final
{
    explicit GraphState(std::uint64_t identity) : owner(identity)
    {
    }
    const std::uint64_t owner;
    std::uint64_t generation = 1;
    bool alive = true;
    bool workRecorded = false;
    bool validatingImports = false;
    GraphPhase phase = GraphPhase::Building;
    std::uint32_t activePass = kNoPass;
    std::vector<ResourceRecord> resources;
    std::vector<std::unique_ptr<PassNode>> passes;
    std::vector<RootRecord> roots;
    std::vector<LogicalTransition> transitions;
    GraphStatistics statistics;
    std::vector<CompileStage> compilationStages;
    std::vector<PassPlanInfo> passPlan;
    std::vector<ResourceVersionInfo> versionPlan;
    std::vector<std::uint32_t> versionOffsets, versionReaders, executionOrder, resourceSlots;
    std::vector<std::uint8_t> livePasses, liveResources;
    std::vector<DependencyEdge> dependencies;
    DependencyIndex dependencyIndex;
    std::vector<LifetimeInterval> lifetimes;
    std::vector<PhysicalAllocation> allocations;
    std::string canonicalPlan;
    CompileStage currentStage = CompileStage::ValidateDeclarations;
    std::uint32_t contextPass = kNoPass, contextResource = kNoPass, contextVersion = kNoPass;
    std::optional<GraphDiagnostic> lastError;
    std::string contextResourceName;
    // 仅 Execute callback 期间有效；RgResources 用它把已解析的 graph 资源绑定回当前声明。
    Rhi::IRhiDevice* executingDevice = nullptr;
    Rhi::FrameToken executingFrame;
    std::vector<CallbackResolution> callbackResolutions;
    // 只有 Execute 阶段填入；imported 借用，transient 句柄借自 pool，同槽多 owner 分时共用。
    std::vector<Rhi::TextureHandle> textures;
    std::vector<Rhi::BufferHandle> buffers;

    void RequirePhase(GraphPhase expected) const;
    template <class Handle> const ResourceRecord& Check(Handle handle, bool latest = true) const
    {
        if (!alive || !handle || handle.Owner() != owner || handle.Generation() != generation ||
            handle.Resource() >= resources.size())
            throw StaleGraphHandleError("graph handle owner/generation/resource is stale");
        const auto& resource = resources[handle.Resource()];
        constexpr auto kind = std::is_same_v<Handle, RgTexture> ? ResourceKind::Texture : ResourceKind::Buffer;
        if (resource.kind != kind || handle.Version() >= resource.versions.size() ||
            (latest && handle.Version() + std::uint64_t{1} != resource.versions.size()))
            throw StaleGraphHandleError("resource=" + resource.name + ": graph kind/version is stale");
        return resource;
    }
    template <class Handle> Handle Issue(std::uint32_t resource, std::uint32_t version) const
    {
        return Handle(owner, generation, resource, version);
    }
    std::uint32_t AddResource(ResourceRecord resource);
    RgTexture AddTexture(std::string_view name, const Rhi::TextureDesc& desc, const TextureImport* imported = nullptr);
    RgBuffer AddBuffer(std::string_view name, const Rhi::BufferDesc& desc, const BufferImport* imported = nullptr);
    template <class Handle> void Root(Handle handle, bool present)
    {
        contextResourceName.clear();
        contextResource = handle.Resource();
        contextVersion = handle.Version();
        const auto& checked = Check(handle);
        if (!checked.imported)
            throw GraphCompileError("resource=" + checked.name +
                                    ": transient Export/Present cannot transfer ownership");
        if (present && (checked.kind != ResourceKind::Texture || checked.finalAccess != Rhi::ResourceAccess::Present))
            throw GraphCompileError("resource=" + checked.name +
                                    ": Present requires imported texture with final Present");
        if (checked.rooted)
            throw GraphCompileError("resource=" + checked.name + ": duplicate output root");
        roots.push_back({handle.Resource(), handle.Version(), present});
        resources[handle.Resource()].rooted = true;
    }
    template <class Handle>
    Handle Use(Handle handle, Rhi::ResourceAccess access, bool write, WriteCoverage coverage, Rhi::ShaderStage stages)
    {
        contextResourceName.clear();
        contextPass = activePass;
        contextResource = handle.Resource();
        contextVersion = handle.Version();
        const auto& resource = Check(handle);
        ValidateAccess(resource, access, false);
        if (access == Rhi::ResourceAccess::Present)
            throw GraphCompileError("Present is an output root, not a pass Read/Write access");
        if (IsWrite(access) != write)
            throw GraphCompileError("resource=" + resource.name + ": Read/Write access direction mismatch");
        if (coverage != WriteCoverage::Preserve && coverage != WriteCoverage::Full)
            throw GraphCompileError("unknown write coverage");
        if (coverage == WriteCoverage::Full && (!write || access != Rhi::ResourceAccess::CopyDestination))
            throw GraphCompileError("Full coverage is restricted to complete copy writes; attachments require Clear");
        ValidateStages(stages);
        auto& uses = passes.at(activePass)->uses;
        const auto existing = std::find_if(uses.begin(), uses.end(),
                                           [&](const ResourceUse& use) { return use.resource == handle.Resource(); });
        if (existing != uses.end())
        {
            if (!write && !existing->write && existing->version == handle.Version() && existing->access == access)
            {
                existing->stages = existing->stages | stages;
                return handle;
            }
            throw GraphCompileError("pass=" + passes[activePass]->name + " resource=" + resource.name +
                                    ": conflicting uses in the same pass");
        }
        if (write && resource.rooted)
            throw StaleGraphHandleError("resource=" + resource.name + ": output root already seals the final version");
        auto version = handle.Version();
        if (write)
        {
            if (version == std::numeric_limits<std::uint32_t>::max())
                throw GraphCompileError("resource version exhausted");
            ++version;
            resources[handle.Resource()].versions.push_back({activePass, ContentState::Undefined});
        }
        uses.push_back({resource.kind, handle.Resource(), version, handle.Version(), access, stages, write, coverage});
        return Issue<Handle>(handle.Resource(), version);
    }
    void Attach(RgTexture handle, Rhi::LoadOp load, Rhi::StoreOp store, bool depth, std::array<float, 4> color,
                float clearDepth, std::uint8_t stencil);
    void CompileDeclarations();
    void ClearPlan();
    void SetStage(CompileStage stage);
    GraphDiagnostic CaptureDiagnostic(std::exception_ptr error);
    void ValidateDeclarations();
    void CreateNodes();
    void CreateDependencies();
    void FindRoots();
    void CullPasses();
    void SortPasses();
    void AnalyzeLifetimes();
    void AllocateSlots();
    void BuildAccessPlan();
    void EmitPlan();
    Rhi::ResourceSetHandle CreateFrameResourceSet(std::uint32_t pass, const Rhi::ResourceSetDesc& desc);
    void ValidateUseContent(std::uint32_t pass, const ResourceUse& use, ContentState& content) const;
    bool PreservesPrevious(std::uint32_t pass, const ResourceUse& use) const;
    GraphStorageStatistics StorageStatistics() const;
    GraphDumpBundle Dumps(const GraphDumpContext& context) const;
    static bool IsWrite(Rhi::ResourceAccess access);
    static bool IsRead(Rhi::ResourceAccess access);
    static void ValidateStages(Rhi::ShaderStage stages);
    static void ValidateAccess(const ResourceRecord& resource, Rhi::ResourceAccess access, bool allowNone);
    static std::string Name(std::string_view name, const char* role);
};
} // namespace MiniEngine::RenderGraph::Detail
