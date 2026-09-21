#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace MiniEngine
{
enum class Key : std::uint8_t
{
    Unknown = 0,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Escape,
    Space,
    Enter,
    Tab,
    Backspace,
    Left,
    Right,
    Up,
    Down,
    Shift,
    Control,
    Alt,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Count
};

enum class MouseButton : std::uint8_t
{
    Left,
    Right,
    Middle,
    Count
};

struct Int2
{
    std::int32_t x = 0;
    std::int32_t y = 0;
};

class InputState
{
  public:
    void BeginFrame() noexcept;
    void Clear() noexcept;
    void ClearMouse() noexcept;

    void SetKey(Key key, bool isDown) noexcept;

    [[nodiscard]] bool IsKeyDown(Key key) const noexcept;
    [[nodiscard]] bool WasKeyPressed(Key key) const noexcept;
    [[nodiscard]] bool WasKeyReleased(Key key) const noexcept;

    void SetMouseButton(MouseButton button, bool isDown) noexcept;

    [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const noexcept;
    [[nodiscard]] bool WasMouseButtonPressed(MouseButton button) const noexcept;
    [[nodiscard]] bool WasMouseButtonReleased(MouseButton button) const noexcept;

    void SetMousePosition(std::int32_t x, std::int32_t y) noexcept;
    void AddMouseWheel(float delta) noexcept;

    [[nodiscard]] Int2 MousePosition() const noexcept;
    [[nodiscard]] Int2 MouseDelta() const noexcept;
    [[nodiscard]] float MouseWheel() const noexcept;

  private:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(MouseButton::Count);

    std::array<bool, kKeyCount> m_keys{};
    std::array<bool, kKeyCount> m_pressedKeys{};
    std::array<bool, kKeyCount> m_releasedKeys{};
    std::array<bool, kMouseButtonCount> m_mouseButtons{};
    std::array<bool, kMouseButtonCount> m_pressedMouseButtons{};
    std::array<bool, kMouseButtonCount> m_releasedMouseButtons{};

    Int2 m_mousePosition{};
    Int2 m_mouseDelta{};
    float m_mouseWheel = 0.0F;
    bool m_hasMousePosition = false;
};
} // namespace MiniEngine