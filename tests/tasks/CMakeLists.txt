# M7-03：任务系统契约、所有权与生命周期测试（纯 CPU，无 GPU/窗口）。
# 这些用例是 M7-A07/A08/A09 的证据：全局有锁队列的功能与压力、线程命名与 jthread
# 生命周期、1/N worker 下 nested group 与 wait-help 不死锁。
add_executable(MiniEngineTaskTests
    TaskSystemTests.cpp
    TaskStressTests.cpp
    TaskStealingTests.cpp
)

target_link_libraries(MiniEngineTaskTests
    PRIVATE
        MiniEngineTasks
        GTest::gtest_main
)

target_compile_features(MiniEngineTaskTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEngineTaskTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineTaskTests)

# 压力与竞争用例（bursty publish / submit racing shutdown）会做 500—1000 轮，
# Release 也要留出余量；超时视为失败（挂起就是死锁回归，不允许人工重试）。
# 单标签：带分号的 LABELS 会在 gtest_discover_tests 的脚本参数里被拆坏（MSB3073）。
gtest_discover_tests(
    MiniEngineTaskTests
    DISCOVERY_TIMEOUT 60
    PROPERTIES
        TIMEOUT 300
        LABELS "tasks"
)
