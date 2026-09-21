// ============================================================================
// TextureMipBuilder.cpp — 确定性 CPU mip 链生成的实现
// 里程碑：M4-02
// 职责：实现 usage 角色化滤波。像素在处理时以 float（0..1）表示；sRGB 角色
//       （BaseColor/Emissive）在每对 tap 上做 sRGB→线性→平均→线性→sRGB 往返，
//       保证滤波发生在感知正确的线性域；Normal 角色把字节解码为向量后平均并
//       重归一化；其余 usage 全通道线性平均。
// 关联：tools/asset_cooker/src/TextureMipBuilder.h（契约）
// ============================================================================

#include "TextureMipBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace MiniEngine::Tools
{
namespace
{
constexpr float kSrgbCutoff = 0.0031308F;
constexpr float kSrgbSlope = 12.92F;
constexpr float kSrgbScale = 1.055F;
constexpr float kSrgbExponent = 1.0F / 2.4F;

// sRGB 传输函数（与 PbrMathTests 的 CPU 参考一致，05 篇 tone map 复用）。
float LinearToSrgb(const float linear)
{
    return linear <= kSrgbCutoff ? kSrgbSlope * linear : kSrgbScale * std::pow(linear, kSrgbExponent) - 0.055F;
}

float SrgbToLinear(const float encoded)
{
    return encoded <= 0.04045F ? encoded / kSrgbSlope : std::pow((encoded + 0.055F) / kSrgbScale, 2.4F);
}

std::byte EncodeByte(const float value)
{
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::byte>(static_cast<unsigned char>(clamped * 255.0F + 0.5F));
}

float DecodeByte(const std::byte value)
{
    return static_cast<float>(std::to_integer<unsigned char>(value)) / 255.0F;
}
} // namespace

bool BuildTextureMipChain(const std::uint32_t width, const std::uint32_t height, const TextureUsage usage,
                          const std::vector<std::byte>& topLevelRgba8, BuiltTextureMips& out, std::string& error)
{
    out.pixels.clear();
    out.levels.clear();

    // 与 TextureArtifactWriter 相同的防御：rowPitch 的 u32 表示 + 顶层字节数。
    if (width == 0 || height == 0)
    {
        error = "texture dimensions must be non-zero";
        return false;
    }
    if (width > (std::numeric_limits<std::uint32_t>::max() - 3U) / 4U)
    {
        error = "texture width overflow";
        return false;
    }
    const std::uint64_t topLevelPitch = static_cast<std::uint64_t>(width) * 4U;
    if (topLevelRgba8.size() != topLevelPitch * static_cast<std::uint64_t>(height))
    {
        error = "top-level rgba8 size does not equal width*4*height";
        return false;
    }
    if (usage == TextureUsage::HdrEnvironment)
    {
        // HDR 环境（RGBA16F）的 mip 链随 04 篇 IBL 一起接入；M4-02 只处理 RGBA8 角色。
        error = "HDR environment mip chain is not part of M4-02";
        return false;
    }

    const bool srgbDomain = usage == TextureUsage::BaseColor || usage == TextureUsage::Emissive;
    const bool normalDomain = usage == TextureUsage::Normal;

    std::uint32_t currentWidth = width;
    std::uint32_t currentHeight = height;
    std::vector<std::byte> current = topLevelRgba8;

    while (true)
    {
        const std::uint32_t levelPitch = currentWidth * 4U;
        const std::uint64_t levelBytes = static_cast<std::uint64_t>(levelPitch) * currentHeight;
        TextureMipLevel level;
        level.offset = out.pixels.size();
        level.rowPitch = levelPitch;
        level.byteSize = static_cast<std::uint32_t>(levelBytes);
        out.levels.push_back(level);
        out.pixels.insert(out.pixels.end(), current.begin(), current.end());

        if (currentWidth == 1 && currentHeight == 1)
        {
            break; // 1×1 终止（02 篇 Mips 契约）
        }

        // 目标尺寸：odd 用 max(1, size/2)（下取整，clamp 源坐标吸收余数）。
        const std::uint32_t nextWidth = std::max(1U, currentWidth / 2U);
        const std::uint32_t nextHeight = std::max(1U, currentHeight / 2U);
        std::vector<std::byte> next(static_cast<std::size_t>(nextWidth) * nextHeight * 4U, std::byte{0});

        // 2×2 box：源坐标 clamp 到边界（odd 尺寸时最后一列/行被采样两次）。
        const auto sample = [&](const std::uint32_t x, const std::uint32_t y) -> std::array<std::byte, 4>
        {
            const std::uint32_t clampedX = std::min(x, currentWidth - 1U);
            const std::uint32_t clampedY = std::min(y, currentHeight - 1U);
            const std::size_t base = (static_cast<std::size_t>(clampedY) * currentWidth + clampedX) * 4U;
            return {current[base], current[base + 1], current[base + 2], current[base + 3]};
        };

        for (std::uint32_t y = 0; y < nextHeight; ++y)
        {
            for (std::uint32_t x = 0; x < nextWidth; ++x)
            {
                const std::array<std::byte, 4> taps[4] = {sample(x * 2U, y * 2U), sample(x * 2U + 1U, y * 2U),
                                                          sample(x * 2U, y * 2U + 1U),
                                                          sample(x * 2U + 1U, y * 2U + 1U)};
                std::array<std::byte, 4> filtered{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};

                if (srgbDomain)
                {
                    // sRGB 角色：先解码到线性，平均后再编码回 sRGB（alpha 线性平均）。
                    float linear[3]{};
                    float alpha{};
                    for (const std::array<std::byte, 4>& tap : taps)
                    {
                        linear[0] += SrgbToLinear(DecodeByte(tap[0]));
                        linear[1] += SrgbToLinear(DecodeByte(tap[1]));
                        linear[2] += SrgbToLinear(DecodeByte(tap[2]));
                        alpha += DecodeByte(tap[3]);
                    }
                    filtered[0] = EncodeByte(LinearToSrgb(linear[0] * 0.25F));
                    filtered[1] = EncodeByte(LinearToSrgb(linear[1] * 0.25F));
                    filtered[2] = EncodeByte(LinearToSrgb(linear[2] * 0.25F));
                    filtered[3] = EncodeByte(alpha * 0.25F);
                }
                else if (normalDomain)
                {
                    // Normal 角色：解码为单位向量域，平均后重归一化；退化时用 +Z，
                    // A 恒 255（normal map 不使用 alpha）。
                    float normal[3]{};
                    for (const std::array<std::byte, 4>& tap : taps)
                    {
                        normal[0] += DecodeByte(tap[0]) * 2.0F - 1.0F;
                        normal[1] += DecodeByte(tap[1]) * 2.0F - 1.0F;
                        normal[2] += DecodeByte(tap[2]) * 2.0F - 1.0F;
                    }
                    const float length =
                        std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                    if (length > 1.0e-6F)
                    {
                        normal[0] /= length;
                        normal[1] /= length;
                        normal[2] /= length;
                    }
                    else
                    {
                        normal[0] = 0.0F;
                        normal[1] = 0.0F;
                        normal[2] = 1.0F;
                    }
                    filtered[0] = EncodeByte(normal[0] * 0.5F + 0.5F);
                    filtered[1] = EncodeByte(normal[1] * 0.5F + 0.5F);
                    filtered[2] = EncodeByte(normal[2] * 0.5F + 0.5F);
                    filtered[3] = EncodeByte(1.0F);
                }
                else
                {
                    // 线性数据角色：全通道直接平均。
                    float sum[4]{};
                    for (const std::array<std::byte, 4>& tap : taps)
                    {
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            sum[channel] += DecodeByte(tap[channel]);
                        }
                    }
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        filtered[channel] = EncodeByte(sum[channel] * 0.25F);
                    }
                }

                const std::size_t destination = (static_cast<std::size_t>(y) * nextWidth + x) * 4U;
                for (int channel = 0; channel < 4; ++channel)
                {
                    next[destination + static_cast<std::size_t>(channel)] = filtered[channel];
                }
            }
        }

        currentWidth = nextWidth;
        currentHeight = nextHeight;
        current = std::move(next);
    }
    return true;
}
} // namespace MiniEngine::Tools
