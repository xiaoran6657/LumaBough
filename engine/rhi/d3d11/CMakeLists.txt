# M6-06：旧 concrete 与新 adapter 共用不依赖 World/Renderer 的原生机制。
add_library(MiniEngineD3D11Runtime STATIC src/D3D11Error.cpp src/D3D11Capabilities.cpp src/D3D11IblResources.cpp src/D3D11ShaderCompiler.cpp)
target_include_directories(MiniEngineD3D11Runtime PUBLIC include PRIVATE src)
target_link_libraries(MiniEngineD3D11Runtime PUBLIC MiniEngineRhiPublic PRIVATE MiniEngine::Core d3d11 d3dcompiler dxgi dxguid)
target_compile_definitions(MiniEngineD3D11Runtime PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(MiniEngineD3D11Runtime PUBLIC cxx_std_20)
set_target_properties(MiniEngineD3D11Runtime PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineD3D11Runtime)

# CMakeLists.txt — MiniEngineD3D11 具体渲染后端库
# 关联：docs/architecture/README.md、docs/architecture/README.md

add_library(
    MiniEngineD3D11
    STATIC
        src/D3D11AssetCache.cpp
        src/D3D11Camera.cpp
        src/D3D11GpuTimer.cpp
        src/D3D11HdrTarget.cpp
        src/D3D11Renderer.cpp
        src/D3D11Screenshot.cpp
        src/D3D11ShadowMap.cpp
)

add_library(MiniEngine::D3D11 ALIAS MiniEngineD3D11)

target_include_directories(
    MiniEngineD3D11
    PUBLIC
        include
)

# Assets/World PUBLIC：D3D11AssetCache.h 引用 AssetHandle/MeshAsset/TextureAsset；
# D3D11Renderer 消费 MiniEngine::World::RenderItem（07 篇 CPU snapshot → GPU 上传）。
# D3D 库 PRIVATE：仅实现文件使用。
target_link_libraries(
    MiniEngineD3D11
    PUBLIC
        MiniEngine::Assets
        MiniEngine::World
    PRIVATE
        MiniEngine::Core
        d3d11
        d3dcompiler
        dxgi
        dxguid
        windowscodecs
        ole32
)

target_compile_features(
    MiniEngineD3D11
    PUBLIC
        cxx_std_20
)

target_compile_definitions(
    MiniEngineD3D11
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(
    MiniEngineD3D11
    PROPERTIES
        CXX_EXTENSIONS OFF
)

miniengine_set_project_warnings(MiniEngineD3D11)

# M6-02：仅加入真实能力查询；旧 concrete 渲染路径不变。
target_link_libraries(MiniEngineD3D11 PRIVATE MiniEngineD3D11Runtime)
target_link_libraries(MiniEngineD3D11 PRIVATE MiniEngineRhiPublic)

# M6-06 生产 public RHI adapter，native include 不向上游传播。
add_library(MiniEngineD3D11RhiAdapter STATIC src/D3D11RhiBackend.cpp src/D3D11GpuProfiler.cpp)
target_include_directories(MiniEngineD3D11RhiAdapter PRIVATE src include "../src")
# M7-02：GPU zone 通过 profiling 门面接入；Tracy 关闭时该库只提供返回 nullptr 的工厂。
target_link_libraries(MiniEngineD3D11RhiAdapter PUBLIC MiniEngineRhiPublic PRIVATE MiniEngineD3D11Runtime MiniEngine::Core MiniEngineProfiling)
target_compile_definitions(MiniEngineD3D11RhiAdapter PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(MiniEngineD3D11RhiAdapter PUBLIC cxx_std_20)
set_target_properties(MiniEngineD3D11RhiAdapter PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineD3D11RhiAdapter)
