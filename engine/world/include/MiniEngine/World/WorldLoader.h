// ============================================================================
// WorldLoader.h — .meworld 产物的两遍实例化加载
// 里程碑：M3（.meworld 序列化 + whole-world reload）
// 职责：把 .meworld（STRS / ENTY / TRFM / MSHR 四分块（chunk））实例化为 World：
//       pass1 建实体、层级与本地矩阵，pass2 按 128 位 AssetId 解析 Mesh / Texture
//       句柄并挂 MeshRenderer。all-or-nothing：失败返回 false 且不产出 World，
//       旧场景保持最后一次正确版本（last-known-good）。
// 关联：docs/architecture/DECISIONS.md §7、§8
//       tools/asset_cooker/src/MeworldWriter.h（写入端，wire 布局对称）
// ============================================================================

#pragma once

#include <MiniEngine/World/World.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Assets
{
class AssetManager;
} // namespace MiniEngine::Assets

namespace MiniEngine::World
{
// 06 篇 deferral 的落地：.meworld 两遍实例化。
//   pass1：读 ENTY/STRS/TRFM → CreateEntity + SetLocalMatrix + SetParent（parent
//          必须先于 child，文件顺序即序列化顺序）+ UpdateTransforms；
//   pass2：读 MSHR → 按 128-bit AssetId 在 AssetManager 池中解析 Handle →
//          SetMeshRenderer。
// all-or-nothing：任何失败返回 false，out.world 为空（旧世界不受影响）。临时世界由
// 调用方在帧边界整体替换，不保留旧 runtime Entity ID（07 篇 World reload）。
struct WorldLoadResult final
{
    // 构建成功的世界；调用方在帧边界整体替换旧世界（不保留旧 Entity ID）。
    std::unique_ptr<World> world;
    // 序列化顺序对应的 Entity（调用方可按下标访问 Name/Transform/Renderer）。
    std::vector<Entity> entities;
};

// 从 .meworld 字节构建 World（两遍实例化，all-or-nothing）。
//
// 本函数自行 Parse（kind=World）并校验四个分块（chunk）的数量与载荷尺寸一致性；
// whole-world reload 在 BeginCommit 之后、FinishCommit 之前调用它，
// 因此失败时旧的 Registry / 资产池 / World 均不受影响。
// pass1 要求 ENTY 的 parent 下标严格小于 child 下标（文件顺序即序列化顺序），
// 单趟即可保证父实体先于子实体建立层级。
//
// 参数：
//   artifactBytes —— .meworld 文件的完整字节
//   assets        —— 用于把 AssetId 解析为 Mesh / Texture 句柄的资产池
//   out           —— 成功时写入 world 与按序列化顺序排列的 entities；失败时被清空
//   error         —— 失败时写入可读原因
// 返回：成功为 true；任一步失败为 false（out 保持为空）。
[[nodiscard]] bool TryBuildWorldFromArtifact(std::span<const std::byte> artifactBytes, Assets::AssetManager& assets,
                                             WorldLoadResult& out, std::string& error);
} // namespace MiniEngine::World
