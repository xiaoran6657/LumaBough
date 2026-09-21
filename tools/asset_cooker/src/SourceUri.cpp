// ============================================================================
// SourceUri.cpp — glTF URI 沙箱解析的实现
// 里程碑：M3（02 篇 G1）
// 职责：实现 ResolveSourceUri 的七步校验（空 URI → scheme/绝对路径/反斜杠 →
//       一次 percent-decode → 组件级包含检查 → reparse point 链 → 存在性与
//       常规文件 → 输出规范相对路径），任一步失败即拒绝并给出原因枚举。
//       Windows API 只在本文件与 WicImage 使用。
// 关联：tools/asset_cooker/src/SourceUri.h（允许/拒绝清单与验证顺序）
//       docs/architecture/README.md 第 5 节
// ============================================================================

#include "SourceUri.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cctype>
#include <optional>
#include <vector>

namespace MiniEngine::Tools
{
namespace
{
int HexDigitValue(const char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

// 只 percent-decode 一次；非法编码与解码出的控制字符（含 NUL）明确失败，
// 避免 "%2e%2e%2f" 之类的双重编码绕过。
std::optional<std::string> PercentDecodeOnce(const std::string_view text)
{
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const char c = text[index];
        if (c != '%')
        {
            decoded.push_back(c);
            continue;
        }
        if (index + 2 >= text.size())
        {
            return std::nullopt;
        }
        const int high = HexDigitValue(text[index + 1]);
        const int low = HexDigitValue(text[index + 2]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        const auto value = static_cast<char>(high * 16 + low);
        if (static_cast<unsigned char>(value) < 0x20U)
        {
            return std::nullopt;
        }
        decoded.push_back(value);
        index += 2;
    }
    return decoded;
}

bool HasUnsupportedScheme(const std::string_view uri) noexcept
{
    if (const auto schemeEnd = uri.find("://"); schemeEnd != std::string_view::npos)
    {
        return true;
    }
    // 无 "//" 的 scheme 形式（如 file:/path、mailto:x）只接受 data:。
    const auto colon = uri.find(':');
    if (colon == std::string_view::npos)
    {
        return false;
    }
    const std::string_view prefix = uri.substr(0, colon);
    return prefix != "data";
}

bool HasRootNameOrDrive(const std::string_view uri) noexcept
{
    // Windows 盘符 "C:" 或 UNC "\\\\server\\share"，以及以 '/'、'\\' 开头的绝对路径。
    if (uri.size() >= 2 && uri[1] == ':' && std::isalpha(static_cast<unsigned char>(uri[0])))
    {
        return true;
    }
    const char first = uri.empty() ? '\0' : uri.front();
    return first == '/' || first == '\\';
}

// 按路径组件比较包含关系：不能用字符串前缀，否则 <root>2 会被误判为 <root> 的子目录。
// MSVC 的 path 比较在 Windows 上按本地规则（大小写不敏感）。
bool IsInsideByComponents(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt)
    {
        if (candidateIt == candidate.end() || *candidateIt != *rootIt)
        {
            return false;
        }
    }
    return true;
}

// 路径链上任一组件是 reparse point（symlink/junction）即拒绝：
// 同一依赖可能通过别名逃逸，或产生不稳定身份。
bool ChainHasReparsePoint(const std::filesystem::path& root, const std::filesystem::path& target)
{
    const std::filesystem::path relative = target.lexically_relative(root);
    std::filesystem::path current = root;
    for (const auto& component : relative)
    {
        if (component.empty() || component == "." || component == "..")
        {
            continue;
        }
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return false; // 不存在由后续 MissingFile 判定
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool Fail(const UriRejectReason reason, const std::string_view message,
                        UriRejectReason& outReason, std::string& error)
{
    outReason = reason;
    error = std::string(message);
    return false;
}
} // namespace

const char* UriRejectReasonName(const UriRejectReason reason) noexcept
{
    switch (reason)
    {
        case UriRejectReason::Ok:
            return "Ok";
        case UriRejectReason::EmptyUri:
            return "EmptyUri";
        case UriRejectReason::InvalidPercentEncoding:
            return "InvalidPercentEncoding";
        case UriRejectReason::DecodedControlCharacter:
            return "DecodedControlCharacter";
        case UriRejectReason::UnsupportedScheme:
            return "UnsupportedScheme";
        case UriRejectReason::AbsolutePath:
            return "AbsolutePath";
        case UriRejectReason::RootNameOrDrive:
            return "RootNameOrDrive";
        case UriRejectReason::BackslashSeparator:
            return "BackslashSeparator";
        case UriRejectReason::EscapesSourceRoot:
            return "EscapesSourceRoot";
        case UriRejectReason::MissingFile:
            return "MissingFile";
        case UriRejectReason::NotARegularFile:
            return "NotARegularFile";
        case UriRejectReason::ReparsePoint:
            return "ReparsePoint";
    }
    return "Unknown";
}

bool IsGlbDataUri(const std::string_view uri) noexcept
{
    return uri.starts_with("data:");
}

bool ResolveSourceUri(const std::string_view uri,
                      const std::filesystem::path& sourceDirectory,
                      const std::filesystem::path& sourceRoot,
                      ResolvedSourceUri& resolved,
                      UriRejectReason& reason,
                      std::string& error)
{
    reason = UriRejectReason::Ok;
    error.clear();

    // 1. 空 URI
    if (uri.empty())
    {
        return Fail(UriRejectReason::EmptyUri, "empty URI", reason, error);
    }

    // Data URI：不解析文件系统，内容由 source glTF hash 覆盖。
    if (IsGlbDataUri(uri))
    {
        resolved = ResolvedSourceUri{};
        resolved.isDataUri = true;
        return true;
    }

    // 2. 拒绝 scheme、root name、盘符、绝对路径、'\' 分隔符
    if (HasUnsupportedScheme(uri))
    {
        return Fail(UriRejectReason::UnsupportedScheme, "URI scheme is not allowed: " + std::string(uri), reason,
                    error);
    }
    if (HasRootNameOrDrive(uri))
    {
        return Fail(UriRejectReason::RootNameOrDrive, "absolute path or drive letter is not allowed: " +
                                                          std::string(uri),
                    reason, error);
    }
    if (uri.find('\\') != std::string_view::npos)
    {
        return Fail(UriRejectReason::BackslashSeparator,
                    "backslash separator is not allowed in glTF URI: " + std::string(uri), reason, error);
    }

    // 3. percent decode 一次后组合并 lexical normalize
    const std::optional<std::string> decoded = PercentDecodeOnce(uri);
    if (!decoded.has_value())
    {
        return Fail(UriRejectReason::InvalidPercentEncoding, "invalid percent encoding: " + std::string(uri), reason,
                    error);
    }
    if (decoded->empty())
    {
        return Fail(UriRejectReason::EmptyUri, "URI decodes to an empty path", reason, error);
    }

    const std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(sourceRoot);
    const std::filesystem::path combined = (sourceDirectory / *decoded).lexically_normal();
    const std::filesystem::path canonicalTarget = std::filesystem::weakly_canonical(combined);

    // 4. 按路径组件确认仍在 source root 内
    if (!IsInsideByComponents(normalizedRoot, canonicalTarget) || canonicalTarget == normalizedRoot)
    {
        return Fail(UriRejectReason::EscapesSourceRoot,
                    "URI escapes source root after normalization: " + std::string(uri), reason, error);
    }

    // 5. 路径链上的 reparse point
    if (ChainHasReparsePoint(normalizedRoot, canonicalTarget))
    {
        return Fail(UriRejectReason::ReparsePoint,
                    "URI path chain contains a reparse point: " + std::string(uri), reason, error);
    }

    // 6. 必须存在且是普通文件（不是目录、不是设备）
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::status(canonicalTarget, statusError);
    if (statusError || !std::filesystem::exists(status))
    {
        return Fail(UriRejectReason::MissingFile, "URI target does not exist: " + std::string(uri), reason, error);
    }
    if (!std::filesystem::is_regular_file(status))
    {
        return Fail(UriRejectReason::NotARegularFile, "URI target is not a regular file: " + std::string(uri), reason,
                    error);
    }

    // 7. 输出规范化相对路径（'/' 分隔），供依赖清单使用
    resolved = ResolvedSourceUri{};
    resolved.absolutePath = canonicalTarget;
    resolved.normalizedRelativePath = canonicalTarget.lexically_relative(normalizedRoot).generic_string();
    resolved.isDataUri = false;
    return true;
}
}  // namespace MiniEngine::Tools
