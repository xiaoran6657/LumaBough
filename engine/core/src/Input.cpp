#include <MiniEngine/Core/Input.h>

#include <algorithm>

namespace MiniEngine
{
namespace
{
bool IsValidKey(const Key key)
{
    return key > Key::Unknown && key < Key::Count;
}

bool IsValidMouseButton(const MouseButton button)
{
    return button < MouseButton::Count;
}

std::size_t ToIndex(const Key key)
{
    return static_cast<std::size_t>(key);
}

std::size_t ToIndex(const MouseButton button)
{
    return static_cast<std::size_t>(button);
}
} // namespace

void InputState::BeginFrame() noexcept
{
    m_pressedKeys.fill(false);
    m_releasedKeys.fill(false);
    m_pressedMouseButtons.fill(false);
    m_releasedMouseButtons.fill(false);
    m_mouseDelta = {};
    m_mouseWheel = 0.0F;
}

void InputState::Clear() noexcept
{
    m_keys.fill(false);
    m_pressedKeys.fill(false);
    m_releasedKeys.fill(false);
    ClearMouse();
}

void InputState::ClearMouse() noexcept
{
    m_mouseButtons.fill(false);
    m_pressedMouseButtons.fill(false);
    m_releasedMouseButtons.fill(false);
    m_mouseDelta = {};
    m_mouseWheel = 0.0F;
    m_hasMousePosition = false;
}

void InputState::SetKey(const Key key, const bool isDown) noexcept
{
    if (!IsValidKey(key))
    {
        return;
    }

    const std::size_t index = ToIndex(key);
    if (m_keys[index] == isDown)
    {
        return;
    }

    m_keys[index] = isDown;
    (isDown ? m_pressedKeys : m_releasedKeys)[index] = true;
}

bool InputState::IsKeyDown(const Key key) const noexcept
{
    return IsValidKey(key) && m_keys[ToIndex(key)];
}

bool InputState::WasKeyPressed(const Key key) const noexcept
{
    return IsValidKey(key) && m_pressedKeys[ToIndex(key)];
}

bool InputState::WasKeyReleased(const Key key) const noexcept
{
    return IsValidKey(key) && m_releasedKeys[ToIndex(key)];
}

void InputState::SetMouseButton(const MouseButton button, const bool isDown) noexcept
{
    if (!IsValidMouseButton(button))
    {
        return;
    }

    const std::size_t index = ToIndex(button);
    if (m_mouseButtons[index] == isDown)
    {
        return;
    }

    m_mouseButtons[index] = isDown;
    (isDown ? m_pressedMouseButtons : m_releasedMouseButtons)[index] = true;
}

bool InputState::IsMouseButtonDown(const MouseButton button) const noexcept
{
    return IsValidMouseButton(button) && m_mouseButtons[ToIndex(button)];
}

bool InputState::WasMouseButtonPressed(const MouseButton button) const noexcept
{
    return IsValidMouseButton(button) && m_pressedMouseButtons[ToIndex(button)];
}

bool InputState::WasMouseButtonReleased(const MouseButton button) const noexcept
{
    return IsValidMouseButton(button) && m_releasedMouseButtons[ToIndex(button)];
}

void InputState::SetMousePosition(const std::int32_t x, const std::int32_t y) noexcept
{
    if (m_hasMousePosition)
    {
        m_mouseDelta.x += x - m_mousePosition.x;
        m_mouseDelta.y += y - m_mousePosition.y;
    }

    m_mousePosition = {.x = x, .y = y};
    m_hasMousePosition = true;
}

void InputState::AddMouseWheel(const float delta) noexcept
{
    m_mouseWheel += delta;
}

Int2 InputState::MousePosition() const noexcept
{
    return m_mousePosition;
}

Int2 InputState::MouseDelta() const noexcept
{
    return m_mouseDelta;
}

float InputState::MouseWheel() const noexcept
{
    return m_mouseWheel;
}
} // namespace MiniEngine
