add_executable(
    MiniEngineCoreTests
    BuildInfoTests.cpp
    FileSystemTests.cpp
    LogTests.cpp
    FixedStepTests.cpp
    InputTests.cpp
)

target_link_libraries(
    MiniEngineCoreTests
    PRIVATE
        MiniEngine::Core
        GTest::gtest_main
)

target_compile_features(
    MiniEngineCoreTests
    PRIVATE
        cxx_std_20
)

set_target_properties(
    MiniEngineCoreTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)

miniengine_set_project_warnings(MiniEngineCoreTests)

gtest_discover_tests(
    MiniEngineCoreTests
    DISCOVERY_TIMEOUT 30
)