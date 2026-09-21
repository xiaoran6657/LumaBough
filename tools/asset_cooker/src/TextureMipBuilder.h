#pragma once

// ============================================================================
// TextureMipBuilder.h — 确定性 CPU mip 链生成（02 篇「Texture v2 / Mips」）
// 里程碑：M4-02
// 职责：由顶层 RGBA8 像素生成完整 mip 链。滤波按 usage 角色化（glTF 传输函数契约）：
//       BaseColor/Emissive 在线性域 2×2 box 后重新 sRGB 编码；Normal 解码向量→
//       平均→normalize→重编码（A 恒 255）；MetallicRoughness/Occlusion 全通道线性
//       平均。odd 尺寸取 max(1, size/2) 并 clamp 源坐标。绝不调用 GPU GenerateMips
//       ——正式资产的 mip 必须字节确定（M3 deterministic Cache 的延续）。
// 关联：docs/architecture/README.md「Mips」
//       tools/asset_cooker/src/TextureArtifactWriter.cpp（MIPS chunk 写入）
// ============================================================================

#include <MiniEngine/Assets/TextureFormatV2.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// 语义契约类型来自 Assets 层；本命名空间内统一裸名使用。
using MiniEngine::Assets::TextureUsage;

// 单个 mip 级在像素字节流内的定位（MIPS chunk 的每条记录）。
struct TextureMipLevel final
{
    std::uint64_t offset{};   // 相对像素流起点
    std::uint32_t rowPitch{}; // 该级行距（字节）
    std::uint32_t byteSize{}; // 该级总字节
};

// 生成结果：级联像素流 + 每级描述（mip0 在前）。
struct BuiltTextureMips final
{
    std::vector<std::byte> pixels;
    std::vector<TextureMipLevel> levels;
};

// 由顶层 RGBA8（width×height，top-to-bottom）生成完整 mip 链直至 1×1。
// 失败（尺寸为 0、rowPitch 溢出、顶层字节数不符）返回 false 且 error 非空。
[[nodiscard]] bool BuildTextureMipChain(std::uint32_t width, std::uint32_t height, TextureUsage usage,
                                        const std::vector<std::byte>& topLevelRgba8, BuiltTextureMips& out,
                                        std::string& error);
} // namespace MiniEngine::Tools
