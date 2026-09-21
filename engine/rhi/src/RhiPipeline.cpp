#include <MiniEngine/Rhi/RhiPipeline.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <tuple>

namespace MiniEngine::Rhi
{
namespace
{
[[noreturn]] void Invalid(const char* operation, const char* reason)
{
    throw RhiValidationError({RhiErrorCode::InvalidArgument, operation, "PipelineContract", "", "", reason});
}
bool IsHash(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
                                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
void Number(std::string& key, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        key.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
void Text(std::string& key, std::string_view value)
{
    Number(key, value.size());
    key.append(value);
}
void Float(std::string& key, float value)
{
    if (!std::isfinite(value))
        Invalid("PipelineKey", "non-finite pipeline value");
    Number(key, std::bit_cast<std::uint32_t>(value == 0 ? 0.0F : value));
}
void Binding(std::string& key, const BindingLayoutEntry& e)
{
    Number(key, e.binding);
    Number(key, static_cast<unsigned>(e.type));
    Number(key, e.count);
    Number(key, static_cast<unsigned>(e.visibility));
    Number(key, e.dynamicOffset);
    Number(key, e.uniformBytes);
    Number(key, static_cast<unsigned>(e.textureDimension));
    Number(key, e.comparisonSampler);
}
void MatchShader(const ShaderDesc& shader, std::span<const ResourceSetLayoutDesc> layouts)
{
    for (const auto& need : shader.manifest.bindings)
    {
        if (need.set >= layouts.size())
            Invalid("CreatePipeline", "shader requires a missing logical set");
        const auto& entries = layouts[need.set].entries;
        const auto found =
            std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.binding == need.binding; });
        if (found == entries.end() || found->type != need.type || found->count != need.count ||
            !HasFlag(found->visibility, shader.stage) || found->uniformBytes < need.uniformBytes ||
            found->textureDimension != need.textureDimension || found->comparisonSampler != need.comparisonSampler)
            Invalid("CreatePipeline", "reflection and logical layout do not match");
    }
}
void InterfaceKey(std::string& key, const ShaderDesc& shader)
{
    Text(key, shader.semanticHash);
    Text(key, shader.entryPoint);
    Number(key, static_cast<unsigned>(shader.stage));
    auto bindings = shader.manifest.bindings;
    std::sort(bindings.begin(), bindings.end(),
              [](const auto& a, const auto& b) { return std::tie(a.set, a.binding) < std::tie(b.set, b.binding); });
    Number(key, bindings.size());
    for (const auto& b : bindings)
    {
        Number(key, b.set);
        BindingLayoutEntry e{
            b.binding, b.type, b.count, shader.stage, false, b.uniformBytes, b.textureDimension, b.comparisonSampler};
        Binding(key, e);
    }
    auto inputs = shader.manifest.vertexInputs;
    std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b)
              { return std::tie(a.semantic, a.semanticIndex) < std::tie(b.semantic, b.semanticIndex); });
    Number(key, inputs.size());
    for (const auto& v : inputs)
    {
        Number(key, static_cast<unsigned>(v.semantic));
        Number(key, v.semanticIndex);
        Number(key, static_cast<unsigned>(v.format));
    }
    Number(key, shader.manifest.colorOutputMask);
    Number(key, shader.manifest.writesDepth);
}
} // namespace
std::uint32_t VertexFormatBytes(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float2:
        return 8;
    case VertexFormat::Float3:
        return 12;
    case VertexFormat::Float4:
        return 16;
    }
    Invalid("VertexLayout", "unknown vertex format");
}
void ValidateResourceSetLayout(const ResourceSetLayoutDesc& layout)
{
    if (layout.set >= 3)
        Invalid("SetLayout", "set index exceeds M6 contract");
    std::set<std::uint16_t> seen;
    for (const auto& e : layout.entries)
    {
        const auto visibility = static_cast<unsigned>(e.visibility);
        if (!seen.insert(e.binding).second || e.count == 0 || e.count > 128 || e.type > BindingType::Sampler ||
            visibility == 0 || visibility > 3 || (e.dynamicOffset && e.type != BindingType::UniformBuffer))
            Invalid("SetLayout", "invalid binding type/count/visibility or duplicate binding");
        if (e.uniformBytes > 65536 || e.uniformBytes % 16 || (e.type != BindingType::UniformBuffer && e.uniformBytes))
            Invalid("SetLayout", "invalid uniform byte requirement");
        if (e.textureDimension > TextureDimension::TextureCube ||
            (e.type != BindingType::SampledTexture && e.textureDimension != TextureDimension::Texture2D) ||
            (e.type != BindingType::Sampler && e.comparisonSampler))
            Invalid("SetLayout", "inapplicable texture/sampler requirement");
    }
}
void ValidateShaderDesc(const ShaderDesc& shader)
{
    if (shader.bytecode.empty() || shader.entryPoint.empty() || !IsHash(shader.sourceHash) ||
        !IsHash(shader.semanticHash) || (shader.stage != ShaderStage::Vertex && shader.stage != ShaderStage::Pixel))
        Invalid("ShaderManifest", "missing bytecode, entry, SHA-256 identity or graphics stage");
    std::set<std::pair<std::uint8_t, std::uint16_t>> seen;
    for (const auto& b : shader.manifest.bindings)
    {
        if (!seen.emplace(b.set, b.binding).second)
            Invalid("ShaderManifest", "duplicate logical requirement");
        ResourceSetLayoutDesc l{b.set,
                                {{b.binding, b.type, b.count, shader.stage, false, b.uniformBytes, b.textureDimension,
                                  b.comparisonSampler}},
                                ""};
        ValidateResourceSetLayout(l);
        if (b.type == BindingType::UniformBuffer && b.uniformBytes == 0)
            Invalid("ShaderManifest", "reflection omitted constant buffer size");
    }
    std::set<std::pair<VertexSemantic, std::uint8_t>> inputs;
    for (const auto& input : shader.manifest.vertexInputs)
    {
        (void)VertexFormatBytes(input.format);
        if (input.semantic > VertexSemantic::Color || !inputs.emplace(input.semantic, input.semanticIndex).second)
            Invalid("ShaderManifest", "invalid or duplicate vertex input");
    }
    if (shader.manifest.colorOutputMask > 15 ||
        (shader.stage == ShaderStage::Vertex && (shader.manifest.colorOutputMask || shader.manifest.writesDepth)) ||
        (shader.stage == ShaderStage::Pixel && !shader.manifest.vertexInputs.empty()))
        Invalid("ShaderManifest", "stage and interface signature disagree");
}
void ValidateGraphicsPipeline(const GraphicsPipelineDesc& p, const ShaderDesc& vs, const ShaderDesc* ps,
                              std::span<const ResourceSetLayoutDesc> layouts, const RhiCapabilities& caps)
{
    // owner 保存的 shader 已在创建时验证 bytecode；此处只使用稳定复制的 manifest。
    if (vs.stage != ShaderStage::Vertex || (ps && ps->stage != ShaderStage::Pixel))
        Invalid("CreatePipeline", "shader stages are wrong");
    if (layouts.size() > 3)
        Invalid("CreatePipeline", "too many sets");
    for (std::size_t i = 0; i < layouts.size(); ++i)
    {
        ValidateResourceSetLayout(layouts[i]);
        if (layouts[i].set != i)
            Invalid("CreatePipeline", "set order mismatch");
    }
    MatchShader(vs, layouts);
    if (ps)
        MatchShader(*ps, layouts);
    if (p.topology != PrimitiveTopology::TriangleList || p.cullMode > CullMode::Back ||
        p.frontFace > FrontFace::CounterClockwise || p.depthCompare > CompareOp::Always ||
        !std::isfinite(p.depthBiasClamp) || !std::isfinite(p.slopeScaledDepthBias))
        Invalid("CreatePipeline", "unsupported topology/raster/depth value");
    if (p.sampleCount != 1 || p.colorAttachmentCount > 4 || p.colorAttachmentCount > caps.maxColorAttachments ||
        (p.colorAttachmentCount == 0 && p.depthFormat == Format::Unknown))
        Invalid("CreatePipeline", "invalid attachment count/sample count");
    for (std::size_t i = 0; i < p.colorFormats.size(); ++i)
    {
        if (i >= p.colorAttachmentCount)
        {
            if (p.colorFormats[i] != Format::Unknown)
                Invalid("CreatePipeline", "unused color format must be Unknown");
            continue;
        }
        if (p.colorFormats[i] == Format::Unknown || p.colorFormats[i] == Format::D32Float ||
            p.colorFormats[i] == Format::D24UnormS8Uint || !caps.SupportsColorAttachment(p.colorFormats[i]))
            Invalid("CreatePipeline", "unsupported color attachment format");
    }
    if (p.depthFormat != Format::Unknown &&
        ((p.depthFormat != Format::D32Float && p.depthFormat != Format::D24UnormS8Uint) ||
         !caps.SupportsDepthAttachment(p.depthFormat)))
        Invalid("CreatePipeline", "unsupported depth format");
    if ((p.depthTest || p.depthWrite) && p.depthFormat == Format::Unknown)
        Invalid("CreatePipeline", "depth state requires depth attachment");
    if (p.depthWrite && !p.depthTest)
        Invalid("CreatePipeline", "depth writes require depth test in M6");
    const unsigned outputs = ps ? ps->manifest.colorOutputMask : 0;
    if (outputs != ((1U << p.colorAttachmentCount) - 1U) ||
        (ps && ps->manifest.writesDepth && p.depthFormat == Format::Unknown))
        Invalid("CreatePipeline", "pixel output signature does not match attachments");
    if (p.alphaBlend && p.colorAttachmentCount == 0)
        Invalid("CreatePipeline", "alpha blending needs color output");
    std::set<std::pair<VertexSemantic, std::uint8_t>> attrs;
    for (const auto& a : p.vertexAttributes)
    {
        const auto bytes = VertexFormatBytes(a.format);
        if (a.semantic > VertexSemantic::Color || a.bufferSlot >= 16 || a.offset % 4 ||
            !attrs.emplace(a.semantic, a.semanticIndex).second)
            Invalid("CreatePipeline", "invalid vertex attribute");
        auto input = std::find_if(vs.manifest.vertexInputs.begin(), vs.manifest.vertexInputs.end(), [&](const auto& v)
                                  { return v.semantic == a.semantic && v.semanticIndex == a.semanticIndex; });
        if (input == vs.manifest.vertexInputs.end() || input->format != a.format)
            Invalid("CreatePipeline", "vertex layout does not match reflection");
        for (const auto& b : p.vertexAttributes)
            if (&a != &b && a.bufferSlot == b.bufferSlot &&
                (a.instanceStepRate != b.instanceStepRate ||
                 (a.offset < b.offset + VertexFormatBytes(b.format) && b.offset < a.offset + bytes)))
                Invalid("CreatePipeline", "overlapping vertex attributes or conflicting instance step rates");
    }
    if (attrs.size() != vs.manifest.vertexInputs.size())
        Invalid("CreatePipeline", "missing reflected vertex input");
}
std::string SemanticKey(const ResourceSetLayoutDesc& layout)
{
    ValidateResourceSetLayout(layout);
    std::string key = "M6.SetLayout.v1";
    Number(key, layout.set);
    auto entries = layout.entries;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
    Number(key, entries.size());
    for (const auto& e : entries)
        Binding(key, e);
    return key;
}
std::string PipelineLayoutKey(std::span<const ResourceSetLayoutDesc> layouts)
{
    if (layouts.size() > 3)
        Invalid("PipelineLayoutKey", "too many sets");
    std::string key = "M6.PipelineLayout.v1";
    Number(key, layouts.size());
    for (std::size_t i = 0; i < layouts.size(); ++i)
    {
        if (layouts[i].set != i)
            Invalid("PipelineLayoutKey", "set order mismatch");
        Text(key, SemanticKey(layouts[i]));
    }
    return key;
}
std::string PipelineSemanticKey(const GraphicsPipelineDesc& p, const ShaderDesc& vs, const ShaderDesc* ps,
                                std::span<const ResourceSetLayoutDesc> layouts)
{
    std::string key = "M6.GraphicsPipeline.v1";
    InterfaceKey(key, vs);
    Number(key, ps != nullptr);
    if (ps)
        InterfaceKey(key, *ps);
    Text(key, PipelineLayoutKey(layouts));
    auto attrs = p.vertexAttributes;
    std::sort(attrs.begin(), attrs.end(), [](const auto& a, const auto& b)
              { return std::tie(a.semantic, a.semanticIndex) < std::tie(b.semantic, b.semanticIndex); });
    Number(key, attrs.size());
    for (const auto& a : attrs)
    {
        Number(key, static_cast<unsigned>(a.semantic));
        Number(key, a.semanticIndex);
        Number(key, static_cast<unsigned>(a.format));
        Number(key, a.offset);
        Number(key, a.bufferSlot);
        Number(key, a.instanceStepRate);
    }
    Number(key, static_cast<unsigned>(p.topology));
    Number(key, p.colorAttachmentCount);
    for (auto f : p.colorFormats)
        Number(key, static_cast<unsigned>(f));
    Number(key, static_cast<unsigned>(p.depthFormat));
    Number(key, p.sampleCount);
    Number(key, static_cast<unsigned>(p.cullMode));
    Number(key, static_cast<unsigned>(p.frontFace));
    Number(key, p.depthClip);
    Number(key, static_cast<std::uint32_t>(p.depthBias));
    Float(key, p.depthBiasClamp);
    Float(key, p.slopeScaledDepthBias);
    Number(key, p.depthTest);
    Number(key, p.depthWrite);
    Number(key, static_cast<unsigned>(p.depthCompare));
    Number(key, p.alphaBlend);
    return key;
}
std::uint64_t HashSemanticKey(std::string_view key)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : key)
    {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}
std::uint64_t SemanticHash(const ResourceSetLayoutDesc& layout)
{
    return HashSemanticKey(SemanticKey(layout));
}
bool LayoutCompatible(const ResourceSetLayoutDesc& a, const ResourceSetLayoutDesc& b)
{
    return SemanticKey(a) == SemanticKey(b);
}
} // namespace MiniEngine::Rhi
