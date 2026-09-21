// ============================================================================
// D3D11GpuProfiler.cpp — Tracy D3D11 GPU zone 适配（M7-02）
// 关联：engine/rhi/d3d11/src/D3D11GpuProfiler.h
//       docs/architecture/README.md（第 4 步 GPU zones）
// ============================================================================

#include "D3D11GpuProfiler.h"

#include <MiniEngine/Profiling/Profile.h>

#if ME_ENABLE_TRACY
#include <tracy/TracyD3D11.hpp>
#endif

#include <cstring>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D11
{
namespace
{
#if ME_ENABLE_TRACY
class TracyGpuProfiler final : public GpuProfiler
{
  public:
    TracyGpuProfiler(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_d3dDevice(device), m_d3dContext(context)
    {
        // 刻意不在构造时创建 Tracy context：它会 AddRef device/context 并在设备上建立
    }

    ~TracyGpuProfiler() override
    {
        // 销毁前必须已经有真实完成证明（后端在析构里先 WaitIdle），否则在途 timestamp
        // 查询会被 Tracy 的析构忙等吞掉，掩盖后端自身的生命周期问题。
        DrainZones();
        DestroyContext();
    }

    void BeginZone(const char* name) noexcept override
    {
        EnsureContext();
        if (m_tracy == nullptr || name == nullptr)
            return;
        try
        {
            // 名字由 Tracy 复制（AllocSourceLocation），因此动态 pass 名是安全的；
            // 但名字的调用方仍需避免逐帧高基数字符串（见 M7-02 文档）。
            m_zones.push_back(new tracy::D3D11ZoneScope(m_tracy, 0, "", 0, "M7.Pass", sizeof("M7.Pass") - 1,
                                                       name, std::strlen(name), true));
        }
        catch (...)
        {
            // 观测设施失败不能破坏渲染路径：放弃该 zone。
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
    }

    void Collect() noexcept override
    {
        if (m_tracy != nullptr)
            TracyD3D11Collect(m_tracy);
    }

    void SetCommandList(void*) noexcept override
    {
    }

  private:
    // 连接时创建、断开时释放。未连接的运行（含所有测试）因此完全不触碰设备：
    // Tracy 的 context 会 AddRef device/context 且只在自己的析构里释放，长期持有会让
    // 设备在泄漏普查（DXGI debug 队列）里表现为"仍有外部引用"。
    void EnsureContext() noexcept
    {
        const bool connected = Profiling::IsConnected();
        if (connected == (m_tracy != nullptr))
            return;
        if (connected)
        {
            m_tracy = TracyD3D11Context(m_d3dDevice, m_d3dContext);
            if (m_tracy != nullptr)
                TracyD3D11ContextName(m_tracy, "MiniEngine D3D11", sizeof("MiniEngine D3D11") - 1);
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
        TracyD3D11Collect(m_tracy);
        TracyD3D11Destroy(m_tracy);
        m_tracy = nullptr;
    }

    ID3D11Device* m_d3dDevice = nullptr;          // 非拥有；后端保证生命周期覆盖本对象
    ID3D11DeviceContext* m_d3dContext = nullptr;  // 同上
    TracyD3D11Ctx m_tracy = nullptr;              // 仅连接期间存在
    std::vector<tracy::D3D11ZoneScope*> m_zones;
};
#endif
} // namespace

std::unique_ptr<GpuProfiler> CreateGpuProfiler(void* device, void* immediateContext)
{
#if ME_ENABLE_TRACY
    if (device == nullptr || immediateContext == nullptr)
        return nullptr;
    return std::make_unique<TracyGpuProfiler>(static_cast<ID3D11Device*>(device),
                                              static_cast<ID3D11DeviceContext*>(immediateContext));
#else
    (void)device;
    (void)immediateContext;
    return nullptr;
#endif
}
} // namespace MiniEngine::Rhi::D3D11
