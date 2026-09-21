#include <MiniEngine/Rhi/RhiValidation.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace MiniEngine::Rhi
{
namespace
{
[[noreturn]] void Reject(RhiErrorCode code, const char* operation, const char* type, const std::string& name,
                         std::string_view backend, const char* message)
{
    throw RhiValidationError(RhiError{code, operation, type, name, std::string(backend), message});
}
constexpr std::uint32_t kKnownBufferUsage = 31;
constexpr std::uint32_t kKnownTextureUsage = 31;
bool Known(Format value)
{
    return value > Format::Unknown && value < Format::Count;
}
bool Known(AddressMode value)
{
    return value >= AddressMode::Repeat && value <= AddressMode::Border;
}
bool Known(CompareOp value)
{
    return value >= CompareOp::Never && value <= CompareOp::Always;
}
bool Known(Filter value)
{
    return value >= Filter::Nearest && value <= Filter::Anisotropic;
}

// 同一字段清单驱动二进制 semantic key 与诊断 JSON，避免漏字段或读取对象 padding。
template <class Sink> void Fields(const BufferDesc& d, Sink& s)
{
    s.Add("size", d.size);
    s.Add("usage", d.usage);
    s.Add("memory", d.memory);
}
template <class Sink> void Fields(const TextureDesc& d, Sink& s)
{
    s.Add("dimension", d.dimension);
    s.Add("width", d.extent.width);
    s.Add("height", d.extent.height);
    s.Add("mipLevels", d.mipLevels);
    s.Add("arrayLayers", d.arrayLayers);
    s.Add("sampleCount", d.sampleCount);
    s.Add("format", d.format);
    s.Add("usage", d.usage);
    constexpr std::array names{"clearColor0", "clearColor1", "clearColor2", "clearColor3"};
    for (std::size_t i = 0; i < names.size(); ++i)
        s.Add(names[i], d.clearColorHint[i]);
    s.Add("clearDepth", d.clearDepthHint);
    s.Add("clearStencil", d.clearStencilHint);
}
template <class Sink> void Fields(const SamplerDesc& d, Sink& s)
{
    s.Add("minMagFilter", d.minMagFilter);
    s.Add("mipFilter", d.mipFilter);
    s.Add("addressU", d.addressU);
    s.Add("addressV", d.addressV);
    s.Add("addressW", d.addressW);
    s.Add("comparisonEnabled", d.comparisonEnabled);
    s.Add("comparison", d.comparison);
    s.Add("maxAnisotropy", d.maxAnisotropy);
    s.Add("minLod", d.minLod);
    s.Add("maxLod", d.maxLod);
    constexpr std::array names{"borderColor0", "borderColor1", "borderColor2", "borderColor3"};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        s.Add(names[i], d.borderColor[i]);
    }
}
struct BinaryKey final
{
    std::string bytes;
    void Integer(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
        {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }
    void Name(std::string_view name)
    {
        Integer(name.size());
        bytes.append(name);
    }
    template <class T> void Add(std::string_view name, T value)
    {
        Name(name);
        Integer(static_cast<std::uint64_t>(value));
    }
    void Add(std::string_view name, float value)
    {
        if (!std::isfinite(value))
        {
            Reject(RhiErrorCode::InvalidArgument, "SemanticHash", "Sampler", "", "", "non-finite semantic field");
        }
        Name(name);
        Integer(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
    }
};
std::string Quote(std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        else if (c < 32)
        {
            out += "\\u00";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
        else
        {
            out.push_back(static_cast<char>(c));
        }
    }
    out += '"';
    return out;
}
struct Json final
{
    std::string value = "{\"schemaVersion\":1";
    template <class T> void Add(std::string_view name, T number)
    {
        value += ',' + Quote(name) + ':' + std::to_string(static_cast<std::uint64_t>(number));
    }
    void Add(std::string_view name, float number)
    {
        value += ',' + Quote(name) + ':';
        if (!std::isfinite(number))
        {
            value += Quote(std::isnan(number) ? "NaN" : (number < 0 ? "-Infinity" : "Infinity"));
            return;
        }
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number == 0.0F ? 0.0F : number,
                                          std::chars_format::general, std::numeric_limits<float>::max_digits10);
        value.append(buffer.data(), result.ptr);
    }
    void Add(std::string_view name, bool number)
    {
        value += ',' + Quote(name) + (number ? ":true" : ":false");
    }
};
template <class T> std::string Key(const T& d)
{
    BinaryKey sink;
    // 明确 schema 与类型，跨类型相同数值字段不能共用缓存身份。
    if constexpr (std::is_same_v<T, BufferDesc>)
    {
        sink.Name("BufferDesc/v1");
    }
    else if constexpr (std::is_same_v<T, TextureDesc>)
    {
        sink.Name("TextureDesc/v2");
    }
    else
    {
        sink.Name("SamplerDesc/v1");
    }
    Fields(d, sink);
    return sink.bytes;
}
template <class T> std::uint64_t Hash(const T& d)
{
    std::uint64_t result = 14695981039346656037ULL;
    for (unsigned char byte : Key(d))
    {
        result = (result ^ byte) * 1099511628211ULL;
    }
    return result;
}
template <class T> std::string Diagnostic(const T& d)
{
    Json sink;
    if constexpr (std::is_same_v<T, TextureDesc>)
        sink.value = "{\"schemaVersion\":2";
    Fields(d, sink);
    sink.value += ",\"debugName\":" + Quote(d.debugName) + '}';
    return sink.value;
}
} // namespace
bool CheckedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
        return false;
    }
    result = left + right;
    return true;
}

std::uint32_t MaximumMipLevels(const TextureDesc& d)
{
    std::uint32_t largestExtent = std::max(d.extent.width, d.extent.height);
    std::uint32_t levels = 1;
    while (largestExtent > 1)
    {
        largestExtent /= 2;
        ++levels;
    }
    return levels;
}

std::uint32_t BytesPerPixel(Format format)
{
    switch (format)
    {
    case Format::Rgba8Unorm:
    case Format::Rgba8UnormSrgb:
    case Format::Rg16Float:
    case Format::R32Float:
    case Format::D24UnormS8Uint:
    case Format::D32Float:
        return 4;
    case Format::Rgba16Float:
        return 8;
    case Format::Unknown:
    case Format::Count:
        break;
    }
    return 0;
}

void ValidateTextureUploadDesc(const TextureDesc& d)
{
    auto invalid = [&](const char* text)
    { Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "", text); };
    auto unsupported = [&](const char* text)
    { Reject(RhiErrorCode::Unsupported, "ValidateTextureUpload", "Texture", d.debugName, "", text); };

    // Validate every enum/count before deriving mip dimensions. This keeps malformed input
    // away from any shift-like dimension calculation and gives a structured RHI error.
    if (d.dimension != TextureDimension::Texture2D && d.dimension != TextureDimension::TextureCube)
    {
        invalid("unknown texture dimension");
    }
    if (d.extent.width == 0 || d.extent.height == 0 || d.mipLevels == 0 || d.arrayLayers == 0)
    {
        invalid("zero texture extent, mip, or layer count");
    }
    if (d.sampleCount != 1)
    {
        unsupported("M6 texture upload requires single-sample textures");
    }
    if (BytesPerPixel(d.format) == 0)
    {
        invalid("unknown texture format");
    }
    const auto usage = static_cast<std::uint32_t>(d.usage);
    if (usage == 0 || (usage & ~kKnownTextureUsage) != 0)
    {
        invalid("unknown or empty texture usage");
    }
    if (!HasFlag(d.usage, TextureUsage::CopyDestination))
    {
        invalid("texture upload requires copy-destination usage");
    }
    if (d.dimension == TextureDimension::TextureCube && (d.arrayLayers != 6 || d.extent.width != d.extent.height))
    {
        invalid("cube requires six square layers");
    }
    if (d.dimension == TextureDimension::Texture2D && d.arrayLayers != 1)
    {
        unsupported("M6 does not support texture arrays");
    }
    if (d.mipLevels > MaximumMipLevels(d))
    {
        invalid("mip chain exceeds texture extent");
    }
    for (const float value : d.clearColorHint)
        if (!std::isfinite(value))
            invalid("clear color hint must be finite");
    if (!std::isfinite(d.clearDepthHint) || d.clearDepthHint < 0.0F || d.clearDepthHint > 1.0F)
        invalid("clear depth hint must be in [0,1]");
    const bool depth = HasFlag(d.usage, TextureUsage::DepthStencil);
    const bool color = HasFlag(d.usage, TextureUsage::ColorAttachment);
    if ((depth && (d.format != Format::D32Float && d.format != Format::D24UnormS8Uint)) || (depth && color) ||
        ((d.format == Format::D32Float || d.format == Format::D24UnormS8Uint) && color))
    {
        invalid("depth/color format and usage conflict");
    }
}

std::uint64_t SpanSize(std::span<const std::byte> bytes, const TextureDesc& d)
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
        {
            Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                   "texture subresource byte span is too large");
        }
    }
    return static_cast<std::uint64_t>(bytes.size());
}

void ValidateBufferDesc(const BufferDesc& d)
{
    auto invalid = [&](const char* text)
    { Reject(RhiErrorCode::InvalidArgument, "ValidateBufferDesc", "Buffer", d.debugName, "", text); };
    const auto usage = static_cast<std::uint32_t>(d.usage);
    if (d.size == 0 || usage == 0 || (usage & ~kKnownBufferUsage) != 0)
    {
        invalid("invalid buffer size/usage");
    }
    if (d.memory < MemoryDomain::GpuOnly || d.memory > MemoryDomain::GpuToCpu)
    {
        invalid("unknown memory domain");
    }
    if (d.size > std::numeric_limits<std::uint32_t>::max())
    {
        Reject(RhiErrorCode::Unsupported, "ValidateBufferDesc", "Buffer", d.debugName, "",
               "M6 buffer size exceeds shared 32-bit limit");
    }
    if (d.memory == MemoryDomain::GpuToCpu && d.usage != BufferUsage::CopyDestination)
    {
        invalid("readback buffer must be copy-destination only");
    }
    if (d.memory == MemoryDomain::CpuToGpu && HasFlag(d.usage, BufferUsage::CopyDestination))
    {
        invalid("CPU upload memory cannot be a GPU copy destination");
    }
    if (HasFlag(d.usage, BufferUsage::Uniform) &&
        (d.size % 16 != 0 || d.size > 65536 || HasFlag(d.usage, BufferUsage::Vertex | BufferUsage::Index)))
    {
        invalid("standalone uniform buffer requires 16-byte size, at most 64 KiB, and no vertex/index usage");
    }
}

void ValidateBufferInitialData(const BufferDesc& d, std::span<const std::byte> initialData)
{
    // Descriptor validation is deliberately first: callers always receive the same
    // structured RHI error for malformed descriptors, independent of the payload.
    ValidateBufferDesc(d);
    if (initialData.empty())
    {
        return;
    }
    if (d.memory == MemoryDomain::GpuToCpu)
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateBufferInitialData", "Buffer", d.debugName, "",
               "readback buffer cannot have initial data");
    }
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    {
        if (initialData.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
        {
            Reject(RhiErrorCode::InvalidArgument, "ValidateBufferInitialData", "Buffer", d.debugName, "",
                   "initial data span is too large");
        }
    }
    if (static_cast<std::uint64_t>(initialData.size()) > d.size)
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateBufferInitialData", "Buffer", d.debugName, "",
               "initial data exceeds buffer size");
    }
}

void ValidateBufferUpload(const BufferDesc& d, std::uint64_t offset, std::span<const std::byte> bytes)
{
    ValidateBufferDesc(d);
    if (bytes.empty())
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateBufferUpload", "Buffer", d.debugName, "",
               "buffer upload data cannot be empty");
    }
    if (d.memory == MemoryDomain::GpuToCpu)
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateBufferUpload", "Buffer", d.debugName, "",
               "readback buffer cannot be uploaded");
    }
    if (d.memory == MemoryDomain::GpuOnly && !HasFlag(d.usage, BufferUsage::CopyDestination))
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateBufferUpload", "Buffer", d.debugName, "",
               "GPU-only buffer upload requires copy-destination usage");
    }
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
        {
            Reject(RhiErrorCode::InvalidArgument, "ValidateBufferUpload", "Buffer", d.debugName, "",
                   "buffer upload span is too large");
        }
    }
    // Subtraction after offset <= size avoids forming an overflowing end offset.
    if (offset > d.size || static_cast<std::uint64_t>(bytes.size()) > d.size - offset)
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateBufferUpload", "Buffer", d.debugName, "",
               "buffer upload range is outside the destination");
    }
}

void ValidateTextureUpload(const TextureDesc& d, std::span<const TextureSubresourceData> subresources)
{
    ValidateTextureUploadDesc(d);
    const auto expectedCount = static_cast<std::uint64_t>(d.arrayLayers) * static_cast<std::uint64_t>(d.mipLevels);
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
    {
        if (expectedCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        {
            Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                   "texture subresource count is not representable");
        }
    }
    if (subresources.size() != static_cast<std::size_t>(expectedCount))
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
               "texture upload must provide every layer and mip subresource");
    }

    const auto bytesPerPixel = static_cast<std::uint64_t>(BytesPerPixel(d.format));
    for (std::uint64_t layer = 0; layer < d.arrayLayers; ++layer)
    {
        std::uint32_t mipWidth = d.extent.width;
        std::uint32_t mipHeight = d.extent.height;
        for (std::uint64_t mip = 0; mip < d.mipLevels; ++mip)
        {
            const auto index = layer * static_cast<std::uint64_t>(d.mipLevels) + mip;
            const auto& source = subresources[static_cast<std::size_t>(index)];
            std::uint64_t rowBytes = 0;
            if (!CheckedMultiply(static_cast<std::uint64_t>(mipWidth), bytesPerPixel, rowBytes))
            {
                Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                       "texture row byte count overflow");
            }
            if (source.rowPitch < rowBytes)
            {
                Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                       "texture row pitch is smaller than the packed row");
            }
            std::uint64_t precedingRows = 0;
            if (!CheckedMultiply(source.rowPitch, static_cast<std::uint64_t>(mipHeight - 1), precedingRows))
            {
                Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                       "texture row pitch footprint overflow");
            }
            std::uint64_t footprint = 0;
            if (!CheckedAdd(precedingRows, rowBytes, footprint))
            {
                Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                       "texture subresource footprint overflow");
            }
            if (source.slicePitch < footprint)
            {
                Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                       "texture slice pitch is smaller than the actual footprint");
            }
            if (SpanSize(source.bytes, d) < footprint)
            {
                Reject(RhiErrorCode::InvalidArgument, "ValidateTextureUpload", "Texture", d.debugName, "",
                       "texture bytes do not cover the actual footprint");
            }
            if (mipWidth > 1)
            {
                mipWidth /= 2;
            }
            if (mipHeight > 1)
            {
                mipHeight /= 2;
            }
        }
    }
}

static void ValidateTextureStructure(const TextureDesc& d, const std::string_view backend)
{
    auto invalid = [&](const char* text)
    { Reject(RhiErrorCode::InvalidArgument, "ValidateTextureDesc", "Texture", d.debugName, backend, text); };
    auto unsupported = [&](const char* text)
    { Reject(RhiErrorCode::Unsupported, "ValidateTextureDesc", "Texture", d.debugName, backend, text); };
    if (d.dimension != TextureDimension::Texture2D && d.dimension != TextureDimension::TextureCube)
    {
        invalid("unknown texture dimension");
    }
    if (d.extent.width == 0 || d.extent.height == 0 || d.mipLevels == 0 || d.arrayLayers == 0)
    {
        invalid("zero texture extent/mip/layer count");
    }
    if (d.dimension == TextureDimension::TextureCube && (d.arrayLayers != 6 || d.extent.width != d.extent.height))
    {
        invalid("cube requires six square layers");
    }
    if (d.dimension == TextureDimension::Texture2D && d.arrayLayers != 1)
    {
        unsupported("M6 does not support texture arrays");
    }
    if (!Known(d.format))
    {
        invalid("unknown texture format");
    }
    const auto usage = static_cast<std::uint32_t>(d.usage);
    if (usage == 0 || (usage & ~kKnownTextureUsage) != 0)
    {
        invalid("unknown or empty texture usage");
    }
    std::uint32_t maxMips = 1;
    for (auto extent = std::max(d.extent.width, d.extent.height); extent > 1; extent >>= 1)
    {
        ++maxMips;
    }
    if (d.mipLevels > maxMips)
    {
        invalid("mip chain exceeds texture extent");
    }
    if (d.sampleCount != 1)
    {
        unsupported("M6 only supports single-sample textures");
    }
    for (const float value : d.clearColorHint)
        if (!std::isfinite(value))
            invalid("clear color hint must be finite");
    if (!std::isfinite(d.clearDepthHint) || d.clearDepthHint < 0.0F || d.clearDepthHint > 1.0F)
        invalid("clear depth hint must be in [0,1]");
    const bool depth = HasFlag(d.usage, TextureUsage::DepthStencil);
    const bool color = HasFlag(d.usage, TextureUsage::ColorAttachment);
    if ((depth && ((d.format != Format::D32Float && d.format != Format::D24UnormS8Uint) || color)) ||
        ((d.format == Format::D32Float || d.format == Format::D24UnormS8Uint) && color))
    {
        invalid("depth/color format and usage conflict");
    }
}
void ValidateTextureDesc(const TextureDesc& d)
{
    ValidateTextureStructure(d, {});
}
void ValidateTextureDesc(const TextureDesc& d, const RhiCapabilities& c)
{
    ValidateTextureStructure(d, ToString(c.backend));
    auto unsupported = [&](const char* text)
    { Reject(RhiErrorCode::Unsupported, "ValidateTextureDesc", "Texture", d.debugName, ToString(c.backend), text); };
    const bool depth = HasFlag(d.usage, TextureUsage::DepthStencil);
    const bool color = HasFlag(d.usage, TextureUsage::ColorAttachment);
    if (d.extent.width > c.maxTextureDimension2D || d.extent.height > c.maxTextureDimension2D)
    {
        unsupported("texture exceeds device extent limit");
    }
    if (HasFlag(d.usage, TextureUsage::Sampled) && !c.SupportsSampled(d.format, d.dimension))
    {
        unsupported("format cannot be sampled");
    }
    if (color && !c.SupportsColorAttachment(d.format, d.dimension))
    {
        unsupported("format cannot be color attachment");
    }
    if (depth && !c.SupportsDepthAttachment(d.format, d.dimension))
    {
        unsupported("format cannot be depth attachment");
    }
    if (HasFlag(d.usage, TextureUsage::CopySource) && !c.SupportsCopySource(d.format, d.dimension))
    {
        unsupported("format cannot be copied from");
    }
    if (HasFlag(d.usage, TextureUsage::CopyDestination) && !c.SupportsCopyDestination(d.format, d.dimension))
    {
        unsupported("format cannot be copied to");
    }
}

void ValidateSamplerDesc(const SamplerDesc& d, const RhiCapabilities& c)
{
    auto invalid = [&](const char* text)
    {
        Reject(RhiErrorCode::InvalidArgument, "ValidateSamplerDesc", "Sampler", d.debugName, ToString(c.backend), text);
    };
    if (!Known(d.minMagFilter) || !Known(d.mipFilter) || !Known(d.addressU) || !Known(d.addressV) ||
        !Known(d.addressW) || !Known(d.comparison))
    {
        invalid("unknown sampler enum");
    }
    if (d.mipFilter == Filter::Anisotropic)
    {
        invalid("anisotropic applies to min/mag, not independent mip filtering");
    }
    if (!std::isfinite(d.maxAnisotropy) || !std::isfinite(d.minLod) || !std::isfinite(d.maxLod) ||
        d.maxAnisotropy < 1 || std::floor(d.maxAnisotropy) != d.maxAnisotropy || d.minLod > d.maxLod)
    {
        invalid("invalid sampler numeric range");
    }
    for (float value : d.borderColor)
    {
        if (!std::isfinite(value))
        {
            invalid("non-finite border color");
        }
    }
    if (d.minMagFilter != Filter::Anisotropic && d.maxAnisotropy != 1)
    {
        invalid("anisotropy requires anisotropic filter");
    }
    if (d.minMagFilter == Filter::Anisotropic && d.mipFilter != Filter::Linear)
    {
        invalid("anisotropic filter requires linear mip filtering");
    }
    if (!std::isfinite(c.maxAnisotropy) || d.maxAnisotropy > c.maxAnisotropy)
    {
        Reject(RhiErrorCode::Unsupported, "ValidateSamplerDesc", "Sampler", d.debugName, ToString(c.backend),
               "sampler exceeds device anisotropy");
    }
}

void RequireM6Capabilities(const RhiCapabilities& c)
{
    auto unsupported = [&](const char* text)
    { Reject(RhiErrorCode::Unsupported, "RequireM6Capabilities", "Device", c.adapterName, ToString(c.backend), text); };
    if (c.backend != RhiBackend::D3D11 && c.backend != RhiBackend::D3D12)
    {
        unsupported("unknown M6 backend");
    }
    if (c.maxTextureDimension2D < 2048 || c.maxColorAttachments < 1)
    {
        unsupported("M6 fixed shadow/HDR extent or attachment capability missing");
    }
    if (c.uniformBufferOffsetAlignment == 0 || !std::has_single_bit(c.uniformBufferOffsetAlignment))
    {
        unsupported("invalid uniform offset alignment");
    }
    if (!c.gpuTimestamps || c.timestampFrequency == 0)
    {
        unsupported("M6 timing requires available timestamps and frequency");
    }
    for (Format format : {Format::Rgba8Unorm, Format::Rgba8UnormSrgb, Format::Rg16Float, Format::Rgba16Float,
                          Format::R32Float, Format::D32Float})
    {
        if (!c.SupportsSampled(format))
        {
            unsupported("required sampled format missing");
        }
    }
    for (Format format : {Format::Rgba8Unorm, Format::Rg16Float, Format::Rgba16Float})
    {
        if (!c.SupportsColorAttachment(format))
        {
            unsupported("required color format missing");
        }
    }
    if (!c.SupportsDepthAttachment(Format::D32Float) || !c.SupportsCopySource(Format::Rgba8Unorm))
    {
        unsupported("shadow depth or screenshot copy capability missing");
    }
    if (!c.depthComparisonSampling || !c.SupportsSampled(Format::Rgba16Float, TextureDimension::TextureCube) ||
        !c.SupportsColorAttachment(Format::Rgba16Float, TextureDimension::TextureCube))
    {
        unsupported("shadow comparison or IBL cube capability missing");
    }
    for (Format format : {Format::Rgba8Unorm, Format::Rgba8UnormSrgb, Format::Rgba16Float})
    {
        if (!c.SupportsCopyDestination(format))
        {
            unsupported("asset texture upload copy capability missing");
        }
    }
    // 固定生产 profile 的 s0 为 linear/wrap；anisotropy 是可选能力。
    SamplerDesc sampler;
    ValidateSamplerDesc(sampler, c);
}

RhiCapabilityAssessment AssessM6Capabilities(const RhiCapabilities& capabilities)
{
    try
    {
        RequireM6Capabilities(capabilities);
        return {RhiCapabilityStatus::Ready, std::nullopt};
    }
    catch (const RhiValidationError& error)
    {
        return {RhiCapabilityStatus::Blocked, error.Error()};
    }
}

std::uint64_t SemanticHash(const BufferDesc& d)
{
    return Hash(d);
}
std::uint64_t SemanticHash(const TextureDesc& d)
{
    return Hash(d);
}
std::uint64_t SemanticHash(const SamplerDesc& d)
{
    return Hash(d);
}
bool SemanticallyEqual(const BufferDesc& a, const BufferDesc& b)
{
    return Key(a) == Key(b);
}
bool SemanticallyEqual(const TextureDesc& a, const TextureDesc& b)
{
    return Key(a) == Key(b);
}
bool SemanticallyEqual(const SamplerDesc& a, const SamplerDesc& b)
{
    return Key(a) == Key(b);
}
std::string ToDiagnosticJson(const BufferDesc& d)
{
    return Diagnostic(d);
}
std::string ToDiagnosticJson(const TextureDesc& d)
{
    return Diagnostic(d);
}
std::string ToDiagnosticJson(const SamplerDesc& d)
{
    return Diagnostic(d);
}
} // namespace MiniEngine::Rhi
