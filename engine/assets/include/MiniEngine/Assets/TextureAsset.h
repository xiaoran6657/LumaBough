// ============================================================================
// TextureAsset.h — 贴图资产的 CPU 载荷（`.metex` v2）
// 里程碑：M3（v1）→ M4-02（v2：usage / 像素格式 / 完整 mip 链）
// 职责：定义 AssetPool<TextureAsset> 的 payload：语义头（usage/pixelFormat/
//       colorSpace/mipCount）+ 级联像素流 + 每级定位。usage 决定 D3D11 SRV 格式
//       与 fallback 内容；mip 链由 Cooker 确定性生成（禁止 GPU GenerateMips）。
//       不含 D3D / 平台类型。
// 关联：docs/architecture/README.md「Texture v2」
//       engine/assets/include/MiniEngine/Assets/TextureFormatV2.h（语义校验）
//       engine/assets/src/AssetManager.cpp（DecodeTextureChunks 的填充与校验）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/TextureFormatV2.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniEngine::Assets
{
// 单个 mip 级在 pixels 字节流内的定位（与 `.metex` v2 的 MIPS chunk 一一对应）。
struct TextureMipInfo final
{
    std::uint64_t offset{};
    std::uint32_t rowPitch{};
    std::uint32_t byteSize{};
};

// CPU texture payload（v2）。pixels 为所有 mip 级联（mip0 在前），每级由
// mips[offset/rowPitch/byteSize] 描述；长度恒等于末级 offset+byteSize。
struct TextureAsset final
{
    // 宽度（像素）；解码端拒绝 0。
    std::uint32_t width{};
    // 高度（像素）；解码端拒绝 0。
    std::uint32_t height{};
    // 贴图角色（决定 SRV 格式/fallback/滤波语义）。
    TextureUsage usage{};
    // 像素存储格式（M4-02 的 RGBA8 角色）；HDR（Rgba16Float）随 04 篇接入。
    TexturePixelFormat pixelFormat{};
    // 颜色空间：Srgb 仅对 BaseColor/Emissive 合法（GPU 侧选 _UNORM_SRGB 视图）。
    TextureColorSpace colorSpace{};
    // mip 级数（≥1，≤ 全链上界；完整链由 Cooker 保证）。
    std::uint32_t mipCount{};
    // 所有 mip 级联的像素（mip0 在前，top-to-bottom）。
    std::vector<std::byte> pixels;
    // 每级定位（elementCount == mipCount）。
    std::vector<TextureMipInfo> mips;
};
} // namespace MiniEngine::Assets
