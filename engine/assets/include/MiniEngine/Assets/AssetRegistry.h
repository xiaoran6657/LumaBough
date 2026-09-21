#pragma once

// Registry 的类型与解析契约：M3-02 提供 BuildKey/ContentHash/DependencyRecord 等
// value types；M3-07（7-A）在同一文件落地 Manifest 严格解析（ParseAndValidate）、
// AssetId 域分隔派生（DeriveAssetId）与确定性排序的 entry 访问。
// Manifest 由 Cooker 发布（schemaVersion=1，字段固定），runtime 拒绝任何
// 未知名、错误类型与不安全路径——Registry 是 Manifest diff 的事实地基。
#include <MiniEngine/Assets/AssetId.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace MiniEngine::Assets
{
// 资产种类：决定消费路径——Mesh / Texture / Material 进入对应的类型化池，World 由
// WorldLoader 两遍实例化（不落池）。Manifest 里的 kind 字符串与这里一一对应。
// Material 为 M4-02 新增（`.memat`，kind 字符串 "material"）。
enum class AssetKind
{
    Mesh,
    Texture,
    World,
    Material
};

// 构建身份哈希：由"构建输入"派生——导入器版本、Recipe 内容、Profile 与格式版本等。
// 与 ContentHash 的分工：ContentHash 描述"输入内容的字节"，BuildKey 描述
// "用哪套规则把内容变成产物"；二者共同决定 artifact 的内容寻址路径与缓存命中判定。
struct BuildKey final
{
    std::array<std::byte, 32> bytes{};
    friend bool operator==(const BuildKey&, const BuildKey&) = default;
};

// 内容哈希：对 artifact 字节做流式 SHA-256 的结果。
// Manifest 记录它，运行时重算比对，保证"读到的产物就是 Cooker 发布的那一份"。
struct ContentHash final
{
    std::array<std::byte, 32> bytes{};
    friend bool operator==(const ContentHash&, const ContentHash&) = default;
};

// 一条构建依赖：相对路径 + 内容哈希 + 种类标签。
// Cooker 侧排序后参与 BuildKey 计算；运行时只解析与持有本类型，
// 用于失效诊断（路径 / 哈希对照）而非自动重建。
struct DependencyRecord final
{
    std::string relativePath;
    ContentHash contentHash;
    std::string kind;
};

// Manifest 中一个资产条目的解析结果。
struct RegistryEntry final
{
    // 逻辑身份：由 canonicalUri 派生（见 DeriveAssetId）。
    AssetId id;
    AssetKind kind{};
    // 规范 URI：身份的唯一来源；大小写 / 分隔符差异都视为不同资产。
    std::string canonicalUri;
    BuildKey buildKey;
    // 产物字节的哈希，运行时重算比对。
    ContentHash artifactHash;
    // Manifest 声明的产物字节数（M7-06 起保留：异步加载用它做读取前的字节预算与
    // 尺寸上限判定，读取后与实际字节数交叉核对）。BakedReader 的 header.fileSize
    // 仍是产物内部尺寸的权威，两者不一致即畸形产物。
    std::uint64_t fileSize = 0;
    // 相对 outputRoot 的产物路径；解析期已通过路径沙箱校验（拒绝绝对路径、
    // '\'、':'、'.'、'..' 与空段），读取时才与 outputRoot 拼接。
    std::filesystem::path artifactRelativePath;
    // 构建依赖清单。M3 的运行时 Manifest 解析器不接受 dependencies 键，
    // Cooker 也不把依赖写入 Manifest，因此本字段当前恒为空。
    std::vector<DependencyRecord> dependencies;
};

// AssetId = SHA-256("MiniEngine/AssetId/v1" '\0' canonicalUri) 截断 16 字节。
// 内容变化不改变身份；URI 变化即新资产。'\0' 分隔防止 "a/b"+"c" 与 "a"+"b/c" 同 ID。
[[nodiscard]] AssetId DeriveAssetId(const std::string& canonicalUri);

// 某一时刻 Manifest 的只读快照：一组确定性排序的条目 + AssetId 索引。
//
// 实例不可变：未知 key、缺 key、重复 key、坏 hex、坏 kind、路径逃逸等全部
// 在 ParseAndValidate 内拒绝，因此持有 const AssetRegistry* 的代码可以放心读取，
// 无需再做防御。Registry 由 AssetManager 在帧边界整体替换，不存在就地修改。
class AssetRegistry final
{
  public:
    // 严格解析 manifest.json 字节：未知名/缺名/坏 hex/坏 kind/不安全路径均失败。
    // 解析到临时实例并完整验证后，由 AssetManager 在帧边界整体 swap。
    [[nodiscard]] static std::optional<AssetRegistry> ParseAndValidate(std::span<const std::byte> manifestBytes,
                                                                       const std::filesystem::path& outputRoot,
                                                                       std::string& error);

    // 按 AssetId 查找条目。
    //
    // 参数：
    //   id —— 逻辑身份
    // 返回：找到为条目指针；不存在为 nullptr。
    [[nodiscard]] const RegistryEntry* Find(AssetId id) const noexcept;

    // entries 按 AssetId 字节序排序：Manifest diff 与事件顺序的 deterministic 基础。
    [[nodiscard]] std::size_t EntryCount() const noexcept;

    // 按排序后的位置取条目，供调用方按确定性顺序遍历（diff 顺序即由此保证）。
    //
    // 参数：
    //   index —— 条目下标，取值 [0, EntryCount())
    // 返回：合法下标为条目指针；越界为 nullptr。
    [[nodiscard]] const RegistryEntry* EntryAt(std::size_t index) const noexcept;

  private:
    // 仅由 ParseAndValidate 在全部校验通过后调用；外部无法组装出未校验的 Registry。
    explicit AssetRegistry(std::vector<RegistryEntry> entries);

    // 条目存储区；顺序由 ParseAndValidate 返回前锁定（AssetId 字节序），构造后不变。
    std::vector<RegistryEntry> m_entries;
    // AssetId → m_entries 下标的加速索引，与 m_entries 一一对应。
    std::unordered_map<AssetId, std::size_t, AssetIdHasher> m_byId;
};
} // namespace MiniEngine::Assets
