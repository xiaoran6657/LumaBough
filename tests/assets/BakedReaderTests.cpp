// ============================================================================
// BakedReaderTests.cpp — 烘焙产物 Reader 的防御清单
// 里程碑：M3-03
// 职责：逐条验证 BakedReader 的结构性防御（magic/版本/kind/fileSize/chunk 越界/
//       对齐/重叠/BuildKey 不符/必需 chunk 缺失或重复）与 GetChunkBytes 边界。
// 关联：engine/assets/src/BakedReader.cpp（被测实现）
//       docs/architecture/README.md（Reader 防御 1-10/13/15）
// ============================================================================

#include <MiniEngine/Assets/BakedReader.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

namespace
{
// 构造一个合法的 64-byte MEA3 header（Mesh、version 1、headerSize 64、chunkCount 0、fileSize 64）。
std::array<std::byte, MiniEngine::Assets::kBakedHeaderSize> MakeValidHeader()
{
    std::array<std::byte, MiniEngine::Assets::kBakedHeaderSize> bytes{};
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    bytes[4] = std::byte{1};   // formatVersion = 1 (LE)
    bytes[6] = std::byte{1};   // assetType = Mesh (LE)
    bytes[8] = std::byte{64};  // headerSize = 64 (LE)
    bytes[16] = std::byte{64}; // fileSize = 64 (LE)
    return bytes;
}

bool ParseHeader(const std::array<std::byte, MiniEngine::Assets::kBakedHeaderSize>& bytes, std::string& error)
{
    MiniEngine::Assets::BakedReadResult result;
    return MiniEngine::Assets::BakedReader::Parse(
        bytes, MiniEngine::Assets::BakedReadExpectation{.kind = MiniEngine::Assets::BakedAssetKind::Mesh}, result,
        error);
}
} // namespace

// M3-03 Reader 防御 #1：最小 file size（magic 不足）。
TEST(BakedReaderTests, RejectsTruncatedInput)
{
    const std::array<std::byte, 4> truncated{std::byte{'M'}, std::byte{'E'}, std::byte{'A'}, std::byte{'3'}};

    MiniEngine::Assets::BakedReadResult result;
    std::string error;
    EXPECT_FALSE(MiniEngine::Assets::BakedReader::Parse(
        truncated, MiniEngine::Assets::BakedReadExpectation{.kind = MiniEngine::Assets::BakedAssetKind::Mesh}, result,
        error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(result.header.fileSize, 0);
    EXPECT_TRUE(result.chunks.empty());
}

// M3-03 Reader 防御 #2a：magic 错误。
TEST(BakedReaderTests, RejectsWrongMagic)
{
    auto bytes = MakeValidHeader();
    bytes[0] = std::byte{'X'};
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
    EXPECT_FALSE(error.empty());
}

// M3-03 Reader 防御 #2b：version 错误。
TEST(BakedReaderTests, RejectsWrongVersion)
{
    auto bytes = MakeValidHeader();
    bytes[4] = std::byte{2};
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M3-03 Reader 防御 #2c：assetType 与 expectation 不匹配。
TEST(BakedReaderTests, RejectsWrongKind)
{
    auto bytes = MakeValidHeader();
    bytes[6] = std::byte{2}; // Texture != expected Mesh
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M3-03 Reader 防御 #2d：headerSize 错误。
TEST(BakedReaderTests, RejectsWrongHeaderSize)
{
    auto bytes = MakeValidHeader();
    bytes[8] = std::byte{32};
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M3-03 Reader 防御 #3：fileSize != actualSize。
TEST(BakedReaderTests, RejectsMismatchedFileSize)
{
    auto bytes = MakeValidHeader();
    bytes[16] = std::byte{32};
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M4-02：header.flags 自 M4 起作为 per-kind 格式版本载波，结构层不再强制为 0——
// 非零 flags 必须被接受并透传（具体版本是否支持由语义层判定并输出 recook diagnostic）。
TEST(BakedReaderTests, AcceptsFormatVersionFlagsAndCarriesValue)
{
    auto bytes = MakeValidHeader();
    bytes[56] = std::byte{2}; // .memesh v2 的 flags（kMeshFormatVersion）
    std::string error;
    MiniEngine::Assets::BakedReadResult result;
    ASSERT_TRUE(MiniEngine::Assets::BakedReader::Parse(
        bytes, MiniEngine::Assets::BakedReadExpectation{.kind = MiniEngine::Assets::BakedAssetKind::Mesh}, result,
        error))
        << error;
    EXPECT_EQ(result.header.flags, 2U);
}

TEST(BakedReaderTests, RejectsNonZeroReserved)
{
    auto bytes = MakeValidHeader();
    bytes[60] = std::byte{1};
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M3-03 Reader 防御 #15：调用方传入未经 Parse 的不可信 chunk，取 span 前重新检查。
TEST(BakedReaderTests, RejectsAnUntrustedChunkRangeBeforeCreatingASpan)
{
    const std::array<std::byte, 4> bytes{};
    MiniEngine::Assets::BakedChunk untrusted{};
    untrusted.offset = 3;
    untrusted.size = 2;

    EXPECT_FALSE(MiniEngine::Assets::BakedReader::GetChunkBytes(bytes, untrusted).has_value());
}

// M3-03 Reader 防御 #15b：合法 chunk 返回正确的 span。
TEST(BakedReaderTests, ReturnsSpanForValidChunkRange)
{
    const std::array<std::byte, 16> bytes{};
    MiniEngine::Assets::BakedChunk chunk{};
    chunk.offset = 4;
    chunk.size = 8;

    const auto span = MiniEngine::Assets::BakedReader::GetChunkBytes(bytes, chunk);
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(span->size(), 8);
}

// M3-03 Reader 防御 #13：header BuildKey 与期望不匹配。
TEST(BakedReaderTests, RejectsAHeaderBuildKeyMismatch)
{
    auto bytes = MakeValidHeader();

    std::array<std::byte, 32> differentBuildKey{};
    differentBuildKey[0] = std::byte{1};
    MiniEngine::Assets::BakedReadResult result;
    std::string error;

    EXPECT_FALSE(MiniEngine::Assets::BakedReader::Parse(
        bytes,
        MiniEngine::Assets::BakedReadExpectation{.kind = MiniEngine::Assets::BakedAssetKind::Mesh,
                                                 .buildKey = differentBuildKey},
        result, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(result.header.fileSize, 0);
    EXPECT_TRUE(result.chunks.empty());
}

// M3-03 Reader 防御 #13b：BuildKey 与期望匹配时成功（空文件、无 chunk）。
TEST(BakedReaderTests, AcceptsValidHeaderWithMatchingBuildKey)
{
    auto bytes = MakeValidHeader();

    std::array<std::byte, 32> emptyBuildKey{};
    MiniEngine::Assets::BakedReadResult result;
    std::string error;

    EXPECT_TRUE(MiniEngine::Assets::BakedReader::Parse(
        bytes,
        MiniEngine::Assets::BakedReadExpectation{.kind = MiniEngine::Assets::BakedAssetKind::Mesh,
                                                 .buildKey = emptyBuildKey},
        result, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(result.header.kind, static_cast<std::uint16_t>(MiniEngine::Assets::BakedAssetKind::Mesh));
    EXPECT_EQ(result.header.chunkCount, 0);
    EXPECT_TRUE(result.chunks.empty());
}

// M3-03 Reader 防御 #4/#5：chunkCount 超预算、chunk table 越界。
TEST(BakedReaderTests, RejectsChunkCountOverBudget)
{
    auto bytes = MakeValidHeader();
    // chunkCount = 4097 (> kMaxChunkCount)
    bytes[12] = std::byte{0x01};
    bytes[13] = std::byte{0x10};
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M3-03 Reader 防御 #5：chunk table 越界（chunkCount=1，但 fileSize 只够 header）。
TEST(BakedReaderTests, RejectsChunkTableOutOfBounds)
{
    auto bytes = MakeValidHeader();
    bytes[12] = std::byte{1}; // chunkCount = 1
    std::string error;
    EXPECT_FALSE(ParseHeader(bytes, error));
}

// M3-03 Reader 防御 #10：必需 chunk 恰好一个。
TEST(BakedReaderTests, RequiredChunkMissingIsRejected)
{
    MiniEngine::Assets::BakedReadResult result;
    result.chunks.push_back(MiniEngine::Assets::BakedChunk{.type = {'I', 'N', 'F', 'O'}});

    const std::array<std::array<char, 4>, 2> required{'I', 'N', 'F', 'O', 'V', 'E', 'R', 'T'};
    std::string error;
    EXPECT_FALSE(MiniEngine::Assets::BakedReader::ValidateRequiredChunks(result, required, error));
    EXPECT_FALSE(error.empty());
}

TEST(BakedReaderTests, RequiredChunkDuplicateIsRejected)
{
    MiniEngine::Assets::BakedReadResult result;
    result.chunks.push_back(MiniEngine::Assets::BakedChunk{.type = {'I', 'N', 'F', 'O'}});
    result.chunks.push_back(MiniEngine::Assets::BakedChunk{.type = {'I', 'N', 'F', 'O'}});

    const std::array<std::array<char, 4>, 1> required{'I', 'N', 'F', 'O'};
    std::string error;
    EXPECT_FALSE(MiniEngine::Assets::BakedReader::ValidateRequiredChunks(result, required, error));
    EXPECT_FALSE(error.empty());
}

TEST(BakedReaderTests, RequiredChunkExactlyOnceIsAccepted)
{
    MiniEngine::Assets::BakedReadResult result;
    result.chunks.push_back(MiniEngine::Assets::BakedChunk{.type = {'I', 'N', 'F', 'O'}});
    result.chunks.push_back(MiniEngine::Assets::BakedChunk{.type = {'V', 'E', 'R', 'T'}});

    const std::array<std::array<char, 4>, 2> required{'I', 'N', 'F', 'O', 'V', 'E', 'R', 'T'};
    std::string error;
    EXPECT_TRUE(MiniEngine::Assets::BakedReader::ValidateRequiredChunks(result, required, error));
    EXPECT_TRUE(error.empty());
}

// Add table-driven cases for wrong endian, payload overflow, overlapping chunks,
// and Manifest artifact-hash mismatch (depends on the real writer in M3-04).
// Build positive fixtures with the real writer rather than native struct dumps.
// M3-03 Reader 防御 #11（elementCount×stride == chunk size）与 #12（float finite）
// 依赖每类 artifact 的 chunk schema，属于上层语义层（M3-04 的 mesh/texture/world
// 解析）；BakedReader 提供的通用原语是防御 #10 的 ValidateRequiredChunks。
