// ============================================================================
// D3D12GpuProfiler.h — D3D12 backend-private GPU 观测（M7-02）
// 里程碑：M7-02
// 职责：把 RHI 的 BeginLabel/EndLabel 事件映射成 Tracy 的 GPU zone（写入当前
//       command list），每帧 NewFrame/Collect 管理查询轮转与回读。Tracy 与 D3D12
//       类型都不出本目录；ME_ENABLE_TRACY=0 时工厂返回 nullptr。
// 线程约束：创建/使用/销毁都必须在 render thread；command list 不得跨线程录制。
// ============================================================================

#pragma once

#include <memory>

namespace MiniEngine::Rhi::D3D12
{
class GpuProfiler
{
  public:
    virtual ~GpuProfiler() = default;
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // begin/end 必须与 RHI 的 label 事件一一配对，且都在同一个已打开的 command list 内。
    virtual void BeginZone(const char* name) noexcept = 0;
    virtual void EndZone() noexcept = 0;
    // 每帧开始录制前调用一次：把上一帧的查询区间入队并 signal payload fence。
    virtual void NewFrame() noexcept = 0;
    // 每帧提交之后调用一次：回读已完成的 timestamp。
    virtual void Collect() noexcept = 0;
    // 当前录制中的 ID3D12GraphicsCommandList*（GPU zone 必须写进录制目标）。
    virtual void SetCommandList(void* commandList) noexcept = 0;

  protected:
    GpuProfiler() = default;
};

// device 与 queue 为 ID3D12Device* / ID3D12CommandQueue*（保持本头不含 D3D 类型）。
// Tracy 关闭时返回 nullptr。
[[nodiscard]] std::unique_ptr<GpuProfiler> CreateGpuProfiler(void* device, void* queue);
} // namespace MiniEngine::Rhi::D3D12
