// ============================================================================
// DependencyRecord.cpp — 依赖排序、校验与构造
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 4 节
// ============================================================================
#include "DependencyRecord.h"

#include <algorithm>
#include <filesystem>

namespace MiniEngine::Tools
{
namespace
{
// 小写折叠只用于"大小写别名"检测，不用于 canonical path 本身：
// 磁盘上允许 Demo/A.gltf 与 demo/a.gltf 共存，但若它们被规范化到同一
// canonical path 就是冲突。
[[nodiscard]] std::string ToLowerAscii(const std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const char c : text)
    {
        result.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    }
    return result;
}
}  // namespace

void SortDependencies(std::vector<DependencyRecord>& records)
{
    std::sort(records.begin(), records.end(), [](const DependencyRecord& left, const DependencyRecord& right)
    {
        if (left.kind != right.kind)
        {
            return static_cast<std::uint8_t>(left.kind) < static_cast<std::uint8_t>(right.kind);
        }
        return left.normalizedPathOrName < right.normalizedPathOrName;
    });
}

bool ValidateDependencies(const std::vector<DependencyRecord>& records, std::string& error)
{
    error.clear();

    // 前提：调用方已 SortDependencies。此处再校验"已排序"，把契约钉死。
    for (std::size_t index = 1; index < records.size(); ++index)
    {
        const DependencyRecord& previous = records[index - 1];
        const DependencyRecord& current = records[index];

        if (previous.kind == current.kind)
        {
            if (previous.normalizedPathOrName == current.normalizedPathOrName)
            {
                error = "Duplicate dependency path: " + current.normalizedPathOrName;
                return false;
            }
            if (ToLowerAscii(previous.normalizedPathOrName) == ToLowerAscii(current.normalizedPathOrName))
            {
                error = "Case-alias dependency conflict: " + current.normalizedPathOrName;
                return false;
            }
        }

        const bool orderBroken =
            (static_cast<std::uint8_t>(previous.kind) > static_cast<std::uint8_t>(current.kind)) ||
            (previous.kind == current.kind && previous.normalizedPathOrName > current.normalizedPathOrName);
        if (orderBroken)
        {
            error = "Dependencies are not sorted; call SortDependencies before ValidateDependencies.";
            return false;
        }
    }

    return true;
}

DependencyRecord MakeToolDependency(const std::string_view name, const std::string_view version)
{
    Sha256Builder builder;
    Sha256Digest digest{};
    if (builder.IsReady())
    {
        (void)builder.AppendUtf8WithLength(name);
        (void)builder.AppendUtf8WithLength(version);
        (void)builder.Finish(digest);
    }

    DependencyRecord record;
    record.kind = DependencyKind::Tool;
    record.normalizedPathOrName = std::string{name};
    record.contentHash = digest;
    return record;
}

std::string NormalizeDependencyPath(const std::string_view path)
{
    // 统一 '/' 分隔符并词法归一化；纯字符串运算，不访问文件系统。
    return std::filesystem::path{path}.lexically_normal().generic_string();
}
}  // namespace MiniEngine::Tools