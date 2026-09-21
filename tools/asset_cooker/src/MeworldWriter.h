#pragma once

// ============================================================================
// MeworldWriter.h — .meworld v2（World Baked artifact）写入
// 里程碑：M3（v1）→ M4-02（v2：MeshRenderer 改为 MaterialAssetId）
// 职责：把导入层收集的实体（source-local TRS + 资源 AssetId 引用）序列化为
//       STRS/ENTY/TRFM/MSHR 四分块（chunk）的 .meworld。转换在离线完成：
//       glTF 右手系按 C=diag(-1,1,1,1) 做 C·M·C 后落库 row-major 行向量矩阵。
//       v2 的 MSHR 记录为 flags4 + meshAssetId16 + materialAssetId16（48B/条），
//       材质的 factors/贴图引用全部由 `.memat` 承载，world 不再内联 baseColorFactor。
// 关联：docs/architecture/DECISIONS.md §6
//       docs/architecture/README.md（格式迁移策略表）
//       engine/world/src/WorldLoader.cpp（读取端两遍实例化）
// ============================================================================

#include <MiniEngine/Assets/Sha256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
using Sha256Digest = MiniEngine::Assets::Sha256Digest;

// `.meworld` v2 的格式版本常量 kWorldFormatVersion 见
// engine/assets/include/MiniEngine/Assets/BakedFormat.h（引擎/Cooker 共享口径）。

// .meworld 单实体（source-local 数据，不写 runtime Handle）：
//   本地矩阵已是引擎 row-major/LH（C*M*C 后转置），加载端直接 SetLocalMatrix；
//   资源引用是 128-bit AssetId（mesh → `.memesh`、material → `.memat`）。
struct MeworldEntity final
{
    std::string name; // 空字符串 = 无 NameComponent
    std::int32_t parentIndex{-1}; // -1 = root；否则必须 < 当前 index（parent-before-child）
    std::array<float, 16> localRowMajor{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                         0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    bool hasMesh{};
    bool hasMaterial{};
    std::array<std::byte, 16> meshAssetId{};
    std::array<std::byte, 16> materialAssetId{};
};

// glTF node TRS（translation/rotation[xyzw]/scale）→ 引擎 row-major LH local matrix：
//   M_gltf = T*R*S（column 约定）；M_lh = C*M_gltf*C（C=diag(-1,1,1,1)）；
//   输出 out[i*4+j] = M_lh[j][i]（row-vector/row-major 存储）。
void ConvertNodeTrsToRowMajor(const float translation[3], const float rotation[4], const float scale[3],
                              std::array<float, 16>& out);

// `.meworld` v2：header(kind=World, chunkCount=4, flags=kWorldFormatVersion) +
// STRS/ENTY/TRFM/MSHR。失败返回 false（空场景仍产出 N=0 的合法 artifact）。
[[nodiscard]] bool BuildMeworldArtifact(const Sha256Digest& buildKey,
                                        const std::vector<MeworldEntity>& entities, std::vector<std::byte>& out,
                                        std::string& error);
} // namespace MiniEngine::Tools
