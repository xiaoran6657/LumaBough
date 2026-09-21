// ============================================================================
// AssetPool.h — 单类型资产的槽位池（槽位复用 + 世代 + 修订号）
// 里程碑：M3
// 职责：为某一种 CPU payload 类型 T 提供 AssetId → AssetHandle<T> 的映射与存储。
//       槽位卸载后进入空闲表供复用，世代随之递增使旧句柄失效；修订号只随 Commit
//       递增、Unload 不重置，因此跨生命周期单调——这是"内容已变更"的唯一事实来源。
// 关联：docs/architecture/DECISIONS.md §3、§5
//       engine/rhi/d3d11/src/D3D11AssetCache.cpp（按修订号比较驱动 GPU 重新上传）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/Core/Assert.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MiniEngine::Assets
{
// 单类型资产池：以 AssetId 为键、AssetHandle<T> 为对外身份的槽位存储。
//
// 三条不变式（热重载判定的地基）：
//   1. 世代在槽位每次 Unload 时递增，槽位复用后旧句柄必然失配；
//   2. 修订号只随 Commit 递增、Unload 不重置，因此跨资产生命周期单调递增；
//   3. m_lookup 只含"已分配且存活"的 AssetId，卸载即移除，不留悬空映射。
// 模板参数 T 为 CPU payload 类型（MeshAsset / TextureAsset），不含 GPU / 平台类型。
template <typename T> class AssetPool final
{
  public:
    // 只读视图：payload 裸指针 + 该槽位当前的修订号。
    //
    // 指针只在本次查询期间有效，不得跨 Commit / Unload 持有；
    // 修订号供消费者（如 D3D11AssetCache）判断是否需要重新上传 GPU 资源。
    struct View final
    {
        const T* asset{};
        std::uint64_t revision{};
    };

    // 按 AssetId 解析已加载资产的句柄；不存在则分配槽位并建立映射。
    //
    // 幂等：同一 AssetId 重复调用返回同一句柄，世代不变。
    // AssetManager::BeginCommit 借它在写入 payload 之前先把新增 AssetId 的槽位建好，
    // 使新资产的句柄在 World 重建阶段即可解析（ADR-0004 §7 的 reload 顺序）。
    //
    // 参数：
    //   id —— 逻辑身份；必须有效
    // 返回：与该 AssetId 对应的有效句柄。
    // 失败：id 为全零时记录 Fatal 日志并终止进程（ME_VERIFY）。
    [[nodiscard]] AssetHandle<T> ResolveOrCreate(const AssetId& id)
    {
        ME_VERIFY(id.IsValid(), "AssetPool requires a valid AssetId");

        if (const auto found = m_lookup.find(id); found != m_lookup.end())
        {
            const Slot& slot = m_slots[found->second];
            return {found->second, slot.generation};
        }

        const std::uint32_t index = AcquireSlot();
        Slot& slot = m_slots[index];
        slot.id = id;
        slot.alive = true;
        m_lookup.emplace(id, index);
        return {index, slot.generation};
    }

    // 7-C：已加载 slot 的只读查找。不存在/已卸载 → nullopt。
    // 与 ResolveOrCreate 的区别：不分配、不改任何状态（Removed 卸载用它定位；
    // M4-02 起渲染端经 const AssetManager& 按 `.memat` 引用的 AssetId 解析 Handle）。
    [[nodiscard]] std::optional<AssetHandle<T>> TryFind(const AssetId& id) const
    {
        const auto found = m_lookup.find(id);
        if (found == m_lookup.end())
        {
            return std::nullopt;
        }
        const Slot& slot = m_slots[found->second];
        return AssetHandle<T>{found->second, slot.generation};
    }

    // 写入（或替换）槽位的 payload，并使修订号 +1。
    //
    // 句柄与世代保持不变：热重载因此对外表现为"同一个句柄、内容换了代"，
    // 消费者只需比较修订号即可察觉变化，无需重新解析 AssetId。
    //
    // 参数：
    //   handle —— 由 ResolveOrCreate 签发的有效句柄
    //   asset  —— 待写入的 CPU payload（移入）
    // 返回：写入成功为 true；句柄已失效（世代失配或未分配）为 false。
    // 失败：修订号已达 uint64 上限时记录 Fatal 日志并终止进程（ME_VERIFY）。
    [[nodiscard]] bool Commit(AssetHandle<T> handle, T asset)
    {
        Slot* slot = TryGetSlot(handle);
        if (slot == nullptr)
        {
            return false;
        }

        slot->asset.emplace(std::move(asset));
        ME_VERIFY(slot->revision != std::numeric_limits<std::uint64_t>::max(), "AssetPool revision exhausted");
        ++slot->revision;
        // 注意：revision 跨资产生命周期单调递增（Unload 不重置），
        // 保证 D3D11AssetCache 的"CPU revision > 上次上传 revision"
        // 比较在槽位复用后依然成立；热重载只随 Commit 递增。
        return true;
    }

    // 读取存活槽位的 payload 与修订号。
    //
    // 分配了槽位但尚未写入 payload 的空槽（BeginCommit 的建槽产物）视为未就绪，
    // 因此返回 nullopt 而不是空 payload——消费者不会看到半个资产。
    //
    // 参数：
    //   handle —— 待查询的句柄
    // 返回：句柄有效且 payload 已写入时为 View；否则为 nullopt。
    [[nodiscard]] std::optional<View> TryGet(const AssetHandle<T> handle) const
    {
        const Slot* slot = TryGetSlot(handle);
        if (slot == nullptr || !slot->asset)
        {
            return std::nullopt;
        }
        return View{&*slot->asset, slot->revision};
    }

    // 卸载槽位：清空 payload、移除 AssetId 映射、世代 +1，槽位下标进入空闲表待复用。
    //
    // 修订号刻意不重置：它必须跨生命周期单调，"CPU revision > 上次上传 revision"
    // 这一比较在槽位被另一个资产复用后依然成立。
    //
    // 参数：
    //   handle —— 待卸载的句柄
    // 返回：卸载成功为 true；句柄已失效为 false。
    // 失败：世代已达 uint32 上限时记录 Fatal 日志并终止进程（ME_VERIFY）。
    [[nodiscard]] bool Unload(const AssetHandle<T> handle)
    {
        Slot* slot = TryGetSlot(handle);
        if (slot == nullptr)
        {
            return false;
        }

        m_lookup.erase(slot->id);
        slot->id = AssetId{}; // 失效 slot 不残留旧 AssetId（审计 hygiene：避免误用）
        slot->asset.reset();
        slot->alive = false;
        ME_VERIFY(slot->generation != std::numeric_limits<std::uint32_t>::max(), "AssetPool generation exhausted");
        ++slot->generation;
        m_freeList.push_back(handle.Index());
        return true;
    }

    // 由 Handle 反查当前 AssetId（仅存活 slot 返回）；供 reload 交叉核对
    // "引用它的资产是否仍在新 Manifest Registry 中"（P1-1：阻止 removed-ref world 混过预检）。
    [[nodiscard]] const AssetId* TryGetAssetId(const AssetHandle<T> handle) const
    {
        const Slot* slot = TryGetSlot(handle);
        return slot != nullptr ? &slot->id : nullptr;
    }

  private:
    // 一个资产槽位。下标即句柄的 index，一旦分配便随 m_slots 稳定不变。
    struct Slot final
    {
        // 该槽位当前承载的 AssetId；未分配 / 已卸载时被清空为全零。
        AssetId id{};
        // CPU payload。ResolveOrCreate 建槽后为空（有槽无内容），Commit 后才写入。
        std::optional<T> asset;
        // 世代，初值为 1：0 被 AssetHandle 保留为"无效"，因此新槽位天然可用。
        // 只在 Unload 时递增，槽位复用不重置——这正是旧句柄失效的机制。
        std::uint32_t generation{1};
        // 修订号，只在 Commit 时递增，Unload 不重置（跨生命周期单调）。
        std::uint64_t revision{};
        // 槽位是否已分配；false 时该槽位位于空闲表（或尚未分配）。
        bool alive{};
    };

    // 取得一个可写入的槽位下标：优先从空闲表尾复用（LIFO），否则追加新槽位。
    //
    // 复用槽位不重置世代：该槽位的上一代已在 Unload 时递增过，
    // 因此新资产的句柄世代必然不同于任何仍在外面流传的旧句柄。
    //
    // 返回：可写入的槽位下标。
    // 失败：槽位下标已达 kInvalidIndex 时记录 Fatal 日志并终止进程（ME_VERIFY）。
    [[nodiscard]] std::uint32_t AcquireSlot()
    {
        if (!m_freeList.empty())
        {
            const std::uint32_t index = m_freeList.back();
            m_freeList.pop_back();
            return index;
        }

        ME_VERIFY(m_slots.size() < AssetHandle<T>::kInvalidIndex, "AssetPool slot index exhausted");
        const auto index = static_cast<std::uint32_t>(m_slots.size());
        m_slots.emplace_back();
        return index;
    }

    // 句柄 → 槽位（可变）。世代失配、槽位未分配或下标越界均返回 nullptr，
    // 这就是"失效句柄被拒绝"的全部判定，不需要额外的失效列表。
    [[nodiscard]] Slot* TryGetSlot(const AssetHandle<T> handle)
    {
        if (!handle.IsValid() || handle.Index() >= m_slots.size())
        {
            return nullptr;
        }
        Slot& slot = m_slots[handle.Index()];
        return slot.alive && slot.generation == handle.Generation() ? &slot : nullptr;
    }

    // 上一条的 const 版本，语义相同。
    [[nodiscard]] const Slot* TryGetSlot(const AssetHandle<T> handle) const
    {
        if (!handle.IsValid() || handle.Index() >= m_slots.size())
        {
            return nullptr;
        }
        const Slot& slot = m_slots[handle.Index()];
        return slot.alive && slot.generation == handle.Generation() ? &slot : nullptr;
    }

    // 槽位存储；下标一经分配即稳定，Unload 只清空内容不移除元素。
    std::vector<Slot> m_slots;
    // 空闲槽位下标栈（后进先出），供 AcquireSlot 复用。
    std::vector<std::uint32_t> m_freeList;
    // AssetId → 槽位下标的加速索引；只含已分配且存活的槽位。
    std::unordered_map<AssetId, std::uint32_t, AssetIdHasher> m_lookup;
};
} // namespace MiniEngine::Assets
