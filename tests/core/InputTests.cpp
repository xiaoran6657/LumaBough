#include <MiniEngine/Core/Input.h>

#include <gtest/gtest.h>

TEST(InputTests, ReportsPressedHeldAndReleasedAcrossFrames)
{
    MiniEngine::InputState input;

    input.BeginFrame();
    input.SetKey(MiniEngine::Key::W, true);

    EXPECT_TRUE(input.IsKeyDown(MiniEngine::Key::W));
    EXPECT_TRUE(input.WasKeyPressed(MiniEngine::Key::W));
    EXPECT_FALSE(input.WasKeyReleased(MiniEngine::Key::W));

    input.BeginFrame();

    EXPECT_TRUE(input.IsKeyDown(MiniEngine::Key::W));
    EXPECT_FALSE(input.WasKeyPressed(MiniEngine::Key::W));
    EXPECT_FALSE(input.WasKeyReleased(MiniEngine::Key::W));

    input.SetKey(MiniEngine::Key::W, false);

    EXPECT_FALSE(input.IsKeyDown(MiniEngine::Key::W));
    EXPECT_FALSE(input.WasKeyPressed(MiniEngine::Key::W));
    EXPECT_TRUE(input.WasKeyReleased(MiniEngine::Key::W));

    input.BeginFrame();

    EXPECT_FALSE(input.WasKeyReleased(MiniEngine::Key::W));
}

TEST(InputTests, PreservesPressAndReleaseThatArriveInTheSameFrame)
{
    MiniEngine::InputState input;

    input.BeginFrame();
    input.SetKey(MiniEngine::Key::Escape, true);
    input.SetKey(MiniEngine::Key::Escape, false);

    EXPECT_FALSE(input.IsKeyDown(MiniEngine::Key::Escape));
    EXPECT_TRUE(input.WasKeyPressed(MiniEngine::Key::Escape));
    EXPECT_TRUE(input.WasKeyReleased(MiniEngine::Key::Escape));

    input.BeginFrame();

    EXPECT_FALSE(input.WasKeyPressed(MiniEngine::Key::Escape));
    EXPECT_FALSE(input.WasKeyReleased(MiniEngine::Key::Escape));
}

TEST(InputTests, RepeatedDownMessageDoesNotCreateAnotherTransition)
{
    MiniEngine::InputState input;

    input.BeginFrame();
    input.SetKey(MiniEngine::Key::W, true);
    input.SetKey(MiniEngine::Key::W, true);

    EXPECT_TRUE(input.IsKeyDown(MiniEngine::Key::W));
    EXPECT_TRUE(input.WasKeyPressed(MiniEngine::Key::W));
    EXPECT_FALSE(input.WasKeyReleased(MiniEngine::Key::W));

    input.BeginFrame();
    input.SetKey(MiniEngine::Key::W, true);

    EXPECT_TRUE(input.IsKeyDown(MiniEngine::Key::W));
    EXPECT_FALSE(input.WasKeyPressed(MiniEngine::Key::W));
}

TEST(InputTests, TracksMouseDeltaWheelAndButtonEdges)
{
    MiniEngine::InputState input;

    input.BeginFrame();
    input.SetMousePosition(100, 200);
    input.SetMousePosition(104, 197);
    input.AddMouseWheel(0.5F);
    input.AddMouseWheel(0.5F);
    input.SetMouseButton(MiniEngine::MouseButton::Left, true);

    EXPECT_EQ(input.MousePosition().x, 104);
    EXPECT_EQ(input.MousePosition().y, 197);
    EXPECT_EQ(input.MouseDelta().x, 4);
    EXPECT_EQ(input.MouseDelta().y, -3);
    EXPECT_FLOAT_EQ(input.MouseWheel(), 1.0F);
    EXPECT_TRUE(input.WasMouseButtonPressed(MiniEngine::MouseButton::Left));

    input.BeginFrame();

    EXPECT_EQ(input.MouseDelta().x, 0);
    EXPECT_EQ(input.MouseDelta().y, 0);
    EXPECT_FLOAT_EQ(input.MouseWheel(), 0.0F);
    EXPECT_TRUE(input.IsMouseButtonDown(MiniEngine::MouseButton::Left));
    EXPECT_FALSE(input.WasMouseButtonPressed(MiniEngine::MouseButton::Left));
}

TEST(InputTests, PreservesMousePressAndReleaseThatArriveInTheSameFrame)
{
    MiniEngine::InputState input;

    input.BeginFrame();
    input.SetMouseButton(MiniEngine::MouseButton::Left, true);
    input.SetMouseButton(MiniEngine::MouseButton::Left, false);

    EXPECT_FALSE(input.IsMouseButtonDown(MiniEngine::MouseButton::Left));
    EXPECT_TRUE(input.WasMouseButtonPressed(MiniEngine::MouseButton::Left));
    EXPECT_TRUE(input.WasMouseButtonReleased(MiniEngine::MouseButton::Left));
}

TEST(InputTests, ClearRemovesHeldStateAndMouseHistory)
{
    MiniEngine::InputState input;

    input.SetKey(MiniEngine::Key::A, true);
    input.SetMouseButton(MiniEngine::MouseButton::Right, true);
    input.SetMousePosition(10, 20);
    input.SetMousePosition(15, 25);
    input.AddMouseWheel(1.0F);

    input.Clear();

    EXPECT_FALSE(input.IsKeyDown(MiniEngine::Key::A));
    EXPECT_FALSE(input.IsMouseButtonDown(MiniEngine::MouseButton::Right));
    EXPECT_FALSE(input.WasKeyReleased(MiniEngine::Key::A));
    EXPECT_FALSE(input.WasMouseButtonReleased(MiniEngine::MouseButton::Right));
    EXPECT_EQ(input.MouseDelta().x, 0);
    EXPECT_EQ(input.MouseDelta().y, 0);
    EXPECT_FLOAT_EQ(input.MouseWheel(), 0.0F);

    input.SetMousePosition(100, 200);

    EXPECT_EQ(input.MouseDelta().x, 0);
    EXPECT_EQ(input.MouseDelta().y, 0);
}

TEST(InputTests, ClearMouseDoesNotDiscardKeyboardState)
{
    MiniEngine::InputState input;

    input.BeginFrame();
    input.SetKey(MiniEngine::Key::A, true);
    input.SetMouseButton(MiniEngine::MouseButton::Right, true);
    input.SetMousePosition(10, 20);

    input.ClearMouse();

    EXPECT_TRUE(input.IsKeyDown(MiniEngine::Key::A));
    EXPECT_TRUE(input.WasKeyPressed(MiniEngine::Key::A));
    EXPECT_FALSE(input.IsMouseButtonDown(MiniEngine::MouseButton::Right));
    EXPECT_FALSE(input.WasMouseButtonPressed(MiniEngine::MouseButton::Right));

    input.SetMousePosition(100, 200);
    EXPECT_EQ(input.MouseDelta().x, 0);
    EXPECT_EQ(input.MouseDelta().y, 0);
}
