#pragma once

#include <functional>
#include <source_location>
#include <string_view>

namespace MiniEngine
{
enum class LogLevel
{
    Trace,
    Info,
    Warning,
    Error,
    Fatal
};

// Invoked while the M1 logger mutex is held. A sink must not call WriteLog,
// SetLogSink, or ResetLogSink; doing so violates the non-reentrant contract.
using LogSink = std::function<void(std::string_view)>;

void SetLogSink(LogSink sink);
void ResetLogSink();

void WriteLog(LogLevel level, std::string_view message,
              std::source_location location = std::source_location::current());
} // namespace MiniEngine

#define ME_LOG_TRACE(message) ::MiniEngine::WriteLog(::MiniEngine::LogLevel::Trace, (message))
#define ME_LOG_INFO(message) ::MiniEngine::WriteLog(::MiniEngine::LogLevel::Info, (message))
#define ME_LOG_WARNING(message) ::MiniEngine::WriteLog(::MiniEngine::LogLevel::Warning, (message))
#define ME_LOG_ERROR(message) ::MiniEngine::WriteLog(::MiniEngine::LogLevel::Error, (message))
#define ME_LOG_FATAL(message) ::MiniEngine::WriteLog(::MiniEngine::LogLevel::Fatal, (message))