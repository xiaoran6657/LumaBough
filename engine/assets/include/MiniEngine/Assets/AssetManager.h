// ============================================================================
// AssetManager.h — 资产管线的运行时门面与两阶段提交事务
// 里程碑：M3（7-A / 7-C / 审计 3.1）
// 职责：把 Cooker 发布的 Manifest 变成进程内可查询的资产集合。对外暴露生效中的
//       Registry、类型化资产池与统计；以"Prepare（可失败、不改既有状态）→
//       Begin / FinishCommit（不可失败、帧边界生效）→ ApplyPendingRemovals（延迟
//       卸载）"的事务把验证与提交分离，任何失败都止于最后一次正确版本。
// 关联：docs/architecture/DECISIONS.md §4、§7
//       engine/world/src/WorldLoader.cpp（reload 路径的两段提交调用方）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Assets/AssetPool.h>
#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Assets/TextureAsset.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace MiniEngine::Assets
{
struct MeshAsset;
struct TextureAsset;

// 7-A/7-C：可失败、可回退、可 diff 的 Manifest 加载事务（无 watcher 版）。
//
// PrepareManifestLoad：
//   digest no-op 判定 → 临时 Registry 严格解析 → 与 active Registry diff
//   （Added/Changed 才读取并验证 artifact；Unchanged 跳过、slot/revision 不动；
//   同一 AssetId 的 kind 变更直接拒绝）→ 全部通过才建立 pending。
// Commit 拆两段（审计 3.1 根治 Added-AssetId whole-world reload 限制）：
//   BeginCommit：为 pending 中 Added/Changed 的 mesh/texture 先 ResolveOrCreate 槽位
//     ——新 AssetId 的 Handle 即刻可解析（WorldLoader TryFind 可用），payload 尚未写入；
//     失败路径的副作用仅是"若干空槽"（下次成功 Prepare 会填满，可恢复）。
//   FinishCommit（帧边界，不可失败）：swap Registry；Added → payload 写入（revision=1）；
//     Changed → payload swap、revision+1；Handle/generation 不变；**Removed 先不卸载**——
//     延迟到 ApplyPendingRemovals，供 whole-world 重建过渡期旧 World 的 Handle 保持有效
//     （审计 P1-1：若 commit 先卸载，World 重建失败时旧画面会因 stale handle 消失）。
//   CommitPending() = BeginCommit() + FinishCommit()（完整提交，供初始加载与单测）。
// ApplyPendingRemovals：卸载 Commit 已接受的 Removed 资产（Handle 失效）。
class AssetManager final
{
  public:
    // 累计计数器，用于诊断与单测断言；不参与任何控制流。
    struct Stats final
    {
        std::uint32_t manifestLoads{};    // PrepareManifestLoad 调用次数
        std::uint32_t manifestNoOps{};    // Manifest digest 未变 → no-op
        std::uint32_t manifestRejected{}; // Prepare 失败（解析/diff/hash/header/读文件/World）
        std::uint32_t assetsCommitted{};  // CommitPending 累计提交的 asset 数
        std::uint32_t assetsRemoved{};    // CommitPending 累计卸载的 asset 数
    };

    // 一次 Commit 的确定性报告：AssetId 均按字节序（Registry 排序保证）。
    struct CommitReport final
    {
        std::vector<AssetId> added;
        std::vector<AssetId> changed;
        std::vector<AssetId> removed;
    };

    // 读取并验证一份 Manifest，全部通过则建立待提交事务（pending）。
    //
    // 原子动作：入口先清空任何陈旧 pending，因此 no-op 与任一校验失败都不会残留
    // 过期事务。Manifest digest 未变即为 no-op（返回 true 且不建立 pending）；
    // 解析、diff、artifact 读取、hash、header、chunk 解码任一失败即整体拒绝，
    // active Registry 与资产池完全不动（last-known-good）。
    //
    // 参数：
    //   manifestPath —— Manifest 路径；其父目录被当作 outputRoot 做路径沙箱校验
    //   error        —— 失败时写入的可读原因（含 expected/actual 哈希等诊断信息）
    // 返回：成功建立 pending 或 digest 未变（no-op）为 true；失败为 false。
    [[nodiscard]] bool PrepareManifestLoad(const std::filesystem::path& manifestPath, std::string& error);
    // 两段式提交：BeginCommit 先为 Added/Changed 建槽（新 AssetId 即刻可解析），
    // world 重建成功后调用 FinishCommit 交换 Registry 并写入 payload。
    // CommitPending() 等价于 BeginCommit()+FinishCommit()（初始加载/单测用）。
    void BeginCommit();

    // 结束提交（不可失败）：交换 active Registry、写入 payload、修订号 +1。
    //
    // 本阶段不做任何可能失败的验证——全部验证已在 Prepare 阶段完成，
    // 因此一旦进入，active Registry 必然整体切换到新版本。
    void FinishCommit();

    // 完整提交：BeginCommit + FinishCommit。
    //
    // 供初始加载与不需要"先建临时 World"的场景（单测）使用；whole-world reload
    // 必须分开调用两段，以便在两者之间插入 World 重建与交叉核对。
    void CommitPending();
    // 卸载 CommitPending/FinishCommit 已接受的 Removed 资产。调用方须在"新 World 已整体替换
    // 成功"后调用，以保持旧 World 的 Handle 在过渡期仍有效（last-known-good）。
    void ApplyPendingRemovals();

    // 是否存在待提交事务（Prepare 成功且尚未 FinishCommit）。
    //
    // 返回：存在为 true；否则 false。
    [[nodiscard]] bool HasPendingCommit() const noexcept;

    // 当前生效的 Registry。
    //
    // 返回：从未成功加载过 Manifest 时为 nullptr。
    [[nodiscard]] const AssetRegistry* Registry() const noexcept;
    // PrepareManifestLoad 成功且尚未 Commit 时返回待提交 Registry（reload 先建临时 World 用）。
    [[nodiscard]] const AssetRegistry* PendingRegistry() const noexcept;

    // 当前生效 Manifest 的 SHA-256 digest。
    //
    // 运行时以它作为"Manifest 是否变化"的事实来源（digest 相同即 no-op），
    // 文件系统事件只作提示。从未加载过时为全零。
    //
    // 返回：digest 的只读引用。
    [[nodiscard]] const Sha256Digest& ActiveManifestDigest() const noexcept;

    // 累计统计（见 Stats）。
    //
    // 返回：内部计数器的只读引用。
    [[nodiscard]] const Stats& GetStats() const noexcept;
    // 上一次 CommitPending 的报告；尚未 Commit 过时三个列表为空。
    [[nodiscard]] const CommitReport& LastCommit() const noexcept;

    // 直接暴露 typed Pool：World/渲染侧持有 handle，AssetManager 不做二次抽象。
    // const 重载供只读消费者（如 D3D11AssetCache）读取 payload/revision。
    [[nodiscard]] AssetPool<MeshAsset>& Meshes() noexcept;
    [[nodiscard]] const AssetPool<MeshAsset>& Meshes() const noexcept;
    [[nodiscard]] AssetPool<TextureAsset>& Textures() noexcept;
    [[nodiscard]] const AssetPool<TextureAsset>& Textures() const noexcept;
    [[nodiscard]] AssetPool<MaterialAsset>& Materials() noexcept;
    [[nodiscard]] const AssetPool<MaterialAsset>& Materials() const noexcept;

  private:
    // Prepare 阶段解码完毕、等待 FinishCommit 写入的网格 payload。
    struct PendingMesh final
    {
        AssetId id{};
        MeshAsset asset{};
    };

    // Prepare 阶段解码完毕、等待 FinishCommit 写入的贴图 payload。
    struct PendingTexture final
    {
        AssetId id{};
        TextureAsset asset{};
    };

    // Prepare 阶段解码完毕、等待 FinishCommit 写入的材质 payload（M4-02）。
    struct PendingMaterial final
    {
        AssetId id{};
        MaterialAsset asset{};
    };

    // 待卸载的资产：记录 kind 以便在正确的类型化池中定位。
    // 不放 payload —— 卸载只需要句柄，不需要数据。
    struct PendingRemoval final
    {
        AssetId id{};
        AssetKind kind{};
    };

    // 当前生效的 Registry（nullopt 表示从未成功加载过 Manifest）。
    std::optional<AssetRegistry> m_activeRegistry;
    // 当前生效 Manifest 的 digest。
    Sha256Digest m_activeManifestDigest{};
    // 是否已有生效 Manifest：false 时第一次 Prepare 跳过 no-op 判定（没有可比较的旧值）。
    bool m_hasActiveManifest{};

    // 待提交事务。m_pendingRegistry 存在即代表 HasPendingCommit；
    // 各 pending 容器在 FinishCommit 前保持有效，m_pendingRemovals 甚至活到
    // ApplyPendingRemovals 才被消费（Removed 的延迟卸载）。
    std::optional<AssetRegistry> m_pendingRegistry;
    Sha256Digest m_pendingManifestDigest{};
    std::vector<PendingMesh> m_pendingMeshes;
    std::vector<PendingTexture> m_pendingTextures;
    std::vector<PendingMaterial> m_pendingMaterials;
    std::vector<PendingRemoval> m_pendingRemovals;
    CommitReport m_pendingReport;

    // 上一次完整提交的 diff 报告，供诊断与测试断言。
    CommitReport m_lastCommitReport{};
    Stats m_stats{};
    // 两类资产的类型化池；直接暴露给 World / 渲染侧，不再包一层抽象。
    AssetPool<MeshAsset> m_meshes;
    AssetPool<TextureAsset> m_textures;
    AssetPool<MaterialAsset> m_materials;
};
} // namespace MiniEngine::Assets
