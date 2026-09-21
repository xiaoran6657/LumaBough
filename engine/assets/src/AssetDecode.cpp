// ============================================================================
// AssetDecode.cpp — 内部解码原语的唯一实现（M3 逻辑原样下沉）
// 里程碑：M3（原 AssetManager.cpp 内匿名命名空间实现，M7-06 抽取）
// 职责：见 Internal/AssetDecode.h。抽取属于纯搬运：函数体、错误文本与校验顺序
//       都与 M3 版本逐字一致，只有链接属性与命名空间变化。
// 关联：engine/assets/src/AssetManager.cpp、engine/assets/async/src/AsyncAssetLoader.cpp
// ============================================================================

#include <MiniEngine/Assets/Internal/AssetDecode.h>

#include <MiniEngine/Assets/Internal/BakedReadUtil.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Assets/TextureFormatV2.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace MiniEngine::Assets::Internal
{
using Internal::ChunkExtractStatus;
using Internal::FindUniqueChunk;
using Internal::ReadU32LittleEndian;

BakedAssetKind ToBakedKind(const AssetKind kind) noexcept
{
    switch (kind)
    {
    case AssetKind::Mesh:
        return BakedAssetKind::Mesh;
    case AssetKind::Texture:
        return BakedAssetKind::Texture;
    case AssetKind::Material:
        return BakedAssetKind::Material;
    case AssetKind::World:
        break;
    }
    return BakedAssetKind::World;
}

bool DecodeMeshChunks(std::span<const std::byte> artifactBytes, const BakedReadResult& readResult, MeshAsset& mesh,
                      std::string& error)
{
    constexpr std::array<char, 4> kVertType{'V', 'E', 'R', 'T'};
    constexpr std::array<char, 4> kIndxType{'I', 'N', 'D', 'X'};

    if (readResult.header.flags != kMeshFormatVersion)
    {
        error = "unsupported mesh formatVersion=" + std::to_string(readResult.header.flags) +
                "; delete derived cache and recook with M4";
        return false;
    }

    const BakedChunk* vert = nullptr;
    const BakedChunk* indx = nullptr;
    if (FindUniqueChunk(readResult.chunks, kVertType, vert) == ChunkExtractStatus::Duplicate)
    {
        error = "multiple VERT chunks";
        return false;
    }
    if (FindUniqueChunk(readResult.chunks, kIndxType, indx) == ChunkExtractStatus::Duplicate)
    {
        error = "multiple INDX chunks";
        return false;
    }
    if (vert == nullptr && indx == nullptr)
    {
        return true; // header-only 占位（测试 fixture / 旧 writer）
    }
    if (vert == nullptr || indx == nullptr)
    {
        error = "VERT and INDX chunks must appear together";
        return false;
    }
    if (vert->stride != sizeof(PbrVertex))
    {
        error = "unsupported mesh vertex stride (expected 48)";
        return false;
    }

    const auto vertSpan = BakedReader::GetChunkBytes(artifactBytes, *vert);
    const auto indxSpan = BakedReader::GetChunkBytes(artifactBytes, *indx);
    if (!vertSpan.has_value() || !indxSpan.has_value())
    {
        error = "chunk byte range invalid";
        return false;
    }
    mesh.vertexCount = vert->elementCount;
    mesh.vertexStride = vert->stride;
    mesh.vertexData.assign(vertSpan->begin(), vertSpan->end());
    mesh.indexCount = indx->elementCount;
    mesh.indexStride = indx->stride;
    mesh.indexData.assign(indxSpan->begin(), indxSpan->end());
    return true;
}

// `.metex` v2 常量（与 TextureArtifactWriter 的 wire 布局一一对应）。
inline constexpr std::uint32_t kTextureInfoSizeV2 = 28;     // 7 × u32
inline constexpr std::uint32_t kTextureMipsRecordSize = 12; // offset/rowPitch/byteSize

bool DecodeTextureChunks(std::span<const std::byte> artifactBytes, const BakedReadResult& readResult,
                         TextureAsset& texture, std::string& error)
{
    constexpr std::array<char, 4> kInfoType{'I', 'N', 'F', 'O'};
    constexpr std::array<char, 4> kDataType{'D', 'A', 'T', 'A'};
    constexpr std::array<char, 4> kMipsType{'M', 'I', 'P', 'S'};

    if (readResult.header.flags != kTextureFormatVersion)
    {
        error = "unsupported texture formatVersion=" + std::to_string(readResult.header.flags) +
                "; delete derived cache and recook with M4";
        return false;
    }

    const BakedChunk* info = nullptr;
    const BakedChunk* data = nullptr;
    const BakedChunk* mips = nullptr;
    if (FindUniqueChunk(readResult.chunks, kInfoType, info) == ChunkExtractStatus::Duplicate ||
        FindUniqueChunk(readResult.chunks, kDataType, data) == ChunkExtractStatus::Duplicate ||
        FindUniqueChunk(readResult.chunks, kMipsType, mips) == ChunkExtractStatus::Duplicate)
    {
        error = "duplicate INFO/DATA/MIPS chunk";
        return false;
    }
    if (info == nullptr && data == nullptr && mips == nullptr)
    {
        return true; // header-only 占位（测试 fixture / 旧 writer）
    }
    if (info == nullptr || data == nullptr || mips == nullptr)
    {
        error = "INFO, DATA and MIPS chunks must appear together";
        return false;
    }

    const auto infoSpan = BakedReader::GetChunkBytes(artifactBytes, *info);
    const auto dataSpan = BakedReader::GetChunkBytes(artifactBytes, *data);
    const auto mipsSpan = BakedReader::GetChunkBytes(artifactBytes, *mips);
    if (!infoSpan.has_value() || !dataSpan.has_value() || !mipsSpan.has_value())
    {
        error = "chunk byte range invalid";
        return false;
    }
    if (infoSpan->size() < kTextureInfoSizeV2)
    {
        error = "INFO chunk is truncated";
        return false;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixelFormat = 0;
    std::uint32_t colorSpace = 0;
    std::uint32_t usage = 0;
    std::uint32_t mipCount = 0;
    std::uint32_t rowPitch = 0;
    if (!ReadU32LittleEndian(*infoSpan, 0, width) || !ReadU32LittleEndian(*infoSpan, 4, height) ||
        !ReadU32LittleEndian(*infoSpan, 8, pixelFormat) || !ReadU32LittleEndian(*infoSpan, 12, colorSpace) ||
        !ReadU32LittleEndian(*infoSpan, 16, usage) || !ReadU32LittleEndian(*infoSpan, 20, mipCount) ||
        !ReadU32LittleEndian(*infoSpan, 24, rowPitch))
    {
        error = "INFO chunk fields out of bounds";
        return false;
    }

    const TextureHeaderV2 header{width,
                                 height,
                                 mipCount,
                                 static_cast<TexturePixelFormat>(pixelFormat),
                                 static_cast<TextureColorSpace>(colorSpace),
                                 static_cast<TextureUsage>(usage)};
    if (!IsValid(header))
    {
        error = "texture header violates the usage/colorSpace/mip contract";
        return false;
    }
    if (mipsSpan->size() != static_cast<std::size_t>(mipCount) * kTextureMipsRecordSize)
    {
        error = "MIPS chunk size does not equal mipCount*12";
        return false;
    }
    // mip0 rowPitch 按 pixelFormat 分派：RGBA8 = 4B/像素、RGBA16F = 8B/像素
    // （M4-04 HdrEnvironment）。未知格式已被 IsValid 的 usage/格式契约拒绝，
    // 此处只需覆盖两种合法取值。
    const std::uint32_t bytesPerPixel = header.pixelFormat == TexturePixelFormat::Rgba16Float ? 8U : 4U;
    if (rowPitch != width * bytesPerPixel)
    {
        error = "mip0 rowPitch does not equal width*bytes-per-pixel";
        return false;
    }

    texture.width = width;
    texture.height = height;
    texture.usage = header.usage;
    texture.pixelFormat = header.pixelFormat;
    texture.colorSpace = header.colorSpace;
    texture.mipCount = mipCount;
    texture.mips.resize(mipCount);
    for (std::uint32_t level = 0; level < mipCount; ++level)
    {
        std::uint32_t levelOffset = 0;
        std::uint32_t levelRowPitch = 0;
        std::uint32_t levelBytes = 0;
        const std::size_t recordBase = static_cast<std::size_t>(level) * kTextureMipsRecordSize;
        if (!ReadU32LittleEndian(*mipsSpan, recordBase, levelOffset) ||
            !ReadU32LittleEndian(*mipsSpan, recordBase + 4, levelRowPitch) ||
            !ReadU32LittleEndian(*mipsSpan, recordBase + 8, levelBytes))
        {
            error = "MIPS chunk fields out of bounds";
            return false;
        }
        texture.mips[level] = TextureMipInfo{levelOffset, levelRowPitch, levelBytes};
    }
    // 末级 offset+byteSize 必须恰好覆盖 DATA（级联布局的完备性校验）。
    const TextureMipInfo& last = texture.mips.back();
    if (static_cast<std::uint64_t>(last.offset) + last.byteSize != dataSpan->size())
    {
        error = "MIPS records do not cover the DATA chunk";
        return false;
    }
    texture.pixels.assign(dataSpan->begin(), dataSpan->end());
    return true;
}

bool DecodeMaterialChunks(std::span<const std::byte> artifactBytes, const BakedReadResult& readResult,
                          MaterialAsset& material, std::string& error)
{
    constexpr std::array<char, 4> kInfoType{'I', 'N', 'F', 'O'};
    constexpr std::array<char, 4> kTexrType{'T', 'E', 'X', 'R'};
    constexpr std::size_t kInfoSize = 48;       // 11 float + alphaMode u32
    constexpr std::size_t kTexrRecordSize = 20; // usage u32 + AssetId 16B
    constexpr std::size_t kTexrEntryCount = 5;

    if (readResult.header.flags != kMaterialFormatVersion)
    {
        error = "unsupported material formatVersion=" + std::to_string(readResult.header.flags) +
                "; delete derived cache and recook with M4";
        return false;
    }

    const BakedChunk* info = nullptr;
    const BakedChunk* texr = nullptr;
    if (FindUniqueChunk(readResult.chunks, kInfoType, info) == ChunkExtractStatus::Duplicate ||
        FindUniqueChunk(readResult.chunks, kTexrType, texr) == ChunkExtractStatus::Duplicate)
    {
        error = "duplicate INFO/TEXR chunk";
        return false;
    }
    if (info == nullptr && texr == nullptr)
    {
        return true; // header-only 占位（测试 fixture）
    }
    if (info == nullptr || texr == nullptr)
    {
        error = "INFO and TEXR chunks must appear together";
        return false;
    }

    const auto infoSpan = BakedReader::GetChunkBytes(artifactBytes, *info);
    const auto texrSpan = BakedReader::GetChunkBytes(artifactBytes, *texr);
    if (!infoSpan.has_value() || !texrSpan.has_value())
    {
        error = "chunk byte range invalid";
        return false;
    }
    if (infoSpan->size() < kInfoSize || texrSpan->size() != kTexrEntryCount * kTexrRecordSize)
    {
        error = "INFO/TEXR chunk size mismatch";
        return false;
    }

    const auto readFloat = [&](const std::size_t offset, float& value) -> bool
    {
        std::uint32_t bits = 0;
        if (!ReadU32LittleEndian(*infoSpan, offset, bits))
        {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    };

    std::array<float, 4> baseColorFactor{};
    std::array<float, 3> emissiveFactor{};
    float metallicFactor = 0.0F;
    float roughnessFactor = 0.0F;
    float normalScale = 0.0F;
    float occlusionStrength = 0.0F;
    for (int component = 0; component < 4; ++component)
    {
        if (!readFloat(static_cast<std::size_t>(component) * 4U, baseColorFactor[component]))
        {
            error = "INFO chunk fields out of bounds";
            return false;
        }
    }
    for (int component = 0; component < 3; ++component)
    {
        if (!readFloat(16U + static_cast<std::size_t>(component) * 4U, emissiveFactor[component]))
        {
            error = "INFO chunk fields out of bounds";
            return false;
        }
    }
    if (!readFloat(28U, metallicFactor) || !readFloat(32U, roughnessFactor) || !readFloat(36U, normalScale) ||
        !readFloat(40U, occlusionStrength))
    {
        error = "INFO chunk fields out of bounds";
        return false;
    }
    std::uint32_t alphaMode = 0;
    if (!ReadU32LittleEndian(*infoSpan, 44, alphaMode))
    {
        error = "INFO chunk fields out of bounds";
        return false;
    }

    // 因子域校验（glTF core：baseColor/emissive 各分量 [0,1]，metallic/roughness
    // [0,1]，normalScale/occlusionStrength [0,1]）；越界即拒绝而非静默 clamp。
    const auto checkFactor = [&](const float value, const char* name) -> bool
    {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
        {
            error = std::string(name) + " is not finite or outside [0,1]";
            return false;
        }
        return true;
    };
    for (const float value : baseColorFactor)
    {
        if (!checkFactor(value, "baseColorFactor"))
        {
            return false;
        }
    }
    for (const float value : emissiveFactor)
    {
        if (!checkFactor(value, "emissiveFactor"))
        {
            return false;
        }
    }
    if (!checkFactor(metallicFactor, "metallicFactor") || !checkFactor(roughnessFactor, "roughnessFactor") ||
        !checkFactor(normalScale, "normalScale") || !checkFactor(occlusionStrength, "occlusionStrength"))
    {
        return false;
    }
    if (alphaMode != 0)
    {
        error = "unsupported material alphaMode";
        return false;
    }

    // TEXR：5 条 (usage, AssetId)，usage 必须与固定次序一致（错序视为畸形产物）。
    const std::uint32_t expectedUsages[kTexrEntryCount] = {1U, 2U, 3U, 4U, 5U};
    AssetId ids[kTexrEntryCount];
    for (std::size_t index = 0; index < kTexrEntryCount; ++index)
    {
        const std::size_t recordBase = index * kTexrRecordSize;
        std::uint32_t usageCode = 0;
        if (!ReadU32LittleEndian(*texrSpan, recordBase, usageCode) || usageCode != expectedUsages[index])
        {
            error = "TEXR usage order mismatch";
            return false;
        }
        for (std::size_t byteIndex = 0; byteIndex < 16; ++byteIndex)
        {
            ids[index].bytes[byteIndex] = (*texrSpan)[recordBase + 4 + byteIndex];
        }
    }

    material.baseColorFactor = baseColorFactor;
    material.emissiveFactor = emissiveFactor;
    material.metallicFactor = metallicFactor;
    material.roughnessFactor = roughnessFactor;
    material.normalScale = normalScale;
    material.occlusionStrength = occlusionStrength;
    material.baseColorTexture = ids[0];
    material.metallicRoughnessTexture = ids[1];
    material.normalTexture = ids[2];
    material.occlusionTexture = ids[3];
    material.emissiveTexture = ids[4];
    material.alphaMode = AlphaMode::Opaque;
    return true;
}
} // namespace MiniEngine::Assets::Internal
