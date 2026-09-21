// ============================================================================
// D3D12Diagnostics.cpp — InfoQueue 取走、稳定命名与 DRED 移除取证实现
// 里程碑：M5（02 篇环境、Device 与诊断基线；手抄清单第 2 条）
// 职责：实现 D3D12Diagnostics.h 的三个入口。所有日志经 Core 的 WriteLog 走
//       OutputDebugString，可被 DebugView / Capture-DebugLog.ps1 采集——
//       验收（Gate）读的是日志与 ValidationReport，而不是肉眼观察。
// 关联：docs/architecture/README.md
//       Microsoft Learn：Use DRED to diagnose GPU faults
// ============================================================================
#include "D3D12Diagnostics.h"

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D12::Internal
{
namespace
{
// UTF-16 → UTF-8（DRED/adapter 的调试名都是宽字符；日志统一走 UTF-8）。
std::string Utf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// 用可读名替换 D3D12_MESSAGE_SEVERITY 数值（消息文本本身已含详情，这里只做分级）。
const char* SeverityName(const std::uint32_t severity)
{
    switch (severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        return "CORRUPTION";
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return "ERROR";
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return "WARNING";
    case D3D12_MESSAGE_SEVERITY_INFO:
        return "INFO";
    default:
        return "MESSAGE";
    }
}

// 消息按 severity 映射日志级别（审查意见：全部按 Warning 落日志会把良性 INFO
// 显示成 Warning 行，污染按级别做的 Gate 判读）。CORRUPTION/ERROR/Fatal、
// WARNING/Warning、INFO 及 MESSAGE/Info——文本里仍保留 SeverityName 的原词。
MiniEngine::LogLevel SeverityLogLevel(const std::uint32_t severity)
{
    switch (severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return MiniEngine::LogLevel::Fatal;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return MiniEngine::LogLevel::Warning;
    default:
        return MiniEngine::LogLevel::Info;
    }
}
} // namespace

void SetDebugName(ID3D12Object* object, const std::wstring_view name)
{
    // 空对象或空名称没有命名意义，直接忽略（与 D3D11 的 SetDebugObjectName 同语义）。
    if (object == nullptr || name.empty())
    {
        return;
    }
    ThrowIfFailed(object->SetName(name.data()), "ID3D12Object::SetName");
}

void DrainInfoQueue(ID3D12InfoQueue* queue, ValidationReport& report)
{
    if (queue == nullptr)
    {
        return; // 非 debug 模式没有 InfoQueue：空报告即 Gate 通过
    }

    const UINT64 discarded = queue->GetNumMessagesDiscardedByMessageCountLimit();
    if (discarded != 0U)
    {
        throw std::runtime_error("D3D12 InfoQueue discarded " + std::to_string(discarded) + " message(s)");
    }

    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index)
    {
        // 两次调用协议：先以 nullptr 探测序列化大小，再按该大小取出内容。
        SIZE_T bytes = 0;
        ThrowIfFailed(queue->GetMessage(index, nullptr, &bytes), "ID3D12InfoQueue::GetMessage(size)");
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(queue->GetMessage(index, message, &bytes), "ID3D12InfoQueue::GetMessage");

        ValidationMessage entry;
        entry.category = static_cast<std::uint32_t>(message->Category);
        entry.severity = static_cast<std::uint32_t>(message->Severity);
        entry.id = static_cast<std::uint32_t>(message->ID);
        entry.description = message->pDescription != nullptr ? message->pDescription : "";
        report.messages.push_back(entry);

        // 逐条落日志：Gate 失败时证据直接可读，不必依赖测试进程的 stdout；
        // 级别随 severity 映射，避免良性 INFO 冒充 Warning。
        MiniEngine::WriteLog(SeverityLogLevel(entry.severity),
                             std::string{"d3d12 infoqueue "} + SeverityName(entry.severity) +
                                 " id=" + std::to_string(entry.id) + ": " + entry.description);
    }
    queue->ClearStoredMessages();
}

void SetDxgiDebugName(IDXGIObject* object, const std::string_view name)
{
    if (object == nullptr || name.empty())
    {
        return;
    }
    // DXGI 对象没有 ID3D12Object::SetName，只有 SetPrivateData + WKPDID_D3DDebugObjectName。
    ThrowIfFailed(object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data()),
                  "IDXGIObject::SetPrivateData(WKPDID_D3DDebugObjectName)");
}

bool ReportLiveDeviceObjects(ID3D12Device* device)
{
    if (device == nullptr)
    {
        return false;
    }

    // 非 debug 模式没有 ID3D12DebugDevice：跳过并返回 false（不是失败，只是没有该
    // 诊断通道）。返回值的用途：调用方据此写"报告是否真的发出"，避免把"没跑"记成"跑了"。
    ComPtr<ID3D12DebugDevice> debugDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
    {
        return false;
    }

    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 live-object report summary begin (all objects)");
    // DETAIL 保留内部对象；此接口仍持有 Device，只用于中间诊断，不能充当退出门禁。
    ThrowIfFailed(debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL),
                  "ID3D12DebugDevice::ReportLiveDeviceObjects");
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 live-object report live-object end");
    return true;
}

namespace
{
const char* BreadcrumbOpName(const D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:
        return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:
        return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:
        return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
        return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
        return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:
        return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
        return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
        return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
        return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:
        return "ResolveSubresource";
    default:
        return "Unknown";
    }
}
std::string NodeName(const char* a, const wchar_t* w)
{
    if (w != nullptr)
        return Utf8(w);
    if (a != nullptr)
        return a;
    return "<unnamed>";
}
std::string HexVa(const D3D12_GPU_VIRTUAL_ADDRESS va)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(va));
    return text;
}
} // namespace

std::string FormatDeviceRemovedReport(const HRESULT reason, const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT* breadcrumbs,
                                      const D3D12_DRED_PAGE_FAULT_OUTPUT* pageFault,
                                      const DeviceRemovedContext& context, const std::size_t maxBreadcrumbNodes,
                                      const std::size_t maxAllocationNodes)
{
    std::string out;
    char reasonText[32]{};
    std::snprintf(reasonText, sizeof(reasonText), "0x%08lX", static_cast<unsigned long>(reason));
    out += "d3d12 device removed reason=" + std::string(reasonText) + "\n";
    out += "context currentFence=" + std::to_string(context.currentFence) +
           " lastSubmittedFence=" + std::to_string(context.lastSubmittedFence) +
           " completedFence=" + std::to_string(context.completedFence) + "\n";
    out +=
        "context assetRevision=" + (context.assetRevision.empty() ? std::string{"<missing>"} : context.assetRevision) +
        " shaderRevision=" + (context.shaderRevision.empty() ? std::string{"<missing>"} : context.shaderRevision) +
        "\n";
    if (context.recentPixEvents.empty())
        out += "context recentPIXevents=<none>\n";
    else
        for (std::size_t i = 0; i < context.recentPixEvents.size(); ++i)
            out += "context recentPIXevent[" + std::to_string(i) + "]=" + context.recentPixEvents[i] + "\n";
    if (breadcrumbs == nullptr || breadcrumbs->pHeadAutoBreadcrumbNode == nullptr)
        out += "breadcrumbs=<unavailable>\n";
    else
    {
        std::size_t count = 0;
        const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs->pHeadAutoBreadcrumbNode;
        for (; node != nullptr && count < maxBreadcrumbNodes; node = node->pNext, ++count)
        {
            const UINT completed = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0U;
            out += "breadcrumb[" + std::to_string(count) +
                   "] queue=" + NodeName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW) +
                   " list=" + NodeName(node->pCommandListDebugNameA, node->pCommandListDebugNameW) +
                   " count=" + std::to_string(node->BreadcrumbCount) + " lastCompleted=" + std::to_string(completed);
            const UINT historyCount = (std::min)(node->BreadcrumbCount, 65536U);
            if (completed == 0U || completed > node->BreadcrumbCount || node->pCommandHistory == nullptr ||
                historyCount == 0U)
                out += " lastOperation=<unavailable>\n";
            else
            {
                const UINT index = (completed - 1U) % historyCount;
                out += " lastOperation=" + std::to_string(index) + ":" +
                       BreadcrumbOpName(node->pCommandHistory[index]) + "\n";
            }
        }
        if (node != nullptr)
            out += "breadcrumbs=<truncated>\n";
    }
    if (pageFault == nullptr)
        out += "pageFault=<unavailable>\n";
    else
    {
        out += "pageFault faultVA=" + HexVa(pageFault->PageFaultVA) + "\n";
        const auto appendChain = [&out, maxAllocationNodes](const char* label, const D3D12_DRED_ALLOCATION_NODE* head)
        {
            if (head == nullptr)
            {
                out += std::string(label) + "=<empty>\n";
                return;
            }
            std::size_t count = 0;
            const D3D12_DRED_ALLOCATION_NODE* node = head;
            for (; node != nullptr && count < maxAllocationNodes; node = node->pNext, ++count)
                out += std::string(label) + "[" + std::to_string(count) +
                       "] name=" + NodeName(node->ObjectNameA, node->ObjectNameW) +
                       " type=" + std::to_string(static_cast<UINT>(node->AllocationType)) + "\n";
            if (node != nullptr)
                out += std::string(label) + "=<truncated>\n";
        };
        appendChain("pageFault existing", pageFault->pHeadExistingAllocationNode);
        appendChain("pageFault recentFree", pageFault->pHeadRecentFreedAllocationNode);
    }
    return out;
}
void ReportDeviceRemoved(ID3D12Device* device, const HRESULT reason, const DeviceRemovedContext& context)
{
    if (device == nullptr)
        return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    bool haveBreadcrumbs = false;
    bool havePageFault = false;
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred))))
    {
        haveBreadcrumbs = SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs));
        havePageFault = SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault));
    }
    const std::string report = FormatDeviceRemovedReport(reason, haveBreadcrumbs ? &breadcrumbs : nullptr,
                                                         havePageFault ? &pageFault : nullptr, context);
    MiniEngine::WriteLog(MiniEngine::LogLevel::Fatal, report);
}
void ReportDeviceRemoved(ID3D12Device* device, const HRESULT reason)
{
    ReportDeviceRemoved(device, reason, DeviceRemovedContext{});
}
} // namespace MiniEngine::Rhi::D3D12::Internal
