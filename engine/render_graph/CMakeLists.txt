# M6-07：graph 模型、声明验证与最小有序执行；仅依赖 public RHI。
add_library(MiniEngineRenderGraph STATIC
    src/RenderGraph.cpp src/RenderGraphBuilder.cpp src/RenderGraphCompile.cpp
    src/RenderGraphExecute.cpp src/RgResources.cpp src/DependencyAlgorithms.cpp
    src/GraphDiagnostics.cpp src/RenderGraphDump.cpp src/TransientResourcePool.cpp)
target_include_directories(MiniEngineRenderGraph PUBLIC include)
target_link_libraries(MiniEngineRenderGraph PUBLIC MiniEngineRhiPublic)
target_compile_features(MiniEngineRenderGraph PUBLIC cxx_std_20)
set_target_properties(MiniEngineRenderGraph PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRenderGraph)
