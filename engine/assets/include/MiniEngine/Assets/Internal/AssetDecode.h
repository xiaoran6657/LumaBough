// ============================================================================
// AssetDecode.h — baked chunk → typed CPU payload 的内部解码原语（内部头）
// 里程碑：M3（原实现，2026-09-18 M7-06 从 AssetManager.cpp 抽出）
// 职责：把"已通过 BakedReader 结构校验的 artifact 字节"解码成 typed payload。
//       同步加载（AssetManager::PrepareManifestLoad）与异步加载（M7-06 的 decode
//       task）必须使用**同一份**实现：否则两条路径的 schema 判定会漂移。
// 归属：仅引擎内部使用（Internal/），不导出给 samples；解码是纯 CPU 工作，
//       不接触 RHI / 平台 / 文件系统。
// 关联：docs/architecture/README.md「第 3 步：decode task」
//       engine/assets/src/AssetDecode.cpp（实现）
//       engine/assets/src/AssetManager.cpp（同步路径调用方）
//       engine/assets/async/src/AsyncAssetLoader.cpp（异步 decode task 调用方）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetRegistry.h> // AssetKind（ToBakedKind 的输入域）
#include <MiniEngine/Assets/BakedReader.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>

#include <cstddef>
#include <span>
#include <string>

namespace MiniEngine::Assets::Internal
{
// AssetKind → BakedAssetKind 的一对一映射，供 BakedReader 的 kind 期望值使用。
// World 分支穿过 switch 落到函数尾部的 return，与显式映射 World 等价。
[[nodiscard]] BakedAssetKind ToBakedKind(AssetKind kind) noexcept;

// VERT/INDX chunk → MeshAsset（v2：stride 恒 48 = PbrVertex，索引恒 4）。
// header-only（无 VERT/INDX）保持占位；任一所需类型重复出现视为畸形 → 拒绝。
// header.flags 是各 kind 的格式版本载波：M3 的 v1 文件 flags=0 → 输出 recook
// diagnostic（02 篇「格式迁移策略」，不做静默猜测或原地迁移）。
[[nodiscard]] bool DecodeMeshChunks(std::span<const std::byte> artifactBytes, const BakedReadResult& readResult,
                                    MeshAsset& mesh, std::string& error);

// INFO/MIPS/DATA chunk → TextureAsset v2。
// header.flags != kTextureFormatVersion → recook diagnostic（v1 = 0）。
// 语义校验（usage/format/colorSpace 组合）复用 TextureFormatV2::IsValid；
// MIPS 记录必须覆盖 DATA 全部字节（末级 offset+byteSize == DATA size）。
[[nodiscard]] bool DecodeTextureChunks(std::span<const std::byte> artifactBytes, const BakedReadResult& readResult,
                                       TextureAsset& texture, std::string& error);

// INFO/TEXR chunk → MaterialAsset（`.memat` v1）。
// header.flags != kMaterialFormatVersion → recook diagnostic。
// 语义校验：因子全部有限且在 [0,1]，alphaMode 只接受 0（OPAQUE），
// usage 编码必须与 role 顺序一一对应（TEXR 的 5 条记录是固定次序）。
[[nodiscard]] bool DecodeMaterialChunks(std::span<const std::byte> artifactBytes, const BakedReadResult& readResult,
                                        MaterialAsset& material, std::string& error);
} // namespace MiniEngine::Assets::Internal
