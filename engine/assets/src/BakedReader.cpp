// ============================================================================
// BakedReader.cpp — 烘焙产物的严格校验实现
// 里程碑：M3
// 职责：按 BakedFormat 的 wire 布局逐字段小端解码并校验，不依赖本机字节序与
//       编译器对齐。这里的每一条失败分支都对应 Reader 防御清单的一项，
//       保证"Parse 通过 = 字节自洽、未被篡改"。
// 关联：docs/architecture/DECISIONS.md §1
//       engine/assets/include/MiniEngine/Assets/BakedReader.h（对外的失败语义）
// ============================================================================

#include <MiniEngine/Assets/BakedReader.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace MiniEngine::Assets
{
namespace
{
// 逐字节小端解码一个无符号整数；越界返回 false，成功写入 value。
// 不做 memcpy / 重解释：产物可能来自任意机器，不能依赖本机字节序与对齐。
template <typename T> bool ReadLittleEndian(const std::span<const std::byte> bytes, const std::size_t offset, T& value)
{
    static_assert(std::is_unsigned_v<T>);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    {
        return false;
    }

    value = 0;

    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        value |= static_cast<T>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return true;
}

// 判断 value 是否按 alignment 对齐；alignment 必须为 2 的幂（本文件只传常量）。
bool IsAligned(const std::uint64_t value, const std::uint64_t alignment)
{
    return value % alignment == 0;
}

// 判断 [offset, offset+size) 是否完全落在 [0, total) 内。
// 用减法而非加法判定上界，天然免疫 offset+size 的整数回绕。
bool IsRangeValid(const std::uint64_t offset, const std::uint64_t size, const std::uint64_t total)
{
    return offset <= total && size <= total - offset;
}
} // namespace

bool BakedReader::Parse(const std::span<const std::byte> fileBytes, const BakedReadExpectation& expectation,
                        BakedReadResult& result, std::string& error)
{
    result = {};
    error.clear();

    if (fileBytes.size() < kBakedHeaderSize ||
        std::memcmp(fileBytes.data(), kBakedMagic.data(), kBakedMagic.size()) != 0)
    {
        error = "Invalid or truncated MEA3 header.";
        return false;
    }

    std::uint16_t version{};
    std::uint16_t assetType{};
    std::uint32_t headerSize{};
    std::uint64_t fileSize{};
    std::uint32_t chunkCount{};
    std::uint32_t flags{};
    std::uint32_t reserved{};

    // Offsets must match the explicitly documented 64-byte wire layout.
    if (!ReadLittleEndian(fileBytes, 4, version) || !ReadLittleEndian(fileBytes, 6, assetType) ||
        !ReadLittleEndian(fileBytes, 8, headerSize) || !ReadLittleEndian(fileBytes, 12, chunkCount) ||
        !ReadLittleEndian(fileBytes, 16, fileSize) || !ReadLittleEndian(fileBytes, 56, flags) ||
        !ReadLittleEndian(fileBytes, 60, reserved))
    {
        error = "Invalid or truncated MEA3 header.";
        return false;
    }
    // header.flags 自 M4 起作为"各 kind 的格式版本"载波（Mesh=2/Texture=2/
    // World=2/Material=1），结构层不再强制为 0；具体版本是否被支持由语义层
    // （DecodeMeshChunks 等）判定并输出 recook diagnostic。
    if (version != kBakedFormatVersion || headerSize != kBakedHeaderSize ||
        assetType != static_cast<std::uint16_t>(expectation.kind) || fileSize != fileBytes.size() || reserved != 0)
    {
        error = "MEA3 version, header, kind, flags, or file size mismatch.";
        return false;
    }

    // chunk 表范围：chunkCount 既要尊重上限，也要保证"chunkCount × 描述符大小"
    // 不在 64 位乘法里回绕（用除法回代比较做防溢出判定）。
    const std::uint64_t tableOffset = kBakedHeaderSize;
    const auto tableSize = static_cast<std::uint64_t>(chunkCount) * kChunkDescriptorSize;
    if (chunkCount > kMaxChunkCount || tableSize / kChunkDescriptorSize != chunkCount ||
        !IsRangeValid(tableOffset, tableSize, fileSize))
    {
        error = "MEA3 chunk table is out of bounds.";
        return false;
    }

    BakedReadResult parsed{};
    parsed.header.kind = static_cast<std::uint16_t>(expectation.kind);
    parsed.header.version = version;
    parsed.header.headerSize = headerSize;
    parsed.header.fileSize = fileSize;
    parsed.header.chunkCount = chunkCount;
    std::copy_n(fileBytes.begin() + 24, parsed.header.buildKey.size(), parsed.header.buildKey.begin());
    parsed.header.flags = flags;
    // 给出期望 BuildKey 时必须严格相等：防止"字节自洽但张冠李戴"的产物
    // （例如上一代 artifact 冒充新 Manifest 的条目）被接受。
    if (expectation.buildKey && parsed.header.buildKey != *expectation.buildKey)
    {
        error = "MEA3 BuildKey does not match the expected Manifest or Cooker key.";
        return false;
    }

    // 逐条解码 chunk 描述符并做空间合法性校验：payload 必须落在 chunk 表之后、
    // 16 字节对齐、且与已有 chunk 互不重叠，任一违反都视为畸形产物。
    parsed.chunks.reserve(chunkCount);
    for (std::uint32_t index = 0; index < chunkCount; ++index)
    {
        const std::size_t base = static_cast<std::size_t>(tableOffset + index * kChunkDescriptorSize);
        BakedChunk chunk{};
        std::memcpy(chunk.type.data(), fileBytes.data() + base, chunk.type.size());
        if (!ReadLittleEndian(fileBytes, base + 4, chunk.flags) ||
            !ReadLittleEndian(fileBytes, base + 8, chunk.offset) ||
            !ReadLittleEndian(fileBytes, base + 16, chunk.size) ||
            !ReadLittleEndian(fileBytes, base + 24, chunk.elementCount) ||
            !ReadLittleEndian(fileBytes, base + 28, chunk.stride))
        {
            error = "Failed to decode MEA3 chunk descriptor.";
            return false;
        }

        if (chunk.flags != 0 || !IsAligned(chunk.offset, kChunkAlignment) ||
            !IsRangeValid(chunk.offset, chunk.size, fileSize) || chunk.offset < tableOffset + tableSize)
        {
            error = "MEA3 chunk flags, alignment, or payload range is invalid.";
            return false;
        }

        for (const BakedChunk& previous : parsed.chunks)
        {
            const bool overlaps =
                chunk.offset < previous.offset + previous.size && previous.offset < chunk.offset + chunk.size;
            if (overlaps)
            {
                error = "MEA3 chunk payloads overlap.";
                return false;
            }
        }
        parsed.chunks.push_back(chunk);
    }

    result = std::move(parsed);
    return true;
}

bool BakedReader::ValidateRequiredChunks(const BakedReadResult& result,
                                         const std::span<const std::array<char, 4>> requiredTypes, std::string& error)
{
    for (const std::array<char, 4>& required : requiredTypes)
    {
        std::size_t occurrences = 0;
        for (const BakedChunk& chunk : result.chunks)
        {
            if (chunk.type == required)
            {
                ++occurrences;
            }
        }

        const std::string_view typeName{required.data(), required.size()};
        if (occurrences == 0)
        {
            error = "MEA3 required chunk '" + std::string{typeName} + "' is missing.";
            return false;
        }
        if (occurrences > 1)
        {
            error = "MEA3 required chunk '" + std::string{typeName} + "' appears more than once.";
            return false;
        }
    }
    return true;
}

std::optional<std::span<const std::byte>> BakedReader::GetChunkBytes(const std::span<const std::byte> fileBytes,
                                                                     const BakedChunk& chunk)
{
    if (chunk.offset > fileBytes.size() || chunk.size > fileBytes.size() - chunk.offset)
    {
        return std::nullopt;
    }

    return fileBytes.subspan(static_cast<std::size_t>(chunk.offset), static_cast<std::size_t>(chunk.size));
}
} // namespace MiniEngine::Assets