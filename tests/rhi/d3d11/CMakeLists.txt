# CMakeLists.txt — MiniEngineD3D11Tests 单元测试
# 职责：D3D11 后端测试（Camera 纯数学 + AssetCache 设备级缓存行为 + HLSL 编译 gate
#       + M4-05 HDR target 与后处理常量布局契约）。
# 关联：docs/architecture/README.md、docs/architecture/README.md
#       docs/architecture/README.md（M4-02 审查新增 HLSL gate）

add_executable(
    MiniEngineD3D11Tests
    D3D11AssetCacheTests.cpp
    D3D11CameraTests.cpp
    D3D11HdrTargetTests.cpp
    GpuTimestampDeviceTests.cpp
    HlslCompileGateTests.cpp
    PipelineStatisticsDeviceTests.cpp
)

target_link_libraries(
    MiniEngineD3D11Tests
    PRIVATE
        MiniEngine::D3D11
        GTest::gtest_main
        d3dcompiler
)

# HlslCompileGateTests 需要两件事：
#   1) 引擎内部头 D3D11ShaderCompiler.h（src/），保证 gate 与运行时编译口径唯一同源；
#   2) 仓库 shaders/d3d11 的绝对路径（编译定义注入，测试据此定位 .hlsl）。
target_include_directories(
    MiniEngineD3D11Tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/engine/rhi/d3d11/src
)
target_compile_definitions(
    MiniEngineD3D11Tests
    PRIVATE
        MINIENGINE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/d3d11"
)

target_compile_features(
    MiniEngineD3D11Tests
    PRIVATE
        cxx_std_20
)

set_target_properties(
    MiniEngineD3D11Tests
    PROPERTIES
        CXX_EXTENSIONS OFF
)

miniengine_set_project_warnings(MiniEngineD3D11Tests)

gtest_discover_tests(
    MiniEngineD3D11Tests
    DISCOVERY_TIMEOUT 30
)
