#include "GraphState.h"

namespace MiniEngine::RenderGraph
{
RgResources::RgResources(const std::shared_ptr<Detail::GraphState>& state, std::uint32_t pass, std::uint64_t generation)
    : m_state(state), m_pass(pass), m_generation(generation)
{
}
namespace
{
std::shared_ptr<Detail::GraphState> ResolveActive(const std::weak_ptr<Detail::GraphState>& weak,
                                                  std::uint64_t generation, std::uint32_t pass)
{
    auto state = weak.lock();
    if (!state || !state->alive || state->generation != generation || state->phase != GraphPhase::Executing ||
        state->activePass != pass || state->executingDevice == nullptr)
        throw GraphPhaseError("resolver is outside its executing pass/generation");
    return state;
}
template <class Handle>
std::shared_ptr<Detail::GraphState> Resolve(const std::weak_ptr<Detail::GraphState>& weak, std::uint64_t generation,
                                            std::uint32_t pass, Handle handle)
{
    auto state = ResolveActive(weak, generation, pass);
    state->contextPass = pass;
    state->contextResource = handle.Resource();
    state->contextVersion = handle.Version();
    state->contextResourceName.clear();
    (void)state->Check(handle, false);
    const auto& uses = state->passes[pass]->uses;
    if (std::none_of(uses.begin(), uses.end(), [&](const ResourceUse& use)
                     { return use.resource == handle.Resource() && use.version == handle.Version(); }))
        throw GraphCompileError("pass=" + state->passes[pass]->name + ": resolve requires an exact declared version");
    return state;
}
} // namespace
Rhi::TextureHandle RgResources::Get(RgTexture texture) const
{
    const auto state = Resolve(m_state, m_generation, m_pass, texture);
    const auto physical = state->textures.at(texture.Resource());
    if (!physical)
        throw GraphCompileError("declared texture has no live physical mapping");
    state->callbackResolutions.push_back({ResourceKind::Texture, texture.Resource(), texture.Version(), physical, {}});
    return physical;
}
Rhi::BufferHandle RgResources::Get(RgBuffer buffer) const
{
    const auto state = Resolve(m_state, m_generation, m_pass, buffer);
    const auto physical = state->buffers.at(buffer.Resource());
    if (!physical)
        throw GraphCompileError("declared buffer has no live physical mapping");
    state->callbackResolutions.push_back({ResourceKind::Buffer, buffer.Resource(), buffer.Version(), {}, physical});
    return physical;
}

Rhi::ResourceSetHandle RgResources::CreateResourceSet(const Rhi::ResourceSetDesc& desc) const
{
    const auto state = ResolveActive(m_state, m_generation, m_pass);
    return state->CreateFrameResourceSet(m_pass, desc);
}
} // namespace MiniEngine::RenderGraph

namespace MiniEngine::RenderGraph::Detail
{
namespace
{
template <class Handle> bool IsGraphPhysical(const GraphState& state, ResourceKind kind, Handle physical)
{
    if (!physical)
        return false;
    for (std::uint32_t resource = 0; resource < state.resources.size(); ++resource)
    {
        if (state.resources[resource].kind != kind)
            continue;
        if constexpr (std::is_same_v<Handle, Rhi::TextureHandle>)
        {
            if (kind == ResourceKind::Texture && state.textures[resource] == physical)
                return true;
        }
        else
        {
            if (kind == ResourceKind::Buffer && state.buffers[resource] == physical)
                return true;
        }
    }
    return false;
}
template <class Handle> bool IsResolutionFor(const CallbackResolution& candidate, ResourceKind kind, Handle physical)
{
    if (candidate.kind != kind)
        return false;
    if constexpr (std::is_same_v<Handle, Rhi::TextureHandle>)
        return kind == ResourceKind::Texture && candidate.texture == physical;
    else
        return kind == ResourceKind::Buffer && candidate.buffer == physical;
}
template <class Handle>
void ValidateSetBinding(GraphState& state, std::uint32_t pass, ResourceKind kind, Handle physical,
                        Rhi::ResourceAccess expected, const char* bindingName)
{
    if (!IsGraphPhysical(state, kind, physical))
        return;
    const auto resolved = std::find_if(state.callbackResolutions.begin(), state.callbackResolutions.end(),
                                       [&](const auto& candidate)
                                       {
                                           if (!IsResolutionFor(candidate, kind, physical))
                                               return false;
                                           const auto& uses = state.passes.at(pass)->uses;
                                           return std::any_of(uses.begin(), uses.end(),
                                                              [&](const ResourceUse& use)
                                                              {
                                                                  return use.kind == kind &&
                                                                         use.resource == candidate.resource &&
                                                                         use.version == candidate.version &&
                                                                         !use.write && use.access == expected;
                                                              });
                                       });
    if (resolved == state.callbackResolutions.end())
        throw GraphCompileError("pass=" + state.passes.at(pass)->name + " resource-set " + bindingName +
                                ": frame graph resource must be resolved by this pass with a compatible read access");
    state.contextResource = resolved->resource;
    state.contextVersion = resolved->version;
}
} // namespace

Rhi::ResourceSetHandle GraphState::CreateFrameResourceSet(std::uint32_t pass, const Rhi::ResourceSetDesc& desc)
{
    if (!alive || phase != GraphPhase::Executing || activePass != pass || executingDevice == nullptr ||
        pass >= passes.size())
        throw GraphPhaseError("resource-set creation is outside its executing pass/generation");
    contextPass = pass;
    contextResource = contextVersion = kNoPass;
    for (const auto& binding : desc.bindings)
    {
        if (binding.type == Rhi::BindingType::SampledTexture)
            ValidateSetBinding(*this, pass, ResourceKind::Texture, binding.texture, Rhi::ResourceAccess::SampledRead,
                               "sampled binding");
        else if (binding.type == Rhi::BindingType::UniformBuffer)
            ValidateSetBinding(*this, pass, ResourceKind::Buffer, binding.buffer.buffer,
                               Rhi::ResourceAccess::UniformRead, "uniform binding");
    }
    return executingDevice->CreateFrameResourceSet(executingFrame, desc);
}
} // namespace MiniEngine::RenderGraph::Detail
