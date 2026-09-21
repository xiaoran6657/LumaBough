#include "HandleRegistry.h"
#include <atomic>

namespace MiniEngine::Rhi::Detail
{
std::uint64_t AcquireRegistryOwner()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;)
    {
        if (value == std::numeric_limits<std::uint64_t>::max())
        {
            throw RhiValidationError(RhiError{RhiErrorCode::OutOfMemory, "CreateRegistry", "Registry", "", "",
                                              "registry owner space exhausted"});
        }
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value;
        }
    }
}
} // namespace MiniEngine::Rhi::Detail
