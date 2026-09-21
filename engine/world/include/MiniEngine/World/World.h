// ============================================================================
// World.h — 实体层级、变换与简单组件的场景图
// 里程碑：M3（06 篇 World/Transform/简单组件）
// 职责：提供实体创建/销毁（世代复用）、父子层级（环与深度预算防护）、本地矩阵与
//       TRS 设置、Name / MeshRenderer 组件，以及帧边界的 UpdateTransforms（dirty
//       增量）与 BuildRenderItems（确定性快照）。只含 CPU 数据，不建 GPU 对象、
//       不解析 AssetId。
// 关联：docs/architecture/DECISIONS.md §7
//       engine/world/src/WorldLoader.cpp（从 .meworld 构建本类型的调用方）
// ============================================================================

#pragma once

#include <MiniEngine/World/WorldTypes.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MiniEngine::World
{
// M3 默认 KeepWorld::No（local 不变，world 随 parent 改变）；
// KeepWorld::Yes 明确失败，不假装支持（06 篇 Parent 规则）。
enum class KeepWorld : std::uint8_t
{
    No,
    Yes
};

// 场景世界：实体槽位池 + 真实父子列表 + 可选组件。
//
// 所有可失败入口（返回 bool）都遵循"失败即无副作用"：不会留下半写入的组件或
// 不一致的父子容器。实体句柄按世代判定失效，与 AssetHandle 的语义一致。
class World final
{
  public:
    // create 取得 free slot 或追加 slot；每个 alive Entity 自动携带 TransformComponent。
    [[nodiscard]] Entity CreateEntity();
    // 销毁该 Entity 及其整个 subtree，移除全部 components，generation+1 后进 free list。
    [[nodiscard]] bool DestroyEntity(Entity entity);
    // 先 bounds，再 alive/generation。
    [[nodiscard]] bool IsAlive(Entity entity) const;

    // matrix 为权威 local 表达：验证 finite/affine 后写 local，并标记 subtree dirty。
    [[nodiscard]] bool SetLocalMatrix(Entity entity, const Matrix4& local);
    // TRS 便捷入口：rotation 先 normalize，再按 row-vector/LH 约定 compose 后走 SetLocalMatrix。
    [[nodiscard]] bool SetLocalTrs(Entity entity, Float3 translation, Quaternion rotation, Float3 scale);
    // 建立或修改父子关系；传入无效 parent 表示把 child 脱离到根。
    //
    // 拒绝条件：KeepWorld::Yes、child 无效、自父子、会成环（candidate 位于 child 的
    // 祖先链上）、或新层级超出 kMaxHierarchyDepth。local 保持不变（KeepWorld::No 语义），
    // 子树被标记 dirty 待下一轮 UpdateTransforms 刷新 world。
    //
    // 参数：
    //   child     —— 被移动的实体
    //   parent    —— 新父实体；无效值表示挂到根
    //   keepWorld —— 仅接受 No（见枚举注释）
    // 返回：成功为 true；任一拒绝为 false 且不产生任何修改。
    [[nodiscard]] bool SetParent(Entity child, Entity parent, KeepWorld keepWorld = KeepWorld::No);

    // 设置名字组件（覆盖旧值）。
    //
    // 参数：
    //   value —— 新名字，直接移入
    // 返回：成功为 true；实体无效为 false。
    [[nodiscard]] bool SetName(Entity entity, std::string value);

    // 移除名字组件。
    //
    // 返回：移除成功为 true；实体无效或本无名字组件为 false。
    [[nodiscard]] bool RemoveName(Entity entity);

    // 读取名字组件。
    //
    // 返回：存在为只读指针（随实体生命周期有效）；否则 nullptr。
    [[nodiscard]] const NameComponent* TryGetName(Entity entity) const;

    // 只接收 CPU Handle，不解析 AssetId，不创建 GPU 对象；无效输入返回 false 且不留半写入组件。
    [[nodiscard]] bool SetMeshRenderer(Entity entity, const MeshRendererComponent& component);
    // 移除网格渲染组件。
    //
    // 返回：移除成功为 true；实体无效或本无该组件为 false。
    [[nodiscard]] bool RemoveMeshRenderer(Entity entity);

    // 读取网格渲染组件。
    //
    // 返回：存在为只读指针（随实体生命周期有效）；否则 nullptr。
    [[nodiscard]] const MeshRendererComponent* TryGetMeshRenderer(Entity entity) const;

    // 读取变换组件。
    //
    // 非常量版本供确需直接改写的调用方使用；常规路径请走 SetLocalMatrix / SetParent，
    // 只有它们会维护 dirty 标记并触发子树重算。
    //
    // 返回：存在为组件指针；否则 nullptr。
    [[nodiscard]] TransformComponent* TryGetTransform(Entity entity);

    // 上一条的只读版本，语义相同。
    [[nodiscard]] const TransformComponent* TryGetTransform(Entity entity) const;

    // 固定更新后调用：显式 stack 做 parent-before-child 遍历，root: world=local，
    // child: world=local*parent.world；发现 alive child 未访问即环/断链，失败。
    void UpdateTransforms();
    // 要求 UpdateTransforms 已完成；按 Entity index 生成 deterministic order。
    [[nodiscard]] std::vector<RenderItem> BuildRenderItems() const;

  private:
    struct EntitySlot final
    {
        std::uint32_t generation{1};
        bool alive{};
        std::optional<NameComponent> name;
        std::optional<TransformComponent> transform;
        std::optional<MeshRendererComponent> meshRenderer;
        // 保存真实子节点列表：销毁 subtree、dirty 传播与 parent-before-child 更新
        // 只遍历真实 children，禁止退化成"只存 parent + 全表扫描"（O(N²)）。
        std::vector<Entity> children;
    };

    // 判断把 child 挂到 candidateParent 下是否会成环（沿候选父链向上找 child）。
    // 访问步数以槽位总数封顶，因此断链 / 已成环的病态数据也不会让本函数死循环。
    [[nodiscard]] bool WouldCreateCycle(Entity child, Entity candidateParent) const;

    // 判断 child 及其整个子树挂到 candidateParent 下后是否仍在 kMaxHierarchyDepth 内。
    [[nodiscard]] bool FitsHierarchyDepth(Entity child, Entity candidateParent) const;

    // 把 root 及其子树的 dirty 全部置位（显式 stack 遍历真实 children，不递归）。
    void MarkSubtreeDirty(Entity root);

    // 实体槽位；下标即 Entity::index，一经分配便稳定存在。
    std::vector<EntitySlot> m_slots;
    // 空闲槽位栈（后进先出），供 CreateEntity 复用。
    std::vector<std::uint32_t> m_freeList;
};
} // namespace MiniEngine::World
