#pragma once

#include <MiniEngine/Rhi/RhiError.h>
#include <MiniEngine/Rhi/RhiHandle.h>

#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace MiniEngine::Rhi
{
namespace Detail
{
// 进程内单调签发 owner，耗尽即失败，永不重用；跨注册表碰撞不能靠 generation 猜测。
std::uint64_t AcquireRegistryOwner();
} // namespace Detail

// 渲染线程所有。Payload 由 registry 持有至 Retire，然后交给后端退休队列。
// Recycle 仅由已证明 GPU 完成的后端调用；这里不实现 fence/设备生命周期。
// MaxGeneration 为编译期策略，测试用小上限覆盖耗尽路径，生产使用完整 uint32。
template <class Handle, class Payload, std::uint32_t MaxGeneration = std::numeric_limits<std::uint32_t>::max()>
class HandleRegistry final
{
    static_assert(std::is_nothrow_move_constructible_v<Payload>);
    static_assert(std::is_nothrow_destructible_v<Payload>);
    static_assert(MaxGeneration > 0);

  public:
    struct Retirement final
    {
        Retirement(Handle handle, Payload value) noexcept : ticket(handle), payload(std::move(value))
        {
        }
        Retirement(const Retirement&) = delete;
        Retirement& operator=(const Retirement&) = delete;
        Retirement(Retirement&&) noexcept = default;
        Retirement& operator=(Retirement&&) = default;
        Handle ticket;
        Payload payload;
    };
    explicit HandleRegistry(std::string objectType = "Resource", std::string backend = "")
        : m_owner(Detail::AcquireRegistryOwner()), m_objectType(std::move(objectType)), m_backend(std::move(backend))
    {
    }
    HandleRegistry(const HandleRegistry&) = delete;
    HandleRegistry& operator=(const HandleRegistry&) = delete;
    HandleRegistry(HandleRegistry&&) = delete;
    HandleRegistry& operator=(HandleRegistry&&) = delete;

    [[nodiscard]] Handle Create(Payload payload, std::string debugName = "")
    {
        for (std::size_t index = 0; index < m_slots.size(); ++index)
        {
            Slot& slot = m_slots[index];
            if (slot.lifecycle == Lifecycle::Free)
            {
                slot.debugName = std::move(debugName);
                slot.payload.emplace(std::move(payload));
                slot.lifecycle = Lifecycle::Alive;
                return Handle{static_cast<std::uint32_t>(index), slot.generation, m_owner};
            }
        }
        if (m_slots.size() >= Handle::kInvalidIndex)
        {
            Fail(RhiErrorCode::OutOfMemory, "Create", "handle index space exhausted");
        }
        try
        {
            m_slots.emplace_back(std::move(payload), std::move(debugName));
        }
        catch (const std::bad_alloc&)
        {
            Fail(RhiErrorCode::OutOfMemory, "Create", "registry allocation failed");
        }
        return Handle{static_cast<std::uint32_t>(m_slots.size() - 1), 1, m_owner};
    }
    // 返回的引用只保持到下一次 registry 修改；调用者不得跨 Create/Retire 保存。
    [[nodiscard]] Payload& Get(Handle handle)
    {
        return *ValidateAlive(handle, "Get").payload;
    }
    [[nodiscard]] const Payload& Get(Handle handle) const
    {
        return *ValidateAlive(handle, "Get").payload;
    }
    [[nodiscard]] Retirement Retire(Handle handle)
    {
        Slot& slot = ValidateAlive(handle, "Retire");
        Retirement retired{handle, std::move(*slot.payload)};
        slot.payload.reset();
        slot.retiringGeneration = slot.generation;
        if (slot.generation < MaxGeneration)
        {
            ++slot.generation;
        }
        slot.lifecycle = Lifecycle::Retiring;
        return retired;
    }
    // 使用带 owner/generation 的退休 ticket，不能按裸 index 回收（避免延迟回调 ABA）。
    void Recycle(Handle ticket)
    {
        Slot& slot = ValidateIndex(ticket, "Recycle");
        if (slot.lifecycle != Lifecycle::Retiring || slot.retiringGeneration != ticket.Generation())
        {
            Fail(RhiErrorCode::InvalidState, "Recycle", "ticket does not identify a pending retirement",
                 slot.debugName);
        }
        slot.lifecycle = slot.retiringGeneration == MaxGeneration ? Lifecycle::Exhausted : Lifecycle::Free;
        slot.retiringGeneration = 0;
    }
    [[nodiscard]] std::size_t SlotCount() const noexcept
    {
        return m_slots.size();
    }
    [[nodiscard]] std::size_t AliveCount() const noexcept
    {
        return Count(Lifecycle::Alive);
    }
    [[nodiscard]] std::size_t RetiringCount() const noexcept
    {
        return Count(Lifecycle::Retiring);
    }
    [[nodiscard]] std::size_t ExhaustedCount() const noexcept
    {
        return Count(Lifecycle::Exhausted);
    }

  private:
    enum class Lifecycle
    {
        Free,
        Alive,
        Retiring,
        Exhausted
    };
    struct Slot final
    {
        explicit Slot(Payload value, std::string name) noexcept : payload(std::move(value)), debugName(std::move(name))
        {
        }
        std::uint32_t generation = 1;
        std::uint32_t retiringGeneration = 0;
        Lifecycle lifecycle = Lifecycle::Alive;
        std::optional<Payload> payload;
        std::string debugName;
    };
    [[noreturn]] void Fail(RhiErrorCode code, const char* operation, const char* message,
                           const std::string& objectName = "") const
    {
        throw RhiValidationError(RhiError{code, operation, m_objectType, objectName, m_backend, message});
    }
    [[nodiscard]] const Slot& ValidateIndex(Handle handle, const char* operation) const
    {
        if (!handle || handle.Owner() != m_owner || handle.Index() >= m_slots.size())
        {
            Fail(RhiErrorCode::InvalidHandle, operation, "invalid index or foreign registry owner");
        }
        return m_slots[handle.Index()];
    }
    [[nodiscard]] Slot& ValidateIndex(Handle handle, const char* operation)
    {
        return const_cast<Slot&>(std::as_const(*this).ValidateIndex(handle, operation));
    }
    [[nodiscard]] const Slot& ValidateAlive(Handle handle, const char* operation) const
    {
        const Slot& slot = ValidateIndex(handle, operation);
        if (slot.lifecycle != Lifecycle::Alive || slot.generation != handle.Generation())
        {
            Fail(RhiErrorCode::InvalidHandle, operation, "stale or non-alive handle", slot.debugName);
        }
        return slot;
    }
    [[nodiscard]] Slot& ValidateAlive(Handle handle, const char* operation)
    {
        return const_cast<Slot&>(std::as_const(*this).ValidateAlive(handle, operation));
    }
    [[nodiscard]] std::size_t Count(Lifecycle lifecycle) const noexcept
    {
        std::size_t count = 0;
        for (const Slot& slot : m_slots)
        {
            count += slot.lifecycle == lifecycle ? 1U : 0U;
        }
        return count;
    }
    const std::uint64_t m_owner;
    std::string m_objectType;
    std::string m_backend;
    std::vector<Slot> m_slots;
};
} // namespace MiniEngine::Rhi
