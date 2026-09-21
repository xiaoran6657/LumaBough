# 前置：父级 tests 树已 include(GoogleTest) 并 enable_testing()；
# 若独立使用本文件，需先自行 include(GoogleTest) 和 enable_testing()。
add_executable(MiniEngineAssetsTests
    AssetHandleTests.cpp
    AssetManagerTests.cpp
    AssetRegistryTests.cpp
    BakedReaderTests.cpp
    Sha256Tests.cpp
)

target_link_libraries(MiniEngineAssetsTests
    PRIVATE
        MiniEngine::Assets
        GTest::gtest_main
)

target_compile_features(MiniEngineAssetsTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEngineAssetsTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineAssetsTests)

gtest_discover_tests(
    MiniEngineAssetsTests
    DISCOVERY_TIMEOUT 30
)

# ---------------------------------------------------------------------------
# M7-06：异步加载流水线（状态机 / 去重 / 取消 / 失败 / 背压）
# 依赖：MiniEngineAssetsAsync（含 I/O 线程与 decode 端口）+ MiniEngineTasks。
# BoundedQueue 用例是白盒：直接包含 async/src 的内部原语（保留私有实现可测性）。
# ---------------------------------------------------------------------------
add_executable(MiniEngineAssetsAsyncTests
    AssetLoadStateTests.cpp
    AsyncAssetLoaderTests.cpp
    BoundedQueueTests.cpp
    UploadCommitTests.cpp
)

target_link_libraries(MiniEngineAssetsAsyncTests
    PRIVATE
        MiniEngine::Assets
        MiniEngine::AssetsAsync
        MiniEngineTasks
        GTest::gtest_main
)

target_include_directories(MiniEngineAssetsAsyncTests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/assets/async/src"
)

target_compile_features(MiniEngineAssetsAsyncTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEngineAssetsAsyncTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineAssetsAsyncTests)

gtest_discover_tests(
    MiniEngineAssetsAsyncTests
    DISCOVERY_TIMEOUT 30
)
