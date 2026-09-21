#include "GraphJson.h"
#include "GraphState.h"

namespace MiniEngine::RenderGraph::Detail
{
namespace
{
std::string_view AccessName(Rhi::ResourceAccess access)
{
    using enum Rhi::ResourceAccess;
    switch (access)
    {
    case None:
        return "None";
    case SampledRead:
        return "SampledRead";
    case ColorWrite:
        return "ColorWrite";
    case DepthRead:
        return "DepthRead";
    case DepthWrite:
        return "DepthWrite";
    case CopySource:
        return "CopySource";
    case CopyDestination:
        return "CopyDestination";
    case Present:
        return "Present";
    case VertexRead:
        return "VertexRead";
    case IndexRead:
        return "IndexRead";
    case UniformRead:
        return "UniformRead";
    }
    return "unknown";
}
std::string_view KindName(ResourceKind kind)
{
    return kind == ResourceKind::Texture ? "texture" : "buffer";
}
std::string_view ContentName(ContentState content)
{
    return content == ContentState::Defined ? "Defined" : "Undefined";
}
void Header(JsonText& out, const GraphDumpContext& context, std::uint64_t hash, std::string_view kind)
{
    out.Raw("{\"schemaVersion\":1,\"kind\":");
    out.String(kind);
    out.Raw(",\"frame\":");
    out.Number(context.frame);
    out.Raw(",\"build\":");
    out.String(context.build);
    out.Raw(",\"commit\":");
    out.String(context.commit);
    out.Raw(",\"planHash\":");
    out.String(HexHash(hash));
}
void WriteStatistics(JsonText& out, const GraphStatistics& stats)
{
    out.Raw("{\"declaredPasses\":");
    out.Number(stats.declaredPasses);
    out.Raw(",\"livePasses\":");
    out.Number(stats.livePasses);
    out.Raw(",\"culledPasses\":");
    out.Number(stats.culledPasses);
    out.Raw(",\"virtualResources\":");
    out.Number(stats.virtualResources);
    out.Raw(",\"liveResources\":");
    out.Number(stats.liveResources);
    out.Raw(",\"resourceVersions\":");
    out.Number(stats.resourceVersions);
    out.Raw(",\"dependencyEdges\":");
    out.Number(stats.dependencyEdges);
    out.Raw(",\"physicalResources\":");
    out.Number(stats.physicalResources);
    out.Raw(",\"physicalTransients\":");
    out.Number(stats.physicalTransients);
    out.Raw(",\"roots\":");
    out.Number(stats.roots);
    out.Raw(",\"logicalTransitions\":");
    out.Number(stats.logicalTransitions);
    out.Raw("}");
}
void WriteTransitions(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& transition : state.transitions)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"pass\":");
        out.Number(transition.pass);
        out.Raw(",\"executionOrder\":");
        out.Index(transition.executionOrder);
        out.Raw(",\"resource\":");
        out.Number(transition.resource);
        out.Raw(",\"physicalSlot\":");
        out.Index(transition.physicalSlot);
        out.Raw(",\"kind\":");
        out.String(KindName(transition.kind));
        out.Raw(",\"before\":");
        out.String(AccessName(transition.before));
        out.Raw(",\"resetContent\":");
        out.Bool(transition.resetContent);
        out.Raw(",\"after\":");
        out.String(AccessName(transition.after));
        out.Raw(",\"stages\":");
        out.Number(static_cast<unsigned>(transition.stages));
        out.Raw(",\"final\":");
        out.Bool(transition.final);
        out.Raw("}");
    }
    out.Raw("]");
}
void WriteAllocation(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& allocation : state.allocations)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"slot\":");
        out.Number(allocation.slot);
        out.Raw(",\"resource\":");
        out.Number(allocation.resource);
        out.Raw(",\"kind\":");
        out.String(KindName(allocation.kind));
        out.Raw(",\"imported\":");
        out.Bool(allocation.imported);
        out.Raw("}");
    }
    out.Raw("]");
}
void WriteLifetimes(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& lifetime : state.lifetimes)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"resource\":");
        out.Number(lifetime.resource);
        out.Raw(",\"firstUse\":");
        out.Index(lifetime.firstUse);
        out.Raw(",\"lastUse\":");
        out.Index(lifetime.lastUse);
        out.Raw(",\"physicalSlot\":");
        out.Index(lifetime.physicalSlot);
        out.Raw(",\"imported\":");
        out.Bool(lifetime.imported);
        out.Raw(",\"usageUnion\":");
        out.Number(lifetime.usageUnion);
        out.Raw("}");
    }
    out.Raw("]");
}
void WritePasses(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& info : state.passPlan)
    {
        if (!first)
            out.Raw(",");
        first = false;
        const auto& pass = *state.passes[info.declaration];
        out.Raw("{\"declaration\":");
        out.Number(info.declaration);
        out.Raw(",\"name\":");
        out.String(pass.name);
        out.Raw(",\"executionOrder\":");
        out.Index(info.executionOrder);
        out.Raw(",\"live\":");
        out.Bool(info.live);
        out.Raw(",\"culledReason\":");
        out.String(info.culledReason);
        out.Raw(",\"sideEffect\":");
        out.String(pass.sideEffect);
        out.Raw(",\"uses\":[");
        bool firstUse = true;
        for (const auto& use : pass.uses)
        {
            if (!firstUse)
                out.Raw(",");
            firstUse = false;
            out.Raw("{\"resource\":");
            out.Number(use.resource);
            out.Raw(",\"version\":");
            out.Number(use.version);
            out.Raw(",\"previousVersion\":");
            out.Number(use.previousVersion);
            out.Raw(",\"access\":");
            out.String(AccessName(use.access));
            out.Raw(",\"stages\":");
            out.Number(static_cast<unsigned>(use.stages));
            out.Raw(",\"write\":");
            out.Bool(use.write);
            out.Raw(",\"coverage\":");
            out.String(use.coverage == WriteCoverage::Full ? "Full" : "Preserve");
            out.Raw("}");
        }
        out.Raw("],\"attachments\":[");
        bool firstAttachment = true;
        for (const auto& attachment : pass.attachments)
        {
            if (!firstAttachment)
                out.Raw(",");
            firstAttachment = false;
            out.Raw("{\"resource\":");
            out.Number(attachment.resource);
            out.Raw(",\"version\":");
            out.Number(attachment.version);
            out.Raw(",\"depth\":");
            out.Bool(attachment.depth);
            out.Raw(",\"load\":");
            out.String(attachment.load == Rhi::LoadOp::Clear  ? "Clear"
                       : attachment.load == Rhi::LoadOp::Load ? "Load"
                                                              : "DontCare");
            out.Raw(",\"store\":");
            out.String(attachment.store == Rhi::StoreOp::Store ? "Store" : "DontCare");
            out.Raw(",\"clearColor\":[");
            for (std::size_t i = 0; i < attachment.clearColor.size(); ++i)
            {
                if (i)
                    out.Raw(",");
                out.Float(attachment.clearColor[i]);
            }
            out.Raw("],\"clearDepth\":");
            out.Float(attachment.clearDepth);
            out.Raw(",\"clearStencil\":");
            out.Number(attachment.clearStencil);
            out.Raw("}");
        }
        out.Raw("]}");
    }
    out.Raw("]");
}

void WriteResources(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    for (std::uint32_t r = 0; r < state.resources.size(); ++r)
    {
        if (r)
            out.Raw(",");
        const auto& resource = state.resources[r];
        out.Raw("{\"resource\":");
        out.Number(r);
        out.Raw(",\"name\":");
        out.String(resource.name);
        out.Raw(",\"kind\":");
        out.String(KindName(resource.kind));
        out.Raw(",\"imported\":");
        out.Bool(resource.imported);
        out.Raw(",\"live\":");
        out.Bool(state.liveResources[r] != 0);
        out.Raw(",\"physicalSlot\":");
        out.Index(state.resourceSlots[r]);
        out.Raw(",\"initialAccess\":");
        out.String(AccessName(resource.initialAccess));
        out.Raw(",\"finalAccess\":");
        out.String(AccessName(resource.finalAccess));
        out.Raw(",\"initialContent\":");
        out.String(ContentName(resource.initialContent));
        out.Raw(",\"descriptor\":");
        if (resource.kind == ResourceKind::Texture)
        {
            auto desc = resource.textureDesc;
            desc.debugName.clear();
            out.Raw(Rhi::ToDiagnosticJson(desc));
        }
        else
        {
            auto desc = resource.bufferDesc;
            desc.debugName.clear();
            out.Raw(Rhi::ToDiagnosticJson(desc));
        }
        out.Raw("}");
    }
    out.Raw("]");
}
void WriteVersions(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& version : state.versionPlan)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"resource\":");
        out.Number(version.resource);
        out.Raw(",\"version\":");
        out.Number(version.version);
        out.Raw(",\"producer\":");
        out.Index(version.producer);
        out.Raw(",\"importedProducer\":");
        out.Bool(version.importedProducer);
        out.Raw(",\"live\":");
        out.Bool(version.live);
        out.Raw(",\"content\":");
        out.String(ContentName(version.content));
        out.Raw(",\"readers\":[");
        for (std::uint32_t i = 0; i < version.readersCount; ++i)
        {
            if (i)
                out.Raw(",");
            out.Number(state.versionReaders[version.readersBegin + i]);
        }
        out.Raw("]}");
    }
    out.Raw("]");
}
void WriteEdges(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& edge : state.dependencies)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"from\":");
        out.Number(edge.from);
        out.Raw(",\"to\":");
        out.Number(edge.to);
        out.Raw(",\"resource\":");
        out.Number(edge.resource);
        out.Raw(",\"version\":");
        out.Number(edge.version);
        out.Raw(",\"hazard\":");
        out.String(ToString(edge.hazard));
        out.Raw(",\"contributesToOutput\":");
        out.Bool(edge.contributesToOutput);
        out.Raw(",\"live\":");
        out.Bool(state.livePasses[edge.from] && state.livePasses[edge.to]);
        out.Raw("}");
    }
    out.Raw("]");
}
void WriteRoots(JsonText& out, const GraphState& state)
{
    out.Raw("[");
    bool first = true;
    for (const auto& root : state.roots)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"kind\":");
        out.String(root.present ? "Present" : "Export");
        out.Raw(",\"resource\":");
        out.Number(root.resource);
        out.Raw(",\"version\":");
        out.Number(root.version);
        out.Raw("}");
    }
    for (std::uint32_t p = 0; p < state.passes.size(); ++p)
    {
        if (state.passes[p]->sideEffect.empty())
            continue;
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"kind\":\"SideEffect\",\"pass\":");
        out.Number(p);
        out.Raw(",\"reason\":");
        out.String(state.passes[p]->sideEffect);
        out.Raw("}");
    }
    out.Raw("]");
}
std::string Dot(const GraphState& state)
{
    JsonText out;
    out.Raw("digraph FrameGraph {\n  rankdir=LR;\n");
    for (const auto& info : state.passPlan)
    {
        out.Raw("  p");
        out.Number(info.declaration);
        out.Raw(" [shape=box,style=filled,fillcolor=");
        out.String(info.live ? "lightblue" : "lightgray");
        out.Raw(",label=");
        out.String(state.passes[info.declaration]->name);
        out.Raw("];\n");
    }
    for (const auto& version : state.versionPlan)
    {
        const auto node = [&]()
        {
            out.Raw("r");
            out.Number(version.resource);
            out.Raw("v");
            out.Number(version.version);
        };
        const auto& resource = state.resources[version.resource];
        out.Raw("  ");
        node();
        out.Raw(" [style=filled,fillcolor=");
        out.String(!version.live ? "lightgray" : resource.imported ? "lightgreen" : "orange");
        out.Raw(",label=");
        out.String(resource.name + " v" + std::to_string(version.version));
        out.Raw("];\n");
        if (version.producer != kNoPass)
        {
            out.Raw("  p");
            out.Number(version.producer);
            out.Raw(" -> ");
            node();
            out.Raw(" [label=\"produce\"];\n");
        }
        for (std::uint32_t i = 0; i < version.readersCount; ++i)
        {
            out.Raw("  ");
            node();
            out.Raw(" -> p");
            out.Number(state.versionReaders[version.readersBegin + i]);
            out.Raw(" [label=\"read\"];\n");
        }
    }
    for (const auto& edge : state.dependencies)
    {
        out.Raw("  p");
        out.Number(edge.from);
        out.Raw(" -> p");
        out.Number(edge.to);
        out.Raw(" [style=dashed,label=");
        out.String(std::string(ToString(edge.hazard)) + " r" + std::to_string(edge.resource) + " v" +
                   std::to_string(edge.version));
        out.Raw("];\n");
    }
    for (std::uint32_t i = 0; i < state.roots.size(); ++i)
    {
        const auto& root = state.roots[i];
        out.Raw("  root");
        out.Number(i);
        out.Raw(" [shape=diamond,label=");
        out.String(root.present ? "Present" : "Export");
        out.Raw("];\n  r");
        out.Number(root.resource);
        out.Raw("v");
        out.Number(root.version);
        out.Raw(" -> root");
        out.Number(i);
        out.Raw(";\n");
    }
    for (std::uint32_t p = 0; p < state.passes.size(); ++p)
    {
        if (state.passes[p]->sideEffect.empty())
            continue;
        out.Raw("  side");
        out.Number(p);
        out.Raw(" [shape=diamond,label=");
        out.String("SideEffect: " + state.passes[p]->sideEffect);
        out.Raw("];\n  p");
        out.Number(p);
        out.Raw(" -> side");
        out.Number(p);
        out.Raw(";\n");
    }
    out.Raw("}\n");
    return std::move(out.text);
}
} // namespace

void GraphState::EmitPlan()
{
    statistics.declaredPasses = static_cast<std::uint32_t>(passes.size());
    statistics.virtualResources = static_cast<std::uint32_t>(resources.size());
    statistics.livePasses = static_cast<std::uint32_t>(executionOrder.size());
    statistics.culledPasses = statistics.declaredPasses - statistics.livePasses;
    statistics.logicalTransitions = static_cast<std::uint32_t>(transitions.size());
    statistics.resourceVersions = static_cast<std::uint32_t>(versionPlan.size());
    statistics.dependencyEdges = static_cast<std::uint32_t>(dependencies.size());
    statistics.liveResources = static_cast<std::uint32_t>(lifetimes.size());
    statistics.physicalResources = static_cast<std::uint32_t>(allocations.size());
    statistics.physicalTransients = static_cast<std::uint32_t>(std::count_if(
        allocations.begin(), allocations.end(), [](const PhysicalAllocation& item) { return !item.imported; }));
    // canonical 内容只含逻辑语义；RHI handle、frame、build、commit 和容量不参与 hash。
    JsonText out(4096 + passes.size() * 256 + resources.size() * 512);
    out.Raw("{\"statistics\":");
    WriteStatistics(out, statistics);
    out.Raw(",\"stages\":[");
    for (std::size_t i = 0; i < compilationStages.size(); ++i)
    {
        if (i)
            out.Raw(",");
        out.String(ToString(compilationStages[i]));
    }
    out.Raw("],\"executionOrder\":[");
    for (std::size_t i = 0; i < executionOrder.size(); ++i)
    {
        if (i)
            out.Raw(",");
        out.Number(executionOrder[i]);
    }
    out.Raw("],\"passes\":");
    WritePasses(out, *this);
    out.Raw(",\"resources\":");
    WriteResources(out, *this);
    out.Raw(",\"versions\":");
    WriteVersions(out, *this);
    out.Raw(",\"edges\":");
    WriteEdges(out, *this);
    out.Raw(",\"roots\":");
    WriteRoots(out, *this);
    out.Raw(",\"lifetimes\":");
    WriteLifetimes(out, *this);
    out.Raw(",\"physicalAllocations\":");
    WriteAllocation(out, *this);
    out.Raw(",\"transitions\":");
    WriteTransitions(out, *this);
    out.Raw("}");
    canonicalPlan = std::move(out.text);
    statistics.planHash = PlanHash(canonicalPlan);
}
GraphDumpBundle GraphState::Dumps(const GraphDumpContext& context) const
{
    if (canonicalPlan.empty())
        throw GraphPhaseError("dump requires a successfully compiled plan");
    GraphDumpBundle result;
    JsonText graph(canonicalPlan.size() + 512);
    Header(graph, context, statistics.planHash, "framegraph");
    graph.Raw(",\"importOwnership\":[");
    bool first = true;
    for (std::uint32_t r = 0; r < resources.size(); ++r)
    {
        const auto& resource = resources[r];
        if (!resource.imported)
            continue;
        if (!first)
            graph.Raw(",");
        first = false;
        graph.Raw("{\"resource\":");
        graph.Number(r);
        graph.Raw(",\"owner\":");
        graph.String(resource.owner);
        graph.Raw(",\"definitionSource\":");
        graph.String(resource.definitionSource);
        graph.Raw("}");
    }
    graph.Raw("],");
    graph.Raw(std::string_view(canonicalPlan).substr(1));
    result.frameGraphJson = std::move(graph.text);
    JsonText access;
    Header(access, context, statistics.planHash, "access-plan");
    access.Raw(",\"transitions\":");
    WriteTransitions(access, *this);
    access.Raw("}");
    result.accessPlanJson = std::move(access.text);
    JsonText transient;
    Header(transient, context, statistics.planHash, "transient-plan");
    transient.Raw(",\"policy\":\"stable_first_fit_exact_descriptor_whole_resource_lane_pool\",\"lifetimes\":");
    WriteLifetimes(transient, *this);
    transient.Raw(",\"physicalAllocations\":");
    WriteAllocation(transient, *this);
    transient.Raw("}");
    result.transientPlanJson = std::move(transient.text);
    result.dot = Dot(*this);
    return result;
}
} // namespace MiniEngine::RenderGraph::Detail

namespace MiniEngine::RenderGraph
{
std::span<const std::uint32_t> CompiledRenderGraph::ExecutionOrder() const
{
    return Lock()->executionOrder;
}
std::span<const PassPlanInfo> CompiledRenderGraph::Passes() const
{
    return Lock()->passPlan;
}
std::span<const ResourceVersionInfo> CompiledRenderGraph::ResourceVersions() const
{
    return Lock()->versionPlan;
}
std::span<const std::uint32_t> CompiledRenderGraph::VersionReaders() const
{
    return Lock()->versionReaders;
}
std::span<const DependencyEdge> CompiledRenderGraph::Dependencies() const
{
    return Lock()->dependencies;
}
std::span<const LifetimeInterval> CompiledRenderGraph::Lifetimes() const
{
    return Lock()->lifetimes;
}
std::span<const PhysicalAllocation> CompiledRenderGraph::PhysicalAllocations() const
{
    return Lock()->allocations;
}
std::span<const CompileStage> CompiledRenderGraph::CompilationStages() const
{
    return Lock()->compilationStages;
}
GraphStorageStatistics CompiledRenderGraph::StorageStatistics() const
{
    return Lock()->StorageStatistics();
}
GraphDumpBundle CompiledRenderGraph::Dumps(const GraphDumpContext& context) const
{
    return Lock()->Dumps(context);
}
} // namespace MiniEngine::RenderGraph
