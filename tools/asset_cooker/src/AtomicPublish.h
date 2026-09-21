// ============================================================================
// AtomicPublish.h — 产物 / Manifest 的原子发布原语
// 里程碑：M3-05
// 职责：把 staging 文件一次性发布为最终路径（同目录 rename），保证运行时永远
//       读不到"写了一半"的文件。两个入口的语义差异见各自上方的既有注释。
// 关联：docs/architecture/DECISIONS.md §2
//       docs/architecture/README.md（发布顺序契约）
// ============================================================================

#pragma once

#include <filesystem>
#include <string>

namespace MiniEngine::Tools
{
// Artifact destinations are immutable. If destination already exists, the
// caller must verify identical bytes and discard staging instead of replacing.
[[nodiscard]] bool PublishImmutableFile(
    const std::filesystem::path& stagedFile,
    const std::filesystem::path& destination,
    std::string& error);

// Coherent snapshots such as manifest.json may replace the previous snapshot.
[[nodiscard]] bool ReplaceSnapshotFile(
    const std::filesystem::path& stagedFile,
    const std::filesystem::path& destination,
    std::string& error);
}
