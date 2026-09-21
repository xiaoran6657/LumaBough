# M6-06：共享 executor 与纯 CLI 参数，不链接 concrete backend。
add_library(MiniEngineRhiSmoke STATIC SmokeScene.cpp RhiSelection.cpp)
target_include_directories(MiniEngineRhiSmoke PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(MiniEngineRhiSmoke PUBLIC MiniEngineRenderer)
target_compile_features(MiniEngineRhiSmoke PUBLIC cxx_std_20)
set_target_properties(MiniEngineRhiSmoke PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRhiSmoke)

add_executable(MiniEngineSandbox main.cpp M6SceneRunner.cpp M7SceneRunner.cpp RhiUploadSink.cpp)
# M7-02：sandbox 是 CPU zone/线程名/counter 的唯一放置点（引擎模块只加各自内部 zone）。
# M7-07：m7-streaming 的异步加载/预算上传需要 MiniEngine::AssetsAsync（decode task + 协调器）。
target_link_libraries(MiniEngineSandbox PRIVATE MiniEngineRhiFactory MiniEngineRhiSmoke MiniEngineBenchmark MiniEngineProfiling MiniEngine::PlatformWindows MiniEngine::Core MiniEngine::AssetsAsync MiniEngineTasks)
target_include_directories(MiniEngineSandbox PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/src" "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>")
target_compile_definitions(MiniEngineSandbox PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(MiniEngineSandbox PRIVATE cxx_std_20)
set_target_properties(MiniEngineSandbox PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineSandbox)
add_dependencies(MiniEngineSandbox MiniEngineRhiShaderPackages)
include(MiniEnginePix)
miniengine_use_pix(MiniEngineSandbox)

execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE m610_source_commit OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE revision_result ERROR_QUIET)
if(NOT revision_result EQUAL 0)
    set(m610_source_commit "uncommitted")
endif()
target_compile_definitions(MiniEngineSandbox PRIVATE M610_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
    M610_BUILD_CONFIG="$<CONFIG>" M610_SOURCE_COMMIT="${m610_source_commit}")

# M6-10：环境预计算复用 legacy 的固定 HLSL/DXIL 和完整调试包。
add_dependencies(MiniEngineSandbox MiniEngineD3D12ShaderArtifacts)
add_custom_command(TARGET MiniEngineSandbox POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/shaders/d3d11" "$<TARGET_FILE_DIR:MiniEngineSandbox>/shaders/d3d11"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${MINIENGINE_D3D12_SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:MiniEngineSandbox>/shaders/d3d12"
    # E4：双变体包按 shaders/<backend>/<name> 平铺发布（d3d11 的 .dxbc 只在 revisions 里，
    # legacy 目录没有），否则运行包换机后会 "shader artifact missing"。
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>/artifacts" "$<TARGET_FILE_DIR:MiniEngineSandbox>/shaders")

# 已安装 RenderDoc 的可选 application API；普通渲染不依赖其 DLL 或工具安装。
find_path(M610_RENDERDOC_INCLUDE_DIR renderdoc_app.h
    HINTS "C:/Program Files/RenderDoc")
if(M610_RENDERDOC_INCLUDE_DIR)
    target_include_directories(MiniEngineSandbox PRIVATE "${M610_RENDERDOC_INCLUDE_DIR}")
    target_compile_definitions(MiniEngineSandbox PRIVATE M610_HAS_RENDERDOC=1)
endif()
