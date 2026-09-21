#include "GraphState.h"
#include <atomic>
#include <cctype>
#include <cmath>

namespace MiniEngine::RenderGraph
{
namespace
{
std::uint64_t AcquireOwner()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;)
    {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw GraphCompileError("graph owner identity exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}
} // namespace
RenderGraph::RenderGraph() : m_state(std::make_shared<Detail::GraphState>(AcquireOwner()))
{
}
RenderGraph::~RenderGraph()
{
    m_state->alive = false;
}
GraphPhase RenderGraph::Phase() const
{
    return m_state->phase;
}
std::size_t RenderGraph::PassCount() const
{
    return m_state->passes.size();
}
std::size_t RenderGraph::ResourceCount() const
{
    return m_state->resources.size();
}
void RenderGraph::Reset()
{
    if (m_state->phase == GraphPhase::Setup || m_state->phase == GraphPhase::Compiling ||
        m_state->phase == GraphPhase::Executing)
        throw GraphPhaseError("Reset during setup/compile/execute is forbidden");
    if (m_state->generation == std::numeric_limits<std::uint64_t>::max())
        throw GraphPhaseError("graph reset generation exhausted");
    ++m_state->generation;
    m_state->resources.clear();
    m_state->passes.clear();
    m_state->roots.clear();
    m_state->ClearPlan();
    m_state->textures.clear();
    m_state->buffers.clear();
    m_state->statistics = {};
    m_state->activePass = Detail::kNoPass;
    m_state->phase = GraphPhase::Building;
}
void RenderGraph::Reserve(GraphCapacity capacity)
{
    m_state->RequirePhase(GraphPhase::Building);
    m_state->passes.reserve(capacity.passes);
    m_state->resources.reserve(capacity.resources);
}
GraphCompileResult RenderGraph::TryCompile(const std::function<void(RenderGraph&)>& declare)
{
    try
    {
        m_state->RequirePhase(GraphPhase::Building);
    }
    catch (...)
    {
        return {std::nullopt, m_state->CaptureDiagnostic(std::current_exception())};
    }
    try
    {
        m_state->lastError.reset();
        if (declare)
            declare(*this);
        GraphCompileResult result;
        result.plan.emplace(Compile());
        return result;
    }
    catch (...)
    {
        if (!m_state->lastError)
            m_state->lastError = m_state->CaptureDiagnostic(std::current_exception());
        m_state->phase = GraphPhase::Failed;
        return {std::nullopt, m_state->lastError};
    }
}
RgTexture RenderGraph::CreateTexture(std::string_view name, const Rhi::TextureDesc& descriptor)
{
    m_state->RequirePhase(GraphPhase::Building);
    return m_state->AddTexture(name, descriptor);
}
RgBuffer RenderGraph::CreateBuffer(std::string_view name, const Rhi::BufferDesc& descriptor)
{
    m_state->RequirePhase(GraphPhase::Building);
    return m_state->AddBuffer(name, descriptor);
}
RgTexture RenderGraph::ImportTexture(std::string_view name, const TextureImport& imported)
{
    m_state->RequirePhase(GraphPhase::Building);
    return m_state->AddTexture(name, imported.descriptor, &imported);
}
RgBuffer RenderGraph::ImportBuffer(std::string_view name, const BufferImport& imported)
{
    m_state->RequirePhase(GraphPhase::Building);
    return m_state->AddBuffer(name, imported.descriptor, &imported);
}
void RenderGraph::Present(RgTexture texture)
{
    m_state->RequirePhase(GraphPhase::Building);
    m_state->Root(texture, true);
}
void RenderGraph::Export(RgTexture texture)
{
    m_state->RequirePhase(GraphPhase::Building);
    m_state->Root(texture, false);
}
void RenderGraph::Export(RgBuffer buffer)
{
    m_state->RequirePhase(GraphPhase::Building);
    m_state->Root(buffer, false);
}
void RenderGraph::AddPassErased(std::string_view name, std::any data, const SetupFunction& setup,
                                ExecuteFunction execute)
{
    m_state->RequirePhase(GraphPhase::Building);
    auto node = std::make_unique<Detail::PassNode>();
    node->name = Detail::GraphState::Name(name, "pass");
    if (std::any_of(m_state->passes.begin(), m_state->passes.end(),
                    [&](const auto& other) { return other->name == node->name; }))
        throw GraphCompileError("duplicate pass name: " + node->name);
    if (m_state->passes.size() >= Detail::kNoPass)
        throw GraphCompileError("graph pass identity exhausted");
    node->data = std::move(data);
    node->invoke = std::move(execute);
    const auto pass = static_cast<std::uint32_t>(m_state->passes.size());
    m_state->passes.push_back(std::move(node));
    m_state->activePass = pass;
    m_state->contextResourceName.clear();
    m_state->contextPass = pass;
    m_state->contextResource = m_state->contextVersion = Detail::kNoPass;
    m_state->phase = GraphPhase::Setup;
    RgBuilder builder(m_state, pass);
    try
    {
        setup(builder, m_state->passes[pass]->data);
        m_state->activePass = Detail::kNoPass;
        m_state->phase = GraphPhase::Building;
    }
    catch (...)
    {
        m_state->lastError = m_state->CaptureDiagnostic(std::current_exception());
        // 失败草稿保留诊断但不能编译；Reset 整体换 generation，逃逸 handle 不会复活。
        m_state->activePass = Detail::kNoPass;
        m_state->phase = GraphPhase::Failed;
        throw;
    }
}
CompiledRenderGraph RenderGraph::Compile()
{
    m_state->RequirePhase(GraphPhase::Building);
    m_state->phase = GraphPhase::Compiling;
    try
    {
        m_state->CompileDeclarations();
        m_state->phase = GraphPhase::Compiled;
        return CompiledRenderGraph(m_state, m_state->generation);
    }
    catch (...)
    {
        m_state->lastError = m_state->CaptureDiagnostic(std::current_exception());
        m_state->phase = GraphPhase::Failed;
        throw;
    }
}
} // namespace MiniEngine::RenderGraph

namespace MiniEngine::RenderGraph::Detail
{
void GraphState::RequirePhase(GraphPhase expected) const
{
    if (!alive || phase != expected)
        throw GraphPhaseError("graph operation is outside its setup/compile/execute phase");
}
std::string GraphState::Name(std::string_view name, const char* role)
{
    if (name.empty() || name.find('\0') != std::string_view::npos ||
        std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; }))
        throw GraphCompileError(std::string(role) + " requires a nonblank owned name/reason");
    return std::string(name);
}
bool GraphState::IsWrite(Rhi::ResourceAccess access)
{
    return access == Rhi::ResourceAccess::ColorWrite || access == Rhi::ResourceAccess::DepthWrite ||
           access == Rhi::ResourceAccess::CopyDestination;
}
bool GraphState::IsRead(Rhi::ResourceAccess access)
{
    return access != Rhi::ResourceAccess::None && !IsWrite(access);
}
void GraphState::ValidateStages(Rhi::ShaderStage stages)
{
    if (stages != Rhi::ShaderStage::Vertex && stages != Rhi::ShaderStage::Pixel &&
        stages != (Rhi::ShaderStage::Vertex | Rhi::ShaderStage::Pixel))
        throw GraphCompileError("invalid graph shader stage mask");
}
void GraphState::ValidateAccess(const ResourceRecord& resource, Rhi::ResourceAccess access, bool allowNone)
{
    using namespace Rhi;
    if (allowNone && access == ResourceAccess::None)
        return;
    if (resource.kind == ResourceKind::Texture)
    {
        TextureUsage required = TextureUsage::None;
        switch (access)
        {
        case ResourceAccess::SampledRead:
            required = TextureUsage::Sampled;
            break;
        case ResourceAccess::ColorWrite:
            required = TextureUsage::ColorAttachment;
            break;
        case ResourceAccess::DepthRead:
        case ResourceAccess::DepthWrite:
            required = TextureUsage::DepthStencil;
            break;
        case ResourceAccess::CopySource:
            required = TextureUsage::CopySource;
            break;
        case ResourceAccess::CopyDestination:
            required = TextureUsage::CopyDestination;
            break;
        case ResourceAccess::Present:
            if (!resource.imported || !HasFlag(resource.textureDesc.usage, TextureUsage::ColorAttachment))
                throw GraphCompileError("resource=" + resource.name + ": Present requires imported color texture");
            return;
        default:
            throw GraphCompileError("resource=" + resource.name + ": invalid texture access");
        }
        if (!HasFlag(resource.textureDesc.usage, required))
            throw GraphCompileError("resource=" + resource.name + ": access exceeds texture usage");
    }
    else
    {
        BufferUsage required = BufferUsage::None;
        switch (access)
        {
        case ResourceAccess::CopySource:
            required = BufferUsage::CopySource;
            break;
        case ResourceAccess::CopyDestination:
            required = BufferUsage::CopyDestination;
            break;
        case ResourceAccess::VertexRead:
            required = BufferUsage::Vertex;
            break;
        case ResourceAccess::IndexRead:
            required = BufferUsage::Index;
            break;
        case ResourceAccess::UniformRead:
            required = BufferUsage::Uniform;
            break;
        default:
            throw GraphCompileError("resource=" + resource.name + ": invalid buffer access");
        }
        if (!HasFlag(resource.bufferDesc.usage, required))
            throw GraphCompileError("resource=" + resource.name + ": access exceeds buffer usage");
    }
}
std::uint32_t GraphState::AddResource(ResourceRecord resource)
{
    if (resources.size() >= std::numeric_limits<std::uint32_t>::max())
        throw GraphCompileError("graph resource identity exhausted");
    for (const auto& other : resources)
    {
        if (resource.name == other.name)
            throw GraphCompileError("duplicate graph resource name: " + resource.name);
        if (resource.imported && other.imported && resource.kind == other.kind &&
            (resource.kind == ResourceKind::Texture ? resource.texture == other.texture
                                                    : resource.buffer == other.buffer))
            throw GraphCompileError("physical resource imported twice under independent virtual identities");
    }
    if (resource.imported)
    {
        if (resource.initialContent != ContentState::Defined && resource.initialContent != ContentState::Undefined)
            throw GraphCompileError("invalid imported content state");
        if (resource.initialContent == ContentState::Defined)
            resource.definitionSource = Name(resource.definitionSource, "Defined import evidence");
        if (resource.owner.empty())
            resource.owner = resource.name;
        else
            resource.owner = Name(resource.owner, "import owner");
        ValidateAccess(resource, resource.initialAccess, true);
        ValidateAccess(resource, resource.finalAccess, false);
    }
    const auto index = static_cast<std::uint32_t>(resources.size());
    resources.push_back(std::move(resource));
    return index;
}
RgTexture GraphState::AddTexture(std::string_view name, const Rhi::TextureDesc& desc, const TextureImport* imported)
{
    contextPass = activePass;
    contextResource = kNoPass;
    contextVersion = 0;
    contextResourceName = name;
    Rhi::ValidateTextureDesc(desc);
    ResourceRecord resource;
    resource.name = Name(name, "resource");
    resource.textureDesc = desc;
    if (imported != nullptr)
    {
        if (!imported->physical)
            throw GraphCompileError("import requires valid physical texture");
        resource.imported = true;
        resource.texture = imported->physical;
        resource.initialAccess = imported->initialAccess;
        resource.finalAccess = imported->finalAccess;
        resource.initialContent = imported->initialContent;
        resource.owner = imported->owner;
        resource.definitionSource = imported->definitionSource;
    }
    return Issue<RgTexture>(AddResource(std::move(resource)), 0);
}
RgBuffer GraphState::AddBuffer(std::string_view name, const Rhi::BufferDesc& desc, const BufferImport* imported)
{
    contextPass = activePass;
    contextResource = kNoPass;
    contextVersion = 0;
    contextResourceName = name;
    Rhi::ValidateBufferDesc(desc);
    ResourceRecord resource;
    resource.name = Name(name, "resource");
    resource.kind = ResourceKind::Buffer;
    resource.bufferDesc = desc;
    if (imported != nullptr)
    {
        if (!imported->physical)
            throw GraphCompileError("import requires valid physical buffer");
        resource.imported = true;
        resource.buffer = imported->physical;
        resource.initialAccess = imported->initialAccess;
        resource.finalAccess = imported->finalAccess;
        resource.initialContent = imported->initialContent;
        resource.owner = imported->owner;
        resource.definitionSource = imported->definitionSource;
    }
    return Issue<RgBuffer>(AddResource(std::move(resource)), 0);
}
} // namespace MiniEngine::RenderGraph::Detail
