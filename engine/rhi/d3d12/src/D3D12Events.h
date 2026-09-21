// ============================================================================
// D3D12Events.h — WinPixEventRuntime 事件作用域与稳定 draw 身份。
// 里程碑：M5-11；供帧抓取定位 entity/material/mesh，不参与渲染状态。
// ============================================================================
#pragma once
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/World/RenderPacket.h>
#include <pix3.h>
#include <string>
namespace MiniEngine::Rhi::D3D12
{
class EventScope final
{
  public:
    EventScope(ID3D12GraphicsCommandList& list, const wchar_t* name) : m_list(list)
    {
        PIXBeginEvent(&m_list, PIX_COLOR_DEFAULT, L"%s", name);
    }
    ~EventScope()
    {
        End();
    }
    // 整帧区域必须在 Command List Close 前结束，异常路径仍由析构兜底。
    void End() noexcept
    {
        if (m_active)
        {
            PIXEndEvent(&m_list);
            m_active = false;
        }
    }
    EventScope(const EventScope&) = delete;
    EventScope& operator=(const EventScope&) = delete;

  private:
    ID3D12GraphicsCommandList& m_list;
    bool m_active = true;
};
inline void MarkDraw(ID3D12GraphicsCommandList& list, const World::RenderDraw& draw)
{
    const std::string name = "entity=" + std::to_string(draw.entityIndex) +
                             " material=" + draw.materialId.ToHexString() + " mesh=" + draw.meshId.ToHexString();
    PIXSetMarker(&list, PIX_COLOR_DEFAULT, "%s", name.c_str());
}
} // namespace MiniEngine::Rhi::D3D12
