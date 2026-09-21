cmake_minimum_required(VERSION 4.2)

project(
    MiniEngine
    VERSION 0.1.0
    DESCRIPTION "A learning-oriented Windows game engine"
    LANGUAGES C CXX
)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MiniEngineWarnings)

add_subdirectory(engine)
# tools 先于 samples：asset_cooker 与 shader_compiler 产出 samples 运行时需要的
# 离线产物（manifest、DXIL），这份顺序依赖是显式的，不靠"恰好先构建过"。
add_subdirectory(tools)
add_subdirectory(samples)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
include(MiniEngineRhiBoundary)
