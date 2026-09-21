// ============================================================================
// RenderProxyLayouts.cpp — 三布局的提取 + 视锥分类内核（M7-08）
// 关联：engine/world/include/MiniEngine/World/RenderProxyLayouts.h
//       engine/world/src/RenderQueueBuilder.cpp（生产 culling；判定规则逐条对齐）
// 关键约束：三个内核的**判定与顺序完全一致**，只有"从哪里读字段"不同：
//   AoS      逐条记录读取 world/localBounds/meshId/...
//   HotCold  hot 数组与 AoS 相同遍历，cold 数组在扫描中零访问
//   SoA      索引式读取并列数组（扫描字段彼此连续）
// 计时口径：都包含"包围球保守变换 + Classify + 收集"的整段扫描（生产
//   CullingStats::cullingCpuMicroseconds 只含 Classify，见头注释）。
// ============================================================================

#include <MiniEngine/World/RenderProxyLayouts.h>

#include <MiniEngine/Core/Assert.h>

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>

namespace MiniEngine::World
{
namespace
{
using Clock = std::chrono::steady_clock;

// FNV-1a：布局等价性的语义身份（不依赖内存布局与填充字节）。
inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void HashByte(std::uint64_t& hash, const std::uint8_t byte)
{
    hash ^= byte;
    hash *= kFnvPrime;
}

void HashU32(std::uint64_t& hash, const std::uint32_t value)
{
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        HashByte(hash, static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
    }
}

void HashAssetId(std::uint64_t& hash, const Assets::AssetId& id)
{
    for (const std::byte byte : id.bytes)
    {
        HashByte(hash, static_cast<std::uint8_t>(byte));
    }
}

[[nodiscard]] double MicrosBetween(const Clock::time_point start, const Clock::time_point end) noexcept
{
    return std::chrono::duration<double, std::micro>(end - start).count();
}

// 生产规则（RenderQueueBuilder.cpp 359-399 行）：查找失败或 culling 关闭时按
// "恒可见"处理（Inside），绝不因数据缺失静默丢物体。
[[nodiscard]] VolumeRelation ClassifyBounds(const bool hasBounds, const bool cullingEnabled, const Frustum& frustum,
                                            const Sphere& bounds)
{
    if (!hasBounds || !cullingEnabled)
    {
        return VolumeRelation::Inside;
    }
    return frustum.Classify(bounds);
}

// 单条代理的公共判定结果（三个内核共用，保证"只有访问形态不同"）。
struct ProxyDecision final
{
    VolumeRelation mainRelation = VolumeRelation::Inside;
    bool shadowCandidate = false;
    VolumeRelation shadowRelation = VolumeRelation::Inside;
};

void FoldMainIntoResult(LayoutCullResult& result, const std::uint32_t entityIndex, const ProxyDecision& decision,
                        const std::uint32_t indexCount)
{
    if (decision.mainRelation == VolumeRelation::Outside)
    {
        ++result.mainCulled;
        return;
    }
    result.mainVisibleEntityIndices.push_back(entityIndex);
    ++result.mainVisible;
    result.trianglesSubmitted += indexCount / 3U;
    if (decision.mainRelation == VolumeRelation::Inside)
    {
        ++result.mainInside;
    }
    else
    {
        ++result.mainIntersecting;
    }
}

void FoldShadowIntoResult(LayoutCullResult& result, const std::uint32_t entityIndex, const ProxyDecision& decision)
{
    if (!decision.shadowCandidate)
    {
        return;
    }
    ++result.shadowCandidates;
    if (decision.shadowRelation == VolumeRelation::Outside)
    {
        ++result.shadowCulled;
        return;
    }
    result.shadowVisibleEntityIndices.push_back(entityIndex);
    ++result.shadowVisible;
}

// 语义 hash：覆盖两组可见序列的 (entityIndex, meshId, materialId) 与全部计数。
// 时间差异只有在 hash 相同的前提下才允许解读（无 SIMD/无 early-out 的等价性锁）。
[[nodiscard]] std::uint64_t ComputeSemanticHash(
    const LayoutCullResult& result, const std::span<const std::pair<std::uint32_t, Assets::AssetId>> meshIds,
    const std::span<const std::pair<std::uint32_t, Assets::AssetId>> materialIds)
{
    std::uint64_t hash = kFnvOffset;
    HashU32(hash, result.candidateObjects);
    HashU32(hash, result.mainVisible);
    HashU32(hash, result.mainCulled);
    HashU32(hash, result.mainInside);
    HashU32(hash, result.mainIntersecting);
    HashU32(hash, result.shadowCandidates);
    HashU32(hash, result.shadowVisible);
    HashU32(hash, result.shadowCulled);
    HashU32(hash, result.trianglesSubmitted);
    const auto foldSequence = [&hash, &meshIds, &materialIds](const std::vector<std::uint32_t>& sequence)
    {
        HashU32(hash, static_cast<std::uint32_t>(sequence.size()));
        for (const std::uint32_t entityIndex : sequence)
        {
            HashU32(hash, entityIndex);
            HashAssetId(hash, meshIds[entityIndex].second);
            HashAssetId(hash, materialIds[entityIndex].second);
        }
    };
    foldSequence(result.mainVisibleEntityIndices);
    foldSequence(result.shadowVisibleEntityIndices);
    return hash;
}

[[nodiscard]] Sphere WorldBoundsOf(const bool hasBounds, const Matrix4& world, const Sphere& localBounds)
{
    if (!hasBounds)
    {
        // 生产同样的退化处理：无法查询 mesh 信息时按世界位置 + 0 半径保守处理。
        return {Float3{world.values[12], world.values[13], world.values[14]}, 0.0F};
    }
    return TransformSphereConservative(localBounds, world);
}
} // namespace

void RenderProxyHotCold::Validate() const
{
    ME_ASSERT(hot.size() == cold.size(), "hot/cold layouts must have equal size");
}

std::size_t RenderProxySoA::ScanBytesPerEntity() const noexcept
{
    // culling 扫描触碰的字段：world + localBounds + worldBounds + 两个 AssetId +
    // entityIndex + indexCount + flags。
    return sizeof(Matrix4) + 2 * sizeof(Sphere) + 2 * sizeof(Assets::AssetId) + 2 * sizeof(std::uint32_t) + 1;
}

std::size_t RenderProxySoA::PayloadBytesPerEntity() const noexcept
{
    return sizeof(Assets::AssetHandle<Assets::MeshAsset>) + sizeof(Assets::AssetHandle<Assets::MaterialAsset>);
}

std::size_t RenderProxySoA::HotBytes() const noexcept
{
    return Size() * (ScanBytesPerEntity() + PayloadBytesPerEntity());
}

std::size_t RenderProxySoA::CapacityBytes() const noexcept
{
    // 真实分配（含各数组 capacity 差异）——"allocations and copy bytes"证据。
    const auto bytes = [](const auto& array)
    {
        using Array = std::decay_t<decltype(array)>;
        return array.capacity() * sizeof(typename Array::value_type);
    };
    return bytes(world) + bytes(localBounds) + bytes(worldBounds) + bytes(meshIds) + bytes(materialIds) +
           bytes(meshes) + bytes(materials) + bytes(entityIndices) + bytes(indexCounts) + bytes(flags) + bytes(cold);
}

void RenderProxySoA::Validate() const
{
    const std::size_t size = Size();
    ME_ASSERT(world.size() == size, "SoA world array length mismatch");
    ME_ASSERT(localBounds.size() == size, "SoA localBounds array length mismatch");
    ME_ASSERT(worldBounds.size() == size, "SoA worldBounds array length mismatch");
    ME_ASSERT(meshIds.size() == size, "SoA meshIds array length mismatch");
    ME_ASSERT(materialIds.size() == size, "SoA materialIds array length mismatch");
    ME_ASSERT(meshes.size() == size, "SoA meshes array length mismatch");
    ME_ASSERT(materials.size() == size, "SoA materials array length mismatch");
    ME_ASSERT(indexCounts.size() == size, "SoA indexCounts array length mismatch");
    ME_ASSERT(flags.size() == size, "SoA flags array length mismatch");
    ME_ASSERT(cold.size() == size, "SoA cold array length mismatch");
}

std::vector<LayoutProxySource> BuildLayoutProxies(const std::span<const RenderItem> items,
                                                  const MeshInfoLookup& meshInfo,
                                                  const MaterialInfoLookup& materialInfo,
                                                  const std::span<const RenderProxyCold> cold)
{
    std::vector<LayoutProxySource> proxies;
    proxies.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        const RenderItem& item = items[index];
        LayoutProxySource proxy;
        proxy.world = item.world;
        proxy.mesh = item.mesh;
        proxy.material = item.material;
        proxy.entityIndex = static_cast<std::uint32_t>(index);
        proxy.mirrored = item.mirrored;
        proxy.castsShadow = item.castsShadow;
        proxy.receivesShadow = item.receivesShadow;
        if (materialInfo)
        {
            if (const std::optional<Assets::AssetId> id = materialInfo(item.material); id.has_value())
            {
                proxy.materialId = *id;
            }
        }
        if (meshInfo)
        {
            if (const std::optional<MeshDrawInfo> info = meshInfo(item.mesh); info.has_value())
            {
                proxy.meshId = info->id;
                proxy.localBounds = info->localBounds;
                proxy.indexCount = info->indexCount;
                proxy.hasBounds = true;
            }
            else
            {
                proxy.hasBounds = false;
            }
        }
        else
        {
            proxy.hasBounds = false;
        }
        if (index < cold.size())
        {
            proxy.cold = cold[index];
        }
        proxies.push_back(std::move(proxy));
    }
    return proxies;
}

RenderProxyAoS ToAoS(const LayoutProxySource& source)
{
    RenderProxyAoS proxy;
    proxy.world = source.world;
    proxy.localBounds = source.localBounds;
    proxy.mesh = source.mesh;
    proxy.material = source.material;
    proxy.meshId = source.meshId;
    proxy.materialId = source.materialId;
    proxy.entityIndex = source.entityIndex;
    proxy.indexCount = source.indexCount;
    proxy.mirrored = source.mirrored;
    proxy.castsShadow = source.castsShadow;
    proxy.receivesShadow = source.receivesShadow;
    proxy.hasBounds = source.hasBounds;
    return proxy;
}

RenderProxyHotCold ToHotCold(const std::span<const LayoutProxySource> sources)
{
    RenderProxyHotCold layout;
    layout.hot.reserve(sources.size());
    layout.cold.reserve(sources.size());
    for (const LayoutProxySource& source : sources)
    {
        layout.hot.push_back(ToAoS(source));
        layout.cold.push_back(source.cold); // 冷数据物理分离：扫描热路径不再触碰
    }
    layout.Validate();
    return layout;
}

RenderProxySoA ToSoA(const std::span<const LayoutProxySource> sources)
{
    RenderProxySoA layout;
    const std::size_t count = sources.size();
    layout.world.reserve(count);
    layout.localBounds.reserve(count);
    layout.worldBounds.reserve(count);
    layout.meshIds.reserve(count);
    layout.materialIds.reserve(count);
    layout.meshes.reserve(count);
    layout.materials.reserve(count);
    layout.entityIndices.reserve(count);
    layout.indexCounts.reserve(count);
    layout.flags.reserve(count);
    layout.cold.reserve(count);
    for (const LayoutProxySource& source : sources)
    {
        layout.world.push_back(source.world);
        layout.localBounds.push_back(source.localBounds);
        layout.worldBounds.push_back(Sphere{});
        layout.meshIds.push_back(source.meshId);
        layout.materialIds.push_back(source.materialId);
        layout.meshes.push_back(source.mesh);
        layout.materials.push_back(source.material);
        layout.entityIndices.push_back(source.entityIndex);
        layout.indexCounts.push_back(source.indexCount);
        std::uint8_t flags = 0;
        flags |= source.mirrored ? 0x01U : 0U;
        flags |= source.castsShadow ? 0x02U : 0U;
        flags |= source.receivesShadow ? 0x04U : 0U;
        flags |= source.hasBounds ? 0x08U : 0U;
        layout.flags.push_back(flags);
        layout.cold.push_back(source.cold);
    }
    layout.Validate();
    return layout;
}

void RefreshPerFrameFields(const LayoutProxySource& source, RenderProxyAoS& layout)
{
    layout.world = source.world;
    layout.mirrored = source.mirrored;
    layout.hasBounds = source.hasBounds;
    layout.castsShadow = source.castsShadow;
}

void RefreshPerFrameFields(const std::span<const LayoutProxySource> sources, RenderProxyHotCold& layout)
{
    ME_ASSERT(sources.size() == layout.Size(), "hot/cold refresh requires matching sizes");
    for (std::size_t index = 0; index < sources.size(); ++index)
    {
        RefreshPerFrameFields(sources[index], layout.hot[index]);
    }
}

void RefreshPerFrameFields(const std::span<const LayoutProxySource> sources, RenderProxySoA& layout)
{
    ME_ASSERT(sources.size() == layout.Size(), "SoA refresh requires matching sizes");
    for (std::size_t index = 0; index < sources.size(); ++index)
    {
        const LayoutProxySource& source = sources[index];
        layout.world[index] = source.world;
        std::uint8_t flags = layout.flags[index];
        flags = source.mirrored ? static_cast<std::uint8_t>(flags | 0x01U) : static_cast<std::uint8_t>(flags & ~0x01U);
        flags =
            source.castsShadow ? static_cast<std::uint8_t>(flags | 0x02U) : static_cast<std::uint8_t>(flags & ~0x02U);
        flags = source.hasBounds ? static_cast<std::uint8_t>(flags | 0x08U) : static_cast<std::uint8_t>(flags & ~0x08U);
        layout.flags[index] = flags;
    }
}

LayoutCullResult CullAoS(const std::span<const RenderProxyAoS> proxies, const Frustum& cameraFrustum,
                         const Frustum& lightFrustum, const CullingOptions& options)
{
    LayoutCullResult result;
    std::vector<std::pair<std::uint32_t, Assets::AssetId>> meshIds;
    std::vector<std::pair<std::uint32_t, Assets::AssetId>> materialIds;
    meshIds.reserve(proxies.size());
    materialIds.reserve(proxies.size());

    const auto start = Clock::now();
    for (const RenderProxyAoS& proxy : proxies)
    {
        ++result.candidateObjects;
        const Sphere worldBounds = WorldBoundsOf(proxy.hasBounds, proxy.world, proxy.localBounds);
        ProxyDecision decision;
        decision.mainRelation = ClassifyBounds(proxy.hasBounds, options.mainCulling, cameraFrustum, worldBounds);
        decision.shadowCandidate = proxy.castsShadow;
        decision.shadowRelation =
            proxy.castsShadow ? ClassifyBounds(proxy.hasBounds, options.shadowCulling, lightFrustum, worldBounds)
                              : VolumeRelation::Inside;
        FoldMainIntoResult(result, proxy.entityIndex, decision, proxy.indexCount);
        FoldShadowIntoResult(result, proxy.entityIndex, decision);
        meshIds.emplace_back(proxy.entityIndex, proxy.meshId);
        materialIds.emplace_back(proxy.entityIndex, proxy.materialId);
    }
    const auto end = Clock::now();
    result.scanCpuMicroseconds = MicrosBetween(start, end);
    result.semanticHash = ComputeSemanticHash(result, meshIds, materialIds);
    return result;
}

LayoutCullResult CullHotCold(const RenderProxyHotCold& proxies, const Frustum& cameraFrustum,
                             const Frustum& lightFrustum, const CullingOptions& options)
{
    // hot 数组与 AoS 完全相同的遍历；cold 数组在扫描中零访问（split 的意义所在，
    // 冷访问路径的额外随机访问代价由等价测试单独验证）。
    proxies.Validate();
    return CullAoS(std::span<const RenderProxyAoS>(proxies.hot.data(), proxies.hot.size()), cameraFrustum, lightFrustum,
                   options);
}

LayoutCullResult CullSoABatched(const RenderProxySoA& proxies, const Frustum& cameraFrustum,
                                const Frustum& lightFrustum, const CullingOptions& options,
                                const std::uint32_t blockSize)
{
    proxies.Validate();
    LayoutCullResult result;
    const std::size_t count = proxies.Size();
    std::vector<std::pair<std::uint32_t, Assets::AssetId>> meshIds;
    std::vector<std::pair<std::uint32_t, Assets::AssetId>> materialIds;
    meshIds.reserve(count);
    materialIds.reserve(count);

    const std::size_t step = blockSize == 0U ? 1U : static_cast<std::size_t>(blockSize);
    const auto start = Clock::now();
    for (std::size_t base = 0; base < count; base += step)
    {
        const std::size_t end = std::min(count, base + step);
        // 块级包围球：实体世界位置（矩阵平移列）的极值盒 + 保守半径上界。
        // 注意：这里**不能**用 `proxies.worldBounds` 数组——SoA 布局的那个数组只在
        // RefreshPerFrameFields 里随帧派生，逐实体内核根本不读它（内核自己用 WorldBoundsOf 现算），
        // 因此它可能是未填充的。块级结论必须是"对每个实体球都成立"的保守结论：
        // 半径上界 = 局部半径 × 3x3 列 L1 范数的最大值（与 TransformSphereConservative 同为保守估计）。
        bool allBounded = true;
        bool allCasters = true;
        const Matrix4& firstWorld = proxies.world[base];
        Float3 minimum{firstWorld.values[12], firstWorld.values[13], firstWorld.values[14]};
        Float3 maximum = minimum;
        float radiusUpperBound = 0.0F;
        for (std::size_t index = base; index < end; ++index)
        {
            const std::uint8_t flags = proxies.flags[index];
            allBounded = allBounded && (flags & 0x08U) != 0U;
            allCasters = allCasters && (flags & 0x02U) != 0U;
            const Matrix4& world = proxies.world[index];
            const float x = world.values[12];
            const float y = world.values[13];
            const float z = world.values[14];
            minimum.x = std::min(minimum.x, x);
            minimum.y = std::min(minimum.y, y);
            minimum.z = std::min(minimum.z, z);
            maximum.x = std::max(maximum.x, x);
            maximum.y = std::max(maximum.y, y);
            maximum.z = std::max(maximum.z, z);
            const float scaleBound =
                std::max(std::abs(world.values[0]) + std::abs(world.values[4]) + std::abs(world.values[8]),
                         std::max(std::abs(world.values[1]) + std::abs(world.values[5]) + std::abs(world.values[9]),
                                  std::abs(world.values[2]) + std::abs(world.values[6]) + std::abs(world.values[10])));
            radiusUpperBound = std::max(radiusUpperBound, proxies.localBounds[index].radius * scaleBound);
        }
        const float halfX = (maximum.x - minimum.x) * 0.5F;
        const float halfY = (maximum.y - minimum.y) * 0.5F;
        const float halfZ = (maximum.z - minimum.z) * 0.5F;
        Sphere blockBounds;
        blockBounds.center = Float3{(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
                                    (minimum.z + maximum.z) * 0.5F};
        blockBounds.radius = std::sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ) + radiusUpperBound;

        // 只有"块内全部有 bounds"（且该视锥开启剔除）时才允许块级结论覆盖逐实体分类：
        // hasBounds=false 的生产契约是"恒可见"（Inside），块级 AABB 对它无意义。
        bool mainFromBlock = false;
        bool shadowFromBlock = false;
        VolumeRelation mainBlockRelation = VolumeRelation::Inside;
        VolumeRelation shadowBlockRelation = VolumeRelation::Inside;
        if (allBounded && options.mainCulling)
        {
            const VolumeRelation relation = cameraFrustum.Classify(blockBounds);
            mainFromBlock = relation != VolumeRelation::Intersecting;
            mainBlockRelation = relation;
        }
        if (allBounded && allCasters && options.shadowCulling)
        {
            const VolumeRelation relation = lightFrustum.Classify(blockBounds);
            shadowFromBlock = relation != VolumeRelation::Intersecting;
            shadowBlockRelation = relation;
        }

        for (std::size_t index = base; index < end; ++index)
        {
            ++result.candidateObjects;
            const std::uint8_t flags = proxies.flags[index];
            const bool hasBounds = (flags & 0x08U) != 0U;
            const bool useMainBlock = mainFromBlock && hasBounds;
            const bool shadowCandidate = (flags & 0x02U) != 0U;
            const bool useShadowBlock = shadowFromBlock && hasBounds;
            // 两条分类都能走块级结论时，连世界包围球都不必算（批量化真正的收益点）。
            Sphere worldBounds;
            if (!useMainBlock || (shadowCandidate && !useShadowBlock))
            {
                worldBounds = WorldBoundsOf(hasBounds, proxies.world[index], proxies.localBounds[index]);
            }

            ProxyDecision decision;
            decision.mainRelation =
                useMainBlock ? mainBlockRelation
                             : ClassifyBounds(hasBounds, options.mainCulling, cameraFrustum, worldBounds);
            decision.shadowCandidate = shadowCandidate;
            decision.shadowRelation =
                !shadowCandidate ? VolumeRelation::Inside
                : useShadowBlock ? shadowBlockRelation
                                 : ClassifyBounds(hasBounds, options.shadowCulling, lightFrustum, worldBounds);
            FoldMainIntoResult(result, proxies.entityIndices[index], decision, proxies.indexCounts[index]);
            FoldShadowIntoResult(result, proxies.entityIndices[index], decision);
            meshIds.emplace_back(proxies.entityIndices[index], proxies.meshIds[index]);
            materialIds.emplace_back(proxies.entityIndices[index], proxies.materialIds[index]);
        }
    }
    const auto end = Clock::now();
    result.scanCpuMicroseconds = MicrosBetween(start, end);
    result.semanticHash = ComputeSemanticHash(result, meshIds, materialIds);
    return result;
}

LayoutCullResult CullSoA(const RenderProxySoA& proxies, const Frustum& cameraFrustum, const Frustum& lightFrustum,
                         const CullingOptions& options)
{
    proxies.Validate();
    LayoutCullResult result;
    const std::size_t count = proxies.Size();
    std::vector<std::pair<std::uint32_t, Assets::AssetId>> meshIds;
    std::vector<std::pair<std::uint32_t, Assets::AssetId>> materialIds;
    meshIds.reserve(count);
    materialIds.reserve(count);

    const auto start = Clock::now();
    for (std::size_t index = 0; index < count; ++index)
    {
        ++result.candidateObjects;
        const std::uint8_t flags = proxies.flags[index];
        const bool hasBounds = (flags & 0x08U) != 0U;
        // 与 AoS 内核对称：变换结果只作为局部量参与分类，不回写数组
        // （回写会给 SoA 增加一次 AoS 没有的存储，破坏单变量对照）。
        const Sphere worldBounds = WorldBoundsOf(hasBounds, proxies.world[index], proxies.localBounds[index]);

        ProxyDecision decision;
        decision.mainRelation = ClassifyBounds(hasBounds, options.mainCulling, cameraFrustum, worldBounds);
        decision.shadowCandidate = (flags & 0x02U) != 0U;
        decision.shadowRelation = decision.shadowCandidate
                                      ? ClassifyBounds(hasBounds, options.shadowCulling, lightFrustum, worldBounds)
                                      : VolumeRelation::Inside;
        FoldMainIntoResult(result, proxies.entityIndices[index], decision, proxies.indexCounts[index]);
        FoldShadowIntoResult(result, proxies.entityIndices[index], decision);
        meshIds.emplace_back(proxies.entityIndices[index], proxies.meshIds[index]);
        materialIds.emplace_back(proxies.entityIndices[index], proxies.materialIds[index]);
    }
    const auto end = Clock::now();
    result.scanCpuMicroseconds = MicrosBetween(start, end);
    result.semanticHash = ComputeSemanticHash(result, meshIds, materialIds);
    return result;
}

std::string CompareLayoutResults(const LayoutCullResult& reference, const LayoutCullResult& candidate)
{
    if (reference.candidateObjects != candidate.candidateObjects)
    {
        return "candidateObjects";
    }
    if (reference.mainVisible != candidate.mainVisible || reference.mainCulled != candidate.mainCulled ||
        reference.mainInside != candidate.mainInside || reference.mainIntersecting != candidate.mainIntersecting)
    {
        return "main counts";
    }
    if (reference.shadowCandidates != candidate.shadowCandidates ||
        reference.shadowVisible != candidate.shadowVisible || reference.shadowCulled != candidate.shadowCulled)
    {
        return "shadow counts";
    }
    if (reference.trianglesSubmitted != candidate.trianglesSubmitted)
    {
        return "trianglesSubmitted";
    }
    if (reference.mainVisibleEntityIndices != candidate.mainVisibleEntityIndices)
    {
        return "main visible sequence";
    }
    if (reference.shadowVisibleEntityIndices != candidate.shadowVisibleEntityIndices)
    {
        return "shadow visible sequence";
    }
    if (reference.semanticHash != candidate.semanticHash)
    {
        return "semantic hash";
    }
    return {};
}

std::string CompareLayoutResultsAgainstPacket(const LayoutCullResult& result, const RenderPacket& packet)
{
    // 失败消息必须带现场（哪一项、两边各是多少）：只在"计数不一致"这一句上打转的
    // 报错无法行动（M7-P02/P32 的教训）。
    const auto counterDiff = [&result, &packet]() -> std::string
    {
        const auto check = [](const char* name, const std::uint32_t kernel, const std::uint32_t production)
        {
            return kernel == production ? std::string{}
                                        : (std::string(name) + " kernel=" + std::to_string(kernel) +
                                           " production=" + std::to_string(production) + "; ");
        };
        std::string diff;
        diff += check("candidateObjects", result.candidateObjects, packet.stats.candidateObjects);
        diff += check("mainVisible", result.mainVisible, packet.stats.mainVisible);
        diff += check("mainCulled", result.mainCulled, packet.stats.mainCulled);
        diff += check("mainInside", result.mainInside, packet.stats.mainInside);
        diff += check("mainIntersecting", result.mainIntersecting, packet.stats.mainIntersecting);
        diff += check("shadowCandidates", result.shadowCandidates, packet.stats.shadowCandidates);
        diff += check("shadowVisible", result.shadowVisible, packet.stats.shadowVisible);
        diff += check("shadowCulled", result.shadowCulled, packet.stats.shadowCulled);
        diff += check("trianglesSubmitted", result.trianglesSubmitted, packet.stats.trianglesSubmitted);
        return diff;
    };
    const std::string counters = counterDiff();
    if (!counters.empty())
    {
        return "culling stats: " + counters;
    }
    // 生产 packet 的两个队列已按稳定排序输出，因此这里比"排序后的可见集合"。
    std::vector<std::uint32_t> packetMain;
    packetMain.reserve(packet.mainOpaque.size());
    for (const RenderDraw& draw : packet.mainOpaque)
    {
        packetMain.push_back(draw.entityIndex);
    }
    std::vector<std::uint32_t> kernelMain = result.mainVisibleEntityIndices;
    std::sort(packetMain.begin(), packetMain.end());
    std::sort(kernelMain.begin(), kernelMain.end());
    if (packetMain != kernelMain)
    {
        return "main visible set";
    }
    std::vector<std::uint32_t> packetShadow;
    packetShadow.reserve(packet.shadowCasters.size());
    for (const RenderDraw& draw : packet.shadowCasters)
    {
        packetShadow.push_back(draw.entityIndex);
    }
    std::vector<std::uint32_t> kernelShadow = result.shadowVisibleEntityIndices;
    std::sort(packetShadow.begin(), packetShadow.end());
    std::sort(kernelShadow.begin(), kernelShadow.end());
    if (packetShadow != kernelShadow)
    {
        return "shadow visible set";
    }
    return {};
}
} // namespace MiniEngine::World
