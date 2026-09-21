// ============================================================================
// BoundedQueue.h — 项目内通用的有界 FIFO 原语（M7-06，内部头）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：提供一个"容量固定、关闭后不再接收"的线程安全 FIFO。它只负责**条目数**上限；
//       字节预算、优先级与条件变量通知由使用方（AsyncAssetLoader）叠加：
//       "Item count alone is insufficient for asset payload queues"。
// 用途：CpuReady 停车区、上传队列。I/O 队列额外需要优先级排序与准入字节预算，
//       因此由 AsyncAssetLoader 内联实现（同样有界），见 AsyncAssetLoader.cpp。
// 关联：docs/architecture/README.md「第 1/4 步」
// ============================================================================

#pragma once

#include <MiniEngine/Core/Assert.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace MiniEngine::Assets
{
template <typename T> class BoundedQueue final
{
  public:
    explicit BoundedQueue(const std::size_t capacity) : m_capacity(capacity)
    {
        ME_ASSERT(capacity > 0, "bounded queue capacity must be positive");
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // 返回：入队成功为 true；已满或已关闭为 false（调用方自行决定背压策略）。
    [[nodiscard]] bool TryPush(T value)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_values.size() >= m_capacity)
        {
            return false;
        }
        m_values.push_back(std::move(value));
        m_highWater = m_values.size() > m_highWater ? m_values.size() : m_highWater;
        return true;
    }

    [[nodiscard]] std::optional<T> TryPop()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_values.empty())
        {
            return std::nullopt;
        }
        T value = std::move(m_values.front());
        m_values.pop_front();
        return value;
    }

    void Close()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }

    [[nodiscard]] std::size_t Size() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_values.size();
    }

    [[nodiscard]] std::size_t Capacity() const noexcept
    {
        return m_capacity;
    }

    // 历史最大深度：A19「满载行为可复现」的机读证据。
    [[nodiscard]] std::size_t HighWater() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_highWater;
    }

    [[nodiscard]] bool IsClosed() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

  private:
    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<T> m_values;
    std::size_t m_highWater = 0;
    bool m_closed = false;
};
} // namespace MiniEngine::Assets
