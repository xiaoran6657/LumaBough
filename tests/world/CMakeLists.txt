# 前置：父级 tests 树已 include(GoogleTest) 并 enable_testing()；
# 若独立使用本文件，需先自行 include(GoogleTest) 和 enable_testing()。
add_executable(MiniEngineWorldTests
    WorldTransformTests.cpp
    WorldLoaderTests.cpp
    FrustumTests.cpp
    RenderQueueBuilderTests.cpp
    CullingSceneTests.cpp
)

target_link_libraries(MiniEngineWorldTests
    PRIVATE
        MiniEngine::World
        GTest::gtest_main
)

target_compile_features(MiniEngineWorldTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEngineWorldTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineWorldTests)

gtest_discover_tests(
    MiniEngineWorldTests
    DISCOVERY_TIMEOUT 30
)