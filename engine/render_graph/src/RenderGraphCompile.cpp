#include "GraphState.h"
#include <numeric>

namespace MiniEngine::RenderGraph::Detail
{
void GraphState::ClearPlan()
{
    workRecorded = false;
    validatingImports = false;
    transitions.clear();
    compilationStages.clear();
    passPlan.clear();
    versionPlan.clear();
    versionOffsets.clear();
    versionReaders.clear();
    executionOrder.clear();
    resourceSlots.clear();
    livePasses.clear();
    liveResources.clear();
    dependencies.clear();
    dependencyIndex.Clear();
    lifetimes.clear();
    allocations.clear();
    canonicalPlan.clear();
    statistics = {};
    currentStage = CompileStage::ValidateDeclarations;
    contextResourceName.clear();
    contextPass = contextResource = contextVersion = kNoPass;
    lastError.reset();
}
void GraphState::SetStage(CompileStage stage)
{
    // 阶段序号也是固定协议：失败不向后推进、不发布半成品。
    if (static_cast<std::size_t>(stage) != compilationStages.size())
        throw GraphCompileError("compiler stages must execute in fixed order");
    currentStage = stage;
    compilationStages.push_back(stage);
    contextResourceName.clear();
    contextPass = contextResource = contextVersion = kNoPass;
}
bool GraphState::PreservesPrevious(std::uint32_t pass, const ResourceUse& use) const
{
    if (!use.write)
        return false;
    const auto& attachments = passes[pass]->attachments;
    const auto found = std::find_if(attachments.begin(), attachments.end(), [&](const AttachmentRecord& attachment)
                                    { return attachment.resource == use.resource; });
    return found != attachments.end() ? found->load == Rhi::LoadOp::Load : use.coverage == WriteCoverage::Preserve;
}
void GraphState::ValidateUseContent(std::uint32_t passIndex, const ResourceUse& use, ContentState& content) const
{
    using namespace Rhi;
    const auto& pass = *passes[passIndex];
    const auto& resource = resources[use.resource];
    const auto failure = [&](const char* reason)
    {
        return "pass=" + pass.name + " resource=" + resource.name + " version=" + std::to_string(use.version) + ": " +
               reason;
    };
    const auto attachment = std::find_if(pass.attachments.begin(), pass.attachments.end(),
                                         [&](const AttachmentRecord& item) { return item.resource == use.resource; });
    const bool needsAttachment = use.access == ResourceAccess::ColorWrite || use.access == ResourceAccess::DepthWrite ||
                                 use.access == ResourceAccess::DepthRead;
    if (needsAttachment && attachment == pass.attachments.end())
        throw GraphCompileError(failure("attachment access has no attachment declaration"));
    if (!use.write)
    {
        if (content != ContentState::Defined)
            throw UndefinedContentError(failure("read of undefined content"));
    }
    else if (attachment != pass.attachments.end())
    {
        if (attachment->load == LoadOp::Load && content != ContentState::Defined)
            throw UndefinedContentError(failure("Load cannot initialize undefined content"));
        if (content != ContentState::Defined && attachment->load != LoadOp::Clear)
            throw UndefinedContentError(failure("first attachment write requires Clear"));
        if (attachment->load == LoadOp::Clear)
            content = ContentState::Defined;
        if (attachment->load == LoadOp::DontCare || attachment->store == StoreOp::DontCare)
            content = ContentState::Undefined;
    }
    else
    {
        if (content == ContentState::Undefined && use.coverage != WriteCoverage::Full)
            throw UndefinedContentError(failure("first copy write must declare Full coverage"));
        if (use.coverage == WriteCoverage::Full)
            content = ContentState::Defined;
    }
}
void GraphState::ValidateDeclarations()
{
    for (std::uint32_t r = 0; r < resources.size(); ++r)
    {
        contextResource = r;
        auto& resource = resources[r];
        if (resource.kind == ResourceKind::Texture)
            Rhi::ValidateTextureDesc(resource.textureDesc);
        else
            Rhi::ValidateBufferDesc(resource.bufferDesc);
        resource.versions[0].content = resource.imported ? resource.initialContent : ContentState::Undefined;
    }
    for (std::uint32_t p = 0; p < passes.size(); ++p)
    {
        contextPass = p;
        for (const auto& use : passes[p]->uses)
        {
            contextResource = use.resource;
            contextVersion = use.version;
            if (use.resource >= resources.size())
                throw StaleGraphHandleError("declaration resource is out of range");
            auto& resource = resources[use.resource];
            if (use.kind != resource.kind || use.version >= resource.versions.size() ||
                use.previousVersion >= resource.versions.size())
                throw StaleGraphHandleError("declaration kind/version is invalid");
            ValidateAccess(resource, use.access, false);
            ValidateStages(use.stages);
            if (IsWrite(use.access) != use.write)
                throw GraphCompileError("declaration access direction mismatch");
            const auto& previous = resource.versions[use.previousVersion];
            if (use.write && (use.version != use.previousVersion + 1 || resource.versions[use.version].writer != p ||
                              (previous.writer != kNoPass && previous.writer >= p)))
                throw StaleGraphHandleError("write does not follow its previous resource version");
            if (!use.write && resource.versions[use.version].writer != kNoPass &&
                resource.versions[use.version].writer >= p)
                throw GraphCompileError("read depends on its own or a later pass");
            // 按 version producer 验证，不能从全局最后状态猜测旧版本。
            auto content = resource.versions[use.write ? use.previousVersion : use.version].content;
            ValidateUseContent(p, use, content);
            if (use.write)
                resource.versions[use.version].content = content;
        }
    }
    for (const auto& root : roots)
    {
        contextPass = kNoPass;
        contextResource = root.resource;
        contextVersion = root.version;
        if (root.resource >= resources.size())
            throw StaleGraphHandleError("root resource is invalid");
        const auto& resource = resources[root.resource];
        if (root.version + std::uint64_t{1} != resource.versions.size())
            throw StaleGraphHandleError("root version is stale");
        if (!resource.imported)
            throw GraphCompileError("transient output cannot transfer ownership");
        if (root.present &&
            (resource.kind != ResourceKind::Texture || resource.finalAccess != Rhi::ResourceAccess::Present))
            throw GraphCompileError("Present root requires imported color texture with final Present");
    }
}

void GraphState::CreateNodes()
{
    passPlan.resize(passes.size());
    livePasses.assign(passes.size(), 0);
    liveResources.assign(resources.size(), 0);
    resourceSlots.assign(resources.size(), kNoPass);
    versionOffsets.resize(resources.size() + 1, 0);
    std::size_t count = 0;
    for (std::size_t r = 0; r < resources.size(); ++r)
    {
        versionOffsets[r] = static_cast<std::uint32_t>(count);
        count += resources[r].versions.size();
        if (count >= kNoPass)
            throw GraphCompileError("resource version node count exhausted");
    }
    versionOffsets.back() = static_cast<std::uint32_t>(count);
    versionPlan.reserve(count);
    for (std::uint32_t p = 0; p < passes.size(); ++p)
        passPlan[p].declaration = p;
    for (std::uint32_t r = 0; r < resources.size(); ++r)
        for (std::uint32_t v = 0; v < resources[r].versions.size(); ++v)
            versionPlan.push_back({r, v, resources[r].versions[v].writer, 0, 0, resources[r].imported && v == 0, false,
                                   resources[r].versions[v].content});
    for (const auto& pass : passes)
        for (const auto& use : pass->uses)
            if (!use.write)
                ++versionPlan[versionOffsets[use.resource] + use.version].readersCount;
    std::uint64_t readers = 0;
    for (auto& version : versionPlan)
    {
        version.readersBegin = static_cast<std::uint32_t>(readers);
        readers += version.readersCount;
        if (readers >= kNoPass)
            throw GraphCompileError("resource reader count exhausted");
    }
    versionReaders.resize(static_cast<std::size_t>(readers));
    std::vector<std::uint32_t> cursor;
    cursor.reserve(versionPlan.size());
    for (const auto& version : versionPlan)
        cursor.push_back(version.readersBegin);
    for (std::uint32_t p = 0; p < passes.size(); ++p)
        for (const auto& use : passes[p]->uses)
            if (!use.write)
                versionReaders[cursor[versionOffsets[use.resource] + use.version]++] = p;
}
void GraphState::CreateDependencies()
{
    std::size_t uses = 0;
    for (const auto& pass : passes)
        uses += pass->uses.size();
    if (uses >= kNoPass / 2)
        throw GraphCompileError("dependency capacity exhausted");
    dependencies.reserve(uses * 2);
    const auto add = [&](std::uint32_t from, std::uint32_t to, const ResourceUse& use, std::uint32_t version,
                         Hazard hazard, bool contributes)
    {
        if (from == to)
            throw GraphCompileError("pass cannot depend on itself");
        dependencies.push_back({from, to, use.resource, version, hazard, contributes});
    };
    for (std::uint32_t p = 0; p < passes.size(); ++p)
    {
        contextPass = p;
        for (const auto& use : passes[p]->uses)
        {
            contextResource = use.resource;
            contextVersion = use.version;
            const auto& version =
                versionPlan[versionOffsets[use.resource] + (use.write ? use.previousVersion : use.version)];
            if (!use.write)
            {
                if (version.producer != kNoPass)
                    add(version.producer, p, use, use.version, Hazard::ReadAfterWrite, true);
                else if (!version.importedProducer)
                    throw UndefinedContentError("read has no producer");
            }
            else
            {
                if (version.producer != kNoPass)
                    add(version.producer, p, use, use.previousVersion, Hazard::WriteAfterWrite,
                        PreservesPrevious(p, use));
                for (auto r = version.readersBegin; r < version.readersBegin + version.readersCount; ++r)
                {
                    if (versionReaders[r] >= p)
                        throw StaleGraphHandleError("reader was declared after version overwrite");
                    add(versionReaders[r], p, use, use.previousVersion, Hazard::WriteAfterRead, false);
                }
            }
        }
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    dependencyIndex.Build(static_cast<std::uint32_t>(passes.size()), dependencies);
}
void GraphState::FindRoots()
{
    statistics.roots = static_cast<std::uint32_t>(roots.size());
    for (const auto& root : roots)
    {
        auto& version = versionPlan[versionOffsets[root.resource] + root.version];
        version.live = true;
        liveResources[root.resource] = 1;
        if (version.producer != kNoPass)
            livePasses[version.producer] = 1;
    }
    for (std::uint32_t p = 0; p < passes.size(); ++p)
        if (!passes[p]->sideEffect.empty())
        {
            livePasses[p] = 1;
            ++statistics.roots;
        }
}
void GraphState::CullPasses()
{
    std::vector<std::uint32_t> queue;
    queue.reserve(passes.size());
    for (std::uint32_t p = 0; p < passes.size(); ++p)
        if (livePasses[p])
            queue.push_back(p);
    for (std::size_t q = 0; q < queue.size(); ++q)
    {
        const auto p = queue[q];
        for (auto i = dependencyIndex.inOffsets[p]; i < dependencyIndex.inOffsets[p + 1]; ++i)
        {
            const auto& edge = dependencies[dependencyIndex.inEdges[i]];
            if (edge.contributesToOutput && !livePasses[edge.from])
            {
                livePasses[edge.from] = 1;
                queue.push_back(edge.from);
            }
        }
    }
    for (std::uint32_t p = 0; p < passes.size(); ++p)
    {
        passPlan[p].live = livePasses[p] != 0;
        if (!livePasses[p])
            continue;
        passPlan[p].culledReason = {};
        for (const auto& use : passes[p]->uses)
        {
            liveResources[use.resource] = 1;
            versionPlan[versionOffsets[use.resource] + use.version].live = true;
            if (PreservesPrevious(p, use))
                versionPlan[versionOffsets[use.resource] + use.previousVersion].live = true;
        }
    }
}
void GraphState::SortPasses()
{
    std::vector<std::string_view> passNames, resourceNames;
    passNames.reserve(passes.size());
    resourceNames.reserve(resources.size());
    for (const auto& pass : passes)
        passNames.push_back(pass->name);
    for (const auto& resource : resources)
        resourceNames.push_back(resource.name);
    executionOrder = StableTopologicalSort(dependencyIndex, dependencies, livePasses, passNames, resourceNames);
    for (std::uint32_t order = 0; order < executionOrder.size(); ++order)
        passPlan[executionOrder[order]].executionOrder = order;
}

void GraphState::AnalyzeLifetimes()
{
    std::vector<LifetimeInterval> candidates(resources.size());
    for (std::uint32_t r = 0; r < resources.size(); ++r)
    {
        candidates[r].resource = r;
        candidates[r].imported = resources[r].imported;
    }
    for (std::uint32_t order = 0; order < executionOrder.size(); ++order)
        for (const auto& use : passes[executionOrder[order]]->uses)
        {
            auto& interval = candidates[use.resource];
            interval.firstUse = std::min(interval.firstUse, order);
            interval.lastUse = order;
            using A = Rhi::ResourceAccess;
            if (use.kind == ResourceKind::Texture)
            {
                const auto flag = use.access == A::SampledRead  ? Rhi::TextureUsage::Sampled
                                  : use.access == A::ColorWrite ? Rhi::TextureUsage::ColorAttachment
                                  : use.access == A::DepthRead || use.access == A::DepthWrite
                                      ? Rhi::TextureUsage::DepthStencil
                                  : use.access == A::CopySource      ? Rhi::TextureUsage::CopySource
                                  : use.access == A::CopyDestination ? Rhi::TextureUsage::CopyDestination
                                                                     : Rhi::TextureUsage::None;
                interval.usageUnion |= static_cast<std::uint32_t>(flag);
            }
            else
            {
                const auto flag = use.access == A::VertexRead        ? Rhi::BufferUsage::Vertex
                                  : use.access == A::IndexRead       ? Rhi::BufferUsage::Index
                                  : use.access == A::UniformRead     ? Rhi::BufferUsage::Uniform
                                  : use.access == A::CopySource      ? Rhi::BufferUsage::CopySource
                                  : use.access == A::CopyDestination ? Rhi::BufferUsage::CopyDestination
                                                                     : Rhi::BufferUsage::None;
                interval.usageUnion |= static_cast<std::uint32_t>(flag);
            }
        }
    lifetimes.reserve(resources.size());
    const auto finalOrder = static_cast<std::uint32_t>(executionOrder.size());
    for (std::uint32_t r = 0; r < resources.size(); ++r)
    {
        if (!liveResources[r])
            continue;
        auto interval = candidates[r];
        if (interval.imported)
        {
            // owner final restore 也是 lifetime 的最后一次使用；Export v0 可没有 pass。
            interval.firstUse = std::min(interval.firstUse, finalOrder);
            interval.lastUse = finalOrder;
        }
        if (interval.firstUse == kNoPass)
            throw GraphCompileError("live transient has no executable use");
        lifetimes.push_back(interval);
    }
}
void GraphState::AllocateSlots()
{
    allocations.reserve(lifetimes.size());
    std::vector<std::size_t> order(lifetimes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](auto a, auto b) { return lifetimes[a].firstUse < lifetimes[b].firstUse; });
    std::vector<std::uint32_t> lastUse;
    for (const auto index : order)
    {
        auto& lifetime = lifetimes[index];
        auto slot = static_cast<std::uint32_t>(allocations.size());
        const auto& resource = resources[lifetime.resource];
        // 完整 descriptor 相等；池绑定同一 device，能力/对齐类不会跨设备混用。
        // 严格小于排除同一 pass 内重叠；只复用完整资源对象。
        if (!resource.imported)
            for (const auto& allocation : allocations)
            {
                const auto& previous = resources[allocation.resource];
                if (!allocation.imported && allocation.kind == resource.kind &&
                    lastUse[allocation.slot] < lifetime.firstUse &&
                    (resource.kind == ResourceKind::Texture
                         ? Rhi::SemanticallyEqual(previous.textureDesc, resource.textureDesc)
                         : Rhi::SemanticallyEqual(previous.bufferDesc, resource.bufferDesc)))
                {
                    slot = allocation.slot;
                    break;
                }
            }
        if (slot == allocations.size())
        {
            allocations.push_back({slot, lifetime.resource, resource.kind, resource.imported});
            lastUse.push_back(lifetime.lastUse);
        }
        else
            lastUse[slot] = lifetime.lastUse;
        resourceSlots[lifetime.resource] = slot;
        lifetime.physicalSlot = slot;
    }
}
void GraphState::BuildAccessPlan()
{
    std::vector<Rhi::ResourceAccess> access(allocations.size(), Rhi::ResourceAccess::None);
    std::vector<std::uint32_t> owners(allocations.size(), kNoPass);
    std::vector<ContentState> content(resources.size(), ContentState::Undefined);
    for (std::uint32_t r = 0; r < resources.size(); ++r)
        if (resources[r].imported && liveResources[r])
        {
            access[resourceSlots[r]] = resources[r].initialAccess;
            content[r] = resources[r].initialContent;
        }
    std::size_t count = lifetimes.size();
    for (const auto p : executionOrder)
        count += passes[p]->uses.size();
    transitions.reserve(count);
    for (std::uint32_t order = 0; order < executionOrder.size(); ++order)
    {
        const auto p = executionOrder[order];
        contextPass = p;
        for (const auto& use : passes[p]->uses)
        {
            contextResource = use.resource;
            contextVersion = use.version;
            ValidateUseContent(p, use, content[use.resource]);
            // 每次 live use 仍发声明 transition；相同 access 不代表本 pass 已声明。
            const auto slot = resourceSlots[use.resource];
            const bool reset = !resources[use.resource].imported && owners[slot] != use.resource;
            transitions.push_back(
                {p, use.resource, use.kind, access[slot], use.access, use.stages, false, order, slot, reset});
            access[slot] = use.access;
            owners[slot] = use.resource;
        }
    }
    const auto finalPass = static_cast<std::uint32_t>(passes.size());
    const auto finalOrder = static_cast<std::uint32_t>(executionOrder.size());
    for (std::uint32_t r = 0; r < resources.size(); ++r)
    {
        const auto& resource = resources[r];
        if (!liveResources[r] || !resource.imported)
            continue;
        contextPass = kNoPass;
        contextResource = r;
        contextVersion = static_cast<std::uint32_t>(resource.versions.size() - 1);
        if (IsRead(resource.finalAccess) && content[r] != ContentState::Defined)
            throw UndefinedContentError("resource=" + resource.name + ": imported final read requires defined content");
        transitions.push_back({finalPass, r, resource.kind, access[resourceSlots[r]], resource.finalAccess,
                               Rhi::ShaderStage::Vertex | Rhi::ShaderStage::Pixel, true, finalOrder, resourceSlots[r]});
    }
}
void GraphState::CompileDeclarations()
{
    ClearPlan();
    compilationStages.reserve(10);
    SetStage(CompileStage::ValidateDeclarations);
    ValidateDeclarations();
    SetStage(CompileStage::CreateNodes);
    CreateNodes();
    SetStage(CompileStage::CreateDependencies);
    CreateDependencies();
    SetStage(CompileStage::FindRoots);
    FindRoots();
    SetStage(CompileStage::Cull);
    CullPasses();
    SetStage(CompileStage::TopologicalSort);
    SortPasses();
    SetStage(CompileStage::Lifetimes);
    AnalyzeLifetimes();
    SetStage(CompileStage::PhysicalSlots);
    AllocateSlots();
    SetStage(CompileStage::AccessTransitions);
    BuildAccessPlan();
    SetStage(CompileStage::EmitPlan);
    EmitPlan();
}
GraphStorageStatistics GraphState::StorageStatistics() const
{
    GraphStorageStatistics result;
    result.declarationBytes =
        resources.capacity() * sizeof(ResourceRecord) + passes.capacity() * sizeof(std::unique_ptr<PassNode>);
    for (const auto& resource : resources)
        result.declarationBytes += resource.versions.capacity() * sizeof(VersionRecord);
    for (const auto& pass : passes)
        result.declarationBytes += sizeof(PassNode) + pass->uses.capacity() * sizeof(ResourceUse) +
                                   pass->attachments.capacity() * sizeof(AttachmentRecord);
    result.planBytes = dependencies.capacity() * sizeof(DependencyEdge) +
                       versionPlan.capacity() * sizeof(ResourceVersionInfo) +
                       passPlan.capacity() * sizeof(PassPlanInfo) + lifetimes.capacity() * sizeof(LifetimeInterval) +
                       allocations.capacity() * sizeof(PhysicalAllocation) +
                       transitions.capacity() * sizeof(LogicalTransition) + canonicalPlan.capacity();
    for (const auto* values :
         {&versionOffsets, &versionReaders, &executionOrder, &resourceSlots, &dependencyIndex.outOffsets,
          &dependencyIndex.inOffsets, &dependencyIndex.outEdges, &dependencyIndex.inEdges})
        result.planBytes += values->capacity() * sizeof(std::uint32_t);
    result.planBytes +=
        livePasses.capacity() + liveResources.capacity() + compilationStages.capacity() * sizeof(CompileStage);
    result.edgeCapacity = dependencies.capacity();
    result.versionCapacity = versionPlan.capacity();
    result.readerCapacity = versionReaders.capacity();
    return result;
}
} // namespace MiniEngine::RenderGraph::Detail
