// ============================================================================
// CookedFileSource.cpp — 生产 I/O 来源：路径沙箱 + 尺寸上限 + 阻塞读（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：见 CookedFileSource.h。读取顺序固定为"沙箱校验 → 尺寸探测 → 上限判定 →
//       分配并读入"，保证超大文件在分配缓冲之前就被拒绝（SizeExceeded 路径不分配）。
// 线程：只在 I/O 线程调用（阻塞）。
// 关联：engine/assets/src/AssetRegistry.cpp（IsValidArtifactRelativePath 同规则）
//       engine/core/src/FileSystem.cpp（ReadBinaryFile 唯一文件读取实现）
// ============================================================================

#include <MiniEngine/Assets/CookedFileSource.h>

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Core/FileSystem.h>
#include <MiniEngine/Core/Log.h>

#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace MiniEngine::Assets
{
namespace
{
// 与 Registry 的路径沙箱同一规则：相对路径、无盘符、无反斜杠、无空段与 "."/".." 段。
// 运行时禁止读取 glTF source（只允许 cooked 产物相对路径）。
[[nodiscard]] bool IsSafeRelativePath(const std::string& path) noexcept
{
    if (path.empty() || path.front() == '/' || path.front() == '\\')
    {
        return false;
    }
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
    {
        return false;
    }

    std::size_t begin = 0;
    while (begin <= path.size())
    {
        const std::size_t end = path.find('/', begin);
        const std::string_view segment(path.data() + begin, (end == std::string::npos ? path.size() : end) - begin);
        if (segment.empty() || segment == "." || segment == "..")
        {
            return false;
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return true;
}
} // namespace

DirectoryCookedFileSource::DirectoryCookedFileSource(std::filesystem::path outputRoot) : m_root(std::move(outputRoot))
{
    ME_VERIFY(!m_root.empty(), "cooked file source requires an output root");
}

CookedFileRead DirectoryCookedFileSource::Read(const std::string& cookedRelativePath, const std::size_t maxBytes)
{
    CookedFileRead result;
    if (!IsSafeRelativePath(cookedRelativePath))
    {
        result.error = AssetLoadErrorCode::PathRejected;
        result.errorText = "artifact path rejected by sandbox";
        return result;
    }

    const std::filesystem::path full = m_root / std::filesystem::path(cookedRelativePath);

    // 尺寸探测在分配之前：超大产物不进入读缓冲（"分配大 buffer 前验证"）。
    std::error_code sizeError;
    const std::uintmax_t fileSize = std::filesystem::file_size(full, sizeError);
    if (sizeError)
    {
        result.error = AssetLoadErrorCode::FileMissing;
        result.errorText = "artifact is missing or unreadable: " + cookedRelativePath;
        return result;
    }
    if (fileSize == 0)
    {
        result.error = AssetLoadErrorCode::FileEmpty;
        result.errorText = "artifact is empty: " + cookedRelativePath;
        return result;
    }
    if (fileSize > static_cast<std::uintmax_t>(maxBytes))
    {
        result.error = AssetLoadErrorCode::AssetTooLarge;
        result.errorText = "artifact exceeds the size limit: " + cookedRelativePath;
        return result;
    }

    BinaryFileResult read = ReadBinaryFile(full);
    if (!read.Succeeded())
    {
        result.error = AssetLoadErrorCode::ReadFailed;
        result.errorText = "read failed: " + cookedRelativePath;
        return result;
    }
    if (read.bytes.size() != static_cast<std::size_t>(fileSize))
    {
        // short read / 读中被截断：不能把半份产物当成功。
        result.error = AssetLoadErrorCode::ReadFailed;
        result.errorText = "short read: " + cookedRelativePath;
        return result;
    }

    result.bytes = std::move(read.bytes);
    return result;
}
} // namespace MiniEngine::Assets
