# ============================================================================
# engine/world/CMakeLists.txt — MiniEngineWorld 静态库（场景图与 .meworld 加载）
# 里程碑：M3（06 篇 + whole-world 篇）
# 职责：定义 MiniEngineWorld：World 实体层级/变换/组件与 WorldLoader 两遍实例化。
#       PUBLIC 依赖 Core 与 Assets（WorldLoader 消费 AssetManager 与 BakedReader）；
#       不依赖 RHI——渲染端单向消费 RenderItem。
# 关联：docs/architecture/DECISIONS.md §7
# ============================================================================

# 场景图库；include 目录 PUBLIC 导出，D3D11 与 Sandbox 由此消费。
add_library(MiniEngineWorld STATIC
    include/MiniEngine/World/World.h
    include/MiniEngine/World/WorldLoader.h
    include/MiniEngine/World/WorldTypes.h
    include/MiniEngine/World/DirectionalLight.h
    include/MiniEngine/World/Frustum.h
    include/MiniEngine/World/RenderPacket.h
    include/MiniEngine/World/RenderProxyLayouts.h
    include/MiniEngine/World/RenderQueueBuilder.h
    src/World.cpp
    src/WorldLoader.cpp
    src/Frustum.cpp
    src/RenderProxyLayouts.cpp
    src/RenderQueueBuilder.cpp
)
add_library(MiniEngine::World ALIAS MiniEngineWorld)


target_include_directories(MiniEngineWorld
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(MiniEngineWorld PUBLIC cxx_std_20)
# M7-02：RenderPacket 构建里有两处 detail zone（cull/sort）。profiling 门面关闭 Tracy 时
# 展开为 ((void)0)，因此这不是 PUBLIC 依赖。
target_link_libraries(MiniEngineWorld PUBLIC MiniEngine::Core MiniEngine::Assets PRIVATE MiniEngineProfiling)
set_target_properties(
    MiniEngineWorld
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineWorld)