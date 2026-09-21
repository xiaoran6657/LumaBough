// ============================================================================
// D3D12ResourceStateTracker.cpp — 设备侧 barrier 生成与提交
// 里程碑：M5（07 篇 Resource Barrier 与状态跟踪；手抄清单第 1 条）
// 职责：实现 D3D12ResourceStateTracker.h。本文件只做三件事：
//   1) 维护 id → 资源指针/名字（**非拥有**）并转发注册/注销给纯 CPU registry；
//   2) 把 registry 的 BarrierRequest 转成 D3D12_RESOURCE_BARRIER 并在 Flush 时发出；
//   3) 断言/诊断（VerifyState 带名字与实际状态，便于定位 pass 顺序问题）。
// barrier 的内容（before/after 是否真的变化、trace hash）全部由 registry 决定，
// 本文件不自行判断"要不要发"——避免两处各有一套规则（05/06 篇同一原则）。
// 关联：docs/architecture/README.md（Pass barriers / Barrier batching）
// ============================================================================
#include "D3D12ResourceStateTracker.h"

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Core/Log.h>

#include <Windows.h>

#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// UTF-16 → UTF-8：只在下述三处拼诊断消息时用，注册时一次性转换后保存。
std::string Utf8(const wchar_t* text)
{
    if (text == nullptr || *text == L'\0')
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}
} // namespace

std::uint64_t D3D12ResourceStateTracker::IdOf(ID3D12Resource& resource) noexcept
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&resource));
}

ResourceKey D3D12ResourceStateTracker::Register(ID3D12Resource& resource, const std::uint32_t subresourceCount,
                                                const D3D12_RESOURCE_STATES initialState, const wchar_t* debugName,
                                                const std::uint32_t generation)
{
    if (subresourceCount == 0U)
    {
        throw std::invalid_argument{"resource must have at least one subresource"};
    }

    const std::uint64_t id = IdOf(resource);
    const ResourceKey key{id, generation};
    // registry 会拒绝重复注册（同一 generation）；这里再补一条"同一 id 的不同 generation
    // 不允许并存"的检查：并存意味着旧引用没有被注销，状态表会出现两个真值。
    if (const auto existing = m_resources.find(id); existing != m_resources.end())
    {
        throw std::logic_error{"resource id already tracked; unregister the previous generation first"};
    }

    m_registry.Register(key, subresourceCount, initialState);
    m_resources.emplace(id, &resource);
    m_names.emplace(id, Utf8(debugName));
    return key;
}

void D3D12ResourceStateTracker::Unregister(const ResourceKey key)
{
    m_registry.Unregister(key);
    m_resources.erase(key.id);
    m_names.erase(key.id);
}

void D3D12ResourceStateTracker::BeginRecording()
{
    m_registry.BeginRecording();
}

void D3D12ResourceStateTracker::Transition(const ResourceKey key, const D3D12_RESOURCE_STATES desired,
                                           const std::uint32_t subresource)
{
    // registry 负责"只在不同状态时生成请求"与 pending 更新；返回值这里不需要。
    static_cast<void>(m_registry.Transition(key, desired, subresource));
}

std::uint32_t D3D12ResourceStateTracker::FlushBarriersTo(ID3D12GraphicsCommandList& commandList)
{
    const std::vector<BarrierRequest>& requests = m_registry.PendingBarriers();
    if (requests.empty())
    {
        return 0U;
    }

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(requests.size());
    for (const BarrierRequest& request : requests)
    {
        const auto resource = m_resources.find(request.key.id);
        if (resource == m_resources.end())
        {
            // 已注销却仍有待发 barrier：说明释放资源的顺序错了（先 Unregister 再 flush）。
            const std::string name =
                m_names.contains(request.key.id) ? std::string{m_names.at(request.key.id)} : std::string{"<unknown>"};
            throw std::logic_error{"pending barrier for an unregistered resource: " + name};
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource->second;
        barrier.Transition.Subresource = request.subresource;
        barrier.Transition.StateBefore = request.before;
        barrier.Transition.StateAfter = request.after;
        barriers.push_back(barrier);
    }

    commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    m_registry.ClearPendingBarriers();
    return static_cast<std::uint32_t>(barriers.size());
}

void D3D12ResourceStateTracker::CommitExecuted()
{
    // registry 会拒绝"仍有未发 barrier 就 commit"（那意味着状态变化没进 command list）。
    m_registry.CommitExecuted();
}

void D3D12ResourceStateTracker::Rollback()
{
    m_registry.Rollback();
}

void D3D12ResourceStateTracker::VerifyState(const ResourceKey key, const D3D12_RESOURCE_STATES expected,
                                            const std::uint32_t subresource) const
{
    const std::string name = m_names.contains(key.id) ? std::string{m_names.at(key.id)} : std::string{"<unknown>"};

    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        if (const auto mismatching = m_registry.FirstMismatchingSubresource(key, expected); mismatching.has_value())
        {
            throw std::logic_error{"resource state mismatch: " + name + " subresource " + std::to_string(*mismatching) +
                                   " is not in the expected state (" +
                                   std::to_string(static_cast<std::uint32_t>(expected)) + ")"};
        }
        return;
    }

    const D3D12_RESOURCE_STATES actual = m_registry.CurrentState(key, subresource);
    if (actual != expected)
    {
        throw std::logic_error{"resource state mismatch: " + name + " subresource " + std::to_string(subresource) +
                               " actual=" + std::to_string(static_cast<std::uint32_t>(actual)) +
                               " expected=" + std::to_string(static_cast<std::uint32_t>(expected))};
    }
}

bool D3D12ResourceStateTracker::IsRecording() const noexcept
{
    return m_registry.IsRecording();
}

D3D12_RESOURCE_STATES D3D12ResourceStateTracker::CurrentState(const ResourceKey key,
                                                              const std::uint32_t subresource) const
{
    return m_registry.CurrentState(key, subresource);
}

std::size_t D3D12ResourceStateTracker::TrackedResourceCount() const noexcept
{
    return m_resources.size();
}

std::uint64_t D3D12ResourceStateTracker::TraceHash() const noexcept
{
    return m_registry.TraceHash();
}

std::uint64_t D3D12ResourceStateTracker::BarrierCount() const noexcept
{
    return m_registry.BarrierCount();
}

std::uint64_t D3D12ResourceStateTracker::CommittedStateHash() const noexcept
{
    return m_registry.CommittedStateHash();
}

std::uint32_t D3D12ResourceStateTracker::PendingBarrierCount() const noexcept
{
    return static_cast<std::uint32_t>(m_registry.PendingBarriers().size());
}
} // namespace MiniEngine::Rhi::D3D12
