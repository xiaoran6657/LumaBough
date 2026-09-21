// ============================================================================
// BakedReader.h — 烘焙产物的严格只读解析器
// 里程碑：M3
// 职责：把 .memesh / .metex / .meworld 的字节解析为"固定头 + chunk 表"，并执行全部
//       结构性防御（magic、版本、尺寸、kind、BuildKey、chunk 越界、对齐、重叠）。
//       解析成功即意味着这串字节自洽且未被篡改；至于某类产物必须含哪些 chunk，
//       属于语义层知识，由调用方用 ValidateRequiredChunks 表达，Reader 不内置。
// 关联：docs/architecture/DECISIONS.md §1
//       engine/assets/src/AssetManager.cpp（Prepare 阶段对每个 artifact 的验证）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/BakedFormat.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Assets
{
// 解析结果：已通过全部结构校验的固定头与 chunk 表。
// 刻意不拷贝 payload——调用方按需用 GetChunkBytes 从原始字节中切片，
// 因此对大贴图的解析不产生额外内存峰值。
struct BakedReadResult final
{
    BakedHeader header{};
    std::vector<BakedChunk> chunks;
};

struct BakedReadExpectation final
{
    BakedAssetKind kind{};
    // inspect may omit this; Cooker staging verification and Runtime must set it
    // from the computed/Manifest BuildKey.
    std::optional<std::array<std::byte, 32>> buildKey;
};

// 烘焙产物的只读解析器（无状态，全静态成员）。
class BakedReader final
{
  public:
    // 解析并校验一个烘焙产物。
    //
    // 校验顺序：magic 与最小长度 → 头字段（version / headerSize / kind / fileSize /
    // flags / reserved）→ chunk 表范围 → BuildKey（若给出）→ 逐 chunk 的 flags、
    // 对齐、文件内范围与互不重叠。任一失败都写 error 并返回 false。
    // result 只在全部通过后被整体赋值，因此失败路径不会留下半解析的中间状态。
    //
    // 参数：
    //   fileBytes   —— 产物文件的完整字节
    //   expectation —— 期望的 kind 与（可选的）BuildKey
    //   result      —— 成功时写入的解析结果
    //   error       —— 失败时写入的可读原因；进入时会被清空
    // 返回：全部校验通过为 true；否则 false。
    [[nodiscard]] static bool Parse(std::span<const std::byte> fileBytes, const BakedReadExpectation& expectation,
                                    BakedReadResult& result, std::string& error);

    // M3-03 Reader 防御 #10：必需 chunk 恰好一个。requiredTypes 为四字符
    // chunk 类型列表；任一类型缺失或出现多次均失败。这是 kind 无关的通用
    // 原语，具体 schema（如 Mesh 需 INFO/VERT/INDX）由上层语义层传入。
    [[nodiscard]] static bool ValidateRequiredChunks(const BakedReadResult& result,
                                                     std::span<const std::array<char, 4>> requiredTypes,
                                                     std::string& error);

    // 从原始字节中切出某个 chunk 的 payload 视图（零拷贝）。
    //
    // 参数：
    //   fileBytes —— 产物文件的完整字节（必须与传给 Parse 的同一份）
    //   chunk     —— 已由 Parse 校验过的 chunk 描述符
    // 返回：成功为指向 payload 的 span；chunk 范围越出文件（正常路径不会发生，
    //       仅当 chunk 被外部构造时）为 nullopt。
    [[nodiscard]] static std::optional<std::span<const std::byte>> GetChunkBytes(std::span<const std::byte> fileBytes,
                                                                                 const BakedChunk& chunk);
};
} // namespace MiniEngine::Assets