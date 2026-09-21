#include "D3D12RhiBackend.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <d3d12sdklayers.h>
#include <limits>
#include <sstream>

namespace MiniEngine::Rhi::D3D12
{
std::uint64_t D3D12RhiBackend::CompletedActual()
{
    try
    {
        m_completedActual = m_queue.CompletedValue();
        return m_completedActual;
    }
    catch (...)
    {
        m_device->ReportDeviceRemoved();
        throw;
    }
}
std::uint64_t D3D12RhiBackend::PollCompleted()
{
    if (!m_initialized)
        return 0;
    const auto actual = CompletedActual();
    while (!m_ownerToFence.empty() && m_ownerToFence.begin()->second <= actual)
    {
        m_completedOwner = std::max(m_completedOwner, m_ownerToFence.begin()->first);
        m_ownerToFence.erase(m_ownerToFence.begin());
    }
    std::erase_if(m_retiredReadbacks,
                  [&](const auto& retired)
                  {
                      if (retired.first > actual)
                          return false;
                      --m_livePayloadResources;
                      return true;
                  });
    m_uploadManager.Reclaim(actual);
    m_srvStaging.Reclaim(actual);
    m_srvVisible.Reclaim(actual);
    m_samplerStaging.Reclaim(actual);
    m_samplerVisible.Reclaim(actual);
    m_rtvHeap.Reclaim(actual);
    m_dsvHeap.Reclaim(actual);
    m_psoFactory.DeferredRelease().Reclaim(actual);
    for (auto item = m_logicalAccess.begin(); m_owner && item != m_logicalAccess.end();)
    {
        try
        {
            m_owner->ValidateAlive(item->first);
            ++item;
        }
        catch (const RhiException&)
        {
            item = m_logicalAccess.erase(item);
        }
    }
    return m_completedOwner;
}
std::uint64_t D3D12RhiBackend::WaitFor(std::uint64_t serial)
{
    PollCompleted();
    if (serial <= m_completedOwner)
        return m_completedOwner;
    const auto found = m_ownerToFence.find(serial);
    if (found == m_ownerToFence.end())
        throw std::logic_error("wait requires an actually submitted owner serial");
    try
    {
        m_queue.WaitForSubmittedFence(found->second, "M6 owner completion");
    }
    catch (...)
    {
        m_device->ReportDeviceRemoved();
        throw;
    }
    return PollCompleted();
}
void D3D12RhiBackend::WaitIdle()
{
    if (!m_initialized)
        return;
    try
    {
        // 故障时未提交的 recording 不执行；只等待此前已经提交的工作。
        // 正常 owner 禁止在 active frame 调用本方法。
        if (m_activeFrame || m_stateTracker.IsRecording())
        {
            const auto next = m_queue.NextFenceValue();
            if (next > 1)
                m_queue.WaitForSubmittedFence(next - 1, "M6 failed recording cleanup");
        }
        else
        {
            m_queue.FlushGpu("M6 explicit idle or shutdown");
        }
        PollCompleted();
    }
    catch (...)
    {
        m_device->ReportDeviceRemoved();
        throw;
    }
}
std::optional<TimestampResult> D3D12RhiBackend::TryReadTimestamp(ResourcePayload& queryPayload)
{
    auto& query = RequirePayload(queryPayload, PayloadKind::Timestamp);
    if (!query.timestampWritten || !query.timestampResolved || !query.timestampFence ||
        CompletedActual() < query.timestampFence)
        return std::nullopt;
    if (!query.queryReadback || !query.timestampFrequency)
        throw std::logic_error("completed timestamp is missing its readback/frequency");
    void* mapped = nullptr;
    const D3D12_RANGE range{0, sizeof(std::uint64_t)};
    ThrowIfFailed(query.queryReadback->Map(0, &range, &mapped), "Map completed timestamp");
    std::uint64_t ticks = 0;
    std::memcpy(&ticks, mapped, sizeof(ticks));
    const D3D12_RANGE noWrites{0, 0};
    query.queryReadback->Unmap(0, &noWrites);
    TimestampResult result;
    result.ticks = ticks;
    result.frequency = query.timestampFrequency;
    result.frameSerial = query.timestampOwnerSerial;
    return result;
}
std::optional<TextureReadbackResult> D3D12RhiBackend::TryReadTextureReadback(ResourcePayload& bufferPayload)
{
    auto& buffer = RequirePayload(bufferPayload, PayloadKind::Buffer);
    if (!buffer.readbackPending || buffer.readbackConsumed || !buffer.readbackFence ||
        CompletedActual() < buffer.readbackFence)
        return std::nullopt;
    auto* readback = buffer.readbackStaging ? buffer.readbackStaging.Get() : buffer.resource.Get();
    const auto capacity = buffer.readbackStaging ? buffer.readbackStagingSize : buffer.nativeSize;
    if (!readback || buffer.readbackTotalBytes > capacity ||
        buffer.readbackTotalBytes > std::numeric_limits<std::size_t>::max())
        throw std::logic_error("completed texture readback exceeds native buffer bounds");
    std::vector<std::byte> raw(static_cast<std::size_t>(buffer.readbackTotalBytes));
    void* mapped = nullptr;
    const D3D12_RANGE range{0, raw.size()};
    ThrowIfFailed(readback->Map(0, &range, &mapped), "Map completed screenshot");
    std::memcpy(raw.data(), mapped, raw.size());
    const D3D12_RANGE noWrites{0, 0};
    readback->Unmap(0, &noWrites);
    const auto offset = buffer.readbackFootprint.Offset;
    if (offset > raw.size())
        throw std::logic_error("readback footprint offset is invalid");
    auto result = NormalizeRgba8Readback(std::span(raw).subspan(static_cast<std::size_t>(offset)),
                                         buffer.readbackFootprint.Footprint.RowPitch, buffer.readbackExtent,
                                         buffer.readbackFormat);
    result.frameSerial = buffer.readbackOwnerSerial;
    result.source = buffer.readbackSource;
    buffer.readbackConsumed = true;
    return result;
}
void D3D12RhiBackend::InjectBackendFaultForTesting(std::string_view fault)
{
    if (fault == "skip-next-read-transition")
    {
        m_skipNextTransition = true;
        return;
    }
    if (fault == "remove-device")
    {
        // M7-DEVICE-LOST-INJECT：把设备**真的**移除（ID3D12Device5::RemoveDevice），
        // 不是"假装移除"。语义与真实 TDR 一致：后续 API 调用返回 DXGI_ERROR_DEVICE_REMOVED、
        // GetDeviceRemovedReason() 给出原因、DRED 输出 breadcrumbs。
        //
        // 为什么用 RemoveDevice 而不是制造 GPU 挂起：真实挂起要等 TdrDelay（默认 2s）且依赖
        // 驱动行为（同一台机器上不确定可复现），而本注入的目的是**确定性地**触发"设备丢失后的
        // 响亮失败"路径——调用方契约（不静默继续、不半提交）与移除原因无关。
        auto* device = static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
        Microsoft::WRL::ComPtr<ID3D12Device5> device5;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))))
        {
            device5->RemoveDevice();
            return;
        }
        // 老驱动/无 ID3D12Device5：响亮失败，不静默降级成"什么都没发生"。
        throw std::runtime_error("remove-device injection requires ID3D12Device5");
    }
    // 未知名称交给基类默认实现（std::runtime_error），与 D3D11 及基类契约一致。
    NativeRhiBackend::InjectBackendFaultForTesting(fault);
}

NativeBackendReport D3D12RhiBackend::Report(bool census)
{
    const auto validation = m_device->DrainInfoQueue();
    for (const auto& message : validation.messages)
    {
        if (message.severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            ++m_diagnosticWarnings;
            m_diagnosticTrace += "native-message=" + std::to_string(message.id) + ":" + message.description + "\n";
        }
    }
    std::ostringstream trace;
    trace << "miniengine.d3d12-native.v1\n"
          << m_diagnosticTrace << "ownerSubmitted=" << m_lastOwnerSerial << " ownerCompleted=" << m_completedOwner
          << " actualCompleted=" << m_completedActual << " submittedBatches=" << m_submittedBatches
          << " barriers=" << m_barriers << " discardNoOps=" << m_discardNoOps << " injectedFaults=" << m_injectedFaults
          << " trackedResources=" << m_stateTracker.TrackedResourceCount()
          << " lastRecordingTraceHash=" << m_stateTracker.TraceHash() << '\n'
          << "descriptorRanges=" << m_srvStaging.ActiveRangeCount() << ',' << m_srvVisible.ActiveRangeCount() << ','
          << m_samplerStaging.ActiveRangeCount() << ',' << m_samplerVisible.ActiveRangeCount() << ','
          << m_rtvHeap.ActiveRangeCount() << ',' << m_dsvHeap.ActiveRangeCount() << '\n'
          << "psoCreated=" << m_psoFactory.Stats().created << " psoCacheHits=" << m_psoFactory.Stats().cacheHits
          << " psoFailed=" << m_psoFactory.Stats().failedCreations << '\n'
          << "uploadRingAllocations=" << m_uploadManager.Stats().ringAllocations
          << " dedicatedAllocations=" << m_uploadManager.Stats().dedicatedAllocations
          << " dedicatedPending=" << m_uploadManager.RetiredDedicatedCount() << '\n';
    std::uint64_t unexpectedNativeObjects = 0;
    if (census && m_capabilities.debugLayerEnabled)
    {
        auto* device = static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
        Microsoft::WRL::ComPtr<ID3D12DebugDevice> debug;
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&debug)), "query census device");
        ThrowIfFailed(debug->ReportLiveDeviceObjects(
                          static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)),
                      "native census");
        const auto live = m_device->DrainInfoQueue();
        bool sawDevice = false;
        for (const auto& message : live.messages)
        {
            sawDevice = sawDevice || message.id == D3D12_MESSAGE_ID_LIVE_DEVICE;
            trace << "census.message=" << message.id << ':' << message.description << '\n';
            // 此时 backend 的 infrastructure 仍在；按类型及具体对象身份排除。
            // 不能按所有 RESOURCE/DESCRIPTORHEAP 类型整体忽略。
            std::vector<const void*> allowed;
            if (message.id == D3D12_MESSAGE_ID_LIVE_RESOURCE)
                allowed = {&m_uploadRing.Native()};
            else if (message.id == D3D12_MESSAGE_ID_LIVE_DESCRIPTORHEAP)
                allowed = {&m_srvStaging.Native(),     &m_srvVisible.Native(), &m_samplerStaging.Native(),
                           &m_samplerVisible.Native(), &m_rtvHeap.Native(),    &m_dsvHeap.Native()};
            else if (message.id == D3D12_MESSAGE_ID_LIVE_ROOTSIGNATURE)
                allowed = {m_m5RootSignature.Get()};
            else if (message.id != D3D12_MESSAGE_ID_LIVE_PIPELINESTATE)
                continue;
            bool known = false;
            for (const auto* object : allowed)
            {
                std::ostringstream address;
                address << object;
                auto expected = address.str(), actual = message.description;
                std::transform(expected.begin(), expected.end(), expected.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(actual.begin(), actual.end(), actual.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                known = known || actual.find(expected) != std::string::npos;
            }
            if (!known)
                ++unexpectedNativeObjects;
        }
        if (!sawDevice)
            throw std::runtime_error("native census did not contain its device");
        trace << "census.sawDevice=true unexpectedNativeResources=" << unexpectedNativeObjects << '\n';
    }
    else if (census)
        trace << "census.available=false debugLayer=false\n";
    NativeBackendReport report;
    report.warningErrors = m_diagnosticWarnings;
    const auto descriptorRanges = m_srvStaging.ActiveRangeCount() + m_srvVisible.ActiveRangeCount() +
                                  m_samplerStaging.ActiveRangeCount() + m_samplerVisible.ActiveRangeCount() +
                                  m_rtvHeap.ActiveRangeCount() + m_dsvHeap.ActiveRangeCount();
    const auto querySlots = m_nextQueryIndex - m_freeQueryIndices.size();
    report.liveResources =
        std::max({m_livePayloadResources, unexpectedNativeObjects, static_cast<std::uint64_t>(descriptorRanges),
                  static_cast<std::uint64_t>(querySlots)});
    report.submittedBatches = m_submittedBatches;
    report.completedSerial = m_completedOwner;
    report.barriers = m_barriers;
    report.discardNoOps = m_discardNoOps;
    report.descriptorRanges = static_cast<std::uint64_t>(descriptorRanges);
    report.uploadBytes = m_uploadRing.IsMapped() ? m_uploadRing.Allocator().OccupiedBytes() : 0;
    report.injectedFaults = m_injectedFaults;
    report.trace = trace.str();
    return report;
}
} // namespace MiniEngine::Rhi::D3D12
