// ============================================================================
// TextureFormatV2.h — `.metex` v2 的纹理语义契约（usage / 像素格式 / 颜色空间 / mip）
// 里程碑：M4（02 篇 Texture v2）
// 职责：定义 TextureHeaderV2（宽高 + mip 数 + 像素格式 + 颜色空间 + usage）及其
//       IsValid 语义校验。usage 决定三件事：像素格式与颜色空间的合法组合
//       （baseColor/emissive 必须 sRGB，normal/occlusion/metallicRoughness 必须
//       linear，HDR 环境必须 RGBA16F——不允许按文件名或扩展名猜测）、mip 滤波
//       算法、GPU SRV 格式。M3 的 isSrgb 布尔由 usage+colorSpace 组合取代。
// 关联：docs/architecture/README.md「Texture v2」
//       tools/asset_cooker/src/TextureArtifactWriter.cpp（v2 写入端）
//       tools/asset_cooker/src/TextureMipBuilder.cpp（usage 角色化滤波）
//       engine/assets/src/AssetManager.cpp（DecodeTextureChunks 读取端）
// ============================================================================

#pragma once

#include <cstdint>

namespace MiniEngine::Assets
{
// 像素存储格式（wire uint8）：
//   Rgba8Unorm  —— 8bit/通道 RGBA（baseColor/emissive 的存储语义仍是 sRGB 编码）
//   Rgba16Float —— HDR panorama（04 篇 IBL 输入），线性 float
enum class TexturePixelFormat : std::uint8_t
{
    Rgba8Unorm = 1,
    Rgba16Float = 2
};

// 颜色空间（wire uint8）：Srgb 表示存储值经 sRGB transfer 编码，
// 采样/滤波时必须先解码到线性（mip 滤波在线性域进行，见 TextureMipBuilder）。
enum class TextureColorSpace : std::uint8_t
{
    Linear = 0,
    Srgb = 1
};

// 贴图角色（wire uint8）：决定合法的 format/colorSpace 组合、mip 滤波算法与
// GPU fallback 内容。与 Binding table 的 t0–t4 一一对应（HdrEnvironment 供 04 篇）。
enum class TextureUsage : std::uint8_t
{
    BaseColor = 1,
    MetallicRoughness = 2,
    Normal = 3,
    Occlusion = 4,
    Emissive = 5,
    HdrEnvironment = 6
};

// `.metex` v2 头（INFO chunk 语义层，wire 上仍按字段显式序列化）。
struct TextureHeaderV2 final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    TexturePixelFormat pixelFormat{};
    TextureColorSpace colorSpace{};
    TextureUsage usage{};
};

// `.metex` 的格式版本（通过 BakedHeader.flags 载波；M3 文件 flags=0 视为 v1）。
inline constexpr std::uint32_t kTextureFormatVersion = 2;

// 语义校验：宽高/mip 数有限，且 usage 与格式/颜色空间的组合符合 glTF 传输函数
// 契约。非法组合（如 normal 贴图标 sRGB）在 Reader 入口即拒绝，不进入解码。
constexpr bool IsValid(const TextureHeaderV2& header)
{
    if (header.width == 0 || header.height == 0 || header.mipCount == 0)
    {
        return false;
    }

    // mip 链长度上界：max(w,h) 每次减半直到 1（odd 尺寸用 max(1, size/2) 下取整）。
    std::uint32_t maximumMipCount = 1;
    for (std::uint32_t extent = header.width > header.height ? header.width : header.height; extent > 1; extent >>= 1U)
    {
        ++maximumMipCount;
    }
    if (header.mipCount > maximumMipCount)
    {
        return false;
    }

    switch (header.usage)
    {
    case TextureUsage::BaseColor:
    case TextureUsage::Emissive:
        // Khronos：base color / emissive 使用 sRGB transfer。
        return header.pixelFormat == TexturePixelFormat::Rgba8Unorm && header.colorSpace == TextureColorSpace::Srgb;
    case TextureUsage::MetallicRoughness:
    case TextureUsage::Normal:
    case TextureUsage::Occlusion:
        // Khronos：normal / occlusion / metallic-roughness 是线性数据。
        return header.pixelFormat == TexturePixelFormat::Rgba8Unorm && header.colorSpace == TextureColorSpace::Linear;
    case TextureUsage::HdrEnvironment:
        // HDR 环境贴图（04 篇）：线性 float，不允许 sRGB 语义。
        return header.pixelFormat == TexturePixelFormat::Rgba16Float && header.colorSpace == TextureColorSpace::Linear;
    default:
        return false;
    }
}
} // namespace MiniEngine::Assets
