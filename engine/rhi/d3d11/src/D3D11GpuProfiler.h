// ============================================================================
// D3D11GpuProfiler.h — D3D11 backend-private GPU 观测（M7-02）
// 里程碑：M7-02
// 职责：把 RHI 的 BeginLabel/EndLabel 事件映射成 Tracy 的 GPU zone，并每帧回读
//       timestamp。Tracy 与 D3D11 类型都不出本目录；ME_ENABLE_TRACY=0 时工厂返回
//       nullptr，调用点在空指针上短路，渲染行为与资源状态完全不变。
// 线程约束：创建/使用/销毁都必须在 render thread（后端自身的线程归属不变）。
// ============================================================================

#pragma once

#include <memory>

namespace MiniEngine::Rhi::D3D11
{
class GpuProfiler
{
  public:
    virtual ~GpuProfiler() = default;
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // begin/end 必须与 RHI 的 label 事件一一配对（zone 作用域由本类在堆上持有）。
    virtual void BeginZone(const char* name) noexcept = 0;
    virtual void EndZone() noexcept = 0;
    // D3D11 无帧级轮转需求（保留接口以与 D3D12 对齐）。
    virtual void NewFrame() noexcept = 0;
    // 每帧提交之后调用一次：结束 disjoint 段并回读已完成的 timestamp。
    virtual void Collect() noexcept = 0;
    // D3D12 需要知道 zone 写入哪个 command list；D3D11 忽略（保持调用点统一）。
    virtual void SetCommandList(void* commandList) noexcept = 0;

  protected:
    GpuProfiler() = default;
};

// device 与 immediateContext 为 ID3D11Device* / ID3D11DeviceContext*（保持本头不含
// D3D 类型）。Tracy 关闭时返回 nullptr。
[[nodiscard]] std::unique_ptr<GpuProfiler> CreateGpuProfiler(void* device, void* immediateContext);
} // namespace MiniEngine::Rhi::D3D11
