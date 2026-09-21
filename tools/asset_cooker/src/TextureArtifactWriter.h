#pragma once

// ============================================================================
// TextureArtifactWriter.h — .metex v2（Texture Baked artifact）写入
// 里程碑：M3-04（v1）→ M4-02（v2：usage / 像素格式 / MIPS chunk）
// 职责：把 TextureMipBuilder 的级联像素流序列化为 Baked 纹理 artifact v2
//       （header + INFO/DATA/MIPS 描述符 + 16 对齐数据区），输出字节确定性。
//       与引擎 DecodeTextureChunks 的读取防御对称，失败返回 false 并给出 error。
// wire 布局 v2（与 BakedReader 契约一致）：
//   header 64B（chunkCount=3，flags=kTextureFormatVersion=2）→
//   INFO/MIPS/DATA 描述符 @64/96/128 →
//   INFO 数据 @160（28B）→ MIPS 数据 @Align16(188)（mipCount×12B）→ DATA 数据。
// INFO payload 28B（u32 little-endian）：
//   width | height | pixelFormat | colorSpace | usage | mipCount | mip0RowPitch
//   描述符语义：INFO elementCount=7、stride=4。
// MIPS payload：mipCount 条记录（offset u64? 否——u32 offset/rowPitch/byteSize 各 4B，
//   共 12B/级，描述符 elementCount=mipCount、stride=12）。offset 相对 DATA 起点。
// DATA：所有 mip 级联像素（mip0 在前）；描述符 elementCount=mip0Height、
//   stride=mip0RowPitch。
// 失败（尺寸为 0、宽溢出、mip 记录与像素流不符）返回 false 且 error 非空。
// 关联：engine/assets/src/AssetManager.cpp（DecodeTextureChunks 读取端）
//       tools/asset_cooker/src/TextureMipBuilder.h（mip 链来源）
// ============================================================================

#include "TextureMipBuilder.h"

#include "Sha256.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
[[nodiscard]] bool BuildTextureArtifact(const Sha256Digest& buildKey, std::uint32_t width, std::uint32_t height,
                                        MiniEngine::Assets::TexturePixelFormat pixelFormat,
                                        MiniEngine::Assets::TextureColorSpace colorSpace,
                                        MiniEngine::Assets::TextureUsage usage, const BuiltTextureMips& mips,
                                        std::vector<std::byte>& out, std::string& error);
} // namespace MiniEngine::Tools
