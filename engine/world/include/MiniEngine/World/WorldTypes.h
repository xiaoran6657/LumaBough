// ============================================================================
// WorldTypes.h — 世界层的公共 POD 类型与实体句柄
// 里程碑：M3（06 篇 World/Transform/简单组件）
// 职责：定义 Float3 / Float4 / Quaternion / Matrix4 数学 POD、Entity 句柄与
//       Name / Transform / MeshRenderer 组件，以及帧渲染快照 RenderItem。
//       全部为值类型，不暴露 D3D / Win32 / XMMATRIX，因此 World 不依赖图形后端。
// 关联：docs/architecture/DECISIONS.md §7
//       engine/world/include/MiniEngine/World/World.h（这些类型的使用方）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetHandle.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace MiniEngine::Assets
{
struct MeshAsset;
struct MaterialAsset;
} // namespace MiniEngine::Assets

namespace MiniEngine::World
{
// 层级深度预算：SetParent 与 .meworld 两遍链接都必须强制执行（06 篇 Parent 规则）。
inline constexpr std::size_t kMaxHierarchyDepth = 1024;

// public 数学 POD：不暴露 D3D、Win32 或 XMMATRIX（06 篇 Math POD 一节）。
// 三维向量；坐标空间遵循 Matrix4 注明的左手系、+Y 向上、+Z 向前约定。
struct Float3 final
{
    float x{};
    float y{};
    float z{};
};

// 四维向量；本层主要用作 RGBA 颜色（各分量 0..1，见 World.cpp 的 IsValidBaseColor）。
struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};
};

// 四元数（x, y, z, w）。默认值即单位四元数（不旋转）；
// SetLocalTrs 会先 normalize 输入，零向量被明确拒绝。
struct Quaternion final
{
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

struct Matrix4 final
{
    // Row-major storage, row-vector convention: world = local * parentWorld。
    // 左手系、+Y up、+Z forward；translation 位于 values[12..14]（最后一 row 前三个元素）。
    // affine 约定：m03/m13/m23（values[3]/[7]/[11]）为 0，m33（values[15]）为 1。
    std::array<float, 16> values{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
};

// 实体句柄：槽位下标 + 世代，与 AssetHandle 同构的世代复用语义。
//
// index 是 World 内槽位数组的下标，generation 在槽位销毁时递增，
// 因此"销毁后重建"的实体不会与仍在流传的旧句柄混淆。
struct Entity final
{
    // 无效槽位下标（sentinel），与任何真实槽位都不冲突。
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    // 槽位下标；kInvalidIndex 表示无效。
    std::uint32_t index{kInvalidIndex};
    // 槽位世代；0 保留给无效句柄（World 的槽位初值为 1）。
    std::uint32_t generation{};

    // 默认 Entity invalid：index 非 sentinel 且 generation 非 0 才有效。
    [[nodiscard]] bool IsValid() const noexcept
    {
        return index != kInvalidIndex && generation != 0;
    }

    friend bool operator==(Entity, Entity) = default;
};

// 可选的名字组件（字节串，来自源资产的字符串表）。
struct NameComponent final
{
    std::string value;
};

// 变换组件：每个存活实体在创建时自动携带，不可移除。
struct TransformComponent final
{
    // 权威的本地表达（SetLocalMatrix / SetParent 的输入）。
    Matrix4 local{};
    // UpdateTransforms 的产出：world = local * parent.world（根节点 world = local）。
    Matrix4 world{};
    // 父实体；无效值表示根节点。
    Entity parent{};
    bool dirty{true}; // worldDirty：local 或祖先变化后待 UpdateTransforms 刷新
    // world 的 3×3 行列式为负（含镜像 / 负缩放）：渲染端据此选择相反的正面绕序。
    bool mirrored{};
};

// 网格渲染组件：引用 Mesh 与 Material 句柄（M4-02 v2：材质语义全部由 `.memat`
// 承载，组件不再内联 baseColorFactor / 纹理句柄）。两个句柄都必须有效
// （SetMeshRenderer 校验）——无材质 primitive 由 Cooker 合成 default `.memat`
// 兜底，保证材质句柄恒有产物可指。
struct MeshRendererComponent final
{
    Assets::AssetHandle<Assets::MeshAsset> mesh;
    Assets::AssetHandle<Assets::MaterialAsset> material;
    // false 时 BuildRenderItems 跳过该实体。
    bool visible{true};
    // 06 篇「两次 culling」：shadow queue 的过滤条件与统计口径都依赖每个物体的
    // 投影/受影标记，不能由渲染端假定"全部投影"。默认 true——M4 固定场景所有
    // 物体默认投影且受影（改为默认 false 会让 shadow pass 静默失效）。
    bool castsShadow{true};
    bool receivesShadow{true};
};

// 当前帧快照：不存 Entity pointer，不拥有 Asset payload / GPU 对象，无 D3D 类型。
// M4-02：材质句柄取代纹理句柄 + baseColorFactor（因子在 MaterialAsset 与 b2 CB 中）。
// M4-06：携带 castsShadow/receivesShadow——shadow queue 是独立 cull 的结果，
// 其过滤条件必须来自 World 侧标记而非渲染端常量。
struct RenderItem final
{
    Matrix4 world{};
    Assets::AssetHandle<Assets::MeshAsset> mesh;
    Assets::AssetHandle<Assets::MaterialAsset> material;
    bool mirrored{};
    bool castsShadow{true};
    bool receivesShadow{true};
};
} // namespace MiniEngine::World
