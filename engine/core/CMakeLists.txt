add_library(
    MiniEngineCore
    STATIC
        src/Assert.cpp
        src/BuildInfo.cpp
        src/FileSystem.cpp
        src/Input.cpp
        src/Log.cpp
)

add_library(MiniEngine::Core ALIAS MiniEngineCore)

target_include_directories(
    MiniEngineCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(MiniEngineCore PUBLIC cxx_std_20)
# M7-02：Log 的互斥量使用可观测锁（Tracy 关闭时退化为 std::mutex）。
target_link_libraries(MiniEngineCore PRIVATE MiniEngineProfiling)
set_target_properties(MiniEngineCore PROPERTIES CXX_EXTENSIONS OFF)

miniengine_set_project_warnings(MiniEngineCore)