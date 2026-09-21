// ============================================================================
// AssetHandle.h — 进程内的带世代资产句柄
// 里程碑：M3
// 职责：定义 AssetHandle<T>（槽位下标 + 世代）及其哈希器。句柄是进程内身份，绝不落盘：
//       磁盘与 Manifest 只用 AssetId。世代让"资产已卸载但句柄仍被持有"可被判定
//       （失效句柄）——槽位复用后旧句柄的世代失配，从而自动失效。
// 关联：docs/architecture/DECISIONS.md §3
//       engine/assets/include/MiniEngine/Assets/AssetPool.h（句柄的唯一签发方）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace MiniEngine::Assets
{
// 进程内资产句柄：资产槽位下标 + 世代（generation）。
//
// 模板参数 T 仅用于类型区分（AssetHandle<MeshAsset> 与 AssetHandle<TextureAsset>
// 不可互换），不承载运行时信息，因此句柄是平凡可拷贝的值类型。
template <typename T> class AssetHandle final
{
  public:
    // 句柄指向的资产类型，供外部在泛型代码中取回 T。
    using AssetType = T;

    // 无效槽位下标：取 uint32 最大值，与任何真实分配的槽位都不冲突。
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    // 默认构造无效句柄（index = kInvalidIndex，generation = 0）。
    constexpr AssetHandle() noexcept = default;

    // 由槽位下标与世代直接组装句柄，供 AssetPool 签发。
    //
    // 参数：
    //   index      —— 槽位下标，由 AssetPool 分配；kInvalidIndex 表示无效
    //   generation —— 槽位世代，槽位每次卸载递增；0 保留给无效句柄
    constexpr AssetHandle(const std::uint32_t index, const std::uint32_t generation) noexcept
        : m_index(index), m_generation(generation)
    {
    }

    // 判断句柄在形式上是否有效。
    //
    // 仅检查 index / generation 的取值，不查询 AssetPool：槽位被卸载再复用后，
    // 旧句柄的形式依然有效，但世代已与池内不符，须由 AssetPool 侧判定为失效句柄。
    //
    // 返回：index 不为 kInvalidIndex 且 generation 不为 0 时为 true。
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_index != kInvalidIndex && m_generation != 0;
    }

    // 槽位下标，供 AssetPool 索引内部存储；无效句柄为 kInvalidIndex。
    [[nodiscard]] constexpr std::uint32_t Index() const noexcept
    {
        return m_index;
    }

    // 世代：槽位每卸载一次递增，用于区分同一槽位上的前后两代资产。
    [[nodiscard]] constexpr std::uint32_t Generation() const noexcept
    {
        return m_generation;
    }

    // 相等比较：index 与 generation 同时相等才相等（默认实现）。
    // 旧句柄世代失配即不等，因此不会被误判为当前槽位的资产。
    friend constexpr bool operator==(AssetHandle, AssetHandle) = default;

  private:
    std::uint32_t m_index{kInvalidIndex};
    std::uint32_t m_generation{0};
};

// AssetHandle<T> 的哈希表哈希器，供以句柄为键的进程内容器（如 D3D11AssetCache）使用。
//
// 混合 index 与 generation：槽位复用后旧句柄的哈希值随之改变，
// 使失效句柄不会命中为新资产建立的条目。
template <typename T> struct AssetHandleHasher final
{
    [[nodiscard]] constexpr std::size_t operator()(const AssetHandle<T> handle) const noexcept
    {
        std::size_t result = static_cast<std::size_t>(handle.Index());
        result ^= static_cast<std::size_t>(handle.Generation()) + static_cast<std::size_t>(0x9E3779B9U) +
                  (result << 6U) + (result >> 2U);
        return result;
    }
};

// 守护契约：句柄按值跨模块传递，且被直接用作 map 键，必须是平凡可拷贝的值类型。
static_assert(std::is_trivially_copyable_v<AssetHandle<int>>);
} // namespace MiniEngine::Assets