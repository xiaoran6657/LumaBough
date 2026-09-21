#include <MiniEngine/Platform/Windows/WindowsWindow.h>

#include <Windows.h>
#include <windowsx.h>

// WinUser.h 定义 IsMinimized(hwnd) 宏，会污染 WindowsWindow 的成员函数定义。
#undef IsMinimized

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace MiniEngine
{
namespace
{
constexpr wchar_t kWindowClassName[] = L"MiniEngine.M1.Window";

// 用当前线程最后一个错误码（GetLastError）组装 std::system_error，附上失败的操作名。
std::system_error MakeLastError(const char* operation)
{
    return std::system_error{static_cast<int>(GetLastError()), std::system_category(), operation};
}

// 把窗口尺寸从 uint32_t 安全转换为 Win32 要求的 int；超过 INT_MAX 时拒绝并抛异常，
// 避免 CreateWindowExW 收到负尺寸。
int CheckedDimension(const std::uint32_t value)
{
    if (value > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        throw std::invalid_argument{"window dimension exceeds INT_MAX"};
    }

    return static_cast<int>(value);
}

// 把 Win32 虚拟键码映射到引擎内部 Key 枚举：先按字母/数字/F 键区间推导，再查表翻译
// 特殊键；未识别键落到 Key::Unknown。
Key TranslateVirtualKey(const WPARAM virtualKey)
{
    const auto value = static_cast<std::uint32_t>(virtualKey);

    if (value >= 'A' && value <= 'Z')
    {
        const auto offset = value - 'A';
        return static_cast<Key>(static_cast<std::uint32_t>(Key::A) + offset);
    }

    if (value >= '0' && value <= '9')
    {
        const auto offset = value - '0';
        return static_cast<Key>(static_cast<std::uint32_t>(Key::Digit0) + offset);
    }

    if (value >= VK_F1 && value <= VK_F12)
    {
        const auto offset = value - VK_F1;
        return static_cast<Key>(static_cast<std::uint32_t>(Key::F1) + offset);
    }

    switch (value)
    {
    case VK_ESCAPE:
        return Key::Escape;
    case VK_SPACE:
        return Key::Space;
    case VK_RETURN:
        return Key::Enter;
    case VK_TAB:
        return Key::Tab;
    case VK_BACK:
        return Key::Backspace;
    case VK_LEFT:
        return Key::Left;
    case VK_RIGHT:
        return Key::Right;
    case VK_UP:
        return Key::Up;
    case VK_DOWN:
        return Key::Down;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Key::Shift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Key::Control;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Key::Alt;
    case VK_INSERT:
        return Key::Insert;
    case VK_DELETE:
        return Key::Delete;
    case VK_HOME:
        return Key::Home;
    case VK_END:
        return Key::End;
    case VK_PRIOR:
        return Key::PageUp;
    case VK_NEXT:
        return Key::PageDown;
    default:
        return Key::Unknown;
    }
}
} // namespace

class WindowsWindow::Impl
{
  public:
    // 注册窗口类并创建原生窗口：先校验模块句柄，再计算带非客户区的外框尺寸，最后
    // 创建窗口并同步客户区尺寸与 DPI。任一步失败都回滚已获取的资源后重抛。
    Impl(const WindowDesc& desc, InputState& input) : m_input{input}, m_instance{GetModuleHandleW(nullptr)}
    {
        if (m_instance == nullptr)
        {
            throw MakeLastError("GetModuleHandleW");
        }

        try
        {
            RegisterWindowClass();

            constexpr DWORD style = WS_OVERLAPPEDWINDOW;
            RECT windowRest{0, 0, CheckedDimension(desc.width), CheckedDimension(desc.height)};

            if (AdjustWindowRectEx(&windowRest, style, FALSE, 0) == FALSE)
            {
                throw MakeLastError("AdjustWindowRectEx");
            }

            const int windowWidth = static_cast<int>(windowRest.right - windowRest.left);
            const int windowHeight = static_cast<int>(windowRest.bottom - windowRest.top);

            const HWND window =
                CreateWindowExW(0, kWindowClassName, desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                windowWidth, windowHeight, nullptr, nullptr, m_instance, this);

            if (window == nullptr)
            {
                throw MakeLastError("CreateWindowExW");
            }

            m_window = window;
            UpdateClientSize();

            const UINT windowDpi = GetDpiForWindow(m_window);
            m_dpi = windowDpi == 0 ? USER_DEFAULT_SCREEN_DPI : windowDpi;

            if (desc.visible)
                ShowWindow(m_window, SW_SHOWDEFAULT);
            UpdateWindow(m_window);
        }
        catch (...)
        {
            DestroyOwnedResources();
            throw;
        }
    }

    ~Impl()
    {
        DestroyOwnedResources();
    }

    // 泵取消息队列并分发到窗口过程；WM_QUIT 置停运行标志并跳过分发。返回窗口是否仍运行。
    [[nodiscard]] bool PumpMessages()
    {
        MSG message{};

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                m_running = false;
                continue;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return m_running;
    }

    // 运行期间无消息时阻塞等待至 timeoutMilliseconds（0 立即返回），进入空闲休眠；
    // 非运行态直接返回避免卡住主循环。用 MsgWaitForMultipleObjectsEx 以便暂停态也能
    // 周期性醒来做热重载轮询，而不是无限阻塞在 WaitMessage。
    void WaitForMessage(std::uint32_t timeoutMilliseconds) const
    {
        if (m_running)
        {
            const DWORD waitResult =
                MsgWaitForMultipleObjectsEx(0, nullptr, timeoutMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (waitResult == WAIT_FAILED)
            {
                throw MakeLastError("MsgWaitForMultipleObjectsEx");
            }
        }
    }

    // 向窗口投递 WM_CLOSE，触发窗口过程里的关闭销毁流程。
    void RequestClose()
    {
        if (m_window != nullptr)
        {
            PostMessageW(m_window, WM_CLOSE, 0, 0);
        }
    }

    [[nodiscard]] bool IsActive() const noexcept
    {
        return m_active;
    }

    [[nodiscard]] bool IsMinimized() const noexcept
    {
        return m_minimized;
    }

    [[nodiscard]] bool IsInSizeMove() const noexcept
    {
        return m_inSizeMove;
    }

    // 窗口停止运行、失焦、最小化或处于尺寸拖动中任一状态成立即应暂停渲染。
    [[nodiscard]] bool ShouldPause() const noexcept
    {
        return !m_running || !m_active || m_minimized || m_inSizeMove;
    }

    // 一次性消费计时重置标志：返回当前请求值并立即清零，保证只被处理一次。
    [[nodiscard]] bool ConsumeTimingResetRequest() noexcept
    {
        const bool requested = m_timingResetRequested;
        m_timingResetRequested = false;
        return requested;
    }

    [[nodiscard]] std::uint32_t ClientWidth() const noexcept
    {
        return m_clientWidth;
    }

    [[nodiscard]] std::uint32_t ClientHeight() const noexcept
    {
        return m_clientHeight;
    }

    [[nodiscard]] std::uint32_t Dpi() const noexcept
    {
        return m_dpi;
    }

    // M7-RESIZE-PATH：真实改变客户区尺寸（产生 WM_SIZE，等价拖动窗口边缘）。
    // 用 AdjustWindowRectEx + SetWindowPos 而不是 MoveWindow 的裸像素：非客户区
    //（标题栏/边框）随 DPI 与样式变化，只有客户区尺寸是渲染侧契约。
    void ResizeClient(const std::uint32_t width, const std::uint32_t height)
    {
        if (m_window == nullptr)
            throw std::logic_error{"ResizeClient requires a window"};
        constexpr DWORD style = WS_OVERLAPPEDWINDOW;
        RECT rest{0, 0, CheckedDimension(width), CheckedDimension(height)};
        if (AdjustWindowRectEx(&rest, style, FALSE, 0) == FALSE)
            throw MakeLastError("AdjustWindowRectEx");
        const int outerWidth = static_cast<int>(rest.right - rest.left);
        const int outerHeight = static_cast<int>(rest.bottom - rest.top);
        if (SetWindowPos(m_window, nullptr, 0, 0, outerWidth, outerHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ==
            FALSE)
            throw MakeLastError("SetWindowPos");
    }

    [[nodiscard]] void* NativeHandle() const noexcept
    {
        return m_window;
    }

  private:
    // 静态窗口过程：WM_NCCREATE 时把 Impl 指针写入 GWLP_USERDATA 建立关联，此后
    // 从 USERDATA 取回实例并转发消息；无关联实例时回退默认窗口过程。
    static LRESULT CALLBACK WindowProcedure(const HWND window, const UINT message, const WPARAM wParam,
                                            const LPARAM lParam) noexcept
    {
        Impl* instance = nullptr;

        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            instance = static_cast<Impl*>(create->lpCreateParams);

            if (instance == nullptr)
            {
                return FALSE;
            }

            instance->m_window = window;
            SetLastError(ERROR_SUCCESS);

            const LONG_PTR previous = SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));

            if (previous == 0 && GetLastError() != ERROR_SUCCESS)
            {
                instance->m_window = nullptr;
                return FALSE;
            }
        }
        else
        {
            instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        if (instance != nullptr)
        {
            return instance->HandleMessage(window, message, wParam, lParam);
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    // 注册窗口类并挂接静态窗口过程；游标加载或类注册失败时抛异常并回滚。
    void RegisterWindowClass()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
        windowClass.lpszClassName = kWindowClassName;

        if (windowClass.hCursor == nullptr)
        {
            throw MakeLastError("LoadCursorW");
        }

        if (RegisterClassExW(&windowClass) == 0)
        {
            throw MakeLastError("RegisterClassExW");
        }

        m_classRegistered = true;
    }

    // 销毁原生窗口并注销窗口类；仅在句柄有效/类确实注册过时执行，析构路径可重复调用。
    void DestroyOwnedResources() noexcept
    {
        if (m_window != nullptr && IsWindow(m_window) != FALSE)
        {
            DestroyWindow(m_window);
        }

        m_window = nullptr;

        if (m_classRegistered)
        {
            UnregisterClassW(kWindowClassName, m_instance);
            m_classRegistered = false;
        }
    }

    // 用 GetClientRect 刷新客户区尺寸缓存；窗口不存在或查询失败时保持旧值。
    void UpdateClientSize() noexcept
    {
        RECT clientRect{};

        if (m_window != nullptr && GetClientRect(m_window, &clientRect) != FALSE)
        {
            m_clientWidth = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
            m_clientHeight = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);
        }
    }

    // 更新鼠标键状态并管理捕获：按下时捕获鼠标到窗口，释放时仅当三个键均未按下才放走捕获。
    void SetMouseButton(const MouseButton button, const bool isDown) noexcept
    {
        m_input.SetMouseButton(button, isDown);

        if (isDown)
        {
            SetCapture(m_window);
            return;
        }

        const bool anyButtonDown = m_input.IsMouseButtonDown(MouseButton::Left) ||
                                   m_input.IsMouseButtonDown(MouseButton::Right) ||
                                   m_input.IsMouseButtonDown(MouseButton::Middle);

        if (!anyButtonDown && GetCapture() == m_window)
        {
            ReleaseCapture();
        }
    }

    // 分发窗口消息：把关闭/销毁/尺寸/DPI/活动/输入等事件翻译为成员状态或 InputState
    // 更新；系统相关消息转发给 DefWindowProcW，其余返回 0 表示已处理。
    LRESULT HandleMessage(const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) noexcept
    {
        switch (message)
        {
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            m_running = false;
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);

            if (m_window == window)
            {
                m_window = nullptr;
            }

            return DefWindowProcW(window, message, wParam, lParam);
        }

        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_SIZE:
        {
            const bool wasMinimized = m_minimized;
            m_minimized = wParam == SIZE_MINIMIZED;
            m_clientWidth = static_cast<std::uint32_t>(LOWORD(lParam));
            m_clientHeight = static_cast<std::uint32_t>(HIWORD(lParam));

            if (m_minimized)
            {
                m_input.Clear();
            }
            else if (wasMinimized)
            {
                m_timingResetRequested = true;
            }

            return 0;
        }

        case WM_DPICHANGED:
        {
            m_dpi = static_cast<std::uint32_t>(HIWORD(wParam));
            const auto* suggestedRect = reinterpret_cast<const RECT*>(lParam);

            SetWindowPos(window, nullptr, suggestedRect->left, suggestedRect->top,
                         suggestedRect->right - suggestedRect->left, suggestedRect->bottom - suggestedRect->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }

        case WM_ACTIVATEAPP:
        {
            const bool active = wParam != FALSE;

            if (m_active != active)
            {
                m_active = active;
                m_timingResetRequested = true;

                if (!m_active)
                {
                    m_input.Clear();
                }
            }

            return 0;
        }

        case WM_ENTERSIZEMOVE:
            m_inSizeMove = true;
            m_input.Clear();
            return 0;

        case WM_EXITSIZEMOVE:
            m_inSizeMove = false;
            m_timingResetRequested = true;
            return 0;

        case WM_KEYDOWN:
            m_input.SetKey(TranslateVirtualKey(wParam), true);
            return 0;

        case WM_KEYUP:
            m_input.SetKey(TranslateVirtualKey(wParam), false);
            return 0;

        case WM_SYSKEYDOWN:
            m_input.SetKey(TranslateVirtualKey(wParam), true);
            return DefWindowProcW(window, message, wParam, lParam);

        case WM_SYSKEYUP:
            m_input.SetKey(TranslateVirtualKey(wParam), false);
            return DefWindowProcW(window, message, wParam, lParam);

        case WM_MOUSEMOVE:
            m_input.SetMousePosition(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONDOWN:
            SetMouseButton(MouseButton::Left, true);
            return 0;

        case WM_LBUTTONUP:
            SetMouseButton(MouseButton::Left, false);
            return 0;

        case WM_RBUTTONDOWN:
            SetMouseButton(MouseButton::Right, true);
            return 0;

        case WM_RBUTTONUP:
            SetMouseButton(MouseButton::Right, false);
            return 0;

        case WM_MBUTTONDOWN:
            SetMouseButton(MouseButton::Middle, true);
            return 0;

        case WM_MBUTTONUP:
            SetMouseButton(MouseButton::Middle, false);
            return 0;

        case WM_MOUSEWHEEL:
            m_input.AddMouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA));
            return 0;

        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != window &&
                (m_input.IsMouseButtonDown(MouseButton::Left) || m_input.IsMouseButtonDown(MouseButton::Right) ||
                 m_input.IsMouseButtonDown(MouseButton::Middle)))
            {
                m_input.ClearMouse();
            }
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    InputState& m_input;
    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    std::uint32_t m_clientWidth = 0;
    std::uint32_t m_clientHeight = 0;
    std::uint32_t m_dpi = USER_DEFAULT_SCREEN_DPI;
    bool m_classRegistered = false;
    bool m_running = true;
    bool m_active = true;
    bool m_minimized = false;
    bool m_inSizeMove = false;
    bool m_timingResetRequested = false;
};

WindowsWindow::WindowsWindow(const WindowDesc& desc, InputState& input) : m_impl{std::make_unique<Impl>(desc, input)}
{
}

WindowsWindow::~WindowsWindow() = default;

bool WindowsWindow::PumpMessages()
{
    return m_impl->PumpMessages();
}

void WindowsWindow::WaitForMessage(std::uint32_t timeoutMilliseconds) const
{
    m_impl->WaitForMessage(timeoutMilliseconds);
}

void WindowsWindow::RequestClose()
{
    m_impl->RequestClose();
}

bool WindowsWindow::IsActive() const noexcept
{
    return m_impl->IsActive();
}

bool WindowsWindow::IsMinimized() const noexcept
{
    return m_impl->IsMinimized();
}

bool WindowsWindow::IsInSizeMove() const noexcept
{
    return m_impl->IsInSizeMove();
}

bool WindowsWindow::ShouldPause() const noexcept
{
    return m_impl->ShouldPause();
}

bool WindowsWindow::ConsumeTimingResetRequest() noexcept
{
    return m_impl->ConsumeTimingResetRequest();
}

std::uint32_t WindowsWindow::ClientWidth() const noexcept
{
    return m_impl->ClientWidth();
}

std::uint32_t WindowsWindow::ClientHeight() const noexcept
{
    return m_impl->ClientHeight();
}

std::uint32_t WindowsWindow::Dpi() const noexcept
{
    return m_impl->Dpi();
}

void WindowsWindow::ResizeClient(const std::uint32_t width, const std::uint32_t height)
{
    m_impl->ResizeClient(width, height);
}

void* WindowsWindow::NativeHandle() const noexcept
{
    // 以 void* 返回 HWND，D3D11Renderer 在实现内转换为 HWND 使用，保持平台边界类型隐藏。
    return m_impl->NativeHandle();
}
} // namespace MiniEngine