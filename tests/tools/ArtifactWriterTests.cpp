// ============================================================================
// ArtifactWriterTests.cpp — Baked writer 字节级 golden（审计 3.3 P2-2 补覆盖）
// 里程碑：M3-04/M3-05
// 职责：把"确定性 artifact 逐字节一致"从手工 PowerShell 双目录比对变成 ctest
//       断言。BakedWriter / MeshArtifactWriter / TextureArtifactWriter /
//       ManifestWriter 是确定性最敏感的字节写入代码，任何偏移/字段/顺序漂移
//       都必须在此显式失败。
// 关联：docs/evidence/（三轮审计闭环记录）；ADR-0004 §1
// ============================================================================

#include "BakedWriter.h"
#include "BuildKey.h"    // Tools::ToHexDigest
#include "CookSession.h" // CookedArtifactRecord
#include "ManifestWriter.h"
#include "MeshArtifactWriter.h"
#include "TextureArtifactWriter.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace BakedWriter = MiniEngine::Tools::BakedWriter;

namespace
{
using MiniEngine::Tools::BuildManifestJson;
using MiniEngine::Tools::BuildMeshArtifact;
using MiniEngine::Tools::BuildTextureArtifact;
using MiniEngine::Tools::CookedArtifactRecord;
using MiniEngine::Tools::Sha256Digest;

[[nodiscard]] Sha256Digest DigestOf(const std::uint8_t fill)
{
    Sha256Digest digest{};
    digest.fill(static_cast<std::byte>(fill));
    return digest;
}

[[nodiscard]] std::uint16_t ReadU16(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U;
}

[[nodiscard]] std::uint32_t ReadU32(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index)
    {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t ReadU64(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool TypeAt(const std::vector<std::byte>& bytes, const std::size_t offset, const char (&type)[5])
{
    return std::to_integer<unsigned char>(bytes[offset]) == static_cast<unsigned char>(type[0]) &&
           std::to_integer<unsigned char>(bytes[offset + 1]) == static_cast<unsigned char>(type[1]) &&
           std::to_integer<unsigned char>(bytes[offset + 2]) == static_cast<unsigned char>(type[2]) &&
           std::to_integer<unsigned char>(bytes[offset + 3]) == static_cast<unsigned char>(type[3]);
}

[[nodiscard]] float ReadFloat(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    std::uint32_t bits = ReadU32(bytes, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace

TEST(ArtifactWriterTests, AlignUpBoundaries)
{
    using BakedWriter::AlignUp;
    EXPECT_EQ(AlignUp(0, 16), 0U);
    EXPECT_EQ(AlignUp(1, 16), 16U);
    EXPECT_EQ(AlignUp(15, 16), 16U);
    EXPECT_EQ(AlignUp(16, 16), 16U);
    EXPECT_EQ(AlignUp(17, 16), 32U);
    EXPECT_EQ(AlignUp(128, 16), 128U);
}

TEST(ArtifactWriterTests, PutU16U32U64AreLittleEndian)
{
    std::vector<std::byte> bytes;
    BakedWriter::PutU16(bytes, 0, 0xBEEFU);
    ASSERT_EQ(bytes.size(), 2U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[0]), 0xEFU);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[1]), 0xBEU);

    BakedWriter::PutU32(bytes, 2, 0x01020304U);
    ASSERT_EQ(bytes.size(), 6U);
    EXPECT_EQ(ReadU32(bytes, 2), 0x01020304U);

    std::vector<std::byte> wide;
    BakedWriter::PutU64(wide, 0, 0x0102030405060708ULL);
    ASSERT_EQ(wide.size(), 8U);
    EXPECT_EQ(ReadU64(wide, 0), 0x0102030405060708ULL);
    EXPECT_EQ(std::to_integer<std::uint8_t>(wide[0]), 0x08U); // LSB first
    EXPECT_EQ(std::to_integer<std::uint8_t>(wide[7]), 0x01U); // MSB last
}

TEST(ArtifactWriterTests, PutValuesExpandBufferWithZeroPadding)
{
    std::vector<std::byte> bytes;
    BakedWriter::PutU16(bytes, 0, 0x0001U); // size 2
    BakedWriter::PutU16(bytes, 6, 0xABCDU); // 扩到 8，中间补零
    ASSERT_EQ(bytes.size(), 8U);
    EXPECT_EQ(ReadU32(bytes, 2), 0x00000000U); // 2..5 全零
    EXPECT_EQ(ReadU16(bytes, 6), 0xABCDU);
}

TEST(ArtifactWriterTests, WriteBakedHeaderFieldOffsets)
{
    using MiniEngine::Assets::kBakedFormatVersion;
    using MiniEngine::Assets::kBakedHeaderSize;
    const Sha256Digest buildKey = DigestOf(0xAB);

    std::vector<std::byte> bytes;
    // M4-02：WriteBakedHeader 增 formatVersion 参数，写入 flags（per-kind 格式版本载波）。
    BakedWriter::WriteBakedHeader(bytes, MiniEngine::Assets::BakedAssetKind::World, buildKey, 3, 200,
                                  MiniEngine::Assets::kWorldFormatVersion);

    ASSERT_EQ(bytes.size(), static_cast<std::size_t>(kBakedHeaderSize));
    EXPECT_EQ(std::to_integer<char>(bytes[0]), 'M');
    EXPECT_EQ(std::to_integer<char>(bytes[1]), 'E');
    EXPECT_EQ(std::to_integer<char>(bytes[2]), 'A');
    EXPECT_EQ(std::to_integer<char>(bytes[3]), '3');
    EXPECT_EQ(ReadU16(bytes, 4), kBakedFormatVersion);
    EXPECT_EQ(ReadU16(bytes, 6), 3U); // World
    EXPECT_EQ(ReadU32(bytes, 8), static_cast<std::uint32_t>(kBakedHeaderSize));
    EXPECT_EQ(ReadU32(bytes, 12), 3U); // chunkCount
    EXPECT_EQ(ReadU64(bytes, 16), 200U);
    for (std::size_t index = 0; index < buildKey.size(); ++index)
    {
        EXPECT_EQ(bytes[24 + index], buildKey[index]);
    }
    EXPECT_EQ(ReadU32(bytes, 56), 2U); // flags = kWorldFormatVersion
    EXPECT_EQ(ReadU32(bytes, 60), 0U); // reserved
}

TEST(ArtifactWriterTests, MeshArtifactLayoutGolden)
{
    using namespace MiniEngine::Assets;
    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    mesh.vertices.resize(2); // PbrVertex（48B）默认位模式不确定，golden 只断言显式写入的 position
    mesh.vertices[0].position[0] = 1.0F;
    mesh.vertices[0].position[1] = 2.0F;
    mesh.vertices[0].position[2] = 3.0F;
    mesh.vertices[1].position[0] = 4.0F;
    mesh.vertices[1].position[1] = 5.0F;
    mesh.vertices[1].position[2] = 6.0F;
    mesh.indices = {0, 1, 0};

    std::vector<std::byte> bytes;
    std::string error;
    ASSERT_TRUE(BuildMeshArtifact(DigestOf(0x11), mesh, bytes, error)) << error;

    // v2：64 header + 2*32 descriptors + VERT(96B)@128 + INDX(12B)@224 → 236
    EXPECT_EQ(bytes.size(), 236U);
    EXPECT_EQ(ReadU16(bytes, 6), static_cast<std::uint16_t>(BakedAssetKind::Mesh));
    EXPECT_EQ(ReadU32(bytes, 12), 2U); // VERT + INDX
    EXPECT_EQ(ReadU64(bytes, 16), 236U);
    EXPECT_EQ(ReadU32(bytes, 56), kMeshFormatVersion); // flags：.memesh v2

    EXPECT_TRUE(TypeAt(bytes, 64, "VERT"));
    EXPECT_EQ(ReadU32(bytes, 68), 0U); // flags
    EXPECT_EQ(ReadU64(bytes, 72), 128U);
    EXPECT_EQ(ReadU64(bytes, 80), 96U);
    EXPECT_EQ(ReadU32(bytes, 88), 2U);  // elementCount
    EXPECT_EQ(ReadU32(bytes, 92), 48U); // stride = sizeof(PbrVertex)

    EXPECT_TRUE(TypeAt(bytes, 96, "INDX"));
    EXPECT_EQ(ReadU64(bytes, 104), 224U);
    EXPECT_EQ(ReadU64(bytes, 112), 12U);
    EXPECT_EQ(ReadU32(bytes, 120), 3U);
    EXPECT_EQ(ReadU32(bytes, 124), 4U);

    // VERT 数据从 @128 开始，每顶点 stride 48；顶点 0 position 即 (1,2,3)，
    // 顶点 1 position 从 @176 开始。
    EXPECT_FLOAT_EQ(ReadFloat(bytes, 128), 1.0F);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, 132), 2.0F);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, 136), 3.0F);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, 176), 4.0F);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, 180), 5.0F);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, 184), 6.0F);
    // INDX 数据起点 @224：首个索引 0 → 前 4 字节为 0（小端）。
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[224]), 0U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[225]), 0U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[226]), 0U);
}

TEST(ArtifactWriterTests, TextureArtifactLayoutGolden)
{
    using namespace MiniEngine::Assets;
    // 2×2 BaseColor（sRGB）顶层 16B；mip 链 = 2×2 + 1×1，级联 20B。
    const std::vector<std::byte> topLevel{std::byte{1},  std::byte{2},  std::byte{3},  std::byte{255},
                                          std::byte{5},  std::byte{6},  std::byte{7},  std::byte{255},
                                          std::byte{9},  std::byte{10}, std::byte{11}, std::byte{255},
                                          std::byte{13}, std::byte{14}, std::byte{15}, std::byte{255}};
    MiniEngine::Tools::BuiltTextureMips mips;
    std::string mipError;
    ASSERT_TRUE(MiniEngine::Tools::BuildTextureMipChain(2, 2, TextureUsage::BaseColor, topLevel, mips, mipError))
        << mipError;
    ASSERT_EQ(mips.levels.size(), 2U);

    std::vector<std::byte> bytes;
    std::string error;
    ASSERT_TRUE(BuildTextureArtifact(DigestOf(0x22), 2, 2, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Srgb,
                                     TextureUsage::BaseColor, mips, bytes, error))
        << error;

    // v2：64 header + 3*32 descriptors + INFO(28B)@160 + MIPS(24B)@192 + DATA(20B)@224 → 244
    EXPECT_EQ(bytes.size(), 244U);
    EXPECT_EQ(ReadU16(bytes, 6), static_cast<std::uint16_t>(BakedAssetKind::Texture));
    EXPECT_EQ(ReadU32(bytes, 12), 3U); // INFO + MIPS + DATA
    EXPECT_EQ(ReadU64(bytes, 16), 244U);
    EXPECT_EQ(ReadU32(bytes, 56), kTextureFormatVersion); // flags：.metex v2

    EXPECT_TRUE(TypeAt(bytes, 64, "INFO"));
    EXPECT_EQ(ReadU64(bytes, 72), 160U); // INFO payload offset
    EXPECT_EQ(ReadU64(bytes, 80), 28U);
    EXPECT_EQ(ReadU32(bytes, 88), 7U); // elementCount：7 × u32
    EXPECT_EQ(ReadU32(bytes, 92), 4U);

    EXPECT_TRUE(TypeAt(bytes, 96, "MIPS"));
    EXPECT_EQ(ReadU64(bytes, 104), 192U);
    EXPECT_EQ(ReadU64(bytes, 112), 24U);
    EXPECT_EQ(ReadU32(bytes, 120), 2U); // elementCount = mipCount
    EXPECT_EQ(ReadU32(bytes, 124), 12U);

    EXPECT_TRUE(TypeAt(bytes, 128, "DATA"));
    EXPECT_EQ(ReadU64(bytes, 136), 224U);
    EXPECT_EQ(ReadU64(bytes, 144), 20U);
    EXPECT_EQ(ReadU32(bytes, 152), 2U); // elementCount = height
    EXPECT_EQ(ReadU32(bytes, 156), 8U); // stride = mip0 rowPitch

    // INFO payload：width | height | pixelFormat | colorSpace | usage | mipCount | mip0RowPitch。
    EXPECT_EQ(ReadU32(bytes, 160), 2U);
    EXPECT_EQ(ReadU32(bytes, 164), 2U);
    EXPECT_EQ(ReadU32(bytes, 168), 1U); // Rgba8Unorm
    EXPECT_EQ(ReadU32(bytes, 172), 1U); // Srgb
    EXPECT_EQ(ReadU32(bytes, 176), 1U); // BaseColor
    EXPECT_EQ(ReadU32(bytes, 180), 2U); // mipCount
    EXPECT_EQ(ReadU32(bytes, 184), 8U); // rowPitch = width*4

    // MIPS 记录：mip0 (0, 8, 16)、mip1 (16, 4, 4)；offset 相对 DATA 起点。
    EXPECT_EQ(ReadU32(bytes, 192), 0U);
    EXPECT_EQ(ReadU32(bytes, 196), 8U);
    EXPECT_EQ(ReadU32(bytes, 200), 16U);
    EXPECT_EQ(ReadU32(bytes, 204), 16U);
    EXPECT_EQ(ReadU32(bytes, 208), 4U);
    EXPECT_EQ(ReadU32(bytes, 212), 4U);

    // DATA：mip0 在前（16B）+ mip1（4B）。
    for (std::size_t index = 0; index < topLevel.size(); ++index)
    {
        EXPECT_EQ(bytes[224 + index], topLevel[index]);
    }
}

TEST(ArtifactWriterTests, TextureArtifactRejectsInvalidInputs)
{
    using namespace MiniEngine::Assets;
    std::vector<std::byte> out;
    std::string error;

    // 先构造一份合法 mip 链（2×2 BaseColor），负向用例逐项破坏单一前置条件。
    MiniEngine::Tools::BuiltTextureMips validMips;
    ASSERT_TRUE(MiniEngine::Tools::BuildTextureMipChain(2, 2, TextureUsage::BaseColor,
                                                        std::vector<std::byte>(16, std::byte{255U}), validMips, error))
        << error;

    // width=0
    EXPECT_FALSE(BuildTextureArtifact(DigestOf(0x22), 0, 2, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Srgb,
                                      TextureUsage::BaseColor, validMips, out, error));
    EXPECT_NE(error.find("non-zero"), std::string::npos) << error;

    // usage/colorSpace 组合违反语义契约（normal 贴图不允许 sRGB，02 篇 Texture v2）。
    EXPECT_FALSE(BuildTextureArtifact(DigestOf(0x22), 2, 2, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Srgb,
                                      TextureUsage::Normal, validMips, out, error));
    EXPECT_NE(error.find("usage/colorSpace"), std::string::npos) << error;

    // mip 记录与像素流不符（级联像素被截断，末级 offset+byteSize 不再覆盖 DATA）。
    MiniEngine::Tools::BuiltTextureMips truncated = validMips;
    truncated.pixels.pop_back();
    EXPECT_FALSE(BuildTextureArtifact(DigestOf(0x22), 2, 2, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Srgb,
                                      TextureUsage::BaseColor, truncated, out, error));
    EXPECT_NE(error.find("do not cover"), std::string::npos) << error;
    EXPECT_TRUE(out.empty());
}

TEST(ArtifactWriterTests, ManifestJsonGoldenAndOrdering)
{
    CookedArtifactRecord mesh;
    mesh.assetUri = "asset://demo/box";
    mesh.kind = "mesh";
    mesh.buildKey = DigestOf(0x02);
    mesh.artifactHash = DigestOf(0x01);
    mesh.fileSize = 204;
    mesh.artifactPath = "cache/aa/0101010101010101010101010101010101010101010101010101010101010101-mesh-0.memesh";

    CookedArtifactRecord texture;
    texture.assetUri = "asset://demo/box#image/0";
    texture.kind = "texture";
    texture.buildKey = DigestOf(0x04);
    texture.artifactHash = DigestOf(0x03);
    texture.fileSize = 152;
    texture.artifactPath = "cache/aa/0303030303030303030303030303030303030303030303030303030303030303-image-0.metex";

    const std::string h01 = MiniEngine::Tools::ToHexDigest(mesh.artifactHash);
    const std::string h02 = MiniEngine::Tools::ToHexDigest(mesh.buildKey);
    const std::string h03 = MiniEngine::Tools::ToHexDigest(texture.artifactHash);
    const std::string h04 = MiniEngine::Tools::ToHexDigest(texture.buildKey);
    ASSERT_EQ(h01.size(), 64U);

    const std::vector<CookedArtifactRecord> records{mesh, texture};
    const std::string profile = "windows-d3d11";
    const std::string actual = BuildManifestJson(records, profile);

    // 确定性字段顺序契约（与 03/05 篇一致）：asset 数组内逐字段固定、无空白的紧凑 JSON。
    const std::string expected =
        "{\"assets\":[{\"artifactHash\":\"" + h01 + "\",\"artifactPath\":\"" + mesh.artifactPath +
        "\",\"assetUri\":\"" + mesh.assetUri + "\",\"buildKey\":\"" + h02 + "\",\"fileSize\":204,\"kind\":\"mesh\"}," +
        "{\"artifactHash\":\"" + h03 + "\",\"artifactPath\":\"" + texture.artifactPath + "\",\"assetUri\":\"" +
        texture.assetUri + "\",\"buildKey\":\"" + h04 + "\",\"fileSize\":152,\"kind\":\"texture\"}],\"profile\":\"" +
        profile + "\",\"schemaVersion\":1}";
    EXPECT_EQ(actual, expected);
    // 确定性：同输入两次调用输出一致；条目顺序=输入顺序（cook 顺序）。
    EXPECT_EQ(BuildManifestJson(records, profile), actual);
    EXPECT_LT(actual.find(mesh.assetUri), actual.find(texture.assetUri));
    EXPECT_EQ(BuildManifestJson({}, profile),
              std::string("{\"assets\":[],\"profile\":\"") + profile + "\",\"schemaVersion\":1}");
}

// D2（M4-09 审查）：重复 assetUri 是发布期就必须暴露的冲突。运行期
// AssetRegistry 对重复 AssetId fail-closed（整份 manifest 被拒），Cooker 必须
// 在 PublishManifest 前把它查出来——这里锁定检测函数本身的行为与确定性。
TEST(ArtifactWriterTests, DuplicateAssetUriDetection)
{
    using MiniEngine::Tools::FindDuplicateAssetUris;

    CookedArtifactRecord environmentA;
    environmentA.assetUri = "asset://environments/m4-baseline";
    environmentA.kind = "texture";
    environmentA.buildKey = DigestOf(0x0A);
    environmentA.artifactHash = DigestOf(0x0B);
    environmentA.fileSize = 1;
    environmentA.artifactPath = "cache/aa/a";

    CookedArtifactRecord environmentB = environmentA; // 同 URI、不同 buildKey/hash
    environmentB.buildKey = DigestOf(0x0C);
    environmentB.artifactHash = DigestOf(0x0D);
    environmentB.artifactPath = "cache/bb/b";

    CookedArtifactRecord mesh;
    mesh.assetUri = "asset://tests/m4-visual-baseline";
    mesh.kind = "mesh";
    mesh.buildKey = DigestOf(0x0E);
    mesh.artifactHash = DigestOf(0x0F);
    mesh.fileSize = 2;
    mesh.artifactPath = "cache/cc/c";

    // 无重复 → 空表。
    EXPECT_TRUE(FindDuplicateAssetUris({mesh, environmentA}).empty());

    // 同 URI 出现两次（不同 buildKey）→ 报告该 URI。
    const std::vector<CookedArtifactRecord> conflicting{mesh, environmentA, environmentB};
    const std::vector<std::string> duplicates = FindDuplicateAssetUris(conflicting);
    ASSERT_EQ(duplicates.size(), 1U);
    EXPECT_EQ(duplicates.front(), "asset://environments/m4-baseline");

    // 两次调用结果一致（确定性）；多个重复 URI 按 URI 字节序排序。
    EXPECT_EQ(FindDuplicateAssetUris(conflicting), duplicates);

    CookedArtifactRecord anotherDup;
    anotherDup.assetUri = "asset://aaa/earlier";
    anotherDup.kind = "mesh";
    anotherDup.buildKey = DigestOf(0x10);
    anotherDup.artifactHash = DigestOf(0x11);
    anotherDup.artifactPath = "cache/dd/d";
    const std::vector<CookedArtifactRecord> multiRecords{anotherDup, anotherDup, mesh, environmentA, environmentB};
    const std::vector<std::string> multi = FindDuplicateAssetUris(multiRecords);
    ASSERT_EQ(multi.size(), 2U);
    EXPECT_EQ(multi.front(), "asset://aaa/earlier"); // 字节序：aaa < environments
    EXPECT_EQ(multi.back(), "asset://environments/m4-baseline");
}
