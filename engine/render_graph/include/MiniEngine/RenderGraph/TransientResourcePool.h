#pragma once
#include <MiniEngine/Rhi/IRhiDevice.h>
#include <memory>

namespace MiniEngine::RenderGraph
{
struct TransientPoolStatistics final
{
    std::uint64_t created = 0, reused = 0, retired = 0;
    // descriptor 的 texel/buffer payload 估算，不声称等于驱动 heap 实际占用。
    std::uint64_t bytes = 0, highWaterBytes = 0;
    std::uint64_t resources = 0, highWaterResources = 0;
};
// 单线程，device 必须活得更久。每帧至多一次 lease；BeginFrame 已等待 lane 安全。
// 跨设备能力/对齐类由绑定 device 隔离；池不等待 GPU、不改变 pass 顺序。
// 正常帧资源保留到该 lane 下一次 lease；失效/析构走 RHI deferred Destroy。
class TransientResourcePool final
{
  public:
    explicit TransientResourcePool(Rhi::IRhiDevice& device);
    ~TransientResourcePool();
    TransientResourcePool(const TransientResourcePool&) = delete;
    TransientResourcePool& operator=(const TransientResourcePool&) = delete;
    [[nodiscard]] TransientPoolStatistics Statistics() const;
    // 显式清空用于 swapchain/场景退出；不得在 lease 内调用。
    void Clear();

  private:
    friend class CompiledRenderGraph;
    void BeginLease(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame);
    Rhi::TextureHandle Acquire(const Rhi::TextureDesc& desc);
    Rhi::BufferHandle Acquire(const Rhi::BufferDesc& desc);
    void EndLease(bool succeeded);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::RenderGraph
