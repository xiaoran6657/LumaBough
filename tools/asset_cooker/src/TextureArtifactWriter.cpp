// ============================================================================
// TextureArtifactWriter.cpp — .metex v2（Texture Baked artifact）写入的实现
// 里程碑：M4-02（v1 见 M3-04；v2 增 usage/像素格式语义与 MIPS chunk）
// 职责：按 wire 偏移显式写出 header(kind=Texture, chunkCount=3)、INFO（28 字节
//       元数据）、MIPS（每级 offset/rowPitch/byteSize）与 DATA（级联像素），
//       全部算术 checked，输出字节确定性。
// 关联：tools/asset_cooker/src/TextureArtifactWriter.h（wire 布局）
//       engine/assets/src/AssetManager.cpp（DecodeTextureChunks 读取端）
// ============================================================================

#include "TextureArtifactWriter.h"

#include "BakedWriter.h"

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/TextureFormatV2.h>

#include <cstring>
#include <limits>

namespace MiniEngine::Tools
{
bool BuildTextureArtifact(const Sha256Digest& buildKey, const std::uint32_t width, const std::uint32_t height,
                          const MiniEngine::Assets::TexturePixelFormat pixelFormat,
                          const MiniEngine::Assets::TextureColorSpace colorSpace,
                          const MiniEngine::Assets::TextureUsage usage, const BuiltTextureMips& mips,
                          std::vector<std::byte>& out, std::string& error)
{
    using namespace MiniEngine::Assets;
    out.clear();
    error.clear();

    // 与引擎 DecodeTextureChunks 完全对称的检查：尺寸非零、语义组合合法、
    // mip 记录与像素流自洽。
    if (width == 0 || height == 0)
    {
        error = "texture dimensions must be non-zero";
        return false;
    }
    if (!IsValid(TextureHeaderV2{width, height, static_cast<std::uint32_t>(mips.levels.size()), pixelFormat,
                                 colorSpace, usage}))
    {
        error = "texture header violates the usage/colorSpace contract";
        return false;
    }
    if (mips.levels.empty() || mips.pixels.size() != mips.levels.back().offset + mips.levels.back().byteSize)
    {
        error = "mip levels do not cover the pixel stream";
        return false;
    }

    constexpr std::size_t kChunkTableStart = static_cast<std::size_t>(kBakedHeaderSize);      // 64
    constexpr std::size_t kInfoDescriptorOffset = kChunkTableStart;                           // 64
    constexpr std::size_t kMipsDescriptorOffset = kChunkTableStart + kChunkDescriptorSize;    // 96
    constexpr std::size_t kDataDescriptorOffset = kChunkTableStart + 2U * kChunkDescriptorSize; // 128
    constexpr std::size_t kInfoSize = 28;                                                     // 7 × u32
    constexpr std::size_t kMipsRecordSize = 12;                                               // offset/rowPitch/byteSize

    const std::size_t kInfoDataOffset = kDataDescriptorOffset + kChunkDescriptorSize;         // 160
    const std::size_t mipsDataOffset = BakedWriter::AlignUp(kInfoDataOffset + kInfoSize, kChunkAlignment);
    const std::size_t dataOffset = BakedWriter::AlignUp(mipsDataOffset + mips.levels.size() * kMipsRecordSize,
                                                        kChunkAlignment);
    const std::size_t artifactSize = dataOffset + mips.pixels.size();

    out.assign(artifactSize, std::byte{0});
    BakedWriter::WriteBakedHeader(out, BakedAssetKind::Texture, buildKey, 3,
                                  static_cast<std::uint64_t>(artifactSize), kTextureFormatVersion);

    BakedWriter::WriteChunkDescriptor(out, kInfoDescriptorOffset, "INFO",
                                      static_cast<std::uint64_t>(kInfoDataOffset), kInfoSize, 7, 4);
    BakedWriter::WriteChunkDescriptor(out, kMipsDescriptorOffset, "MIPS",
                                      static_cast<std::uint64_t>(mipsDataOffset),
                                      mips.levels.size() * kMipsRecordSize,
                                      static_cast<std::uint32_t>(mips.levels.size()), kMipsRecordSize);
    BakedWriter::WriteChunkDescriptor(out, kDataDescriptorOffset, "DATA",
                                      static_cast<std::uint64_t>(dataOffset),
                                      static_cast<std::uint64_t>(mips.pixels.size()), height,
                                      mips.levels.empty() ? 0 : mips.levels.front().rowPitch);

    BakedWriter::PutU32(out, kInfoDataOffset, width);
    BakedWriter::PutU32(out, kInfoDataOffset + 4, height);
    BakedWriter::PutU32(out, kInfoDataOffset + 8, static_cast<std::uint32_t>(pixelFormat));
    BakedWriter::PutU32(out, kInfoDataOffset + 12, static_cast<std::uint32_t>(colorSpace));
    BakedWriter::PutU32(out, kInfoDataOffset + 16, static_cast<std::uint32_t>(usage));
    BakedWriter::PutU32(out, kInfoDataOffset + 20, static_cast<std::uint32_t>(mips.levels.size()));
    BakedWriter::PutU32(out, kInfoDataOffset + 24, mips.levels.empty() ? 0 : mips.levels.front().rowPitch);

    for (std::size_t level = 0; level < mips.levels.size(); ++level)
    {
        const std::size_t recordBase = mipsDataOffset + level * kMipsRecordSize;
        BakedWriter::PutU32(out, recordBase, static_cast<std::uint32_t>(mips.levels[level].offset));
        BakedWriter::PutU32(out, recordBase + 4, mips.levels[level].rowPitch);
        BakedWriter::PutU32(out, recordBase + 8, mips.levels[level].byteSize);
    }

    if (!mips.pixels.empty())
    {
        std::memcpy(out.data() + dataOffset, mips.pixels.data(), mips.pixels.size());
    }
    return true;
}
} // namespace MiniEngine::Tools
