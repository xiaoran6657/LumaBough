// ============================================================================
// AssetId.h — 内容无关的逻辑资产身份（128 位）
// 里程碑：M3
// 职责：定义 AssetId 值类型及其十六进制编解码与哈希表哈希器。AssetId 由规范 URI
//       经 DeriveAssetId 派生，与内容无关：内容变化不改变身份，URI 变化才是新资产。
//       它是 Manifest 条目、资产池查找键与 .meworld 内部引用的统一身份口径。
// 关联：docs/architecture/DECISIONS.md §3
//       engine/assets/src/AssetRegistry.cpp（DeriveAssetId 的域分隔派生实现）
// ============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace MiniEngine::Assets
{
// 资产的逻辑身份：SHA-256(domain '\0' canonicalUri) 的前 16 字节（128 位）。
//
// 全零值保留为"无身份"，由 IsValid 判定；值语义（可拷贝、可按字节比较），
// 可直接作为 map / unordered_map 的键。
struct AssetId final
{
    // 16 字节身份；全零表示未派生 / 已失效（见 IsValid）。
    std::array<std::byte, 16> bytes{};

    // 判断身份是否有效（即是否非全零）。
    //
    // 全零 AssetId 不可能由 DeriveAssetId 从有效 URI 派生，因此被当作"无身份"：
    // 默认构造的 AssetId 与资产池卸载后被清空的槽位 id 都用它表达空值。
    //
    // 返回：存在任一非零字节为 true；全零为 false。
    [[nodiscard]] bool IsValid() const noexcept;

    // 编码为 32 字符小写十六进制文本。
    //
    // 面向日志、诊断与测试断言；不是持久化格式（磁盘上 .meworld 直接存 16 字节二进制）。
    //
    // 返回：32 字符小写十六进制字符串。
    [[nodiscard]] std::string ToHexString() const;

    // 解析十六进制文本为 AssetId。
    //
    // 只接受恰好 32 个字符的输入，每两个字符按十六进制解码一字节；
    // 解析结果若为全零仍视为无效并返回 nullopt，以维持"全零即无身份"的不变式。
    //
    // 参数：
    //   text —— 待解析文本，长度必须恰好为 32
    // 返回：合法且非全零时为 AssetId；长度不符、含非法字符或结果为全零时为 nullopt。
    static std::optional<AssetId> ParseHex(std::string_view text);

    friend bool operator==(const AssetId&, const AssetId&) = default;
};

// AssetId 的哈希表哈希器，供进程内 unordered_map / unordered_set 使用。
//
// 只影响进程内容器的分布，不参与任何持久化或安全语义，因此修改实现不会破坏磁盘格式。
struct AssetIdHasher final
{
    [[nodiscard]] std::size_t operator()(const AssetId& id) const noexcept;
};
} // namespace MiniEngine::Assets