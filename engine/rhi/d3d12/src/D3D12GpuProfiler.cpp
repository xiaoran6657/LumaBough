// ============================================================================
// D3D12GpuProfiler.cpp — Tracy D3D12 GPU zone 适配（M7-02）
// 关联：engine/rhi/d3d12/src/D3D12GpuProfiler.h
//       docs/architecture/README.md（第 4 步 GPU zones）
// ============================================================================

#include "D3D12GpuProfiler.h"

#include <MiniEngine/Profiling/Profile.h>

#if ME_ENABLE_TRACY
#include <tracy/TracyD3D12.hpp>
#endif

#include <cstring>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
#if ME_ENABLE_TRACY
class TracyGpuProfiler final : public GpuProfiler
{
  public:
    TracyGpuProfiler(ID3D12Device* device, ID3D12CommandQueue* queue) : m_d3dDevice(device), m_d3dQueue(queue)
    {
        // 刻意不在构造时创建 Tracy context（会 AddRef device/queue 并建立时间戳堆/readback
        // buffer）；只在连接期间存在，未连接的运行与测试完全不触碰设备。
    }

    ~TracyGpuProfiler() override
    {
        DrainZones();
        DestroyContext();
    }

    void BeginZone(const char* name) noexcept override
    {
        EnsureContext();
        if (m_tracy == nullptr || name == nullptr || m_commandList == nullptr)
            return;
        try
        {
            m_zones.push_back(new tracy::D3D12ZoneScope(m_tracy, 0, "", 0, "M7.Pass", sizeof("M7.Pass") - 1,
                                                       name, std::strlen(name), m_commandList, true));
        }
        catch (...)
        {
            // 观测失败不破坏渲染路径。
        }
    }

    void EndZone() noexcept override
    {
        if (m_zones.empty())
            return;
        delete m_zones.back();
        m_zones.pop_back();
    }

    void NewFrame() noexcept override
    {
        if (m_tracy != nullptr)
            TracyD3D12NewFrame(m_tracy);
    }

    void Collect() noexcept override
    {
        if (m_tracy != nullptr)
            TracyD3D12Collect(m_tracy);
    }

    // 当前录制中的 command list：由后端在每帧开始时设置（GPU zone 必须写进录制目标）。
    void SetCommandList(void* list) noexcept override
    {
        m_commandList = static_cast<ID3D12GraphicsCommandList*>(list);
    }

  private:
    void EnsureContext() noexcept
    {
        const bool connected = Profiling::IsConnected();
        if (connected == (m_tracy != nullptr))
            return;
        if (connected && m_d3dDevice != nullptr && m_d3dQueue != nullptr)
        {
            m_tracy = TracyD3D12Context(m_d3dDevice, m_d3dQueue);
            if (m_tracy != nullptr)
                TracyD3D12ContextName(m_tracy, "MiniEngine D3D12", sizeof("MiniEngine D3D12") - 1);
        }
        else
        {
            DrainZones();
            DestroyContext();
        }
    }

    void DrainZones() noexcept
    {
        while (!m_zones.empty())
            EndZone();
    }

    void DestroyContext() noexcept
    {
        if (m_tracy == nullptr)
            return;
        // D3D12 的 Tracy 析构会忙等未完成的 payload；后端已在 WaitIdle 之后调用本析构。
        TracyD3D12Destroy(m_tracy);
        m_tracy = nullptr;
    }

    ID3D12Device* m_d3dDevice = nullptr; // 非拥有；后端保证生命周期覆盖本对象
    ID3D12CommandQueue* m_d3dQueue = nullptr;
    TracyD3D12Ctx m_tracy = nullptr; // 仅连接期间存在
    ID3D12GraphicsCommandList* m_commandList = nullptr;
    std::vector<tracy::D3D12ZoneScope*> m_zones;
};
#endif
} // namespace

std::unique_ptr<GpuProfiler> CreateGpuProfiler(void* device, void* queue)
{
#if ME_ENABLE_TRACY
    if (device == nullptr || queue == nullptr)
        return nullptr;
    return std::make_unique<TracyGpuProfiler>(static_cast<ID3D12Device*>(device),
                                              static_cast<ID3D12CommandQueue*>(queue));
#else
    (void)device;
    (void)queue;
    return nullptr;
#endif
}
} // namespace MiniEngine::Rhi::D3D12
