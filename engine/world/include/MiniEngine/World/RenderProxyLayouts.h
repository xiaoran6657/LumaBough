// ============================================================================
// RenderProxyLayouts.h — 数据布局实验：AoS / hot-cold / SoA（M7-08）
// 里程碑：M7-08（数据布局实验）
// 职责：把"每帧扫描的渲染代理"用三种内存布局表达，并提供**算法完全一致**的
//       提取 + 视锥分类内核，用于可复现的布局 A/B：
//
//         RenderProxyAoS       —— baseline，字段与 M4/M6 生产 RenderDraw + 本地球同构
//         RenderProxyHotCold   —— 扫描热字段与冷字段（debug name/源资产/诊断）分离
//         RenderProxySoA       —— 扫描字段变成长度一致的并列数组
//
// 冻结项（docs/architecture/README.md「先冻结算法」）：
//   * 相同的 frustum plane、数学精度与可见性规则（复用 Frustum::Classify 与
//     TransformSphereConservative，不引入 SIMD / BVH / early-out）；
//   * 相同对象顺序（内核按输入顺序扫描，输出可见序列与生产 culling 同序）；
//   * 相同输出字段与语义 hash（同一份 FNV-1a 覆盖 entityIndex/meshId/materialId/mirrored）；
//   * 三个内核的统计字段与生产 CullingStats 同名同义（可逐条对照）。
// 生命周期：布局对象是实验期的持续快照，publish 前必须 Validate()；每帧的可变字段
//       （world / worldBounds）由调用方在**计时区外**刷新，内核只做扫描 + 分类。
// 生产布局替换条件：输出 hash 等价 + micro/端到端实验记录通过（本篇不做替换）。
// 关联：docs/architecture/README.md
//       engine/world/src/RenderQueueBuilder.cpp（生产 culling 的真实实现）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/World/Frustum.h>
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <MiniEngine/World/WorldTypes.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Assets
{
struct MeshAsset;
struct MaterialAsset;
} // namespace MiniEngine::Assets

namespace MiniEngine::World
{
// 冷字段：扫描热路径永远不读（hot/cold 实验把这部分从 hot 数组里移出）。
// 内容只用于"冷数据随机访问是否退化"的验证，不参与任何判定。
struct RenderProxyCold final
{
    std::string debugName;            // 调试名（authoring/诊断）
    Assets::AssetId sourceAsset{};    // 源资产身份（.meworld 侧）
    std::uint32_t authoringIndex = 0; // 源文件中的顺序（编辑器用）
    std::uint32_t diagnosticsFlags = 0;
};

// 规范代理：实验的输入事实。由生产输入（RenderItem + 两条 lookup）解析而来，
// 保证 baseline 就是 M6 真实数据，而不是为实验另造一套字段。
struct LayoutProxySource final
{
    Matrix4 world{};      // 与生产 RenderItem.world 同源
    Sphere localBounds{}; // 与生产 MeshDrawInfo.localBounds 同源
    Assets::AssetHandle<Assets::MeshAsset> mesh{};
    Assets::AssetHandle<Assets::MaterialAsset> material{};
    Assets::AssetId meshId{};
    Assets::AssetId materialId{};
    std::uint32_t entityIndex = 0;
    // 与 CullingStats::trianglesSubmitted 同口径：为 0 时不贡献三角形数。
    std::uint32_t indexCount = 0;
    bool mirrored = false;
    bool castsShadow = true;
    bool receivesShadow = true;
    bool hasBounds = true; // lookup 缺失时为 false：生产路径按"恒可见"处理
    RenderProxyCold cold{};
};

// ---- 布局 1：AoS（baseline） ----
// 每个代理一条连续记录；扫描时一条 cache line 里混有扫描字段与不扫描字段。
struct RenderProxyAoS final
{
    Matrix4 world{};
    Sphere localBounds{};
    Sphere worldBounds{}; // 每帧派生（TransformSphereConservative）
    Assets::AssetHandle<Assets::MeshAsset> mesh{};
    Assets::AssetHandle<Assets::MaterialAsset> material{};
    Assets::AssetId meshId{};
    Assets::AssetId materialId{};
    std::uint32_t entityIndex = 0;
    std::uint32_t indexCount = 0;
    bool mirrored = false;
    bool castsShadow = true;
    bool receivesShadow = true;
    bool hasBounds = true;

    [[nodiscard]] std::size_t Bytes() const noexcept
    {
        return sizeof(RenderProxyAoS);
    }
};

// ---- 布局 2：hot/cold split ----
// hot 部分仍是 AoS（本实验只做 split，不改变 hot 内部形态）。
struct RenderProxyHotCold final
{
    std::vector<RenderProxyAoS> hot;
    std::vector<RenderProxyCold> cold;

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return hot.size();
    }
    [[nodiscard]] std::size_t HotBytes() const noexcept
    {
        return hot.size() * sizeof(RenderProxyAoS);
    }
    [[nodiscard]] std::size_t ColdBytes() const noexcept
    {
        return cold.size() * sizeof(RenderProxyCold);
    }

    void Validate() const;
};

// ---- 布局 3：SoA ----
// 扫描字段各自成数组（长度必须一致）；渲染端需要的句柄也保留为并列数组。
struct RenderProxySoA final
{
    std::vector<Matrix4> world;
    std::vector<Sphere> localBounds;
    std::vector<Sphere> worldBounds;
    std::vector<Assets::AssetId> meshIds;
    std::vector<Assets::AssetId> materialIds;
    std::vector<Assets::AssetHandle<Assets::MeshAsset>> meshes;
    std::vector<Assets::AssetHandle<Assets::MaterialAsset>> materials;
    std::vector<std::uint32_t> entityIndices;
    std::vector<std::uint32_t> indexCounts;
    // bit0 mirrored / bit1 castsShadow / bit2 receivesShadow / bit3 hasBounds
    std::vector<std::uint8_t> flags;
    std::vector<RenderProxyCold> cold; // 与 hot 数组等长（只用于冷访问验证）

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return entityIndices.size();
    }
    // 扫描热字段的逐实体字节（culling 内核实际触碰的那部分）。
    [[nodiscard]] std::size_t ScanBytesPerEntity() const noexcept;
    // 载荷字段（句柄）的逐实体字节：渲染端要用，但扫描不碰。
    [[nodiscard]] std::size_t PayloadBytesPerEntity() const noexcept;
    [[nodiscard]] std::size_t HotBytes() const noexcept;
    [[nodiscard]] std::size_t CapacityBytes() const noexcept;

    void Validate() const;
};

// 布局 culling 内核的结果：字段与 CullingStats 同名同义（计时除外）。
struct LayoutCullResult final
{
    std::vector<std::uint32_t> mainVisibleEntityIndices; // 扫描顺序（= 输入顺序）
    std::vector<std::uint32_t> shadowVisibleEntityIndices;
    std::uint32_t candidateObjects = 0;
    std::uint32_t mainVisible = 0;
    std::uint32_t mainCulled = 0;
    std::uint32_t mainInside = 0;
    std::uint32_t mainIntersecting = 0;
    std::uint32_t shadowCandidates = 0;
    std::uint32_t shadowVisible = 0;
    std::uint32_t shadowCulled = 0;
    std::uint32_t trianglesSubmitted = 0;
    // 语义身份 hash（FNV-1a）：覆盖两组可见序列的 (entityIndex, meshId, materialId, mirrored)
    // 与全部计数；布局间必须逐位相同，否则"时间差异"不允许解读。
    std::uint64_t semanticHash = 0;
    // 内核计时：包含"包围球保守变换 + 分类 + 收集"的完整扫描段（比生产的
    // cullingCpuMicroseconds 多含变换，因为布局差异影响的正是这一整段内存访问）。
    double scanCpuMicroseconds = 0.0;
};

// ---- 规范代理构造 ----
// 从生产输入构造规范代理：items 必须来自 World::BuildRenderItems()（确定性顺序），
// meshInfo/materialInfo 与生产 BuildRenderPacket 用的是同一对 lookup。
// cold 为空时冷字段留空（实验 1 的冷数据由调用方提供，用于验证冷访问路径）。
[[nodiscard]] std::vector<LayoutProxySource> BuildLayoutProxies(std::span<const RenderItem> items,
                                                                const MeshInfoLookup& meshInfo,
                                                                const MaterialInfoLookup& materialInfo,
                                                                std::span<const RenderProxyCold> cold = {});

// ---- 布局转换（在计时区外完成，不进入被测内核） ----
[[nodiscard]] RenderProxyAoS ToAoS(const LayoutProxySource& source);
[[nodiscard]] RenderProxyHotCold ToHotCold(std::span<const LayoutProxySource> sources);
[[nodiscard]] RenderProxySoA ToSoA(std::span<const LayoutProxySource> sources);

// 每帧刷新可变字段（world / mirrored / hasBounds 等逐帧量）：
// 计时区外调用，保证三个内核每帧看到的是同一份数据。
void RefreshPerFrameFields(const LayoutProxySource& source, RenderProxyAoS& layout);
void RefreshPerFrameFields(std::span<const LayoutProxySource> sources, RenderProxyHotCold& layout);
void RefreshPerFrameFields(std::span<const LayoutProxySource> sources, RenderProxySoA& layout);

// ---- 三个内核（算法一致，仅内存访问形态不同） ----
[[nodiscard]] LayoutCullResult CullAoS(std::span<const RenderProxyAoS> proxies, const Frustum& cameraFrustum,
                                       const Frustum& lightFrustum, const CullingOptions& options = {});
[[nodiscard]] LayoutCullResult CullHotCold(const RenderProxyHotCold& proxies, const Frustum& cameraFrustum,
                                           const Frustum& lightFrustum, const CullingOptions& options = {});
[[nodiscard]] LayoutCullResult CullSoA(const RenderProxySoA& proxies, const Frustum& cameraFrustum,
                                       const Frustum& lightFrustum, const CullingOptions& options = {});

// M7-LAYOUT-SIMD：SoA 的**批量化内核变体**（块级 early-out）。SoA 的收益通常要配合批量化内核
// 才显现：连续的位置数组让"整块"的包围球可以廉价地算出来，于是块级分类能把
// 两种情况一次性处理掉——块整体在视锥外（全部 Outside）或整体在内（全部 Inside）——
// 不必逐实体调用 Classify/WorldBoundsOf。
// **等价性契约**：块级结论只在"块内所有实体都有 bounds"时生效，且块级包围球取
// "极值中心 + 最大半径 + 中心盒对角半径"（严格包含块内全部实体球），因此 Outside/Inside
// 的覆盖是可靠的；其余情况逐实体回退到与 CullSoA 完全相同的判定。
// 结果（计数 + 可见序列 + 语义 hash）必须与 CullSoA **逐字节相同**——由
// tests/performance/LayoutEquivalenceTests 的等价契约守护。
[[nodiscard]] LayoutCullResult CullSoABatched(const RenderProxySoA& proxies, const Frustum& cameraFrustum,
                                              const Frustum& lightFrustum, const CullingOptions& options = {},
                                              const std::uint32_t blockSize = 64);

// 结果对照（等价性测试与运行器 Debug 校验共用）：
//   CompareLayoutResults         —— 两布局内核之间（计数 + 可见序列 + 语义 hash）；
//   CompareLayoutResultsAgainstPacket —— 布局内核与**生产 BuildRenderPacket** 之间
//                                   （计数逐项 + 两组可见集合按排序后比较）。
// 返回首个差异描述，等价时返回空串。
[[nodiscard]] std::string CompareLayoutResults(const LayoutCullResult& reference, const LayoutCullResult& candidate);
[[nodiscard]] std::string CompareLayoutResultsAgainstPacket(const LayoutCullResult& result, const RenderPacket& packet);
} // namespace MiniEngine::World
