// ============================================================================
// ContentHash.h — 文件级 ContentHash（流式 SHA-256）
// 里程碑：M3-05
// 职责：把"文件字节"映射为 ContentHash。哈希算法本身由 Sha256.h 的
//       Sha256Builder（CNG/BCrypt，已落地并测试）提供，本文件只负责文件的流式
//       分块读取，不重写任何密码学代码。
// 关联：docs/architecture/README.md 第 2 节
// ============================================================================
#pragma once

#include "Sha256.h"

#include <filesystem>
#include <string>

namespace MiniEngine::Tools
{
// ContentHash(file) = SHA-256(file bytes)。
//
// 规则（M3-05 第 2 节）：
//   - binary mode；
//   - 流式读取固定 chunk，不把大文件一次性读入内存；
//   - 所有 chunk 必须送入同一个 Sha256Builder（禁止每块独立 hash 后再拼接 digest）；
//   - 读取过程中短读/文件被修改 → 返回 false，由上层 debounce/retry 处理；
//   - mtime/size 只用于"可能无需重读"的进程内优化，不决定 cache hit；
//   - 路径不进入 ContentHash，只进入 BuildKey record（避免绝对路径污染 key）。
//
// 参数：
//   path   —— 待哈希文件（应在 source-root 沙箱内解析后再传入）。
//   digest —— 成功时写入 32 字节 digest。
//   error  —— 失败时写入可读原因。
// 返回：成功 true；失败 false 且 error 非空。空文件视为成功。
[[nodiscard]] bool ComputeFileHash(const std::filesystem::path& path, Sha256Digest& digest, std::string& error);
}  // namespace MiniEngine::Tools
