#pragma once

// 7-B：Win32 目录 watcher（ReadDirectoryChangesW + OVERLAPPED + 事件对象等待）。
//
// 07 篇两级 Watch 的地基：事件只是 hint——buffer overflow / ERROR_NOTIFY_ENUM_DIR /
// 读失败时发 FullRescanRequired，消费方（Cooker rescan 或 Runtime Manifest diff）
// 必须以 rescan/hash/diff 为准，watcher 永远不是依赖真相。
//
// 线程模型：worker 线程只把事件塞进线程安全队列；消费者线程 PollEvents() 取走。
// watcher 线程禁止直接调用 Cooker / AssetManager / World / D3D11。
// public 头不含 Windows.h（PImpl，与 WindowsWindow 同模式）。

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace MiniEngine::Platform
{
struct DirectoryWatchEvent final
{
    // 相对监视目录的规范化路径（'/' 分隔、UTF-8）。rename 的 old/new 都作为 hint 入队，
    // 由消费方 dirty set 合并（07 篇 debounce）。
    std::vector<std::string> relativePaths;
    // buffer overflow（bytes==0 / ERROR_NOTIFY_ENUM_DIR）或读失败 → 必须 full rescan。
    bool fullRescanRequired{};
};

class DirectoryWatcherWin32 final
{
  public:
    // bufferSize 可注入以便测试 overflow 路径；默认 64 KiB，实际取 max(bufferSize, 4 KiB)。
    // 目录/事件对象创建失败时抛 std::system_error（与 WindowsClock 的失败语义一致）。
    explicit DirectoryWatcherWin32(const std::filesystem::path& directory, std::size_t bufferSize = 64 * 1024);

    ~DirectoryWatcherWin32();
    DirectoryWatcherWin32(const DirectoryWatcherWin32&) = delete;
    DirectoryWatcherWin32& operator=(const DirectoryWatcherWin32&) = delete;

    // 取走并清空事件队列（消费者线程调用，线程安全）。
    [[nodiscard]] std::vector<DirectoryWatchEvent> PollEvents();

    // 请求停止：唤醒 worker、CancelIoEx、drain、join；析构自动调用。
    // Stop 之后不再产生新事件；PollEvents 仍可取走余量。
    void Stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Platform
