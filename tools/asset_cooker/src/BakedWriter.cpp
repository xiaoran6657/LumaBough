// ============================================================================
// BakedWriter.cpp — Baked 产物字节写入原语的实现
// 里程碑：M3-04（审计 3.1 抽取）
// 职责：实现小端整数写入、4 字符 chunk 类型写入、64B header 与 32B 描述符的
//       显式偏移序列化。全部按 wire 偏移写，不依赖结构体内存布局，
//       从而规避 MSVC 对齐 padding 带来的不确定性。
// 关联：tools/asset_cooker/src/BakedWriter.h（契约）
//       engine/assets/src/BakedReader.cpp（读取端逐字段对称）
// ============================================================================

#include "BakedWriter.h"

namespace MiniEngine::Tools::BakedWriter
{
namespace
{
// 全部小端字段都通过这四个写入函数，避免任何一处漏掉位移/掩码。
void PutBytes(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value,
              const std::size_t width)
{
    if (bytes.size() < offset + width)
    {
        bytes.resize(offset + width, std::byte{0});
    }
    for (std::size_t index = 0; index < width; ++index)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}
} // namespace

void PutU16(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value)
{
    PutBytes(bytes, offset, value, 2);
}

void PutU32(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value)
{
    PutBytes(bytes, offset, value, 4);
}

void PutU64(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value)
{
    PutBytes(bytes, offset, value, 8);
}

void PutType4(std::vector<std::byte>& bytes, const std::size_t offset, const char (&type)[5])
{
    if (bytes.size() < offset + 4)
    {
        bytes.resize(offset + 4, std::byte{0});
    }
    bytes[offset] = static_cast<std::byte>(type[0]);
    bytes[offset + 1] = static_cast<std::byte>(type[1]);
    bytes[offset + 2] = static_cast<std::byte>(type[2]);
    bytes[offset + 3] = static_cast<std::byte>(type[3]);
}

void WriteBakedHeader(std::vector<std::byte>& bytes, const MiniEngine::Assets::BakedAssetKind kind,
                      const MiniEngine::Assets::Sha256Digest& buildKey, const std::uint32_t chunkCount,
                      const std::uint64_t fileSize, const std::uint32_t formatVersion)
{
    using namespace MiniEngine::Assets;
    if (bytes.size() < kBakedHeaderSize)
    {
        bytes.resize(static_cast<std::size_t>(kBakedHeaderSize), std::byte{0});
    }
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'A'};
    bytes[3] = std::byte{'3'};
    PutU16(bytes, 4, kBakedFormatVersion);
    PutU16(bytes, 6, static_cast<std::uint16_t>(kind));
    PutU32(bytes, 8, static_cast<std::uint32_t>(kBakedHeaderSize));
    PutU32(bytes, 12, chunkCount);
    PutU64(bytes, 16, fileSize);
    for (std::size_t index = 0; index < buildKey.size(); ++index)
    {
        bytes[24 + index] = buildKey[index];
    }
    // M4 起flags 承载各 kind 的格式版本（kMeshFormatVersion 等），读取端语义层据此
    // 输出 recook diagnostic；reserved 仍恒为 0。
    PutU32(bytes, 56, formatVersion);
    PutU32(bytes, 60, 0); // reserved
}

void WriteChunkDescriptor(std::vector<std::byte>& bytes, const std::size_t offset, const char (&type)[5],
                          const std::uint64_t dataOffset, const std::uint64_t byteSize,
                          const std::uint32_t elementCount, const std::uint32_t stride)
{
    PutType4(bytes, offset, type);
    PutU32(bytes, offset + 4, 0); // flags
    PutU64(bytes, offset + 8, dataOffset);
    PutU64(bytes, offset + 16, byteSize);
    PutU32(bytes, offset + 24, elementCount);
    PutU32(bytes, offset + 28, stride);
}
} // namespace MiniEngine::Tools::BakedWriter
