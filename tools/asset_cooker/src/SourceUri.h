#pragma once

// G1：glTF URI 安全边界（02 篇第 5 节）。
// 允许：相对 source 目录的 URI、percent-decode 后仍在 source root 内的路径、
//       glTF Data URI、GLB 内嵌 bufferView（由 adapter 处理，不经本文件）。
// 拒绝：scheme（http/https/file/自定义）、绝对路径/UNC/盘符、'\' 分隔符、
//       规范化后逃出 source root 的 '..'、空文件、目录、reparse point 别名、
//       非法 percent 编码与解码出的控制字符。
// 验证顺序严格按文档：先 decode → 再拒 scheme/absolute → 组合 + lexical
// normalize → 按路径组件确认包含（不用字符串前缀，避免 source2 误判）→
// 拒绝路径链上的 reparse point → 确认普通文件 → 输出规范化相对路径。
// 绝不在判断是否越界之前打开文件。

#include <filesystem>
#include <string>
#include <string_view>

namespace MiniEngine::Tools
{
enum class UriRejectReason
{
    Ok,
    EmptyUri,
    InvalidPercentEncoding,
    DecodedControlCharacter,
    UnsupportedScheme,
    AbsolutePath,
    RootNameOrDrive,
    BackslashSeparator,
    EscapesSourceRoot,
    MissingFile,
    NotARegularFile,
    ReparsePoint
};

// 一次成功解析的产物：沙箱内的绝对路径 + 规范相对路径 + 是否 Data URI。
struct ResolvedSourceUri final
{
    std::filesystem::path absolutePath;
    // 规范化后相对 source root 的 '/' 分隔路径：写入依赖清单的规范形式。
    std::string normalizedRelativePath;
    // Data URI 不做文件系统解析；其内容仍由 source glTF 的 hash 覆盖。
    bool isDataUri{};
};

// 把拒绝原因枚举转为稳定可读的短名（用于日志与 exit code 诊断），不会返回 nullptr。
[[nodiscard]] const char* UriRejectReasonName(UriRejectReason reason) noexcept;

// Data URI 只允许 glTF 定义的 data: 前缀；由 adapter 解码，本函数仅识别。
[[nodiscard]] bool IsGlbDataUri(std::string_view uri) noexcept;

// 把 glTF 内引用的 URI 解析为 source-root 沙箱内的规范文件（校验顺序见上方文件说明）。
//
// 参数：
//   uri             —— glTF 中出现的原始 URI（可能带 percent 编码）
//   sourceDirectory —— 引用方文件所在目录，用于解析相对 URI
//   sourceRoot      —— 沙箱根；解析结果必须落在其中
//   resolved        —— 成功时写入解析结果；失败时被清空
//   reason          —— 失败时写入拒绝原因枚举
//   error           —— 失败时写入可读原因
// 返回：成功为 true；命中任一拒绝条件为 false。
[[nodiscard]] bool ResolveSourceUri(std::string_view uri,
                                    const std::filesystem::path& sourceDirectory,
                                    const std::filesystem::path& sourceRoot,
                                    ResolvedSourceUri& resolved,
                                    UriRejectReason& reason,
                                    std::string& error);
}
