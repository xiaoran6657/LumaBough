#include <MiniEngine/Core/Log.h>

#include <MiniEngine/Profiling/Profile.h>

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace MiniEngine
{
namespace
{
// M7-02：日志锁走 profiling 门面，Tracy 时间线上能看到真实的竞争/等待；
// 关闭 Tracy 时它就是普通 std::mutex（同一份调用代码）。
Profiling::LockableMutex& GetLogMutex()
{
    static Profiling::LockableMutex mutex{"ME.Log.Mutex"};
    return mutex;
}

LogSink& GetLogSink()
{
    static LogSink sink;
    return sink;
}

std::string_view ToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "Trace";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warning:
        return "Warning";
    case LogLevel::Error:
        return "Error";
    case LogLevel::Fatal:
        return "Fatal";
    default:
        return "Unknown";
    }
}
} // namespace

void SetLogSink(LogSink sink)
{
    const std::scoped_lock lock{GetLogMutex()};
    GetLogSink() = std::move(sink);
}

void ResetLogSink()
{
    const std::scoped_lock lock{GetLogMutex()};
    GetLogSink() = {};
}

void WriteLog(const LogLevel level, const std::string_view message, const std::source_location location)
{
    std::ostringstream stream;
    stream << "[" << ToString(level) << "] " << message << " (" << location.file_name() << ":" << location.line()
           << ")\n";

    const std::string line = stream.str();
    const std::scoped_lock lock{GetLogMutex()};

    if (GetLogSink())
    {
        GetLogSink()(line);
        return;
    }

    std::clog << line;
    std::clog.flush();
}
} // namespace MiniEngine