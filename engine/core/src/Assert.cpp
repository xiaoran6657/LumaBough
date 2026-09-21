#include <MiniEngine/Core/Assert.h>

#include <MiniEngine/Core/Log.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace MiniEngine
{
[[noreturn]] void FailAssertion(const std::string_view expression, const std::string_view message,
                                const std::source_location location)
{
    std::ostringstream stream;
    stream << "Assertion failed: " << expression;

    if (!message.empty())
    {
        stream << " | " << message;
    }

    const std::string failure = stream.str();

    try
    {
        WriteLog(LogLevel::Fatal, failure, location);
    }
    catch (...)
    {
        std::fputs(failure.c_str(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    std::abort();
}
} // namespace MiniEngine