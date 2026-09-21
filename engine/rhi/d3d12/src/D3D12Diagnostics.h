// ============================================================================
// D3D12Diagnostics.h — D3D12 诊断内部模块（InfoQueue / 命名 / DRED）
// 里程碑：M5（02 篇环境、Device 与诊断基线；手抄清单第 2 条）
// 职责：把三件诊断事务集中在后端边界内部：为 ID3D12Object 设置稳定调试名
//       （PIX 与 DRED 报告的可读性来源）、按 Gate 语义取走 InfoQueue 全部消息、
//       以及 Device 移除后输出原因 HRESULT + DRED breadcrumbs + page fault 分配链。
// 内部性说明：本头位于 src/，只被 D3D12Device 实现使用；组合根经
//       D3D12Device 的公共 API（DrainInfoQueue / ReportDeviceRemoved）消费结果，
//       因此 ID3D12* 类型不会出现在任何公共头里。
// 关联：docs/architecture/README.md（InfoQueue / Device removed 节）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

namespace MiniEngine::Rhi::D3D12::Internal
{
// Device removed 报告中的非 DRED 运行时上下文；由组合根在报告时填入。
struct DeviceRemovedContext
{
    std::uint64_t currentFence = 0;
    std::uint64_t lastSubmittedFence = 0;
    std::uint64_t completedFence = 0;
    std::vector<std::string> recentPixEvents;
    std::string assetRevision;
    std::string shaderRevision;
};

// 纯格式化入口：允许 CPU 单测构造原生 DRED 输出，不需要制造真实 GPU fault。
[[nodiscard]] std::string FormatDeviceRemovedReport(HRESULT reason,
                                                    const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT* breadcrumbs,
                                                    const D3D12_DRED_PAGE_FAULT_OUTPUT* pageFault,
                                                    const DeviceRemovedContext& context,
                                                    std::size_t maxBreadcrumbNodes = 64,
                                                    std::size_t maxAllocationNodes = 16);
// 为设备子对象设置稳定的调试名（ID3D12Object::SetName，宽字符）。
//
// 名称会出现在 PIX 资源检查器与 DRED 报告里，是"PIX 中每个对象可解释"的基础；
// 命名必须紧跟创建，且同层对象使用同一前缀（M5.Device / M5.Queue / ...）。
// 空名称或空对象直接忽略。
//
// 失败：SetName 返回失败 HRESULT 时抛 HResultError。
void SetDebugName(ID3D12Object* object, std::wstring_view name);

// 取走 InfoQueue 当前存储的全部消息并清空队列（Gate 的"终点"半边）。
//
// 本后端不设置 retrieval filter，因此取 GetNumStoredMessages 的全量；
// 每条消息保存 category/severity/ID/description 原值，不做任何压制——
// 只有已证明的系统噪声才允许按 message ID 过滤并写入 ADR（02 篇）。
//
// 参数：
//   queue  —— Device 查询出的 ID3D12InfoQueue（可为空：非 debug 模式无队列，直接空报告）
//   report —— 输出：追加到既有报告（不清空调用方传入的容器）
// 失败：GetMessage 两次调用（探测大小、取出内容）的 HRESULT 失败抛 HResultError。
void DrainInfoQueue(ID3D12InfoQueue* queue, ValidationReport& report);

// 为 DXGI 对象设置稳定调试名（IDXGIObject 走 SetPrivateData(WKPDID_D3DDebugObjectName)
// 而不是 ID3D12Object::SetName——两者是不同接口，交换链/适配器属于前者）。
// 空对象或空名称直接忽略；失败抛 HResultError。
void SetDxgiDebugName(IDXGIObject* object, std::string_view name);

// 打印 live-object 报告（04 篇"退出无 live object"的取证入口）。
//
// 经 ID3D12DebugDevice::ReportLiveDeviceObjects(DETAIL) 输出到
// DebugView：列出仍被引用的对象及其引用计数。调用时机：渲染器/队列/交换链等
// 全部释放之后、Device 析构之前——报告为空即"无应用侧泄漏"。
// 非 debug 模式或接口缺失时为无害 no-op（没有调试层就没有该接口，不是失败）。
//
// 参数：
//   device —— 待检查的 Device；为空直接返回
// 返回：真的输出了报告（查到 ID3D12DebugDevice）为 true；非 debug 模式/接口缺失
//       为 false——调用方据此写"报告是否发出"，不得无条件写 true。
[[nodiscard]] bool ReportLiveDeviceObjects(ID3D12Device* device);

// 输出 Device 移除诊断（02 篇「Device removed」的取证三件事）：
//   1. GetDeviceRemovedReason 的 HRESULT（由调用方传入，避免二次查询不一致）；
//   2. DRED auto breadcrumbs：逐节点输出命令列表/队列名、面包屑总数与
//      最后完成的位置（GPU 停在哪一步）；
//   3. page fault 输出：出错 GPU VA 与"当时存在/最近释放"两条分配链。
//
// 数据来自 ID3D12DeviceRemovedExtendedData；DRED 未启用或接口缺失时静默跳过
// （原因 HRESULT 仍然输出——不能因为诊断缺失而丢失移除原因本身）。
//
// 参数：
//   device —— 已移除（或疑似移除）的 Device；为空直接返回
//   reason —— GetDeviceRemovedReason 的返回值
void ReportDeviceRemoved(ID3D12Device* device, HRESULT reason);
void ReportDeviceRemoved(ID3D12Device* device, HRESULT reason, const DeviceRemovedContext& context);
} // namespace MiniEngine::Rhi::D3D12::Internal
