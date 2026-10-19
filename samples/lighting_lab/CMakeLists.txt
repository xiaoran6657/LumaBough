# 单独配置时只编译现有 SHA-256 与本演示，不下载第三方依赖，也不配置测试树。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 4.2)
    project(LightingShowcase LANGUAGES CXX)
    get_filename_component(showcase_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    include("${showcase_root}/cmake/MiniEngineWarnings.cmake")
    add_library(ShowcaseHash STATIC "${showcase_root}/engine/assets/src/Sha256.cpp")
    target_include_directories(ShowcaseHash PUBLIC "${showcase_root}/engine/assets/include")
    target_compile_features(ShowcaseHash PUBLIC cxx_std_20)
    set_property(TARGET ShowcaseHash PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_library(MiniEngine::Assets ALIAS ShowcaseHash)
else()
    set(showcase_root "${PROJECT_SOURCE_DIR}")
endif()

# 演示发布包使用不含源码绝对路径/调试信息的独立 DXIL；不改变历史探针编译契约。
cmake_host_system_information(RESULT showcase_sdk_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE "KitsRoot10")
find_program(SHOWCASE_DXC_EXECUTABLE dxc.exe HINTS
    "${showcase_sdk_root}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64" REQUIRED)
set(showcase_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/compiled-shaders")
set(showcase_shader "${showcase_shader_dir}/LightingShowcase.CSMain.cs.dxil")
add_custom_command(OUTPUT "${showcase_shader}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${showcase_shader_dir}"
    COMMAND "${SHOWCASE_DXC_EXECUTABLE}" -T cs_6_0 -E CSMain -HV 2021 -Ges -WX -O3
        -Qstrip_debug -Fo "${showcase_shader}" "${showcase_root}/shaders/d3d12/LightingShowcase.hlsl"
    DEPENDS "${showcase_root}/shaders/d3d12/LightingShowcase.hlsl"
        "${showcase_root}/shaders/d3d12/PbrCommon.hlsli"
    VERBATIM)
add_custom_target(LightingShowcaseShaders DEPENDS "${showcase_shader}")

add_executable(LightingShowcase LightingShowcase.cpp)
target_compile_features(LightingShowcase PRIVATE cxx_std_20)
target_compile_definitions(LightingShowcase PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    ME_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(LightingShowcase PRIVATE MiniEngine::Assets d3d12 dxgi dxguid d3dcompiler
    windowscodecs ole32)
set_target_properties(LightingShowcase PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set_property(TARGET LightingShowcase PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(MSVC)
    target_link_options(LightingShowcase PRIVATE /PDBALTPATH:LightingShowcase.pdb)
endif()
miniengine_set_project_warnings(LightingShowcase)
add_dependencies(LightingShowcase LightingShowcaseShaders)
add_custom_command(TARGET LightingShowcase POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LightingShowcase>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${showcase_shader}"
        "${showcase_root}/shaders/d3d12/LightingShowcase.hlsl"
        "${showcase_root}/shaders/d3d12/PbrCommon.hlsli"
        "$<TARGET_FILE_DIR:LightingShowcase>/shaders"
    VERBATIM)
