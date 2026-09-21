#pragma once

#include <source_location>
#include <string_view>

namespace MiniEngine
{
[[noreturn]] void FailAssertion(std::string_view expression, std::string_view message,
                                std::source_location location = std::source_location::current());
}

#define ME_VERIFY(expression, message)                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            ::MiniEngine::FailAssertion(#expression, (message));                                                       \
        }                                                                                                              \
    } while (false)

#if defined(NDEBUG)
#define ME_ASSERT(expression, message)                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        (void)sizeof(expression);                                                                                      \
    } while (false)
#else
#define ME_ASSERT(expression, message) ME_VERIFY((expression), (message))
#endif