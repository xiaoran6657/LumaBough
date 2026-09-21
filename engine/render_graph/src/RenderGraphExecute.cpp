#include "GraphState.h"
#include <MiniEngine/RenderGraph/TransientResourcePool.h>

namespace MiniEngine::RenderGraph
{
namespace
{
void ValidateImports(Detail::GraphState& state, Rhi::IRhiDevice& device, const Rhi::FrameToken& frame)
{
    // 全部输入先验证，失败时不创建 transient、不打开 command list、不部分发布 import。
    (void)device.AcquireBackBuffer(frame);
    for (std::uint32_t index = 0; index < state.resources.size(); ++index)
    {
        if (!state.liveResources[index])
            continue;
        state.contextResource = index;
        const auto& resource = state.resources[index];
        if (resource.kind == ResourceKind::Texture)
            Rhi::ValidateTextureDesc(resource.textureDesc, device.Capabilities());
        else
            Rhi::ValidateBufferDesc(resource.bufferDesc);
        if (!resource.imported)
            continue;
        bool defined = false;
        Rhi::ResourceAccess access = Rhi::ResourceAccess::None;
        bool descriptorMatches = false;
        if (resource.kind == ResourceKind::Texture)
        {
            const auto actual = device.QueryTextureState(frame, resource.texture);
            descriptorMatches = Rhi::SemanticallyEqual(resource.textureDesc, actual.descriptor);
            access = actual.access;
            defined = actual.fullyDefined;
            if ((resource.initialAccess == Rhi::ResourceAccess::Present ||
                 resource.finalAccess == Rhi::ResourceAccess::Present) &&
                resource.texture != frame.backBuffer)
                throw GraphCompileError("resource=" + resource.name +
                                        ": Present must identify this frame's backbuffer");
        }
        else
        {
            const auto actual = device.QueryBufferState(frame, resource.buffer);
            descriptorMatches = Rhi::SemanticallyEqual(resource.bufferDesc, actual.descriptor);
            access = actual.access;
            defined = actual.fullyDefined;
        }
        if (!descriptorMatches)
            throw GraphCompileError("resource=" + resource.name + ": imported physical descriptor mismatch");
        if (access != resource.initialAccess)
            throw GraphCompileError("resource=" + resource.name + ": imported initial access disagrees with owner");
        if (resource.initialContent == ContentState::Defined && !defined)
            throw UndefinedContentError("resource=" + resource.name +
                                        ": Defined import lacks actual RHI content proof");
    }
}
std::vector<Rhi::AccessTransition> ResolveTransitions(const Detail::GraphState& state, std::uint32_t pass,
                                                      Rhi::IRhiDevice& device, const Rhi::FrameToken& frame,
                                                      Rhi::IRhiGraphCommandSink& sink)
{
    std::vector<Rhi::AccessTransition> result;
    std::vector<Rhi::GraphResourceImport> resets;
    for (const auto& transition : state.transitions)
        if (transition.pass == pass)
        {
            const auto texture = state.textures[transition.resource];
            const auto buffer = state.buffers[transition.resource];
            // lane 可能来自之前的帧；真实 before 来自设备状态，canonical plan 不依赖运行时句柄。
            const auto before = texture ? device.QueryTextureState(frame, texture).access
                                        : device.QueryBufferState(frame, buffer).access;
            result.push_back({texture, buffer, before, transition.after, transition.stages,
                              state.resources[transition.resource].name});
            if (transition.resetContent)
                resets.push_back({texture, buffer});
        }
    if (!resets.empty())
        sink.ResetTransientContents(resets);
    return result;
}
void VerifyWrites(Detail::GraphState& state, std::uint32_t pass, Rhi::IRhiDevice& device, const Rhi::FrameToken& frame)
{
    for (const auto& use : state.passes[pass]->uses)
    {
        if (!use.write || state.resources[use.resource].versions[use.version].content != ContentState::Defined)
            continue;
        state.contextResource = use.resource;
        state.contextVersion = use.version;
        const bool defined = use.kind == ResourceKind::Texture
                                 ? device.QueryTextureState(frame, state.textures[use.resource]).fullyDefined
                                 : device.QueryBufferState(frame, state.buffers[use.resource]).fullyDefined;
        if (!defined)
            throw UndefinedContentError("pass=" + state.passes[pass]->name +
                                        " resource=" + state.resources[use.resource].name +
                                        ": declared complete write did not define actual RHI contents");
    }
}
} // namespace
CompiledRenderGraph::CompiledRenderGraph(const std::shared_ptr<Detail::GraphState>& state, std::uint64_t generation)
    : m_state(state), m_generation(generation)
{
}
std::shared_ptr<Detail::GraphState> CompiledRenderGraph::Lock() const
{
    if (!m_state || !m_state->alive || m_state->generation != m_generation)
        throw StaleGraphHandleError("compiled graph owner/generation is no longer valid");
    return m_state;
}
GraphStatistics CompiledRenderGraph::Statistics() const
{
    return Lock()->statistics;
}
std::span<const LogicalTransition> CompiledRenderGraph::Transitions() const
{
    return Lock()->transitions;
}
void CompiledRenderGraph::Execute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame)
{
    TransientResourcePool pool(device);
    Execute(device, frame, pool);
}
void CompiledRenderGraph::Execute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame, TransientResourcePool& pool)
{
    // M7-02 观测说明：本模块刻意只依赖 MiniEngineRhiPublic（M6-07 的 composition 契约），
    // 因此不在这里加 CPU zone。图执行的 CPU 成本由组合根的 "RhiSubmit" zone 覆盖，
    // pass 级观察走 backend 的 per-pass GPU zone（BeginLabel/EndLabel 事件）。
    auto state = Lock();
    state->RequirePhase(GraphPhase::Compiled);
    std::vector<Rhi::TextureHandle> textures(state->resources.size());
    std::vector<Rhi::BufferHandle> buffers(state->resources.size());
    state->textures.swap(textures);
    state->buffers.swap(buffers);
    state->phase = GraphPhase::Executing;
    state->workRecorded = false;
    state->contextPass = state->contextResource = state->contextVersion = Detail::kNoPass;
    state->executingDevice = &device;
    state->executingFrame = frame;
    state->callbackResolutions.clear();
    Rhi::IRhiCommandList* commands = nullptr;
    bool rendering = false;
    bool label = false;
    bool leased = false;
    try
    {

        // 空图不打开录制；完整帧 RHI 要求非空图显式恢复当前 backbuffer 到 Present。
        if (state->executionOrder.empty() && state->lifetimes.empty())
        {
            state->textures.clear();

            state->buffers.clear();

            state->callbackResolutions.clear();

            state->executingDevice = nullptr;

            state->executingFrame = {};

            state->phase = GraphPhase::Executed;

            return;
        }
        const bool hasPresent =
            std::any_of(state->transitions.begin(), state->transitions.end(), [](const LogicalTransition& item)
                        { return item.final && item.after == Rhi::ResourceAccess::Present; });
        if (!hasPresent)
        {
            GraphDiagnostic diagnostic;
            diagnostic.code = GraphErrorCode::MissingPresent;
            diagnostic.phase = GraphPhase::Executing;
            diagnostic.stage = CompileStage::EmitPlan;
            diagnostic.message = "nonempty full-frame execution requires a live imported backbuffer with final Present";
            throw GraphCompileError(std::move(diagnostic));
        }
        state->validatingImports = true;
        ValidateImports(*state, device, frame);
        state->validatingImports = false;
        pool.BeginLease(device, frame);
        leased = true;
        for (const auto& allocation : state->allocations)
        {
            const auto index = allocation.resource;
            state->contextResource = index;
            const auto& resource = state->resources[index];
            if (resource.kind == ResourceKind::Texture)
                state->textures[index] = resource.imported ? resource.texture : pool.Acquire(resource.textureDesc);
            else
                state->buffers[index] = resource.imported ? resource.buffer : pool.Acquire(resource.bufferDesc);
        }
        for (const auto& lifetime : state->lifetimes)
        {
            const auto representative = state->allocations[lifetime.physicalSlot].resource;
            state->textures[lifetime.resource] = state->textures[representative];
            state->buffers[lifetime.resource] = state->buffers[representative];
        }
        state->contextResource = Detail::kNoPass;
        commands = &device.BeginGraphics(frame);
        state->workRecorded = true;
        auto& sink = device.GraphCommandSink(frame);
        std::vector<Rhi::GraphResourceImport> imports;
        for (const auto& allocation : state->allocations)
            imports.push_back({state->textures[allocation.resource], state->buffers[allocation.resource]});
        if (!imports.empty())
            sink.ImportResources(imports);
        for (const auto index : state->executionOrder)
        {
            state->contextPass = index;
            state->contextResource = state->contextVersion = Detail::kNoPass;
            const auto& pass = *state->passes[index];
            const auto transitions = ResolveTransitions(*state, index, device, frame, sink);
            if (!transitions.empty())
                sink.ApplyTransitions(transitions);
            commands->BeginLabel(pass.name);
            label = true;
            std::vector<Rhi::ColorAttachment> colors;
            std::optional<Rhi::DepthAttachment> depth;
            Rhi::Extent2D extent;
            for (const auto& attachment : pass.attachments)
            {
                extent = state->resources[attachment.resource].textureDesc.extent;
                const auto physical = state->textures[attachment.resource];
                if (attachment.depth)
                    depth = Rhi::DepthAttachment{physical, attachment.load, attachment.store, attachment.clearDepth,
                                                 attachment.clearStencil};
                else
                    colors.push_back({physical, attachment.load, attachment.store, attachment.clearColor});
            }
            if (!pass.attachments.empty())
            {
                commands->BeginRendering({colors, depth ? &*depth : nullptr, extent});
                rendering = true;
            }
            state->activePass = index;
            state->callbackResolutions.clear();
            RgResources resources(state, index, m_generation);
            pass.invoke(pass.data, resources, *commands);
            state->callbackResolutions.clear();
            if (!state->alive)
                throw GraphPhaseError("graph owner destroyed during execute callback");
            state->activePass = Detail::kNoPass;
            if (rendering)
            {
                commands->EndRendering();
                rendering = false;
            }
            VerifyWrites(*state, index, device, frame);
            commands->EndLabel();
            label = false;
        }
        state->contextPass = state->contextResource = state->contextVersion = Detail::kNoPass;
        // 即使 final 与当前 access 相同，也在所有 callback 之后实际发出 owner 的恢复批次。
        const auto final =
            ResolveTransitions(*state, static_cast<std::uint32_t>(state->passes.size()), device, frame, sink);
        if (!final.empty())
            sink.ApplyTransitions(final);
        device.EndGraphics(frame, *commands);
        pool.EndLease(true);
        leased = false;
        state->textures.clear();
        state->buffers.clear();
        state->callbackResolutions.clear();
        state->executingDevice = nullptr;
        state->executingFrame = {};
        state->phase = GraphPhase::Executed;
    }
    catch (...)
    {
        state->lastError = state->CaptureDiagnostic(std::current_exception());
        state->validatingImports = false;
        state->activePass = Detail::kNoPass;
        if (rendering && commands != nullptr)
        {
            try
            {
                commands->EndRendering();
            }
            catch (...)
            {
            }
        }
        if (label && commands != nullptr)
        {
            try
            {
                commands->EndLabel();
            }
            catch (...)
            {
            }
        }
        try
        {
            if (leased)
                pool.EndLease(false);
        }
        catch (...)
        {
        }
        state->textures.clear();
        state->buffers.clear();
        state->callbackResolutions.clear();
        state->executingDevice = nullptr;
        state->executingFrame = {};
        state->phase = GraphPhase::Failed;
        // 保留原始失败，不补发 Present、不提交半成品 frame；外部 owner 执行恢复/销毁。
        throw;
    }
}

GraphExecutionResult CompiledRenderGraph::TryExecute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame)
{
    TransientResourcePool pool(device);
    return TryExecute(device, frame, pool);
}
GraphExecutionResult CompiledRenderGraph::TryExecute(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame,
                                                     TransientResourcePool& pool)
{
    std::shared_ptr<Detail::GraphState> state;
    try
    {
        state = Lock();
        state->RequirePhase(GraphPhase::Compiled);
    }
    catch (...)
    {
        if (m_state)
            return {false, false, false, m_state->CaptureDiagnostic(std::current_exception())};
        GraphDiagnostic diagnostic;
        diagnostic.code = GraphErrorCode::StaleHandle;
        diagnostic.message = "compiled graph has no owner";
        return {false, false, false, std::move(diagnostic)};
    }
    try
    {
        Execute(device, frame, pool);
        return {true, state->workRecorded, state->workRecorded, {}};
    }
    catch (...)
    {
        auto diagnostic = state->lastError ? *state->lastError : state->CaptureDiagnostic(std::current_exception());
        return {false, state->workRecorded, false, std::move(diagnostic)};
    }
}
} // namespace MiniEngine::RenderGraph
