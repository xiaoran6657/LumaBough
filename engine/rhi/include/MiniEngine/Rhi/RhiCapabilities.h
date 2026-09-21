#pragma once

// M6-02 公共值契约；对象创建/图执行仍由后续 adapter 接线。
// descriptor 可按值准备；span/string_view 仅在消费调用期间借用。

#include <MiniEngine/Rhi/RhiTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace MiniEngine::Rhi
{
// formatSupport 表位掩码；backend 在启动查询（CheckFormatSupport /
// CheckFeatureSupport）后填充，默认全 0 = 全不支持（fail fast）。
inline constexpr std::uint8_t kFormatSupportSampled = 1U << 0U;
inline constexpr std::uint8_t kFormatSupportColor = 1U << 1U;
inline constexpr std::uint8_t kFormatSupportDepth = 1U << 2U;
inline constexpr std::uint8_t kFormatSupportCopySource = 1U << 3U;
inline constexpr std::uint8_t kFormatSupportCopyDestination = 1U << 4U;

struct RhiCapabilities final
{
    // Format::Unknown + 6 种 M6 格式；与 RhiTypes.h 的 Format 枚举顺序一致。
    static constexpr std::size_t kFormatCount = static_cast<std::size_t>(Format::Count);

    RhiBackend backend = RhiBackend::D3D12;
    std::string adapterName;
    std::uint64_t adapterLuid = 0;
    std::string driverVersion;
    std::uint32_t maxColorAttachments = 1;
    std::uint32_t maxTextureDimension2D = 1;
    std::uint32_t uniformBufferOffsetAlignment = 256;
    float maxAnisotropy = 1.0F;
    bool gpuTimestamps = false;
    std::uint64_t timestampFrequency = 0;
    bool depthComparisonSampling = false;
    bool debugLayerEnabled = false;
    bool gpuValidationEnabled = false;
    // 每个元素 = 上述 kFormatSupport* 位的组合，按 Format 枚举下标索引。
    std::array<std::uint8_t, kFormatCount> formatSupport{};
    // cube 必须单独查询，不能由 Texture2D 支持推断。
    std::array<std::uint8_t, kFormatCount> cubeFormatSupport{};

    [[nodiscard]] bool SupportsSampled(Format format, TextureDimension dimension = TextureDimension::Texture2D) const
    {
        return HasFormatSupport(format, kFormatSupportSampled, dimension);
    }

    [[nodiscard]] bool SupportsColorAttachment(Format format,
                                               TextureDimension dimension = TextureDimension::Texture2D) const
    {
        return HasFormatSupport(format, kFormatSupportColor, dimension);
    }

    [[nodiscard]] bool SupportsDepthAttachment(Format format,
                                               TextureDimension dimension = TextureDimension::Texture2D) const
    {
        return HasFormatSupport(format, kFormatSupportDepth, dimension);
    }

    [[nodiscard]] bool SupportsCopySource(Format format, TextureDimension dimension = TextureDimension::Texture2D) const
    {
        return HasFormatSupport(format, kFormatSupportCopySource, dimension);
    }

    [[nodiscard]] bool SupportsCopyDestination(Format format,
                                               TextureDimension dimension = TextureDimension::Texture2D) const
    {
        return HasFormatSupport(format, kFormatSupportCopyDestination, dimension);
    }

  private:
    [[nodiscard]] bool HasFormatSupport(Format format, std::uint8_t bits, TextureDimension dimension) const
    {
        const std::size_t index = static_cast<std::size_t>(format);
        if (format == Format::Unknown || index >= kFormatCount)
        {
            return false;
        }
        if (dimension == TextureDimension::Texture2D)
        {
            return (formatSupport[index] & bits) != 0;
        }
        if (dimension == TextureDimension::TextureCube)
        {
            return (cubeFormatSupport[index] & bits) != 0;
        }
        return false;
    }
};
} // namespace MiniEngine::Rhi
