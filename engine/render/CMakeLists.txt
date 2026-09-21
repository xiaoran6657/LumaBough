# backend-free pass；M6-07 接入 graph 公共依赖，完整 pass 迁移见 M6-10。
add_library(MiniEngineRenderer STATIC src/SmokePasses.cpp src/M6RenderPipeline.cpp src/RenderPacketBuilder.cpp)
target_include_directories(MiniEngineRenderer PUBLIC include)
# M7-05：并行 RenderPacket 构建（worker 只读冻结 RenderItem 快照，cull/extract 纯 CPU）。
# Profiling 为 PRIVATE：只有本模块的 zone 需要 ME_ENABLE_TRACY 编译定义。
target_link_libraries(MiniEngineRenderer PUBLIC MiniEngineRhiPublic MiniEngineRenderGraph MiniEngine::World MiniEngineTasks)
target_link_libraries(MiniEngineRenderer PRIVATE MiniEngineProfiling)
target_compile_features(MiniEngineRenderer PUBLIC cxx_std_20)
set_target_properties(MiniEngineRenderer PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRenderer)

target_sources(MiniEngineRenderer PRIVATE src/M6SceneResources.cpp)
