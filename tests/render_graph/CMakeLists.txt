# 无 GPU 的声明/内容/执行测试通过真实 TraceRhi 公共契约，graph 库本身不链接测试替身。
add_executable(MiniEngineRenderGraphTests RenderGraphBuilderTests.cpp ContentStateTests.cpp
    RenderGraphExecutionTests.cpp GraphCompilerTests.cpp GraphDiagnosticsTests.cpp DependencyAlgorithmsTests.cpp
    TransientPoolTests.cpp TransientLifetimeTests.cpp M610RenderPipelineTests.cpp)
execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE m608_source_commit OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE revision_result ERROR_QUIET)
if(NOT revision_result EQUAL 0)
    set(m608_source_commit "uncommitted")
endif()
target_compile_definitions(MiniEngineRenderGraphTests PRIVATE
    M608_DUMP_DIR="${PROJECT_SOURCE_DIR}/out/m6-08/dumps/$<CONFIG>"
    M608_SOURCE_COMMIT="${m608_source_commit}")
target_include_directories(MiniEngineRenderGraphTests PRIVATE "${PROJECT_SOURCE_DIR}/engine/render_graph/src")
# M611-D01 回归：M6SceneResources 需要按名称匹配的真实 shader 包（生成的 fixture 头，只读）。
target_include_directories(MiniEngineRenderGraphTests PRIVATE "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>")
add_dependencies(MiniEngineRenderGraphTests MiniEngineRhiShaderPackages)
target_link_libraries(MiniEngineRenderGraphTests PRIVATE MiniEngineRenderGraph MiniEngineRenderer MiniEngineTraceRhi GTest::gtest_main)
set_target_properties(MiniEngineRenderGraphTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRenderGraphTests)
gtest_discover_tests(MiniEngineRenderGraphTests PROPERTIES TIMEOUT 60)

file(GLOB graph_public_headers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/engine/render_graph/include/MiniEngine/RenderGraph/*.h")
set(graph_header_units)
foreach(header IN LISTS graph_public_headers)
    get_filename_component(name "${header}" NAME)
    set(unit "${CMAKE_CURRENT_BINARY_DIR}/header-smoke/${name}.cpp")
    file(WRITE "${unit}" "#include <MiniEngine/RenderGraph/${name}>\n#if defined(_WINDOWS_) || defined(_D3D11_H_) || defined(__d3d12_h__)\n#error Native platform header leaked\n#endif\n")
    list(APPEND graph_header_units "${unit}")
endforeach()
add_executable(RenderGraphHeaderSmoke RenderGraphHeaderSmoke.cpp ${graph_header_units})
target_link_libraries(RenderGraphHeaderSmoke PRIVATE MiniEngineRenderGraph)
set_target_properties(RenderGraphHeaderSmoke PROPERTIES CXX_EXTENSIONS OFF UNITY_BUILD OFF)
miniengine_set_project_warnings(RenderGraphHeaderSmoke)
add_test(NAME RenderGraphHeaderSmoke COMMAND RenderGraphHeaderSmoke)

# M6-11：engine/render 公共头各自独立编译；native 平台/图形头不得经公共头传递。
file(GLOB renderer_public_headers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/engine/render/include/MiniEngine/Render/*.h")
set(renderer_header_units)
foreach(header IN LISTS renderer_public_headers)
    get_filename_component(name "${header}" NAME)
    set(unit "${CMAKE_CURRENT_BINARY_DIR}/renderer-header-smoke/${name}.cpp")
    file(WRITE "${unit}" "#include <MiniEngine/Render/${name}>\n#if defined(_WINDOWS_) || defined(_D3D11_H_) || defined(__d3d12_h__)\n#error Native platform header leaked\n#endif\n")
    list(APPEND renderer_header_units "${unit}")
endforeach()
add_executable(RendererHeaderSmoke RendererHeaderSmoke.cpp ${renderer_header_units})
target_link_libraries(RendererHeaderSmoke PRIVATE MiniEngineRenderer)
set_target_properties(RendererHeaderSmoke PROPERTIES CXX_EXTENSIONS OFF UNITY_BUILD OFF)
miniengine_set_project_warnings(RendererHeaderSmoke)
add_test(NAME RendererHeaderSmoke COMMAND RendererHeaderSmoke)

# 原生 smoke 单独接线；graph 库仍仅依赖 public RHI。
add_executable(MiniEngineRenderGraphNativeTests RenderGraphNativeTests.cpp TransientNativeTests.cpp)
target_include_directories(MiniEngineRenderGraphNativeTests PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/src")
target_include_directories(MiniEngineRenderGraphNativeTests PRIVATE
    "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>")
add_dependencies(MiniEngineRenderGraphNativeTests MiniEngineM604ShaderPackages)
target_link_libraries(MiniEngineRenderGraphNativeTests PRIVATE MiniEngineRenderGraph
    MiniEngineRhiFactory MiniEngine::PlatformWindows MiniEngine::Core GTest::gtest_main)
target_compile_definitions(MiniEngineRenderGraphNativeTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    M609_NATIVE_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/out/m6-09/native/$<CONFIG>"
    M609_BUILD_CONFIG="$<CONFIG>" M609_SOURCE_COMMIT="${m608_source_commit}")
set_target_properties(MiniEngineRenderGraphNativeTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRenderGraphNativeTests)
include(MiniEnginePix)
miniengine_use_pix(MiniEngineRenderGraphNativeTests)
gtest_discover_tests(MiniEngineRenderGraphNativeTests PROPERTIES TIMEOUT 120)

# M6-08：单独进程替换 new/delete，避免污染其他目标。
add_executable(MiniEngineRenderGraphCpuCostTests GraphCpuCostTests.cpp)
target_include_directories(MiniEngineRenderGraphCpuCostTests PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/rhi/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
target_link_libraries(MiniEngineRenderGraphCpuCostTests PRIVATE MiniEngineRenderGraph MiniEngineRhiFactory
    MiniEngineD3D12Runtime MiniEngine::PlatformWindows MiniEngine::Core GTest::gtest_main d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineRenderGraphCpuCostTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    M608_COST_ARTIFACT_DIR="${PROJECT_SOURCE_DIR}/out/m6-08/cost/$<CONFIG>")
set_target_properties(MiniEngineRenderGraphCpuCostTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRenderGraphCpuCostTests)
miniengine_use_pix(MiniEngineRenderGraphCpuCostTests)
gtest_discover_tests(MiniEngineRenderGraphCpuCostTests PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
