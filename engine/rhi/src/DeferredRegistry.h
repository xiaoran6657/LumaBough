#pragma once

#include "HandleRegistry.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MiniEngine::Rhi
{
struct RegistryCounts final
{
    std::size_t alive = 0;
    std::size_t retiring = 0;
    std::size_t slots = 0;
    std::size_t exhausted = 0;
};

// 渲染线程所有；serial 是单 graphics stream 的逻辑提交号，不是 native fence。
// MarkUsed 必须在录制引用时调用，包含尚未提交的帧。Collect 的 completed 只能
// 由后端真实完成证明给出；不能把 CPU EndFrame 或命令列表 Close 当作完成。
template <class Handle, class Payload> class DeferredRegistry final
{
    struct Entry final
    {
        Payload payload;
        std::uint64_t lastUse = 0;
    };
    using Registry = HandleRegistry<Handle, Entry>;

  public:
    explicit DeferredRegistry(std::string objectType = "Resource", std::string backend = "")
        : m_registry(objectType, backend), m_objectType(std::move(objectType)), m_backend(std::move(backend))
    {
    }
    Handle Create(Payload payload, std::string name = "")
    {
        return m_registry.Create(Entry{std::move(payload), 0}, std::move(name));
    }
    Payload& Get(Handle handle)
    {
        return m_registry.Get(handle).payload;
    }
    const Payload& Get(Handle handle) const
    {
        return m_registry.Get(handle).payload;
    }
    std::uint64_t LastUse(Handle handle) const
    {
        return m_registry.Get(handle).lastUse;
    }
    void MarkUsed(Handle handle, std::uint64_t serial)
    {
        auto& entry = m_registry.Get(handle);
        if (serial == 0 || serial <= m_completed)
        {
            throw RhiValidationError({RhiErrorCode::InvalidState, "MarkUsed", m_objectType, "", m_backend,
                                      "use must belong to a future submission"});
        }
        entry.lastUse = std::max(entry.lastUse, serial);
    }
    void Destroy(Handle handle)
    {
        // 先验证和分配退休容器，分配失败时仍保持 Alive；不能先 Retire 再丢失 payload。
        (void)m_registry.Get(handle);
        try
        {
            m_retiring.reserve(m_retiring.size() + 1);
            auto node = std::make_unique<std::optional<typename Registry::Retirement>>();
            node->emplace(m_registry.Retire(handle));
            m_retiring.push_back(std::move(node));
        }
        catch (const std::bad_alloc&)
        {
            throw RhiValidationError({RhiErrorCode::OutOfMemory, "Destroy", m_objectType, "", m_backend,
                                      "retirement queue allocation failed; resource remains alive"});
        }
    }
    // 已知完成的 resize/事务回滚路径，不分配退休节点；未完成对象不能绕过退休队列。
    void DestroyCompleted(Handle handle)
    {
        if (m_registry.Get(handle).lastUse > m_completed)
        {
            throw RhiValidationError({RhiErrorCode::InvalidState, "DestroyCompleted", m_objectType, "", m_backend,
                                      "backend completion does not cover this resource"});
        }
        std::optional<typename Registry::Retirement> retired{m_registry.Retire(handle)};
        const auto ticket = retired->ticket;
        retired.reset();
        m_registry.Recycle(ticket);
    }
    void Collect(std::uint64_t completed)
    {
        if (completed < m_completed)
        {
            throw RhiValidationError(
                {RhiErrorCode::InvalidState, "Collect", "Device", "", m_backend, "completion cannot move backwards"});
        }
        m_completed = completed;
        // 原位稳定压缩，要求 payload 只可 move-construct，不要求 move-assign。
        for (std::size_t i = 0; i < m_retiring.size();)
        {
            if (m_retiring[i]->value().payload.lastUse <= completed)
            {
                const Handle ticket = m_retiring[i]->value().ticket;
                // 先释放 backend payload，再开放 slot；后移仅移动 unique_ptr 退休节点。
                m_retiring.erase(m_retiring.begin() + static_cast<std::ptrdiff_t>(i));
                m_registry.Recycle(ticket);
            }
            else
            {
                ++i;
            }
        }
    }
    RegistryCounts Stats() const
    {
        return {m_registry.AliveCount(), m_registry.RetiringCount(), m_registry.SlotCount(),
                m_registry.ExhaustedCount()};
    }

  private:
    Registry m_registry;
    std::string m_objectType;
    std::string m_backend;
    // unique_ptr 节点保证 vector 整理不对用户 payload 做 move assignment。
    std::vector<std::unique_ptr<std::optional<typename Registry::Retirement>>> m_retiring;
    std::uint64_t m_completed = 0;
};
} // namespace MiniEngine::Rhi
