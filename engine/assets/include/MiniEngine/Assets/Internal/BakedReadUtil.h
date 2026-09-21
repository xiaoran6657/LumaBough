#pragma once

// ============================================================================
// BakedReadUtil.h — Assets 内部共享读取小工具（非公共 API）
// 里程碑：M3-04（审计 3.1 结构性重构收敛）
// 职责：AssetManager.cpp 与 WorldLoader.cpp 各自复制过一份 "读 u32 little-endian"
//       与 "从 chunk 表取唯一 chunk" 的实现，语义相同、错误约定略不同（bool vs
//       三态）。本头收敛唯一实现：LE 解码 + 三态（Absent/Unique/Duplicate）查找，
//       两个消费者都能无歧义表达"缺失/重复/恰好一个"。
// 注意：本头位于 Assets 的 include 目录内仅供引擎内部使用（World 已链接 Assets
//       的 PUBLIC include，因此可直接 include）。不是稳定公共 API——路径/签名后续
//       可能变更，外部工具请勿依赖。
// ============================================================================

#include <MiniEngine/Assets/BakedFormat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MiniEngine::Assets::Internal
{
// 从 chunk 表提取某种类型的唯一 chunk。
//   Absent    —— 无该类型（header-only 占位 artifact 合法场景）
//   Unique    —— 恰好一个（返回指针）
//   Duplicate —— 出现多次（畸形，调用方拒绝）
enum class ChunkExtractStatus
{
    Absent,
    Unique,
    Duplicate
};

// outChunk 在所有返回路径上都被先置空，调用方无需预先初始化；
// 只有返回 Unique 时才指向有效的 chunk 描述符。
[[nodiscard]] inline ChunkExtractStatus FindUniqueChunk(const std::vector<BakedChunk>& chunks,
                                                        const std::array<char, 4>& type, const BakedChunk*& outChunk)
{
    outChunk = nullptr;
    for (const BakedChunk& chunk : chunks)
    {
        if (chunk.type == type)
        {
            if (outChunk != nullptr)
            {
                return ChunkExtractStatus::Duplicate;
            }
            outChunk = &chunk;
        }
    }
    return outChunk != nullptr ? ChunkExtractStatus::Unique : ChunkExtractStatus::Absent;
}

// 从 bytes[offset] 读 4 字节小端 u32；越界返回 false，成功写入 value。
[[nodiscard]] inline bool ReadU32LittleEndian(const std::span<const std::byte> bytes, const std::size_t offset,
                                              std::uint32_t& value)
{
    if (offset > bytes.size() || 4 > bytes.size() - offset)
    {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < 4; ++index)
    {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return true;
}
} // namespace MiniEngine::Assets::Internal
