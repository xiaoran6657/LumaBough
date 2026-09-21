// ============================================================================
// AssetManager.cpp — Manifest 加载事务与资产池接线的实现
// 里程碑：M3（7-A / 7-C / glTF 导入 / WIC bake / 审计 3.1、审计 4）
// 职责：实现 AssetManager.h 的事务：Prepare 阶段把"验证"全部做完（digest、解析、
//       diff、artifact 读取与哈希、header、chunk 解码），Begin/FinishCommit 与
//       ApplyPendingRemovals 则只做不会失败的搬运。文件读取统一走 Core 的
//       ReadBinaryFile，chunk 解码复用 Internal/BakedReadUtil.h。
// 关联：docs/architecture/DECISIONS.md §4、§7
//       engine/world/src/WorldLoader.cpp（reload 路径的调用方）
// ============================================================================

#include <MiniEngine/Assets/AssetManager.h>

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/BakedReader.h>
#include <MiniEngine/Assets/Internal/AssetDecode.h> // M7-06：解码原语与异步 decode 共用同一实现
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>
#include <MiniEngine/Core/FileSystem.h> // 审计 4：文件读取收敛到 Core 唯一实现

#include <limits>
#include <utility>

namespace MiniEngine::Assets
{

bool AssetManager::PrepareManifestLoad(const std::filesystem::path& manifestPath, std::string& error)
{
    error.clear();
    // P2-1（二轮审查）：每次 Prepare 都是原子动作——先清空任何陈旧 pending。
    // 上一轮"延迟卸载"让 pending 跨调用存活；若本次 no-op（digest 与 active 相同）
    // 或任一校验失败却不清空，会残留过期的 pending（HasPendingCommit 为真、
    // 用旧 R2 建 World、Commit 后 registry 与磁盘 manifest 一帧不一致）。
    // 旧 pendingRemovals 未 Apply 即丢弃是安全的：它按旧 active registry 计算，已过期。
    m_pendingRegistry.reset();
    m_pendingMeshes.clear();
    m_pendingTextures.clear();
    m_pendingMaterials.clear();
    m_pendingRemovals.clear();
    ++m_stats.manifestLoads;

    const MiniEngine::BinaryFileResult manifestRead = MiniEngine::ReadBinaryFile(manifestPath);
    if (!manifestRead.Succeeded())
    {
        ++m_stats.manifestRejected;
        error = "failed to read manifest: " + manifestPath.string() + ": " + manifestRead.error;
        return false;
    }
    const std::vector<std::byte>& manifestBytes = manifestRead.bytes;

    // 事件只是 hint，digest 才是事实：Manifest 未变 → no-op（07 篇两级 Watch）。
    const Sha256Digest manifestDigest = Sha256(manifestBytes);
    if (m_hasActiveManifest && manifestDigest == m_activeManifestDigest)
    {
        ++m_stats.manifestNoOps;
        return true;
    }

    const std::filesystem::path outputRoot = manifestPath.parent_path();
    std::string stageError;
    std::optional<AssetRegistry> registry = AssetRegistry::ParseAndValidate(manifestBytes, outputRoot, stageError);
    if (!registry)
    {
        ++m_stats.manifestRejected;
        error = "manifest rejected: " + stageError;
        return false;
    }

    // 07 篇 Manifest diff：Added/Changed/Unchanged/Removed。
    // 只有 Added/Changed 读取并验证 artifact；Unchanged 的 slot/revision 不动；
    // Removed 在 Commit 时卸载。entries 均按 AssetId 字节序 → 报告确定性。
    CommitReport pendingReport;
    std::vector<const RegistryEntry*> toLoad;
    if (m_activeRegistry)
    {
        for (std::size_t index = 0; index < registry->EntryCount(); ++index)
        {
            const RegistryEntry& entry = *registry->EntryAt(index);
            const RegistryEntry* previous = m_activeRegistry->Find(entry.id);
            if (previous == nullptr)
            {
                pendingReport.added.push_back(entry.id);
                toLoad.push_back(&entry);
                continue;
            }
            if (previous->kind != entry.kind)
            {
                // 同一 AssetId 的类型改变是错误：要求改 URI（07 篇 diff 规则）。
                ++m_stats.manifestRejected;
                error = "asset kind changed for " + entry.canonicalUri + "; change the URI instead";
                return false;
            }
            if (previous->artifactHash.bytes != entry.artifactHash.bytes ||
                previous->buildKey.bytes != entry.buildKey.bytes ||
                previous->artifactRelativePath != entry.artifactRelativePath)
            {
                pendingReport.changed.push_back(entry.id);
                toLoad.push_back(&entry);
            }
            // else Unchanged：跳过
        }
        for (std::size_t index = 0; index < m_activeRegistry->EntryCount(); ++index)
        {
            const RegistryEntry& activeEntry = *m_activeRegistry->EntryAt(index);
            if (registry->Find(activeEntry.id) == nullptr)
            {
                pendingReport.removed.push_back(activeEntry.id);
            }
        }
    }
    else
    {
        for (std::size_t index = 0; index < registry->EntryCount(); ++index)
        {
            const RegistryEntry& entry = *registry->EntryAt(index);
            pendingReport.added.push_back(entry.id);
            toLoad.push_back(&entry);
        }
    }

    std::vector<PendingMesh> pendingMeshes;
    std::vector<PendingTexture> pendingTextures;
    std::vector<PendingMaterial> pendingMaterials;
    std::vector<PendingRemoval> pendingRemovals;
    for (const AssetId& removedId : pendingReport.removed)
    {
        pendingRemovals.push_back(PendingRemoval{removedId, m_activeRegistry->Find(removedId)->kind});
    }

    for (const auto& entry : toLoad)
    {
        // World 条目只做 artifact 验证（hash + BakedReader kind=World），不持有池内
        // slot；两遍实例化在 WorldLoader 消费（.meworld 篇），失败保留旧 snapshot。
        const MiniEngine::BinaryFileResult artifactRead =
            MiniEngine::ReadBinaryFile(outputRoot / entry->artifactRelativePath);
        if (!artifactRead.Succeeded())
        {
            ++m_stats.manifestRejected;
            error = "failed to read artifact for " + entry->canonicalUri + ": " + artifactRead.error;
            return false;
        }
        const std::vector<std::byte>& artifactBytes = artifactRead.bytes;

        // artifactHash 重算：expected/actual 都写进 error（07 篇诊断要求）。
        const Sha256Digest artifactDigest = Sha256(artifactBytes);
        if (artifactDigest != entry->artifactHash.bytes)
        {
            ++m_stats.manifestRejected;
            error = "artifact hash mismatch for " + entry->canonicalUri + ": expected " +
                    ToHexDigest(entry->artifactHash.bytes) + " actual " + ToHexDigest(artifactDigest);
            return false;
        }

        BakedReadExpectation expectation{};
        expectation.kind = Internal::ToBakedKind(entry->kind);
        expectation.buildKey = entry->buildKey.bytes;
        BakedReadResult readResult;
        if (!BakedReader::Parse(artifactBytes, expectation, readResult, stageError))
        {
            ++m_stats.manifestRejected;
            error = "artifact header rejected for " + entry->canonicalUri + ": " + stageError;
            return false;
        }

        // G4：Mesh 优先从 VERT/INDX chunk 解码真实 payload；header-only 占位保留。
        switch (entry->kind)
        {
        case AssetKind::Mesh:
        {
            MeshAsset mesh;
            if (!Internal::DecodeMeshChunks(artifactBytes, readResult, mesh, stageError))
            {
                ++m_stats.manifestRejected;
                error = "mesh chunks rejected for " + entry->canonicalUri + ": " + stageError;
                return false;
            }
            pendingMeshes.push_back(PendingMesh{entry->id, std::move(mesh)});
            break;
        }
        case AssetKind::Texture:
        {
            TextureAsset texture;
            if (!Internal::DecodeTextureChunks(artifactBytes, readResult, texture, stageError))
            {
                ++m_stats.manifestRejected;
                error = "texture chunks rejected for " + entry->canonicalUri + ": " + stageError;
                return false;
            }
            pendingTextures.push_back(PendingTexture{entry->id, std::move(texture)});
            break;
        }
        case AssetKind::Material:
        {
            MaterialAsset material;
            if (!Internal::DecodeMaterialChunks(artifactBytes, readResult, material, stageError))
            {
                ++m_stats.manifestRejected;
                error = "material chunks rejected for " + entry->canonicalUri + ": " + stageError;
                return false;
            }
            pendingMaterials.push_back(PendingMaterial{entry->id, std::move(material)});
            break;
        }
        case AssetKind::World:
            break;
        }
    }

    // 全部验证通过才建立 pending；此前的任何失败都不触碰本函数外的状态。
    m_pendingRegistry = std::move(*registry);
    m_pendingManifestDigest = manifestDigest;
    m_pendingMeshes = std::move(pendingMeshes);
    m_pendingTextures = std::move(pendingTextures);
    m_pendingMaterials = std::move(pendingMaterials);
    m_pendingRemovals = std::move(pendingRemovals);
    m_pendingReport = std::move(pendingReport);
    return true;
}

void AssetManager::BeginCommit()
{
    if (!m_pendingRegistry)
    {
        return;
    }
    // 审计 3.1：先为 Added/Changed 建槽，让新 AssetId 的 Handle 在 world 重建前即可解析。
    // 本阶段不做任何可能失败的验证（payload 写入留给 FinishCommit）；若后续 world 重建失败，
    // 副作用仅为若干无 payload 的空槽——下次成功 Prepare/Finish 会填满，可恢复。
    for (const PendingMesh& pending : m_pendingMeshes)
    {
        static_cast<void>(m_meshes.ResolveOrCreate(pending.id));
    }
    for (const PendingTexture& pending : m_pendingTextures)
    {
        static_cast<void>(m_textures.ResolveOrCreate(pending.id));
    }
    for (const PendingMaterial& pending : m_pendingMaterials)
    {
        static_cast<void>(m_materials.ResolveOrCreate(pending.id));
    }
}

void AssetManager::FinishCommit()
{
    if (!m_pendingRegistry)
    {
        return;
    }

    m_activeRegistry = std::move(*m_pendingRegistry);
    m_pendingRegistry.reset();
    m_activeManifestDigest = m_pendingManifestDigest;
    m_hasActiveManifest = true;
    m_lastCommitReport = m_pendingReport;

    // P1-1：Removed 资产**延迟**到 ApplyPendingRemovals 再卸载。理由：reload 路径先建临时
    // World、成功后整体替换；若这里就 Unload，旧 World 的 Handle 会立即失效，而 World 重建
    // 失败时旧画面将因 stale handle 被跳过（物体静默消失），破坏 last-known-good 承诺。
    // m_pendingRemovals 保留到 ApplyPendingRemovals 消费（帧边界整体替换成功之后）。

    // commit 阶段不做任何可能失败的验证：pending 已在 Prepare 阶段完全就绪。
    // 报告以 diff 阶段的 pendingReport 为权威来源，此处不再追加（防双计）。
    // BeginCommit 已建槽：ResolveOrCreate 幂等，返回同一 Handle。
    for (PendingMesh& pending : m_pendingMeshes)
    {
        const auto handle = m_meshes.ResolveOrCreate(pending.id);
        static_cast<void>(m_meshes.Commit(handle, std::move(pending.asset)));
        ++m_stats.assetsCommitted;
    }
    m_pendingMeshes.clear();

    for (PendingTexture& pending : m_pendingTextures)
    {
        const auto handle = m_textures.ResolveOrCreate(pending.id);
        static_cast<void>(m_textures.Commit(handle, std::move(pending.asset)));
        ++m_stats.assetsCommitted;
    }
    m_pendingTextures.clear();

    for (PendingMaterial& pending : m_pendingMaterials)
    {
        const auto handle = m_materials.ResolveOrCreate(pending.id);
        static_cast<void>(m_materials.Commit(handle, std::move(pending.asset)));
        ++m_stats.assetsCommitted;
    }
    m_pendingMaterials.clear();
}

void AssetManager::CommitPending()
{
    BeginCommit();
    FinishCommit();
}

void AssetManager::ApplyPendingRemovals()
{
    for (PendingRemoval& removal : m_pendingRemovals)
    {
        bool unloaded = false;
        if (removal.kind == AssetKind::Mesh)
        {
            if (const auto handle = m_meshes.TryFind(removal.id))
            {
                unloaded = m_meshes.Unload(*handle);
            }
        }
        else if (removal.kind == AssetKind::Texture)
        {
            if (const auto handle = m_textures.TryFind(removal.id))
            {
                unloaded = m_textures.Unload(*handle);
            }
        }
        else if (removal.kind == AssetKind::Material)
        {
            if (const auto handle = m_materials.TryFind(removal.id))
            {
                unloaded = m_materials.Unload(*handle);
            }
        }
        if (unloaded)
        {
            ++m_stats.assetsRemoved;
        }
    }
    m_pendingRemovals.clear();
}

bool AssetManager::HasPendingCommit() const noexcept
{
    return m_pendingRegistry.has_value();
}

const AssetRegistry* AssetManager::PendingRegistry() const noexcept
{
    return m_pendingRegistry ? &*m_pendingRegistry : nullptr;
}

const AssetRegistry* AssetManager::Registry() const noexcept
{
    return m_activeRegistry ? &*m_activeRegistry : nullptr;
}

const Sha256Digest& AssetManager::ActiveManifestDigest() const noexcept
{
    return m_activeManifestDigest;
}

const AssetManager::Stats& AssetManager::GetStats() const noexcept
{
    return m_stats;
}

const AssetManager::CommitReport& AssetManager::LastCommit() const noexcept
{
    return m_lastCommitReport;
}

AssetPool<MeshAsset>& AssetManager::Meshes() noexcept
{
    return m_meshes;
}

const AssetPool<MeshAsset>& AssetManager::Meshes() const noexcept
{
    return m_meshes;
}

AssetPool<TextureAsset>& AssetManager::Textures() noexcept
{
    return m_textures;
}

const AssetPool<TextureAsset>& AssetManager::Textures() const noexcept
{
    return m_textures;
}

AssetPool<MaterialAsset>& AssetManager::Materials() noexcept
{
    return m_materials;
}

const AssetPool<MaterialAsset>& AssetManager::Materials() const noexcept
{
    return m_materials;
}
} // namespace MiniEngine::Assets
