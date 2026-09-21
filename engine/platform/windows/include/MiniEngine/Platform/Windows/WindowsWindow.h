#pragma once

#include <MiniEngine/Core/Input.h>

#include <cstdint>
#include <memory>
#include <string>

namespace MiniEngine
{
struct WindowDesc
{
    std::wstring title = L"MiniEngine";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool visible = true; // 后台验收可创建真实隐藏窗口，不改变交互窗口默认行为。
};

class WindowsWindow
{
  public:
    WindowsWindow(const WindowDesc& desc, InputState& input);
    ~WindowsWindow();

    WindowsWindow(const WindowsWindow&) = delete;
    WindowsWindow& operator=(const WindowsWindow&) = delete;
    WindowsWindow(WindowsWindow&&) = delete;
    WindowsWindow& operator=(WindowsWindow&&) = delete;

    [[nodiscard]] bool PumpMessages();
    // 阻塞等待消息到达，或超过 timeoutMilliseconds 后返回（0 表示立即返回，供暂停态周期性轮询）。
    void WaitForMessage(std::uint32_t timeoutMilliseconds) const;
    void RequestClose();

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] bool IsMinimized() const noexcept;
    [[nodiscard]] bool IsInSizeMove() const noexcept;
    [[nodiscard]] bool ShouldPause() const noexcept;
    [[nodiscard]] bool ConsumeTimingResetRequest() noexcept;

    [[nodiscard]] std::uint32_t ClientWidth() const noexcept;
    [[nodiscard]] std::uint32_t ClientHeight() const noexcept;
    [[nodiscard]] std::uint32_t Dpi() const noexcept;
    // 真实改变窗口客户区尺寸（M7-RESIZE-PATH）：走 SetWindowPos + AdjustWindowRectEx，
    // 因此会产生真正的 WM_SIZE —— 等价用户拖动边缘，而不是只改交换链 extent。
    void ResizeClient(std::uint32_t width, std::uint32_t height);
    // 返回不透明窗口句柄（实际为 HWND 的 void*），供 RHI 后端创建交换链而不泄漏 Win32 类型。
    [[nodiscard]] void* NativeHandle() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine