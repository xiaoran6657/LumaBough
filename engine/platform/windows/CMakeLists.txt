# ============================================================================
# engine/platform/windows/CMakeLists.txt — MiniEnginePlatformWindows 静态库
# 里程碑：M1（窗口/时钟）+ M3（7-B DirectoryWatcherWin32）
# 职责：承载 Win32 平台代码：WindowsWindow / WindowsClock / DirectoryWatcherWin32。
#       PUBLIC 依赖 Core；user32 为 PRIVATE（只有实现文件用）；UNICODE 等宏为
#       PRIVATE 定义，避免泄漏到消费者的编译命令行。
# 关联：docs/architecture/DECISIONS.md
# ============================================================================

add_library(
    MiniEnginePlatformWindows
    STATIC
        src/DirectoryWatcherWin32.cpp
        src/ProcessMemoryWin32.cpp
        src/WindowsClock.cpp
        src/WindowsWindow.cpp
)

add_library(
    MiniEngine::PlatformWindows
    ALIAS
    MiniEnginePlatformWindows
)

target_include_directories(
    MiniEnginePlatformWindows
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(
    MiniEnginePlatformWindows
    PUBLIC
        MiniEngine::Core
    PRIVATE
        user32
)

target_compile_features(
    MiniEnginePlatformWindows
    PUBLIC
        cxx_std_20
)

target_compile_definitions(
    MiniEnginePlatformWindows
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(
    MiniEnginePlatformWindows
    PROPERTIES
        CXX_EXTENSIONS OFF
)

miniengine_set_project_warnings(MiniEnginePlatformWindows)
