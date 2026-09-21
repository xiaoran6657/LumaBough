// ============================================================================
// DirectoryWatcherWin32.cpp — Win32 目录 watcher 的 worker 线程实现
// 里程碑：M3（7-B）
// 职责：以 ReadDirectoryChangesW + OVERLAPPED + 事件对象等待驱动 worker 循环：
//       解析 FILE_NOTIFY_INFORMATION 链、UTF-16→UTF-8 与 '\'→'/' 规范化，
//       把事件作为 hint 入队；overflow / bytes==0 / 读失败统一归一为
//       FullRescanRequired。退出序列 CancelIoEx → drain → 关句柄 → join。
//       Windows 类型只存在于本文件（public 头 PImpl 隔离）。
// 关联：docs/architecture/DECISIONS.md §9
//       engine/platform/windows/include/MiniEngine/Platform/Windows/DirectoryWatcherWin32.h
// ============================================================================

#include <MiniEngine/Platform/Windows/DirectoryWatcherWin32.h>

// WIN32_LEAN_AND_MEAN/UNICODE/NOMINMAX 由 target_compile_definitions 统一提供，
// 源文件不得重复定义（C4005 → /WX）。Windows 类型只存在于本文件。
#include <Windows.h>

#include <algorithm>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace MiniEngine::Platform
{
namespace
{
// 监视的事件类别（Microsoft Learn：ReadDirectoryChangesW）：
//   FILE_NAME —— 文件改名/创建/删除；DIR_NAME —— 子目录改名/创建/删除；
//   SIZE —— 文件大小变化；LAST_WRITE —— 最后写入时间变化（仅在落盘后检测）。
// 只订阅与"源文件内容变了"相关的类别，过滤无关噪音。
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_LAST_WRITE;

// 通知缓冲下限（字节）：过小的缓冲会让 overflow 频繁发生，退化为反复 full rescan。
constexpr std::size_t kMinBufferSize = 4 * 1024;

// UTF-16 → UTF-8。先查询所需字节数再分配，失败（含 characterCount 非正）返回空串。
std::string Utf16ToUtf8(const wchar_t* text, const int characterCount)
{
    if (characterCount <= 0)
    {
        return {};
    }
    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, text, characterCount, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(byteCount), '\0');
    static_cast<void>(
        WideCharToMultiByte(CP_UTF8, 0, text, characterCount, result.data(), byteCount, nullptr, nullptr));
    return result;
}

// 路径分隔符统一为 '/'，与引擎侧的路径约定（AssetId / Manifest / 沙箱校验）保持一致。
std::string NormalizeSeparators(std::string text)
{
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}
} // namespace

struct DirectoryWatcherWin32::Impl final
{
    explicit Impl(const std::filesystem::path& directory, const std::size_t requestedBufferSize)
        : buffer(std::max(requestedBufferSize, kMinBufferSize))
    {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent == nullptr)
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "DirectoryWatcherWin32: CreateEventW(stop)");
        }
        ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ioEvent == nullptr)
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "DirectoryWatcherWin32: CreateEventW(io)");
        }
        overlapped.hEvent = ioEvent;

        // FILE_LIST_DIRECTORY + 共享读写删 + BACKUP_SEMANTICS（打开目录）+ OVERLAPPED。
        directoryHandle =
            CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "DirectoryWatcherWin32: CreateFileW(watched directory)");
        }
    }

    ~Impl()
    {
        StopAndJoin();
        if (directoryHandle != nullptr && directoryHandle != INVALID_HANDLE_VALUE)
        {
            static_cast<void>(CloseHandle(directoryHandle));
        }
        if (ioEvent != nullptr)
        {
            static_cast<void>(CloseHandle(ioEvent));
        }
        if (stopEvent != nullptr)
        {
            static_cast<void>(CloseHandle(stopEvent));
        }
    }

    void Start()
    {
        worker = std::jthread([this](std::stop_token stopToken) { WorkerLoop(std::move(stopToken)); });
    }

    void StopAndJoin()
    {
        if (worker.joinable())
        {
            worker.request_stop();
            // stop_callback 也会 SetEvent；这里再 Set 一次作双保险。
            static_cast<void>(SetEvent(stopEvent));
            worker.join();
        }
    }

    [[nodiscard]] DirectoryWatchEvent ParseNotifications(const DWORD bytesTransferred)
    {
        DirectoryWatchEvent event;
        // FILE_NOTIFY_INFORMATION 以 DWORD 对齐排布在 buffer 中；vector 分配满足
        // max_align_t，reinterpret_cast 安全。NextEntryOffset 链式遍历，
        // 偏移超过 bytesTransferred 即停（防御畸形链）。
        const auto* base = reinterpret_cast<const std::byte*>(buffer.data());
        DWORD offset = 0;
        while (offset < bytesTransferred)
        {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            // FileNameLength 是字节数且不含 null terminator（07 篇防御）。
            const std::string name = NormalizeSeparators(
                Utf16ToUtf8(info->FileName, static_cast<int>(info->FileNameLength / sizeof(WCHAR))));
            if (!name.empty())
            {
                event.relativePaths.push_back(name);
            }
            if (info->NextEntryOffset == 0)
            {
                break;
            }
            offset += info->NextEntryOffset;
        }
        return event;
    }

    void Enqueue(DirectoryWatchEvent&& event)
    {
        const std::lock_guard lock{queueMutex};
        queue.push_back(std::move(event));
    }

    void EnqueueFullRescan()
    {
        DirectoryWatchEvent event;
        event.fullRescanRequired = true;
        Enqueue(std::move(event));
    }

    void WorkerLoop(std::stop_token stopToken)
    {
        // stop 请求 → SetEvent 唤醒 WaitForMultipleObjects。
        const std::stop_callback callback{stopToken, [this] { static_cast<void>(SetEvent(stopEvent)); }};

        while (!stopToken.stop_requested())
        {
            static_cast<void>(ResetEvent(ioEvent));
            const BOOL issued = ReadDirectoryChangesW(directoryHandle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                      TRUE, kNotifyFilter, nullptr, &overlapped, nullptr);
            if (issued == FALSE)
            {
                if (stopToken.stop_requested())
                {
                    break;
                }
                EnqueueFullRescan();
                // 有限退避，避免立即重试变成 busy loop；stop 请求会提前唤醒。
                static_cast<void>(WaitForSingleObject(stopEvent, 100));
                continue;
            }

            const HANDLE waits[2] = {ioEvent, stopEvent};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0)
            {
                // stop 信号或等待失败：退出前 CancelIoEx 并 drain，保证
                // buffer/handle 在 IO 结束后才销毁（Impl 析构顺序安全）。
                break;
            }

            DWORD bytesTransferred = 0;
            if (GetOverlappedResult(directoryHandle, &overlapped, &bytesTransferred, FALSE) == FALSE)
            {
                // overflow（ERROR_NOTIFY_ENUM_DIR）或其他 IO 错误：不丢事件后继续，
                // 发 full rescan 并重新武装（07 篇 overflow 策略）。
                EnqueueFullRescan();
                continue;
            }
            if (bytesTransferred == 0)
            {
                // 07 篇：bytes==0 同样视为 overflow → full rescan。
                EnqueueFullRescan();
                continue;
            }

            Enqueue(ParseNotifications(bytesTransferred));
        }

        static_cast<void>(CancelIoEx(directoryHandle, &overlapped));
        DWORD dropped = 0;
        static_cast<void>(GetOverlappedResult(directoryHandle, &overlapped, &dropped, TRUE));
    }

    // 被监视目录的句柄（FILE_LIST_DIRECTORY + BACKUP_SEMANTICS + OVERLAPPED）。
    HANDLE directoryHandle{};
    // 异步 IO 控制块；hEvent 绑定 ioEvent，供 WaitForMultipleObjects 等待完成。
    OVERLAPPED overlapped{};
    // 单次 ReadDirectoryChangesW 完成信号（手动重置）。
    HANDLE ioEvent{};
    // 停止信号（手动重置）：Stop / stop_callback 都会 SetEvent 唤醒等待。
    HANDLE stopEvent{};
    // 通知缓冲；大小取 max(请求值, kMinBufferSize)，可注入小值以测试 overflow 路径。
    std::vector<std::byte> buffer;
    // 保护 queue：worker 入队、消费者 PollEvents 取走。
    std::mutex queueMutex;
    // 待消费事件队列；worker 只入队，不直接触碰任何上层模块。
    std::vector<DirectoryWatchEvent> queue;
    std::jthread worker; // 最后声明：先于句柄/缓冲析构
};

DirectoryWatcherWin32::DirectoryWatcherWin32(const std::filesystem::path& directory, const std::size_t bufferSize)
    : m_impl{std::make_unique<Impl>(directory, bufferSize)}
{
    m_impl->Start();
}

DirectoryWatcherWin32::~DirectoryWatcherWin32()
{
    Stop();
}

std::vector<DirectoryWatchEvent> DirectoryWatcherWin32::PollEvents()
{
    const std::lock_guard lock{m_impl->queueMutex};
    return std::exchange(m_impl->queue, {});
}

void DirectoryWatcherWin32::Stop()
{
    if (m_impl != nullptr)
    {
        m_impl->StopAndJoin();
    }
}
} // namespace MiniEngine::Platform
