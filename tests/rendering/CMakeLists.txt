# ============================================================================
# CMakeLists.txt — 渲染层测试（M4：rendering 契约与 PBR 数学）
# 里程碑：M4
# 职责：聚合 MiniEngineRenderingTests。M4-02 先落 TextureFormatTests（.metex v2
#       语义契约）与 PbrMathTests（BRDF CPU 参考）；M4-07 接入 ImageComparisonTests
#       （视觉回归比较器数学 + 截图元数据 INCOMPARABLE）。PbrMathTests 的 CPU 参考
#       公式与 shaders/d3d11/PbrCommon.hlsli 一致，进 shader 前先在 CPU 域锁定行为。
# 关联：docs/architecture/README.md「配合 code 模板的手抄清单」第 6 条
#       docs/architecture/README.md「自动化测试」
# ============================================================================
add_executable(MiniEngineRenderingTests
    PbrMathTests.cpp
    TextureFormatTests.cpp
    ImageComparisonTests.cpp
    TimingTests.cpp
)

# ImageComparisonTests 使用公共头 D3D11ImageComparison.h（与 capture 实现同模块）。
target_link_libraries(MiniEngineRenderingTests
    PRIVATE
        MiniEngine::Assets
        MiniEngine::D3D11
        GTest::gtest_main
)

target_compile_features(MiniEngineRenderingTests PRIVATE cxx_std_20)
miniengine_set_project_warnings(MiniEngineRenderingTests)

include(GoogleTest)
gtest_discover_tests(MiniEngineRenderingTests
    DISCOVERY_TIMEOUT 30
)

# M7-05：并行 RenderPacket 构建的确定性测试（serial oracle 对照、边界尺寸 × workers × chunk）。
add_executable(MiniEngineRenderPacketTests
    RenderPacketDeterminismTests.cpp
)

target_link_libraries(MiniEngineRenderPacketTests
    PRIVATE
        MiniEngineRenderer
        MiniEngineTasks
        GTest::gtest_main
)

target_compile_features(MiniEngineRenderPacketTests PRIVATE cxx_std_20)
set_target_properties(MiniEngineRenderPacketTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRenderPacketTests)

gtest_discover_tests(MiniEngineRenderPacketTests
    DISCOVERY_TIMEOUT 60
    PROPERTIES TIMEOUT 600
)
