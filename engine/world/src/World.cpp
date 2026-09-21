// ============================================================================
// World.cpp — 实体生命周期、层级防护与 dirty 增量变换的实现
// 里程碑：M3（06 篇 + 审计 P2）
// 职责：实现 World.h 的契约：世代复用的实体池、显式 stack 的 parent-before-child
//       遍历（不递归，无栈深风险）、row-vector / 左手系的 TRS 组合，以及
//       "只重算 dirty 子树 + 收尾仍有 dirty 即环/断链"的增量更新与兜底断言。
// 关联：docs/architecture/DECISIONS.md §7
//       engine/world/include/MiniEngine/World/World.h（对外的失败语义）
// ============================================================================

#include <MiniEngine/World/World.h>

#include <MiniEngine/Core/Assert.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace MiniEngine::World
{
namespace
{
// 浮点容差：affine 结构校验与行列式下限共用。
constexpr float kEpsilon = 1.0e-6F;

// row-vector 语义下的矩阵乘（world = local * parentWorld）：
// result[r][c] = Σ left[r][k] * right[k][c]。
Matrix4 Multiply(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result{};
    result.values.fill(0.0F);
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            for (std::size_t inner = 0; inner < 4; ++inner)
            {
                result.values[row * 4 + column] += left.values[row * 4 + inner] * right.values[inner * 4 + column];
            }
        }
    }
    return result;
}

// 左上 3×3 子块的行列式：用于判定镜像（负值）与退化（接近 0）的变换。
float Determinant3x3(const Matrix4& value)
{
    const auto& m = value.values;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
           m[2] * (m[4] * m[9] - m[5] * m[8]);
}

bool IsValidAffineTransform(const Matrix4& value)
{
    const auto& m = value.values;
    const bool finite = std::ranges::all_of(m, [](const float component) { return std::isfinite(component); });
    // row-vector affine 约定：m03=m13=m23=0，m33=1，translation 位于 m30/m31/m32。
    const bool affine = std::abs(m[3]) <= kEpsilon && std::abs(m[7]) <= kEpsilon && std::abs(m[11]) <= kEpsilon &&
                        std::abs(m[15] - 1.0F) <= kEpsilon;
    // 接近零 determinant 的 transform 在 World input 阶段拒绝（06 篇 mirrored 一节）。
    return finite && affine && std::abs(Determinant3x3(value)) > kEpsilon;
}
} // namespace

Entity World::CreateEntity()
{
    std::uint32_t index{};
    if (m_freeList.empty())
    {
        ME_VERIFY(m_slots.size() < Entity::kInvalidIndex, "World entity slot index exhausted");
        index = static_cast<std::uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    else
    {
        index = m_freeList.back();
        m_freeList.pop_back();
    }

    EntitySlot& slot = m_slots[index];
    slot.alive = true;
    slot.name.reset();
    slot.transform = TransformComponent{};
    slot.meshRenderer.reset();
    slot.children.clear();
    return Entity{index, slot.generation};
}

bool World::DestroyEntity(const Entity entity)
{
    if (!IsAlive(entity))
    {
        return false;
    }

    // 先用显式容器收集整个 subtree（M3：递归销毁该 baked world 创建的 subtree）。
    std::vector<Entity> subtree{entity};
    for (std::size_t current = 0; current < subtree.size(); ++current)
    {
        const EntitySlot& slot = m_slots[subtree[current].index];
        subtree.insert(subtree.end(), slot.children.begin(), slot.children.end());
    }

    const Entity parent = m_slots[entity.index].transform->parent;
    if (IsAlive(parent))
    {
        auto& siblings = m_slots[parent.index].children;
        std::erase(siblings, entity);
    }

    for (const Entity current : subtree)
    {
        EntitySlot& slot = m_slots[current.index];
        slot.name.reset();
        slot.transform.reset();
        slot.meshRenderer.reset();
        slot.children.clear();
        slot.alive = false;
        // generation 达到上限时明确 invariant failure，不允许 wrap 让远古 Entity 复活。
        ME_VERIFY(slot.generation != std::numeric_limits<std::uint32_t>::max(), "World entity generation exhausted");
        ++slot.generation;
        m_freeList.push_back(current.index);
    }
    return true;
}

bool World::IsAlive(const Entity entity) const
{
    return entity.IsValid() && entity.index < m_slots.size() && m_slots[entity.index].alive &&
           m_slots[entity.index].generation == entity.generation;
}

bool World::SetLocalMatrix(const Entity entity, const Matrix4& local)
{
    TransformComponent* transform = TryGetTransform(entity);
    if (transform == nullptr || !IsValidAffineTransform(local))
    {
        return false;
    }
    transform->local = local;
    MarkSubtreeDirty(entity);
    return true;
}

bool World::SetLocalTrs(const Entity entity, const Float3 translation, const Quaternion rotation, const Float3 scale)
{
    // Quaternion 输入 normalize；零向量明确拒绝。
    const float lengthSquared =
        rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0F)
    {
        return false;
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    const float x = rotation.x * inverseLength;
    const float y = rotation.y * inverseLength;
    const float z = rotation.z * inverseLength;
    const float w = rotation.w * inverseLength;

    // row-vector 约定（p' = p * M）下 M = S * R * T：
    // 前 3 行为缩放过的旋转行，translation 位于最后一行。左手系：绕 +Y 正角度把 +Z 转向 +X。
    Matrix4 local{};
    local.values[0] = scale.x * (1.0F - 2.0F * (y * y + z * z));
    local.values[1] = scale.x * (2.0F * (x * y + w * z));
    local.values[2] = scale.x * (2.0F * (x * z - w * y));
    local.values[4] = scale.y * (2.0F * (x * y - w * z));
    local.values[5] = scale.y * (1.0F - 2.0F * (x * x + z * z));
    local.values[6] = scale.y * (2.0F * (y * z + w * x));
    local.values[8] = scale.z * (2.0F * (x * z + w * y));
    local.values[9] = scale.z * (2.0F * (y * z - w * x));
    local.values[10] = scale.z * (1.0F - 2.0F * (x * x + y * y));
    local.values[12] = translation.x;
    local.values[13] = translation.y;
    local.values[14] = translation.z;
    return SetLocalMatrix(entity, local);
}

bool World::SetParent(const Entity child, const Entity parent, const KeepWorld keepWorld)
{
    if (keepWorld == KeepWorld::Yes)
    {
        // M3 明确失败，不假装支持 KeepWorld::Yes（06 篇 Parent 规则）。
        return false;
    }

    TransformComponent* childTransform = TryGetTransform(child);
    if (childTransform == nullptr || child == parent)
    {
        return false;
    }
    if (parent.IsValid() &&
        (TryGetTransform(parent) == nullptr || WouldCreateCycle(child, parent) || !FitsHierarchyDepth(child, parent)))
    {
        return false;
    }

    // 重复设置同一 parent 不产生重复 child：先比较再改容器。
    if (childTransform->parent == parent)
    {
        return true;
    }

    if (IsAlive(childTransform->parent))
    {
        auto& oldSiblings = m_slots[childTransform->parent.index].children;
        std::erase(oldSiblings, child);
    }

    childTransform->parent = parent;
    if (parent.IsValid())
    {
        m_slots[parent.index].children.push_back(child);
    }
    MarkSubtreeDirty(child);
    return true;
}

bool World::SetName(const Entity entity, std::string value)
{
    if (!IsAlive(entity))
    {
        return false;
    }

    m_slots[entity.index].name = NameComponent{std::move(value)};
    return true;
}

bool World::RemoveName(const Entity entity)
{
    if (!IsAlive(entity) || !m_slots[entity.index].name)
    {
        return false;
    }

    m_slots[entity.index].name.reset();
    return true;
}

const NameComponent* World::TryGetName(const Entity entity) const
{
    return IsAlive(entity) && m_slots[entity.index].name ? &*m_slots[entity.index].name : nullptr;
}

bool World::SetMeshRenderer(const Entity entity, const MeshRendererComponent& component)
{
    // 只验证 CPU Handle；typed Handle 是否指向有效 slot 由 Assets 层语义负责。
    // M4-02 v2：材质句柄取代纹理句柄 + baseColorFactor。
    if (!IsAlive(entity) || !component.mesh.IsValid() || !component.material.IsValid())
    {
        return false;
    }

    m_slots[entity.index].meshRenderer = component;
    return true;
}

bool World::RemoveMeshRenderer(const Entity entity)
{
    if (!IsAlive(entity) || !m_slots[entity.index].meshRenderer)
    {
        return false;
    }

    m_slots[entity.index].meshRenderer.reset();
    return true;
}

const MeshRendererComponent* World::TryGetMeshRenderer(const Entity entity) const
{
    return IsAlive(entity) && m_slots[entity.index].meshRenderer ? &*m_slots[entity.index].meshRenderer : nullptr;
}

TransformComponent* World::TryGetTransform(const Entity entity)
{
    return IsAlive(entity) && m_slots[entity.index].transform ? &*m_slots[entity.index].transform : nullptr;
}

const TransformComponent* World::TryGetTransform(const Entity entity) const
{
    return IsAlive(entity) && m_slots[entity.index].transform ? &*m_slots[entity.index].transform : nullptr;
}

bool World::WouldCreateCycle(const Entity child, Entity candidateParent) const
{
    // 沿 proposed parent chain 向上，最多 entityCount 步，超过即视为环。
    std::size_t visited{};
    while (candidateParent.IsValid())
    {
        if (candidateParent == child)
        {
            return true;
        }
        const TransformComponent* transform = TryGetTransform(candidateParent);
        if (transform == nullptr || ++visited > m_slots.size())
        {
            return true;
        }
        candidateParent = transform->parent;
    }
    return false;
}

bool World::FitsHierarchyDepth(const Entity child, Entity candidateParent) const
{
    std::size_t ancestorDepth{};
    while (candidateParent.IsValid())
    {
        const TransformComponent* transform = TryGetTransform(candidateParent);
        if (transform == nullptr || ++ancestorDepth >= kMaxHierarchyDepth)
        {
            return false;
        }
        candidateParent = transform->parent;
    }

    // 新 subtree 深度也要计入预算。
    std::vector<std::pair<Entity, std::size_t>> pending{{child, 1}};
    while (!pending.empty())
    {
        const auto [current, relativeDepth] = pending.back();
        pending.pop_back();
        if (ancestorDepth + relativeDepth > kMaxHierarchyDepth)
        {
            return false;
        }

        for (const Entity descendant : m_slots[current.index].children)
        {
            pending.emplace_back(descendant, relativeDepth + 1);
        }
    }
    return true;
}

void World::MarkSubtreeDirty(const Entity root)
{
    // 显式 stack 遍历真实 children，不递归。
    std::vector<Entity> pending{root};
    while (!pending.empty())
    {
        const Entity current = pending.back();
        pending.pop_back();
        if (TransformComponent* transform = TryGetTransform(current))
        {
            transform->dirty = true;
        }

        for (const Entity child : m_slots[current.index].children)
        {
            pending.push_back(child);
        }
    }
}

void World::UpdateTransforms()
{
    // 增量更新（审计 P2：dirty 之前"付费但零收益"）：只重算 dirty 节点以及它的
    // 受影响子树（父级本轮重算 → 子级必须跟随）。父级未变且自身未 dirty 的节点
    // 保留上一轮的 world（数值不变，直接剪掉乘法）。
    struct Frame final
    {
        Entity entity;
        bool parentChanged;
    };
    std::vector<Frame> pending;
    pending.reserve(m_slots.size());
    for (std::uint32_t index = 0; index < m_slots.size(); ++index)
    {
        const EntitySlot& slot = m_slots[index];
        if (slot.alive && slot.transform && !slot.transform->parent.IsValid())
        {
            pending.push_back(Frame{Entity{index, slot.generation}, false});
        }
    }

    while (!pending.empty())
    {
        const Frame frame = pending.back();
        pending.pop_back();
        TransformComponent* transform = TryGetTransform(frame.entity);
        if (transform == nullptr)
        {
            continue;
        }

        const bool recompute = frame.parentChanged || transform->dirty;
        if (recompute)
        {
            if (transform->parent.IsValid())
            {
                const TransformComponent* parent = TryGetTransform(transform->parent);
                transform->world = parent != nullptr ? Multiply(transform->local, parent->world) : transform->local;
            }
            else
            {
                transform->world = transform->local;
            }
            // negative scale 可令 world determinant 为负：RenderItem 用它选择 rasterizer state。
            transform->mirrored = Determinant3x3(transform->world) < 0.0F;
            transform->dirty = false;
        }

        for (const Entity child : m_slots[frame.entity.index].children)
        {
            pending.push_back(Frame{child, recompute});
        }
    }

    // 仍带 dirty 说明存在环/断链（该子树无 root 可达）：invariant failure。
    for (const EntitySlot& slot : m_slots)
    {
        if (slot.alive && slot.transform && slot.transform->dirty)
        {
            ME_VERIFY(false, "Cycle or broken parent link leaves dirty transforms after UpdateTransforms");
        }
    }
}

std::vector<RenderItem> World::BuildRenderItems() const
{
    // 按 slot 顺序遍历 = 按 Entity index 的 deterministic order。
    std::vector<RenderItem> result;
    for (const EntitySlot& slot : m_slots)
    {
        if (!slot.alive || !slot.transform || !slot.meshRenderer || !slot.meshRenderer->visible)
        {
            continue;
        }

        ME_VERIFY(!slot.transform->dirty, "UpdateTransforms must run before BuildRenderItems");
        // 06 篇：shadow 标记随 item 一起提取，shadow queue 的过滤与统计才有真实依据。
        result.push_back(RenderItem{slot.transform->world, slot.meshRenderer->mesh, slot.meshRenderer->material,
                                    slot.transform->mirrored, slot.meshRenderer->castsShadow,
                                    slot.meshRenderer->receivesShadow});
    }
    return result;
}
} // namespace MiniEngine::World
