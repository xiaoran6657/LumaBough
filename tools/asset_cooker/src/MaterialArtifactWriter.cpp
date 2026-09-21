// ============================================================================
// MaterialArtifactWriter.cpp — `.memat` v1 写入的实现
// 里程碑：M4-02
// 职责：按 wire 偏移显式写出 header(kind=Material, chunkCount=2)、INFO（factors +
//       alpha mode）与 TEXR（五个 role 的 AssetId），float 以位模式 little-endian
//       写出（不依赖本机浮点布局的对齐），输出字节确定性。
// 关联：tools/asset_cooker/src/MaterialArtifactWriter.h（wire 布局）
//       engine/assets/src/AssetManager.cpp（DecodeMaterialChunks 读取端）
// ============================================================================

#include "MaterialArtifactWriter.h"

#include "BakedWriter.h"

#include <MiniEngine/Assets/TextureFormatV2.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace MiniEngine::Tools
{
bool BuildMaterialArtifact(const MiniEngine::Assets::Sha256Digest& buildKey,
                           const MiniEngine::Assets::MaterialAsset& material, std::vector<std::byte>& out,
                           std::string& error)
{
    using namespace MiniEngine::Assets;
    out.clear();

    error.clear();

    // 与引擎 DecodeMaterialChunks 相同的因子防御：全部有限且在 glTF core 允许域内
    //（baseColor/emissive 分量 [0,1]，metallic/roughness [0,1]，scale/strength [0,1]）。
    const auto checkFactor = [&](const float value, const char* name) -> bool
    {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
        {
            error = std::string(name) + " is not finite or outside [0,1]";
            return false;
        }
        return true;
    };
    for (const float value : material.baseColorFactor)
    {
        if (!checkFactor(value, "baseColorFactor"))
        {
            return false;
        }
    }
    for (const float value : material.emissiveFactor)
    {
        if (!checkFactor(value, "emissiveFactor"))
        {
            return false;
        }
    }
    if (!checkFactor(material.metallicFactor, "metallicFactor") ||
        !checkFactor(material.roughnessFactor, "roughnessFactor") ||
        !checkFactor(material.normalScale, "normalScale") || !checkFactor(material.occlusionStrength, "occlusionStrength"))
    {
        return false;
    }
    if (material.alphaMode != AlphaMode::Opaque)
    {
        error = "M4 only supports opaque materials";
        return false;
    }

    constexpr std::size_t kChunkTableStart = static_cast<std::size_t>(kBakedHeaderSize);    // 64
    constexpr std::size_t kInfoDescriptorOffset = kChunkTableStart;                         // 64
    constexpr std::size_t kTexrDescriptorOffset = kChunkTableStart + kChunkDescriptorSize;  // 96
    constexpr std::size_t kInfoSize = 48;                                                   // 11f + alphaMode
    constexpr std::size_t kTexrRecordSize = 20;                                             // usage4 + id16
    constexpr std::size_t kTexrEntryCount = 5;

    const std::size_t kInfoDataOffset = kTexrDescriptorOffset + kChunkDescriptorSize;        // 128
    const std::size_t texrDataOffset = BakedWriter::AlignUp(kInfoDataOffset + kInfoSize, kChunkAlignment);
    const std::size_t artifactSize = texrDataOffset + kTexrEntryCount * kTexrRecordSize;

    out.assign(artifactSize, std::byte{0});
    BakedWriter::WriteBakedHeader(out, BakedAssetKind::Material, buildKey, 2,
                                  static_cast<std::uint64_t>(artifactSize), kMaterialFormatVersion);
    BakedWriter::WriteChunkDescriptor(out, kInfoDescriptorOffset, "INFO",
                                      static_cast<std::uint64_t>(kInfoDataOffset), kInfoSize, 12, 4);
    BakedWriter::WriteChunkDescriptor(out, kTexrDescriptorOffset, "TEXR",
                                      static_cast<std::uint64_t>(texrDataOffset),
                                      kTexrEntryCount * kTexrRecordSize, kTexrEntryCount, kTexrRecordSize);

    // float 以位模式 little-endian 写出（PutU32 复用 BakedWriter 的唯一写路径）。
    const auto putFloat = [&](const std::size_t offset, const float value)
    {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        BakedWriter::PutU32(out, offset, bits);
    };

    std::size_t cursor = kInfoDataOffset;
    for (const float value : material.baseColorFactor)
    {
        putFloat(cursor, value);
        cursor += 4;
    }
    for (const float value : material.emissiveFactor)
    {
        putFloat(cursor, value);
        cursor += 4;
    }
    putFloat(cursor, material.metallicFactor);
    cursor += 4;
    putFloat(cursor, material.roughnessFactor);
    cursor += 4;
    putFloat(cursor, material.normalScale);
    cursor += 4;
    putFloat(cursor, material.occlusionStrength);
    cursor += 4;
    BakedWriter::PutU32(out, cursor, static_cast<std::uint32_t>(material.alphaMode));

    // TEXR：usage 与 role 顺序一一对应（Binding table t0–t4）。
    const std::pair<TextureUsage, const AssetId*> entries[kTexrEntryCount] = {
        {TextureUsage::BaseColor, &material.baseColorTexture},
        {TextureUsage::MetallicRoughness, &material.metallicRoughnessTexture},
        {TextureUsage::Normal, &material.normalTexture},
        {TextureUsage::Occlusion, &material.occlusionTexture},
        {TextureUsage::Emissive, &material.emissiveTexture},
    };
    for (std::size_t index = 0; index < kTexrEntryCount; ++index)
    {
        const std::size_t recordBase = texrDataOffset + index * kTexrRecordSize;
        BakedWriter::PutU32(out, recordBase, static_cast<std::uint32_t>(entries[index].first));
        std::memcpy(out.data() + recordBase + 4, entries[index].second->bytes.data(), 16);
    }
    return true;
}
} // namespace MiniEngine::Tools
