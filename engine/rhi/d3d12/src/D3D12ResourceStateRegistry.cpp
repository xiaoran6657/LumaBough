// ============================================================================
// D3D12ResourceStateRegistry.cpp — 状态机实现（pending/commit/rollback + trace）
// 里程碑：M5（07 篇 Resource Barrier 与状态跟踪；手抄清单第 1/2 条）
// 职责：实现 D3D12ResourceStateRegistry.h。三条纪律：
//   1) 只在不同状态时生成 barrier（不发 before==after）；
//   2) pending 与 committed 严格分离：Execute 成功才 commit，失败必须 Rollback；
//   3) 未注册的 key 一律显式失败（07 篇：未知 state 不允许猜）。
// trace hash 用 FNV-1a 64 覆盖"本帧 barrier 序列"（资源 id/generation/subresource/
// before/after 全部入哈希），因此任何一处顺序或状态变化都会改变它。
// 关联：docs/architecture/README.md（Tracker 边界 / Barrier batching）
// ============================================================================
#include "D3D12ResourceStateRegistry.h"

#include <stdexcept>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// FNV-1a 64：与 sandbox 的 packet hash、测试里的期望值同一算法口径。
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void Mix(std::uint64_t& hash, const std::uint64_t value)
{
    hash ^= value;
    hash *= kFnvPrime;
}

void MixState(std::uint64_t& hash, const D3D12_RESOURCE_STATES state)
{
    Mix(hash, static_cast<std::uint64_t>(state));
}
} // namespace

void ResourceStateRegistry::Register(const ResourceKey key, const std::uint32_t subresourceCount,
                                     const D3D12_RESOURCE_STATES initialState)
{
    if (subresourceCount == 0U)
    {
        throw std::invalid_argument{"resource must have at least one subresource"};
    }
    const auto [iterator, inserted] = m_states.try_emplace(key);
    if (!inserted)
    {
        // 重复注册同一个 (id, generation) 说明调用方忘了在重建时递增 generation——
        // 那会让"旧句柄"继续有效，正是 07 篇要防的状态污染。
        throw std::logic_error{"resource already registered with the same generation"};
    }
    iterator->second.committed.assign(subresourceCount, initialState);
    iterator->second.pending = iterator->second.committed;
    // 槽位按注册顺序递增（与地址无关），使 trace/state hash 跨 run 稳定。
    iterator->second.traceSlot = m_nextTraceSlot++;
}

void ResourceStateRegistry::Unregister(const ResourceKey key)
{
    m_states.erase(key);
}

void ResourceStateRegistry::BeginRecording()
{
    if (m_recording)
    {
        throw std::logic_error{"BeginRecording called while already recording"};
    }
    for (auto& [key, state] : m_states)
    {
        static_cast<void>(key);
        state.pending = state.committed;
    }
    m_pendingBarriers.clear();
    m_traceHash = kFnvOffsetBasis;
    m_barrierCount = 0;
    m_recording = true;
}

ResourceStateRegistry::State& ResourceStateRegistry::At(const ResourceKey key)
{
    const auto iterator = m_states.find(key);
    if (iterator == m_states.end())
    {
        throw std::out_of_range{"resource is not registered in the state registry"};
    }
    return iterator->second;
}

const ResourceStateRegistry::State& ResourceStateRegistry::At(const ResourceKey key) const
{
    const auto iterator = m_states.find(key);
    if (iterator == m_states.end())
    {
        throw std::out_of_range{"resource is not registered in the state registry"};
    }
    return iterator->second;
}

std::vector<BarrierRequest> ResourceStateRegistry::Transition(const ResourceKey key,
                                                              const D3D12_RESOURCE_STATES desired,
                                                              const std::uint32_t subresource)
{
    if (!m_recording)
    {
        throw std::logic_error{"Transition requires an active recording (call BeginRecording)"};
    }
    State& state = At(key);

    std::vector<BarrierRequest> created;
    const auto transitionOne = [&](const std::uint32_t index)
    {
        const D3D12_RESOURCE_STATES current = state.pending.at(index);
        if (current == desired)
        {
            // 07 篇：不发 before==after transition（冗余 barrier 会掩盖真实错误）。
            return;
        }
        BarrierRequest request;
        request.key = key;
        request.subresource = index;
        request.before = current;
        request.after = desired;
        state.pending[index] = desired;

        // trace 入哈希（顺序敏感：同一次调用内按 subresource 升序）。
        // 用注册槽位而不是资源指针：地址每次运行都不同，直接哈希指针会让
        // "每帧 trace hash 跨 run 稳定"这条验收永远无法成立。
        Mix(m_traceHash, state.traceSlot);
        Mix(m_traceHash, key.generation);
        Mix(m_traceHash, index);
        MixState(m_traceHash, request.before);
        MixState(m_traceHash, request.after);
        ++m_barrierCount;

        m_pendingBarriers.push_back(request);
        created.push_back(request);
    };

    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        for (std::uint32_t index = 0; index < state.pending.size(); ++index)
        {
            transitionOne(index);
        }
    }
    else
    {
        if (subresource >= state.pending.size())
        {
            throw std::out_of_range{"subresource index exceeds the registered subresource count"};
        }
        transitionOne(subresource);
    }
    return created;
}

const std::vector<BarrierRequest>& ResourceStateRegistry::PendingBarriers() const noexcept
{
    return m_pendingBarriers;
}

void ResourceStateRegistry::ClearPendingBarriers()
{
    m_pendingBarriers.clear();
}

void ResourceStateRegistry::CommitExecuted()
{
    if (!m_recording)
    {
        throw std::logic_error{"CommitExecuted requires an active recording"};
    }
    if (!m_pendingBarriers.empty())
    {
        // 还有未发出的 barrier 就 commit，意味着有些状态变化从未进入 command list：
        // 那会让 committed 与 GPU 真实状态分叉（07 篇：flush 之后再 commit）。
        throw std::logic_error{"CommitExecuted requires all barriers to be flushed"};
    }
    for (auto& [key, state] : m_states)
    {
        static_cast<void>(key);
        state.committed = state.pending;
    }
    m_recording = false;
}

void ResourceStateRegistry::Rollback()
{
    for (auto& [key, state] : m_states)
    {
        static_cast<void>(key);
        state.pending = state.committed;
    }
    m_pendingBarriers.clear();
    m_recording = false;
}

bool ResourceStateRegistry::IsRecording() const noexcept
{
    return m_recording;
}

D3D12_RESOURCE_STATES ResourceStateRegistry::CurrentState(const ResourceKey key, const std::uint32_t subresource) const
{
    const State& state = At(key);
    if (subresource >= state.pending.size())
    {
        throw std::out_of_range{"subresource index exceeds the registered subresource count"};
    }
    // recording 中看 pending（GPU 尚未确认），否则看 committed。
    return m_recording ? state.pending[subresource] : state.committed[subresource];
}

bool ResourceStateRegistry::AllSubresourcesMatch(const ResourceKey key, const D3D12_RESOURCE_STATES state) const
{
    return !FirstMismatchingSubresource(key, state).has_value();
}

std::optional<std::uint32_t> ResourceStateRegistry::FirstMismatchingSubresource(const ResourceKey key,
                                                                                const D3D12_RESOURCE_STATES state) const
{
    const State& record = At(key);
    const std::vector<D3D12_RESOURCE_STATES>& view = m_recording ? record.pending : record.committed;
    for (std::uint32_t index = 0; index < view.size(); ++index)
    {
        if (view[index] != state)
        {
            return index;
        }
    }
    return std::nullopt;
}

std::size_t ResourceStateRegistry::TrackedResourceCount() const noexcept
{
    return m_states.size();
}

std::uint32_t ResourceStateRegistry::SubresourceCount(const ResourceKey key) const
{
    return static_cast<std::uint32_t>(At(key).committed.size());
}

std::uint64_t ResourceStateRegistry::TraceHash() const noexcept
{
    return m_traceHash;
}

std::uint64_t ResourceStateRegistry::BarrierCount() const noexcept
{
    return m_barrierCount;
}

std::uint64_t ResourceStateRegistry::CommittedStateHash() const noexcept
{
    std::uint64_t hash = kFnvOffsetBasis;
    for (const auto& [key, state] : m_states)
    {
        // 同样使用注册槽位（不是地址），使该 hash 可跨 run 比较。
        Mix(hash, state.traceSlot);
        Mix(hash, key.generation);
        Mix(hash, state.committed.size());
        for (const D3D12_RESOURCE_STATES committed : state.committed)
        {
            MixState(hash, committed);
        }
    }
    return hash;
}
} // namespace MiniEngine::Rhi::D3D12
