// ============================================================================
// WorldLoader.cpp — .meworld 两遍实例化的实现
// 里程碑：M3（.meworld 序列化 + whole-world reload）
// 职责：解析 STRS / ENTY / TRFM / MSHR 四分块（chunk）并实例化 World。所有偏移按
//       wire 布局显式小端读取，逐项校验（数量一致、parent 先于 child、名字不越界、
//       浮点有限、引用的资产已加载），任一失败即整体放弃。
// 关联：docs/architecture/DECISIONS.md §7、§8
//       tools/asset_cooker/src/MeworldWriter.cpp（写入端偏移的对称实现）
// ============================================================================

#include <MiniEngine/World/WorldLoader.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/BakedReader.h>
#include <MiniEngine/Assets/Internal/BakedReadUtil.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace MiniEngine::World
{
namespace
{
// 审计 3.1：ReadU32/FindChunk 收敛为 Assets 内部唯一实现（AssetManager 同源），
// 不再在本文件复制一份。
using MiniEngine::Assets::Internal::ChunkExtractStatus;
using MiniEngine::Assets::Internal::FindUniqueChunk;
using MiniEngine::Assets::Internal::ReadU32LittleEndian;

// ENTY / STRS 的 sentinel：0xFFFFFFFF 表示"无父实体" / "无名字"。
constexpr std::uint32_t kEntityNameNone = 0xFFFFFFFFU;
constexpr std::uint32_t kEntityParentNone = 0xFFFFFFFFU;
// MSHR v2 条目的引用标志位：为 0 的条目代表"无渲染组件"的纯变换实体。
constexpr std::uint32_t kMshrFlagMesh = 1U;
constexpr std::uint32_t kMshrFlagMaterial = 2U;
// MSHR v2 条目布局（48 字节）：flags(4) + mesh AssetId(16) + material AssetId(16) +
// reserved(12)。材质语义（factors/贴图）全部由 `.memat` 承载（M4-02 v2）。
constexpr std::uint32_t kMshrStride = 48U;
// TRFM 条目布局（64 字节）：16 个 float 的 row-major 本地矩阵。
constexpr std::uint32_t kTrfmStride = 64U;

constexpr std::array<char, 4> kChunkStrs{'S', 'T', 'R', 'S'};
constexpr std::array<char, 4> kChunkEnty{'E', 'N', 'T', 'Y'};
constexpr std::array<char, 4> kChunkTrfm{'T', 'R', 'F', 'M'};
constexpr std::array<char, 4> kChunkMshr{'M', 'S', 'H', 'R'};

// 读 4 字节小端并按 IEEE-754 解释为 float；非有限值（NaN / Inf）视为损坏而拒绝。
bool ReadFloat(const std::span<const std::byte> bytes, const std::size_t offset, float& value)
{
    std::uint32_t bits = 0;
    if (!ReadU32LittleEndian(bytes, offset, bits))
    {
        return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}

// .meworld 的四个必需 chunk 都必须是"恰好一个"（缺失或重复均畸形）。
bool RequireUniqueChunk(const std::vector<Assets::BakedChunk>& chunks, const std::array<char, 4>& type,
                        const Assets::BakedChunk*& chunk)
{
    return FindUniqueChunk(chunks, type, chunk) == ChunkExtractStatus::Unique;
}
} // namespace

bool TryBuildWorldFromArtifact(const std::span<const std::byte> artifactBytes, Assets::AssetManager& assets,
                               WorldLoadResult& out, std::string& error)
{
    out = {};

    Assets::BakedReadExpectation expectation{};
    expectation.kind = Assets::BakedAssetKind::World;
    Assets::BakedReadResult readResult;
    if (!Assets::BakedReader::Parse(artifactBytes, expectation, readResult, error))
    {
        return false;
    }

    // M4-02 v2：header.flags 为世界格式版本；v1（flags=0）输出 recook diagnostic。
    if (readResult.header.flags != Assets::kWorldFormatVersion)
    {
        error = "unsupported world formatVersion=" + std::to_string(readResult.header.flags) +
                "; delete derived cache and recook with M4";
        return false;
    }

    const Assets::BakedChunk* strsChunk = nullptr;
    const Assets::BakedChunk* entyChunk = nullptr;
    const Assets::BakedChunk* trfmChunk = nullptr;
    const Assets::BakedChunk* mshrChunk = nullptr;
    if (!RequireUniqueChunk(readResult.chunks, kChunkStrs, strsChunk) ||
        !RequireUniqueChunk(readResult.chunks, kChunkEnty, entyChunk) ||
        !RequireUniqueChunk(readResult.chunks, kChunkTrfm, trfmChunk) ||
        !RequireUniqueChunk(readResult.chunks, kChunkMshr, mshrChunk))
    {
        error = "meworld requires exactly one STRS/ENTY/TRFM/MSHR chunk each";
        return false;
    }

    const auto strsSpan = Assets::BakedReader::GetChunkBytes(artifactBytes, *strsChunk);
    const auto entySpan = Assets::BakedReader::GetChunkBytes(artifactBytes, *entyChunk);
    const auto trfmSpan = Assets::BakedReader::GetChunkBytes(artifactBytes, *trfmChunk);
    const auto mshrSpan = Assets::BakedReader::GetChunkBytes(artifactBytes, *mshrChunk);
    if (!strsSpan.has_value() || !entySpan.has_value() || !trfmSpan.has_value() || !mshrSpan.has_value())
    {
        error = "meworld chunk byte range invalid";
        return false;
    }

    std::uint32_t strsCount = 0;
    std::uint32_t entyCount = 0;
    std::uint32_t trfmCount = 0;
    std::uint32_t mshrCount = 0;
    if (!ReadU32LittleEndian(*strsSpan, 0, strsCount) || !ReadU32LittleEndian(*entySpan, 0, entyCount) ||
        !ReadU32LittleEndian(*trfmSpan, 0, trfmCount) || !ReadU32LittleEndian(*mshrSpan, 0, mshrCount))
    {
        error = "meworld chunk is truncated";
        return false;
    }
    // 四个分块（chunk）描述同一批实体，数量必须一致；不一致即文件损坏。
    if (strsCount != entyCount || strsCount != trfmCount || strsCount != mshrCount)
    {
        error = "meworld chunk counts disagree";
        return false;
    }
    // 载荷尺寸必须足以容纳声明的条数（ENTY 每条 8 字节、TRFM/MSHR 每条 64 字节、
    // STRS 每条名字至少 4 字节长度前缀），防止后续按下标寻址越界；
    // 空世界（count=0）只含 count 前缀，是合法产物。
    if (strsCount > 0 && (strsSpan->size() < 4ULL + static_cast<std::uint64_t>(strsCount) * 4ULL ||
                          entySpan->size() < 4ULL + static_cast<std::uint64_t>(strsCount) * 8ULL ||
                          trfmSpan->size() < 4ULL + static_cast<std::uint64_t>(strsCount) * kTrfmStride ||
                          mshrSpan->size() < 4ULL + static_cast<std::uint64_t>(strsCount) * kMshrStride))
    {
        error = "meworld chunk payload smaller than declared count";
        return false;
    }

    auto world = std::make_unique<World>();
    std::vector<Entity> entities;
    entities.reserve(strsCount);

    // ---- pass1：实体 + 层级 + 本地矩阵 + 名字 ----
    for (std::uint32_t index = 0; index < strsCount; ++index)
    {
        const std::size_t entyBase = 4 + static_cast<std::size_t>(index) * 8U;

        std::uint32_t parentIndex = kEntityParentNone;
        std::uint32_t nameIndex = kEntityNameNone;
        if (!ReadU32LittleEndian(*entySpan, entyBase, parentIndex) ||
            !ReadU32LittleEndian(*entySpan, entyBase + 4, nameIndex))
        {
            error = "meworld ENTY entry is truncated";
            return false;
        }
        if (parentIndex != kEntityParentNone && parentIndex >= index)
        {
            error = "meworld parent must precede child (serialized order)";
            return false;
        }

        const Entity entity = world->CreateEntity();
        entities.push_back(entity);

        // 名字：长度 0 = 无 NameComponent；nameIndex 指向 STRS 中该实体名字长度字段。
        if (nameIndex != kEntityNameNone)
        {
            std::uint32_t nameLength = 0;
            if (nameIndex >= strsSpan->size() || 4 > strsSpan->size() - nameIndex ||
                !ReadU32LittleEndian(*strsSpan, nameIndex, nameLength) || nameLength > strsSpan->size() - nameIndex - 4)
            {
                error = "meworld STRS name is out of bounds";
                return false;
            }
            if (nameLength > 0)
            {
                std::string name;
                name.resize(nameLength);
                for (std::size_t character = 0; character < nameLength; ++character)
                {
                    name[character] =
                        static_cast<char>(std::to_integer<std::uint8_t>((*strsSpan)[nameIndex + 4 + character]));
                }
                if (!world->SetName(entity, std::move(name)))
                {
                    error = "meworld SetName failed for entity " + std::to_string(index);
                    return false;
                }
            }
        }

        // 本地矩阵（row-major，已在 Cooker 按 02 篇 C*M*C 转置落库）。
        const std::size_t trfmBase = 4 + static_cast<std::size_t>(index) * kTrfmStride;
        Matrix4 local;
        for (std::size_t component = 0; component < 16; ++component)
        {
            if (!ReadFloat(*trfmSpan, trfmBase + component * 4U, local.values[component]))
            {
                error = "meworld TRFM contains a non-finite value";
                return false;
            }
        }
        if (!world->SetLocalMatrix(entity, local))
        {
            error = "meworld SetLocalMatrix rejected entity " + std::to_string(index);
            return false;
        }

        if (parentIndex != kEntityParentNone)
        {
            if (!world->SetParent(entity, entities[parentIndex]))
            {
                error = "meworld SetParent rejected entity " + std::to_string(index);
                return false;
            }
        }
    }

    // ---- pass2：MSHR AssetId → Handle 解析并挂 MeshRenderer ----
    for (std::uint32_t index = 0; index < strsCount; ++index)
    {
        const std::size_t mshrBase = 4 + static_cast<std::size_t>(index) * kMshrStride;
        std::uint32_t flags = 0;
        if (!ReadU32LittleEndian(*mshrSpan, mshrBase, flags))
        {
            error = "meworld MSHR entry is truncated";
            return false;
        }
        if ((flags & kMshrFlagMesh) == 0U)
        {
            continue;
        }

        Assets::AssetId meshId{};
        std::memcpy(meshId.bytes.data(), mshrSpan->data() + mshrBase + 4, meshId.bytes.size());
        const auto meshHandle = assets.Meshes().TryFind(meshId);
        if (!meshHandle.has_value())
        {
            error = "meworld references an unloaded mesh asset " + meshId.ToHexString();
            return false;
        }

        Assets::AssetId materialId{};
        Assets::AssetHandle<Assets::MaterialAsset> materialHandle;
        if ((flags & kMshrFlagMaterial) != 0U)
        {
            std::memcpy(materialId.bytes.data(), mshrSpan->data() + mshrBase + 20, materialId.bytes.size());
            const auto found = assets.Materials().TryFind(materialId);
            if (!found.has_value())
            {
                error = "meworld references an unloaded material asset " + materialId.ToHexString();
                return false;
            }
            materialHandle = *found;
        }

        MeshRendererComponent component;
        component.mesh = *meshHandle;
        component.material = materialHandle;
        component.visible = true;
        if (!world->SetMeshRenderer(entities[index], component))
        {
            error = "meworld SetMeshRenderer rejected entity " + std::to_string(index);
            return false;
        }
    }

    world->UpdateTransforms();
    out.world = std::move(world);
    out.entities = std::move(entities);
    return true;
}
} // namespace MiniEngine::World
