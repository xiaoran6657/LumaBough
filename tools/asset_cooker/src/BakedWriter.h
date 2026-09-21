#pragma once

// ============================================================================
// BakedWriter.h — 引擎 Baked 产物（.memesh/.metex/.meworld）共享写入原语
// 里程碑：M3-04（审计 3.1 结构性重构抽取）
// 职责：BakedHeader（64B）与 32B chunk descriptor 是三种 artifact 共用的固定布局；
//       此前 BuildMeshArtifact / BuildTextureArtifact / BuildMeworldArtifact 各自
//       复制 write16/32/64/type 与 header 填写逻辑。本模块收敛唯一写入路径，
//       保证 byte-for-byte 与既有 writer 一致（确定性 artifact 的硬约束）。
// 关联：engine/assets/include/MiniEngine/Assets/BakedFormat.h（wire 布局契约）
// ============================================================================

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/Sha256.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniEngine::Tools::BakedWriter
{
// 向上对齐到 alignment（chunk 数据区 offset 必须 16 对齐，BakedReader 契约）。
[[nodiscard]] constexpr std::size_t AlignUp(const std::size_t value, const std::size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// 小端写入。缓冲区不足以容纳 offset+宽度时自动扩展（既有 writer 的扩展语义）。
// 调用方预先分配完整缓冲时不会触发扩展，输出字节逐位一致。
void PutU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value);
void PutU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value);
void PutU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value);
// 4 字符 chunk 类型（"VERT"/"INDX"/"INFO"/"DATA"/"STRS"…，忽略结尾 NUL）。
// 审计 3.3：类型固定为 char(&)[5]（字符串字面量），杜绝 const char* 无界读 type[0..3]。
void PutType4(std::vector<std::byte>& bytes, std::size_t offset, const char (&type)[5]);

// 64B header：magic "MEA3" + version/kind/headerSize/chunkCount/fileSize + buildKey(32) + flags/reserved。
// M4 起 flags = 调用方传入的 per-kind 格式版本（kMeshFormatVersion 等），reserved 恒为 0；
// 缓冲不足 64B 时自动扩展。
void WriteBakedHeader(std::vector<std::byte>& bytes, MiniEngine::Assets::BakedAssetKind kind,
                      const MiniEngine::Assets::Sha256Digest& buildKey, std::uint32_t chunkCount,
                      std::uint64_t fileSize, std::uint32_t formatVersion);

// 32B chunk descriptor：type[4] + flags(u32,0) + offset(u64) + size(u64) + elementCount(u32) + stride(u32)。
// type 同上取字符串字面量（编译期保证 4 字符 + NUL）。
void WriteChunkDescriptor(std::vector<std::byte>& bytes, std::size_t offset, const char (&type)[5],
                          std::uint64_t dataOffset, std::uint64_t byteSize, std::uint32_t elementCount,
                          std::uint32_t stride);
} // namespace MiniEngine::Tools::BakedWriter
