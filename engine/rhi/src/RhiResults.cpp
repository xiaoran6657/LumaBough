#include <MiniEngine/Rhi/RhiResults.h>
#include <algorithm>
#include <limits>
#include <locale>
#include <sstream>

namespace MiniEngine::Rhi
{
TextureReadbackResult NormalizeRgba8Readback(std::span<const std::byte> bytes, std::uint64_t rowPitch, Extent2D extent,
                                             Format format)
{
    const std::uint64_t packed = static_cast<std::uint64_t>(extent.width) * 4;
    if (extent.width == 0 || extent.height == 0 || rowPitch < packed ||
        (format != Format::Rgba8Unorm && format != Format::Rgba8UnormSrgb) ||
        (extent.height > 1 && rowPitch > (std::numeric_limits<std::uint64_t>::max() - packed) / (extent.height - 1)) ||
        rowPitch * (extent.height - 1) + packed > bytes.size() ||
        packed > std::numeric_limits<std::size_t>::max() / extent.height)
        throw RhiValidationError({RhiErrorCode::InvalidArgument, "NormalizeRgba8Readback", "Readback", "", "",
                                  "invalid or truncated RGBA8 rows"});
    TextureReadbackResult result;
    result.extent = extent;
    result.format = Format::Rgba8Unorm;
    result.rowPitch = packed;
    result.bytes.resize(static_cast<std::size_t>(packed * extent.height));
    for (std::uint32_t row = 0; row < extent.height; ++row)
        std::copy_n(bytes.data() + static_cast<std::size_t>(row * rowPitch), static_cast<std::size_t>(packed),
                    result.bytes.data() + static_cast<std::size_t>(row * packed));
    return result;
}
std::optional<double> TimestampDeltaSeconds(const TimestampResult& begin, const TimestampResult& end)
{
    if (begin.unavailable || end.unavailable || begin.disjoint || end.disjoint || begin.frequency == 0 ||
        begin.frequency != end.frequency || begin.frameSerial == 0 || begin.frameSerial != end.frameSerial ||
        begin.validBits == 0 || begin.validBits > 64 || begin.validBits != end.validBits)
        return std::nullopt;
    const auto mask =
        begin.validBits == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << begin.validBits) - 1;
    if (begin.ticks > mask || end.ticks > mask)
        return std::nullopt;
    const auto delta = (end.ticks - begin.ticks) & mask;
    return static_cast<double>(delta) / static_cast<double>(begin.frequency);
}
std::string TimestampResultJson(const std::optional<TimestampResult>& value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto result = value.value_or(TimestampResult{0, 0, 0, false, 0, true});
    out << "{\"schema\":\"miniengine.timestamp.v1\",\"ready\":" << (value ? "true" : "false")
        << ",\"unavailable\":" << (result.unavailable ? "true" : "false") << ",\"ticks\":" << result.ticks
        << ",\"frequency\":" << result.frequency << ",\"validBits\":" << static_cast<unsigned>(result.validBits)
        << ",\"frameSerial\":" << result.frameSerial << ",\"disjoint\":" << (result.disjoint ? "true" : "false") << '}';
    return out.str();
}
std::string TextureReadbackResultJson(const std::optional<TextureReadbackResult>& value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto* result = value ? &*value : nullptr;
    out << "{\"schema\":\"miniengine.readback.v1\",\"ready\":" << (result ? "true" : "false")
        << ",\"unavailable\":" << (!result || result->unavailable ? "true" : "false")
        << ",\"format\":\"RGBA8\",\"width\":" << (result ? result->extent.width : 0)
        << ",\"height\":" << (result ? result->extent.height : 0) << ",\"rowPitch\":" << (result ? result->rowPitch : 0)
        << ",\"byteCount\":" << (result ? result->bytes.size() : 0)
        << ",\"frameSerial\":" << (result ? result->frameSerial : 0)
        << ",\"source\":{\"index\":" << (result ? result->source.Index() : 0)
        << ",\"generation\":" << (result ? result->source.Generation() : 0)
        << ",\"owner\":" << (result ? result->source.Owner() : 0) << "}}";
    return out.str();
}
} // namespace MiniEngine::Rhi
