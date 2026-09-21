# 前置：父级 tests 树已 include(GoogleTest) 并 enable_testing()；
# 若独立使用本文件，需先自行 include(GoogleTest) 和 enable_testing()。
add_executable(MiniEnginePlatformTests
    DirectoryWatcherWin32Tests.cpp
)

target_link_libraries(MiniEnginePlatformTests
    PRIVATE
        MiniEngine::PlatformWindows
        GTest::gtest_main
)

target_compile_features(MiniEnginePlatformTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEnginePlatformTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEnginePlatformTests)

gtest_discover_tests(
    MiniEnginePlatformTests
    DISCOVERY_TIMEOUT 30
)
