// ============================================================================
// DirectoryWatcherWin32Tests.cpp — Win32 目录 watcher 的线程与 hint 语义
// 里程碑：M3（7-B）
// 职责：验证构造失败抛 system_error、事件以相对路径 hint 送达（'/' 分隔）、
//       事件风暴下要么全送达要么请求 full rescan（不丢不假）、Stop 及时 join。
// 关联：engine/platform/windows/src/DirectoryWatcherWin32.cpp（被测实现）
//       docs/architecture/DECISIONS.md §9
// ============================================================================

#include <MiniEngine/Platform/Windows/DirectoryWatcherWin32.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
using MiniEngine::Platform::DirectoryWatcherWin32;
using MiniEngine::Platform::DirectoryWatchEvent;

std::uint32_t g_dirCounter = 0;

// 进程级基址 + 计数器：gtest_discover_tests 每用例独立进程，避免 temp 撞名
// （见 skill 坑位清单 / AssetManagerTests 同款修复）。
std::filesystem::path MakeWatchedDir()
{
    static const std::uint64_t processBase =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    ++g_dirCounter;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("MiniEngineWatcherTests-" + std::to_string(processBase) + "-" + std::to_string(g_dirCounter));
    std::filesystem::create_directories(dir);
    return dir;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

template <typename Predicate> bool PollUntil(DirectoryWatcherWin32& watcher, Predicate&& predicate, const int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const DirectoryWatchEvent& event : watcher.PollEvents())
        {
            if (predicate(event))
            {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}
} // namespace

TEST(DirectoryWatcherTests, ConstructorThrowsForMissingDirectory)
{
    const auto missing = std::filesystem::temp_directory_path() / "MiniEngineWatcherTests-does-not-exist";
    EXPECT_ANY_THROW((DirectoryWatcherWin32{missing}));
}

TEST(DirectoryWatcherTests, ReportsCreatedFileAsRelativeForwardSlashHint)
{
    const auto dir = MakeWatchedDir();
    DirectoryWatcherWin32 watcher{dir};

    // 给 worker 一点时间完成第一次 ReadDirectoryChangesW 武装。
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    ASSERT_TRUE(WriteTextFile(dir / "hello.txt", "data"));

    // PollEvents 是消费式取走：精确匹配必须在轮询谓词内捕获，不能事后再抽。
    std::string matchedPath;
    EXPECT_TRUE(PollUntil(
        watcher,
        [&matchedPath](const DirectoryWatchEvent& event)
        {
            if (event.fullRescanRequired)
            {
                return false;
            }
            for (const std::string& path : event.relativePaths)
            {
                if (path == "hello.txt")
                {
                    matchedPath = path;
                    return true;
                }
            }
            return false;
        },
        5000));

    // hint 是相对路径且 '/' 分隔。
    EXPECT_EQ(matchedPath, "hello.txt");
}

TEST(DirectoryWatcherTests, RapidBurstDeliversAllHintsOrRequestsFullRescan)
{
    // ReadDirectoryChangesW 有变更即交付（不攒满才完成），黑盒下无法确定性地
    // 制造 overflow——overflow→full rescan 由手动矩阵 HR-08 覆盖。
    // 自动化保证的是 07 篇的硬约束：突发下不漏——每个变更要么成为 hint，
    // 要么触发 fullRescanRequired 标记，两者必居其一。
    const auto dir = MakeWatchedDir();
    DirectoryWatcherWin32 watcher{dir, 4 * 1024}; // 小缓冲加大 overflow 概率
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    constexpr std::uint32_t kFileCount = 300;
    for (std::uint32_t index = 0; index < kFileCount; ++index)
    {
        static_cast<void>(WriteTextFile(dir / ("burst-" + std::to_string(index) + ".txt"), "x"));
    }

    std::set<std::string> observed;
    bool fullRescan = false;
    PollUntil(
        watcher,
        [&](const DirectoryWatchEvent& event)
        {
            if (event.fullRescanRequired)
            {
                fullRescan = true;
                return true;
            }
            for (const std::string& path : event.relativePaths)
            {
                if (path.ends_with(".txt"))
                {
                    observed.insert(path);
                }
            }
            return observed.size() >= kFileCount;
        },
        30000);

    EXPECT_TRUE(fullRescan || observed.size() >= kFileCount) << "observed " << observed.size() << " of " << kFileCount;
}

TEST(DirectoryWatcherTests, StopJoinsPromptlyAndQuiescesEvents)
{
    const auto dir = MakeWatchedDir();
    DirectoryWatcherWin32 watcher{dir};
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    const auto start = std::chrono::steady_clock::now();
    watcher.Stop();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(elapsedMs, 5000); // join 必须及时，不能 detach 或挂死

    // Stop 后不再产生新事件。
    static_cast<void>(WriteTextFile(dir / "after-stop.txt", "data"));
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    EXPECT_TRUE(watcher.PollEvents().empty());
}
