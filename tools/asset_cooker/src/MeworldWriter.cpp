// ============================================================================
// MeworldWriter.cpp — .meworld（World Baked artifact）写入的实现
// 里程碑：M3（whole-world 篇）
// 职责：实现 C*M*C 手性转换（glTF 右手系 → 引擎左手系 row-major row-vector）
//       与 STRS/ENTY/TRFM/MSHR 四分块（chunk）的显式偏移序列化；
//       MSHR 引用 128-bit AssetId，无贴图材质引用 Cooker 合成的 1×1 白 .metex。
// 关联：tools/asset_cooker/src/MeworldWriter.h（布局与转换公式）
//       engine/world/src/WorldLoader.cpp（读取端两遍实例化）
// ============================================================================

#include "MeworldWriter.h"

#include "BakedWriter.h"

#include <MiniEngine/Assets/BakedFormat.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace MiniEngine::Tools
{
namespace
{
constexpr std::uint32_t kEntityNameNone = 0xFFFFFFFFU;
constexpr std::uint32_t kEntityParentNone = 0xFFFFFFFFU;
// MSHR v2：flags4 + meshAssetId16 + materialAssetId16 + 保留 12 = 48B/条。
// 材质语义（factors/贴图）全部由 `.memat` 承载，world 只引用身份。
constexpr std::uint32_t kMshrFlagMesh = 1U;
constexpr std::uint32_t kMshrFlagMaterial = 2U;
constexpr std::uint32_t kMshrStride = 48U;
constexpr std::uint32_t kTrfmStride = 64U; // 16 float row-major
} // namespace

void ConvertNodeTrsToRowMajor(const float translation[3], const float rotation[4], const float scale[3],
                              std::array<float, 16>& out)
{
    // 标准列向量旋转矩阵（quaternion x,y,z,w；FastGltf Decompose 输出与该顺序一致）。
    const float qx = rotation[0];
    const float qy = rotation[1];
    const float qz = rotation[2];
    const float qw = rotation[3];
    const float xx = qx * qx;
    const float yy = qy * qy;
    const float zz = qz * qz;
    const float xy = qx * qy;
    const float xz = qx * qz;
    const float yz = qy * qz;
    const float xw = qx * qw;
    const float yw = qy * qw;
    const float zw = qz * qw;

    // M_gltf = T*R*S（column 约定）：第 c 列 = s_c * R 的第 c 列；第 3 列 = translation。
    float m[4][4]{};
    m[0][0] = (1 - 2 * (yy + zz)) * scale[0];
    m[1][0] = (2 * (xy + zw)) * scale[0];
    m[2][0] = (2 * (xz - yw)) * scale[0];
    m[0][1] = (2 * (xy - zw)) * scale[1];
    m[1][1] = (1 - 2 * (xx + zz)) * scale[1];
    m[2][1] = (2 * (yz + xw)) * scale[1];
    m[0][2] = (2 * (xz + yw)) * scale[2];
    m[1][2] = (2 * (yz - xw)) * scale[2];
    m[2][2] = (1 - 2 * (xx + yy)) * scale[2];
    m[3][3] = 1.0F;
    m[0][3] = translation[0];
    m[1][3] = translation[1];
    m[2][3] = translation[2];

    // M_lh[i][j] = sign_i * M_gltf[i][j] * sign_j（sign = -1,1,1,1，C=diag(-1,1,1,1)）。
    // out 为引擎 row-major（row-vector）：out[i*4+j] = M_lh[j][i]。
    const float sign[4] = {-1.0F, 1.0F, 1.0F, 1.0F};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t col = 0; col < 4; ++col)
        {
            out[col * 4U + row] = sign[row] * sign[col] * m[row][col];
        }
    }
}

bool BuildMeworldArtifact(const Sha256Digest& buildKey, const std::vector<MeworldEntity>& entities,
                          std::vector<std::byte>& out, std::string& error)
{
    using namespace MiniEngine::Assets;
    out.clear();
    if (entities.size() > std::numeric_limits<std::uint32_t>::max())
    {
        error = "too many world entities";
        return false;
    }
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        if (entities[index].parentIndex >= static_cast<std::int32_t>(index) && entities[index].parentIndex >= 0)
        {
            error = "world entity parent must precede child";
            return false;
        }
        for (const float value : entities[index].localRowMajor)
        {
            if (!std::isfinite(value))
            {
                error = "world entity transform is not finite";
                return false;
            }
        }
    }

    const std::uint32_t count = static_cast<std::uint32_t>(entities.size());

    // ---- STRS：u32 count + 每个实体一条 (u32 byteLen [+ bytes])，空名长度为 0 ----
    std::vector<std::byte> strs;
    BakedWriter::PutU32(strs, 0, count);
    std::vector<std::uint32_t> entryNameIndex(entities.size(), kEntityNameNone);
    std::size_t writePosition = 4;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        if (entities[index].name.size() > std::numeric_limits<std::uint32_t>::max() - 4U)
        {
            error = "world entity name too long";
            return false;
        }
        entryNameIndex[index] = static_cast<std::uint32_t>(writePosition);
        BakedWriter::PutU32(strs, writePosition, static_cast<std::uint32_t>(entities[index].name.size()));
        writePosition += 4;
        if (!entities[index].name.empty())
        {
            for (const char ch : entities[index].name)
            {
                strs.push_back(static_cast<std::byte>(ch));
            }
            writePosition += entities[index].name.size();
        }
    }

    // ---- ENTY：u32 count + 每实体 8B (parentIndex, nameIndex) ----
    std::vector<std::byte> enty;
    BakedWriter::PutU32(enty, 0, count);
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const std::uint32_t parent = entities[index].parentIndex < 0
                                         ? kEntityParentNone
                                         : static_cast<std::uint32_t>(entities[index].parentIndex);
        BakedWriter::PutU32(enty, 4 + index * 8U, parent);
        BakedWriter::PutU32(enty, 4 + index * 8U + 4U, entryNameIndex[index]);
    }

    // ---- TRFM：u32 count + 每实体 16 float row-major ----
    std::vector<std::byte> trfm;
    BakedWriter::PutU32(trfm, 0, count);
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const std::size_t base = 4 + index * kTrfmStride;
        for (std::size_t component = 0; component < 16; ++component)
        {
            const float value = entities[index].localRowMajor[component];
            std::uint32_t bits{};
            std::memcpy(&bits, &value, sizeof(bits));
            BakedWriter::PutU32(trfm, base + component * 4U, bits);
        }
    }

    // ---- MSHR v2：u32 count + 每实体 48B（flags/meshAssetId16/materialAssetId16/保留12） ----
    std::vector<std::byte> mshr(4ULL + static_cast<std::uint64_t>(count) * kMshrStride, std::byte{0});
    BakedWriter::PutU32(mshr, 0, count); // 与 STRS/ENTY/TRFM 一致的实体数前缀（loader 按此校验）
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const std::size_t base = 4 + index * kMshrStride;
        std::uint32_t flags = 0;
        if (entities[index].hasMesh)
        {
            flags |= kMshrFlagMesh;
        }
        if (entities[index].hasMaterial)
        {
            flags |= kMshrFlagMaterial;
        }
        BakedWriter::PutU32(mshr, base, flags);
        std::memcpy(mshr.data() + base + 4, entities[index].meshAssetId.data(), 16);
        std::memcpy(mshr.data() + base + 20, entities[index].materialAssetId.data(), 16);
        // [36..48) 保留 12 字节，保持 16 字节倍数步长以便未来扩展不改 stride。
    }

    // 布局：descriptors@64（4×32B）→ STRS 数据 @Align16(192) → ENTY → TRFM → MSHR。
    constexpr std::size_t kChunkCount = 4;
    constexpr std::size_t kTableStart = kBakedHeaderSize;
    const std::size_t kDataStart =
        BakedWriter::AlignUp(kTableStart + kChunkCount * kChunkDescriptorSize, kChunkAlignment);

    const std::vector<std::byte>* payloads[kChunkCount] = {&strs, &enty, &trfm, &mshr};
    std::size_t chunkOffsets[kChunkCount]{};
    std::size_t fileSize = kDataStart;
    for (std::size_t index = 0; index < kChunkCount; ++index)
    {
        // 每个 chunk 的 offset 必须 16 对齐（BakedReader 契约）。
        fileSize = BakedWriter::AlignUp(fileSize, kChunkAlignment);
        chunkOffsets[index] = fileSize;
        fileSize += payloads[index]->size();
    }

    out.assign(fileSize, std::byte{0});
    BakedWriter::WriteBakedHeader(out, BakedAssetKind::World, buildKey, kChunkCount,
                                  static_cast<std::uint64_t>(fileSize), kWorldFormatVersion);

    const char chunkTypes[kChunkCount][5] = {"STRS", "ENTY", "TRFM", "MSHR"};
    for (std::size_t index = 0; index < kChunkCount; ++index)
    {
        const std::size_t descriptorBase = kTableStart + index * kChunkDescriptorSize;
        BakedWriter::WriteChunkDescriptor(out, descriptorBase, chunkTypes[index],
                                          static_cast<std::uint64_t>(chunkOffsets[index]),
                                          static_cast<std::uint64_t>(payloads[index]->size()), count, 0);
        if (payloads[index]->size() > 0)
        {
            std::memcpy(out.data() + chunkOffsets[index], payloads[index]->data(), payloads[index]->size());
        }
    }
    return true;
}
} // namespace MiniEngine::Tools
