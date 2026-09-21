#include "GraphState.h"
#include <cmath>

namespace MiniEngine::RenderGraph
{
RgBuilder::RgBuilder(const std::shared_ptr<Detail::GraphState>& state, std::uint32_t pass)
    : m_state(state), m_pass(pass), m_generation(state->generation)
{
}
std::shared_ptr<Detail::GraphState> RgBuilder::Lock() const
{
    auto state = m_state.lock();
    if (!state || !state->alive || state->generation != m_generation || state->phase != GraphPhase::Setup ||
        state->activePass != m_pass)
        throw GraphPhaseError("builder is only valid during its synchronous setup callback");
    return state;
}
RgTexture RgBuilder::CreateTexture(std::string_view name, const Rhi::TextureDesc& descriptor)
{
    return Lock()->AddTexture(name, descriptor);
}
RgBuffer RgBuilder::CreateBuffer(std::string_view name, const Rhi::BufferDesc& descriptor)
{
    return Lock()->AddBuffer(name, descriptor);
}
RgTexture RgBuilder::ImportTexture(std::string_view name, const TextureImport& imported)
{
    return Lock()->AddTexture(name, imported.descriptor, &imported);
}
RgBuffer RgBuilder::ImportBuffer(std::string_view name, const BufferImport& imported)
{
    return Lock()->AddBuffer(name, imported.descriptor, &imported);
}
RgTexture RgBuilder::Read(RgTexture texture, Rhi::ResourceAccess access, Rhi::ShaderStage stages)
{
    return Lock()->Use(texture, access, false, WriteCoverage::Preserve, stages);
}
RgBuffer RgBuilder::Read(RgBuffer buffer, Rhi::ResourceAccess access, Rhi::ShaderStage stages)
{
    return Lock()->Use(buffer, access, false, WriteCoverage::Preserve, stages);
}
RgTexture RgBuilder::Write(RgTexture texture, Rhi::ResourceAccess access, WriteCoverage coverage)
{
    return Lock()->Use(texture, access, true, coverage, Rhi::ShaderStage::Pixel);
}
RgBuffer RgBuilder::Write(RgBuffer buffer, Rhi::ResourceAccess access, WriteCoverage coverage)
{
    return Lock()->Use(buffer, access, true, coverage, Rhi::ShaderStage::Vertex);
}
void RgBuilder::SetColorAttachment(RgTexture texture, Rhi::LoadOp load, Rhi::StoreOp store,
                                   std::array<float, 4> clearColor)
{
    Lock()->Attach(texture, load, store, false, clearColor, 1.0F, 0);
}
void RgBuilder::SetDepthAttachment(RgTexture texture, Rhi::LoadOp load, Rhi::StoreOp store, float clearDepth,
                                   std::uint8_t clearStencil)
{
    Lock()->Attach(texture, load, store, true, {}, clearDepth, clearStencil);
}
void RgBuilder::Present(RgTexture texture)
{
    Lock()->Root(texture, true);
}
void RgBuilder::Export(RgTexture texture)
{
    Lock()->Root(texture, false);
}
void RgBuilder::Export(RgBuffer buffer)
{
    Lock()->Root(buffer, false);
}
void RgBuilder::SideEffect(std::string_view reason)
{
    auto state = Lock();
    auto& effect = state->passes[m_pass]->sideEffect;
    if (!effect.empty())
        throw GraphCompileError("pass already has a side-effect reason");
    effect = Detail::GraphState::Name(reason, "side effect");
}
} // namespace MiniEngine::RenderGraph
namespace MiniEngine::RenderGraph::Detail
{
void GraphState::Attach(RgTexture handle, Rhi::LoadOp load, Rhi::StoreOp store, bool depth, std::array<float, 4> color,
                        float clearDepth, std::uint8_t stencil)
{
    using namespace Rhi;
    const auto& resource = Check(handle);
    const auto& desc = resource.textureDesc;
    if (desc.dimension != TextureDimension::Texture2D || desc.mipLevels != 1 || desc.arrayLayers != 1)
        throw GraphCompileError("resource=" + resource.name + ": attachment requires single mip/layer Texture2D");
    if ((load != LoadOp::Load && load != LoadOp::Clear && load != LoadOp::DontCare) ||
        (store != StoreOp::Store && store != StoreOp::DontCare))
        throw GraphCompileError("invalid attachment load/store operation");
    if (!std::isfinite(clearDepth) || clearDepth < 0 || clearDepth > 1 ||
        std::any_of(color.begin(), color.end(), [](float value) { return !std::isfinite(value); }))
        throw GraphCompileError("invalid attachment clear value");
    const auto& pass = *passes[activePass];
    const auto use = std::find_if(pass.uses.begin(), pass.uses.end(), [&](const ResourceUse& value)
                                  { return value.resource == handle.Resource() && value.version == handle.Version(); });
    if (use == pass.uses.end() || (!depth && (!use->write || use->access != ResourceAccess::ColorWrite)) ||
        (depth && use->access != ResourceAccess::DepthWrite && use->access != ResourceAccess::DepthRead))
        throw GraphCompileError("attachment must match this pass's declared output/read-only depth version");
    if (depth && use->access == ResourceAccess::DepthRead && (load != LoadOp::Load || store != StoreOp::Store))
        throw GraphCompileError("read-only depth requires Load/Store");
    std::size_t colorCount = 0;
    for (const auto& attachment : pass.attachments)
    {
        if (attachment.resource == handle.Resource() || (depth && attachment.depth))
            throw GraphCompileError("duplicate attachment or multiple depth attachments");
        if (!attachment.depth)
            ++colorCount;
        const auto& other = resources[attachment.resource].textureDesc;
        if (desc.extent != other.extent || desc.sampleCount != other.sampleCount)
            throw GraphCompileError("attachment extents/sample counts disagree");
    }
    if (!depth && colorCount >= 4)
        throw GraphCompileError("M6 supports at most four color attachments");
    passes[activePass]->attachments.push_back(
        {handle.Resource(), handle.Version(), load, store, depth, color, clearDepth, stencil});
}
} // namespace MiniEngine::RenderGraph::Detail
