#pragma once

// M6-02 公共值契约；对象创建/图执行仍由后续 adapter 接线。
// descriptor 可按值准备；span/string_view 仅在消费调用期间借用。

#include <MiniEngine/Rhi/RhiHandle.h>
#include <MiniEngine/Rhi/RhiTypes.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Rhi
{
enum class BufferUsage : std::uint32_t
{
    None = 0,
    Vertex = 1U << 0U,
    Index = 1U << 1U,
    Uniform = 1U << 2U,
    CopySource = 1U << 3U,
    CopyDestination = 1U << 4U
};

constexpr BufferUsage operator|(BufferUsage left, BufferUsage right)
{
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr bool HasFlag(BufferUsage value, BufferUsage flag)
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

enum class TextureUsage : std::uint32_t
{
    None = 0,
    Sampled = 1U << 0U,
    ColorAttachment = 1U << 1U,
    DepthStencil = 1U << 2U,
    CopySource = 1U << 3U,
    CopyDestination = 1U << 4U
};

constexpr TextureUsage operator|(TextureUsage left, TextureUsage right)
{
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr bool HasFlag(TextureUsage value, TextureUsage flag)
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

enum class MemoryDomain : std::uint8_t
{
    GpuOnly,
    CpuToGpu,
    GpuToCpu
};

struct Extent2D final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    auto operator<=>(const Extent2D&) const = default;
};

struct BufferDesc final
{
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryDomain memory = MemoryDomain::GpuOnly;
    std::string debugName;
    bool operator==(const BufferDesc&) const = default;
};

struct TextureDesc final
{
    TextureDimension dimension = TextureDimension::Texture2D;
    Extent2D extent;
    std::uint16_t mipLevels = 1;
    std::uint16_t arrayLayers = 1;
    std::uint8_t sampleCount = 1;
    Format format = Format::Unknown;
    TextureUsage usage = TextureUsage::None;
    std::string debugName;
    // 附件分配的 clear 优化提示；不初始化内容，也不改变 LoadOp 的定义性。
    // D3D11 可忽略；D3D12 用于 native optimized clear value。
    std::array<float, 4> clearColorHint{0, 0, 0, 1};
    float clearDepthHint = 1.0F;
    std::uint8_t clearStencilHint = 0;
    bool operator==(const TextureDesc&) const = default;
};

struct BufferView final
{
    BufferHandle buffer;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct SamplerDesc final
{
    Filter minMagFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    bool comparisonEnabled = false;
    CompareOp comparison = CompareOp::LessEqual;
    float maxAnisotropy = 1.0F;
    float minLod = 0.0F;
    float maxLod = 1000.0F;
    std::array<float, 4> borderColor{};
    std::string debugName;
    bool operator==(const SamplerDesc&) const = default;
};

enum class BindingType : std::uint8_t
{
    UniformBuffer,
    SampledTexture,
    Sampler
};

struct BindingLayoutEntry final
{
    std::uint16_t binding = 0;
    BindingType type = BindingType::UniformBuffer;
    std::uint16_t count = 1;
    ShaderStage visibility = ShaderStage::Vertex;
    bool dynamicOffset = false;
    std::uint32_t uniformBytes = 0; // 反射要求的最小常量区间；0 表示 layout 不额外限制。
    TextureDimension textureDimension = TextureDimension::Texture2D;
    bool comparisonSampler = false;
    bool operator==(const BindingLayoutEntry&) const = default;
};

struct ResourceSetLayoutDesc final
{
    std::uint8_t set = 0;
    std::vector<BindingLayoutEntry> entries;
    std::string debugName;
};

struct ResourceBinding final
{
    std::uint16_t binding = 0;
    std::uint16_t arrayElement = 0;
    BindingType type = BindingType::UniformBuffer;
    BufferView buffer;
    TextureHandle texture;
    SamplerHandle sampler;
};

struct ResourceSetDesc final
{
    ResourceSetLayoutHandle layout;
    std::vector<ResourceBinding> bindings;
    std::string debugName;
};

// 离线 reflection 已转换为逻辑 binding；这里不保存 register/root/table 数字。
struct ShaderBindingRequirement final
{
    std::uint8_t set = 0;
    std::uint16_t binding = 0;
    BindingType type = BindingType::UniformBuffer;
    std::uint16_t count = 1;
    std::uint32_t uniformBytes = 0;
    TextureDimension textureDimension = TextureDimension::Texture2D;
    bool comparisonSampler = false;
    bool operator==(const ShaderBindingRequirement&) const = default;
};
struct ShaderVertexInput final
{
    VertexSemantic semantic = VertexSemantic::Position;
    VertexFormat format = VertexFormat::Float3;
    std::uint8_t semanticIndex = 0;
    bool operator==(const ShaderVertexInput&) const = default;
};
struct ShaderInterface final
{
    std::vector<ShaderBindingRequirement> bindings;
    std::vector<ShaderVertexInput> vertexInputs; // 不含 SV_VertexID 等系统生成输入。
    std::uint8_t colorOutputMask = 0;
    bool writesDepth = false;
    bool operator==(const ShaderInterface&) const = default;
};
// bytecode 在调用期间借用；sourceHash 是源文件 SHA-256；semanticHash 绑定源闭包与逻辑接口。
// 高层用 assetId+stage 请求，变体由 loader 选择，pass 不拼后端路径。
struct ShaderDesc final
{
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const std::byte> bytecode;
    std::string semanticHash;
    std::string entryPoint;
    std::string debugName;
    std::string sourceHash;
    ShaderInterface manifest;
};

struct VertexAttributeDesc final
{
    VertexSemantic semantic = VertexSemantic::Position;
    VertexFormat format = VertexFormat::Float3;
    std::uint16_t offset = 0;
    std::uint8_t bufferSlot = 0;
    std::uint8_t instanceStepRate = 0;
    std::uint8_t semanticIndex = 0;
    bool operator==(const VertexAttributeDesc&) const = default;
};

struct PipelineLayoutDesc final
{
    std::array<ResourceSetLayoutHandle, 3> sets{};
    std::uint8_t setCount = 0;
    std::string debugName;
};

struct GraphicsPipelineDesc final
{
    ShaderHandle vertexShader;
    ShaderHandle pixelShader;
    PipelineLayoutHandle layout;
    std::vector<VertexAttributeDesc> vertexAttributes;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    std::array<Format, 4> colorFormats{};
    std::uint8_t colorAttachmentCount = 0;
    Format depthFormat = Format::Unknown;
    std::uint8_t sampleCount = 1;
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::Clockwise;
    bool depthClip = true;
    std::int32_t depthBias = 0;
    float depthBiasClamp = 0.0F;
    float slopeScaledDepthBias = 0.0F;
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompare = CompareOp::Less;
    bool alphaBlend = false;
    std::string debugName;
};

struct TextureSubresourceData final
{
    std::span<const std::byte> bytes;
    std::uint64_t rowPitch = 0;
    std::uint64_t slicePitch = 0;
};

struct DynamicBufferSlice final
{
    BufferView view;
    std::uint64_t frameSerial = 0;
};
} // namespace MiniEngine::Rhi
