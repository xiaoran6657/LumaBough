#include <MiniEngine/Core/Log.h>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
class LogSinkResetter
{
  public:
    ~LogSinkResetter()
    {
        MiniEngine::ResetLogSink();
    }

    LogSinkResetter() = default;
    LogSinkResetter(const LogSinkResetter&) = delete;
    LogSinkResetter& operator=(const LogSinkResetter&) = delete;
};

TEST(LogTests, CaptureLevelMessageAndSourceLocation)
{
    [[maybe_unused]] LogSinkResetter resetter;
    std::string captured;

    MiniEngine::SetLogSink([&](const std::string_view line) { captured.append(line); });

    MiniEngine::WriteLog(MiniEngine::LogLevel::Warning, "captured warning");

    EXPECT_NE(captured.find("[Warning]"), std::string::npos);
    EXPECT_NE(captured.find("captured warning"), std::string::npos);
    EXPECT_NE(captured.find("LogTests.cpp:"), std::string::npos);
}
} // namespace