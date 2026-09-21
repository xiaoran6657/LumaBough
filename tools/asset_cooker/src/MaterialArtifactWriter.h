#pragma once

// ============================================================================
// MaterialArtifactWriter.h — `.memat` v1（Material Baked artifact）写入
// 里程碑：M4-02
// 职责：把 MaterialAsset 序列化为 Baked 材质 artifact（header + INFO/TEXR +
//       16 对齐数据区），输出字节确定性。与引擎 DecodeMaterialChunks 的读取
//       防御对称：失败返回 false 并给出 error（Cooker 按 Serialize 退出码 7 处理）。
// wire 布局 v1（与 BakedReader 契约一致）：
//   header 64B（chunkCount=2，flags=kMaterialFormatVersion=1）→
//   INFO/TEXR 描述符 @64/96 → INFO 数据 @128（48B）→ TEXR 数据 @Align16(176)。
// INFO payload 48B（little-endian）：
//   baseColorFactor(4f) emissiveFactor(3f) metallic/roughness/normalScale/
//   occlusionStrength(4f) 共 11 个 float（44B）+ alphaMode(u32，0=Opaque)。
//   描述符 elementCount=12、stride=4。
// TEXR payload：固定 5 条记录 × 20B（usage u32 + AssetId 16B），按
//   BaseColor/MetallicRoughness/Normal/Occlusion/Emissive 顺序；未使用的 role
//   AssetId 为全零。描述符 elementCount=5、stride=20。
// 关联：engine/assets/include/MiniEngine/Assets/MaterialAsset.h（载荷契约）
//       engine/assets/src/AssetManager.cpp（DecodeMaterialChunks 读取端）
// ============================================================================

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/Sha256.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
using MiniEngine::Assets::MaterialAsset;

[[nodiscard]] bool BuildMaterialArtifact(const MiniEngine::Assets::Sha256Digest& buildKey, const MaterialAsset& material,
                                         std::vector<std::byte>& out, std::string& error);
} // namespace MiniEngine::Tools
