#pragma once

// M6-02 公共值契约；对象创建/图执行仍由后续 adapter 接线。
// descriptor 可按值准备；span/string_view 仅在消费调用期间借用。

#include <cstdint>
#include <string_view>

namespace MiniEngine::Rhi
{
enum class RhiBackend : std::uint8_t
{
    D3D11,
    D3D12
};

enum class Format : std::uint8_t
{
    Unknown,
    Rgba8Unorm,
    Rgba8UnormSrgb,
    Rg16Float,
    Rgba16Float,
    R32Float,
    D32Float,
    D24UnormS8Uint,
    Count
};

// 顶点存储与纹理格式分离；对应现有 PBR 的 float2/3/4 输入。
enum class VertexFormat : std::uint8_t
{
    Float2,
    Float3,
    Float4
};

enum class VertexSemantic : std::uint8_t
{
    Position,
    Normal,
    Tangent,
    TexCoord,
    Color
};

enum class TextureDimension : std::uint8_t
{
    Texture2D,
    TextureCube
};

enum class ShaderStage : std::uint8_t
{
    Vertex = 1U << 0U,
    Pixel = 1U << 1U
};

constexpr ShaderStage operator|(ShaderStage left, ShaderStage right)
{
    return static_cast<ShaderStage>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

constexpr bool HasFlag(ShaderStage value, ShaderStage flag)
{
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

enum class ResourceAccess : std::uint8_t
{
    None,
    SampledRead,
    ColorWrite,
    DepthRead,
    DepthWrite,
    CopySource,
    CopyDestination,
    Present,
    VertexRead,
    IndexRead,
    UniformRead
};

enum class LoadOp : std::uint8_t
{
    Load,
    Clear,
    DontCare
};

enum class StoreOp : std::uint8_t
{
    Store,
    DontCare
};

enum class IndexType : std::uint8_t
{
    UInt16,
    UInt32
};

enum class PrimitiveTopology : std::uint8_t
{
    TriangleList
};

enum class Filter : std::uint8_t
{
    Nearest,
    Linear,
    Anisotropic
};

enum class AddressMode : std::uint8_t
{
    Repeat,
    Clamp,
    Border
};

enum class CompareOp : std::uint8_t
{
    Never,
    Less,
    LessEqual,
    Equal,
    GreaterEqual,
    Greater,
    Always
};

enum class CullMode : std::uint8_t
{
    None,
    Front,
    Back
};

enum class FrontFace : std::uint8_t
{
    Clockwise,
    CounterClockwise
};

[[nodiscard]] constexpr std::string_view ToString(RhiBackend backend)
{
    switch (backend)
    {
    case RhiBackend::D3D11:
        return "d3d11";
    case RhiBackend::D3D12:
        return "d3d12";
    }
    return "unknown";
}
} // namespace MiniEngine::Rhi
