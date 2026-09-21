// ============================================================================
// D3D12FrameContext.h — 单帧提交上下文（allocator + submittedFenceValue）
// 里程碑：M5（03 篇 Queue、Fence 与 FrameContext；手抄清单第 1 条）
// 职责：固定"三帧在飞行"的所有权单元：每个 FrameContext 拥有一个 DIRECT
//       CommandAllocator 与"该 context 最后一次提交时的 fence 值"。它是
//       BeginFrame 等待判定（fence 未完成才等）与 allocator 复用安全性的
//       唯一数据来源——allocator 在 submittedFenceValue 完成前绝不能 Reset。
// 内部性说明：本类型与 D3D12Queue 同属后端内部（src/），只有渲染器与
//       测试消费；模板把它放在 include/，与仓库"D3D 内部类型不进公共头"
//       的惯例不符，落地时移入 src/（M5-01 已确立的同型偏差，见其记录 §5）。
// 演进约定：upload marker（06 篇）、query/readback slice（08 篇）落地时在此
//       增补字段——它们同样按 submittedFenceValue 打 tag、按完成 fence 回收。
// 关联：docs/architecture/README.md（固定结构与 BeginFrame）
//       docs/architecture/DECISIONS.md（决策 1/2/4）
// ============================================================================
#pragma once

#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

namespace MiniEngine::Rhi::D3D12
{
// 一帧的提交上下文。值语义小对象，但 allocator 本身是 COM 引用计数资源。
struct D3D12FrameContext final
{
    // 本 context 专属的命令分配器；只在 submittedFenceValue 完成后 Reset。
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> directAllocator;

    // 该 context 最后一次提交的 fence 值；0 表示从未提交（无需等待）。
    // 只能由 D3D12Queue::ExecuteAndSignal 在 Signal 成功后写回（单一写点）。
    std::uint64_t submittedFenceValue = 0;

    // 诊断计数：本 context 已完成的帧数（debug trace 用，不参与判定）。
    std::uint64_t debugFrameCount = 0;
};
} // namespace MiniEngine::Rhi::D3D12
