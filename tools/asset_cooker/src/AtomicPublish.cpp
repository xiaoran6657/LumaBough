// ============================================================================
// AtomicPublish.cpp — 原子发布的 Win32 实现
// 里程碑：M3-05
// 职责：以 MoveFileExW 完成同目录 rename 发布；两个公开入口只差一个
//       MOVEFILE_REPLACE_EXISTING 旗标（artifact 不可变 vs Manifest 快照）。
// 关联：docs/architecture/DECISIONS.md §2
//       Microsoft Learn：MoveFileExW（winbase.h，REPLACE_EXISTING / WRITE_THROUGH 语义）
// ============================================================================

#include "AtomicPublish.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <system_error>

namespace MiniEngine::Tools
{
namespace
{
// 同目录 rename 发布的公共实现：
//   1. 建目标目录；
//   2. 校验 staging 是常规文件；
//   3. 校验 staging 与目标在同一目录——MoveFileExW 的原子性只在同一卷内成立，
//      跨卷会退化为"复制 + 删除"，目标文件会短暂缺失或写一半，因此直接拒绝；
//   4. MoveFileExW 发布。MOVEFILE_WRITE_THROUGH 确保数据真正落盘后函数才返回。
// 任一步失败都写 error 返回 false，目标保持原样（不会出现半发布的文件）。
bool MovePublishedFile(
    const std::filesystem::path& stagedFile,
    const std::filesystem::path& destination,
    const bool replaceExisting,
    std::string& error)
{
    error.clear();
    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError)
    {
        error = "Failed to create destination directory: " + fileError.message();
        return false;
    }

    const bool stagedIsRegularFile = std::filesystem::is_regular_file(stagedFile, fileError);
    if (fileError)
    {
        error = "Failed to inspect staged file: " + fileError.message();
        return false;
    }
    if (!stagedIsRegularFile)
    {
        error = "Staged path is not a regular file.";
        return false;
    }

    const bool sameDirectory =
        std::filesystem::equivalent(stagedFile.parent_path(), destination.parent_path(), fileError);
    if (fileError || !sameDirectory)
    {
        error = "Staged and destination files must be in the same existing directory.";
        return false;
    }

    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(
            stagedFile.c_str(),
            destination.c_str(),
            flags))
    {
        const std::error_code windowsError(
            static_cast<int>(GetLastError()),
            std::system_category());
        error = "MoveFileExW failed: " + windowsError.message();
        return false;
    }
    return true;
}
}

bool PublishImmutableFile(
    const std::filesystem::path& stagedFile,
    const std::filesystem::path& destination,
    std::string& error)
{
    return MovePublishedFile(stagedFile, destination, false, error);
}

bool ReplaceSnapshotFile(
    const std::filesystem::path& stagedFile,
    const std::filesystem::path& destination,
    std::string& error)
{
    return MovePublishedFile(stagedFile, destination, true, error);
}
}
