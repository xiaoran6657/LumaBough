// ============================================================================
// BakedFormat.h — 烘焙产物的公共 wire 格式常量与结构体
// 里程碑：M3
// 职责：定义 .memesh / .metex / .meworld 共用的 64 字节固定头、32 字节 chunk 描述符
//       以及对齐与数量上限常量。Cooker（写入端）与运行时（读取端）共享本头，
//       是格式冻结的唯一口径；任何改动都破坏磁盘兼容，须随 ADR 版本化处理。
// 关联：docs/architecture/DECISIONS.md §1
//       engine/assets/src/BakedReader.cpp（按本布局逐字段严格校验）
// ============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace MiniEngine::Assets
{
// magic "MEA3"（MiniEngine Asset v3）：Reader 的第一道拒绝关口，
// 用来在读任何字段之前就排除掉非烘焙产物与截断文件。
inline constexpr std::array<char, 4> kBakedMagic{'M', 'E', 'A', '3'};
// 格式版本：Reader 只接受与自身完全一致的版本，不做跨版本的兼容猜测。
inline constexpr std::uint16_t kBakedFormatVersion = 1;
// 固定头大小（字节）：含 magic 在内，同时也是 chunk 表的起始偏移。
inline constexpr std::uint64_t kBakedHeaderSize = 64;
// 单个 chunk 描述符的大小（字节）。
inline constexpr std::uint64_t kChunkDescriptorSize = 32;
// chunk payload 起始偏移的对齐粒度（字节）；Reader 会拒绝未对齐的 offset。
inline constexpr std::uint64_t kChunkAlignment = 16;
// chunk 数量上限：与描述符大小一起把 chunk 表限制在可校验范围内，
// 防止恶意 / 损坏的 chunkCount 诱使 Reader 计算越界偏移。
inline constexpr std::uint32_t kMaxChunkCount = 4096;

// 产物种类在 wire 上的编码（uint16）。
// 与 Manifest 侧的 AssetKind 语义一一对应（AssetManager 内 ToBakedKind 做转换），
// 但取值在此独立固定，使磁盘格式不随内存枚举的取值变化而漂移。
// Material 为 M4-02 新增（`.memat`）；Reader 拒绝一切未列出的 kind。
enum class BakedAssetKind : std::uint16_t
{
    Mesh = 1,
    Texture = 2,
    World = 3,
    Material = 4
};

// 磁盘 wire 布局（64 字节，小端）：
//   [0..4)   magic "MEA3"
//   [4..6)   version (uint16)
//   [6..8)   kind/assetType (uint16)
//   [8..12)  headerSize (uint32)
//   [12..16) chunkCount (uint32)
//   [16..24) fileSize (uint64)
//   [24..56) buildKey (32 bytes)
//   [56..60) flags (uint32)
//   [60..64) reserved (uint32)
// 本结构体成员顺序与 wire 布局一一对应（除前置 magic 外），
// 可安全用于"结构体 + 前置 magic"方式的写入；static_assert 锁定总尺寸。
struct BakedHeader final
{
    std::uint16_t version{};
    std::uint16_t kind{};
    std::uint32_t headerSize{};
    std::uint32_t chunkCount{};
    std::uint64_t fileSize{};
    std::array<std::byte, 32> buildKey{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};
// 守护契约：结构体尺寸必须等于 wire 头大小，否则写入端与读取端的偏移约定会静默错位。
static_assert(sizeof(BakedHeader) == kBakedHeaderSize);

// chunk 描述符（磁盘 wire 布局 32 字节，小端）：
//   [0..4)   type（四字符标识，如 VERT / INDX / INFO / DATA / STRS / ENTY / TRFM / MSHR）
//   [4..8)   flags（必须为 0，保留给后续格式演进）
//   [8..16)  offset（payload 在文件内的绝对偏移，须 16 字节对齐且落在 chunk 表之后）
//   [16..24) size（payload 字节数）
//   [24..28) elementCount（元素个数；非元素化 chunk 由各格式自行约定）
//   [28..32) stride（单元素字节步长；非元素化 chunk 由各格式自行约定）
// 成员顺序与 wire 布局一致（总尺寸由下面的 static_assert 锁定），
// 但 BakedReader 仍逐字段做小端解码，不依赖本机字节序与编译器对齐。
struct BakedChunk final
{
    std::array<char, 4> type{};
    std::uint32_t flags{};
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint32_t elementCount{};
    std::uint32_t stride{};
};
// 守护契约：描述符尺寸必须等于 wire 上每条记录的大小。
static_assert(sizeof(BakedChunk) == kChunkDescriptorSize);

// 各 kind 的格式版本（M4 起通过 BakedHeader.flags 载波；常量按 kind 分布在
// PbrVertex.h / TextureFormatV2.h / MaterialAsset.h / 此处（World））。
// Reader 语义层据此输出 "unsupported ... formatVersion=N; delete derived cache
// and recook with M4" 的 recook diagnostic，不做静默猜测或原地迁移。
inline constexpr std::uint32_t kWorldFormatVersion = 2;
} // namespace MiniEngine::Assets