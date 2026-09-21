# M7-01：性能契约与统计纯 CPU 测试（无 GPU、无窗口）。
# schema 字段名或分位数语义被改动时，这些用例必须一起失败。
add_executable(MiniEnginePerformanceContractTests
    StatisticsTests.cpp
    BenchmarkSchemaTests.cpp
    LayoutEquivalenceTests.cpp
)

target_link_libraries(MiniEnginePerformanceContractTests
    PRIVATE
        MiniEngineBenchmark
        MiniEngine::World
        MiniEngineTasks
        GTest::gtest_main
)

target_compile_features(MiniEnginePerformanceContractTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEnginePerformanceContractTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEnginePerformanceContractTests)

gtest_discover_tests(
    MiniEnginePerformanceContractTests
    DISCOVERY_TIMEOUT 30
)

# 真实 JSON 解析校验：C++ 单测只做结构检查，这里用标准解析器读回串行化结果，
# 并核对 recipe 与机器环境 manifest 的必需字段（M7-A06）。
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME M7SchemaJsonEvidence
    COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m7_schema.py"
        --exe "$<TARGET_FILE:MiniEnginePerformanceContractTests>"
        --recipe "${PROJECT_SOURCE_DIR}/assets/recipes/m7-performance-scenes.json"
        --environment "${PROJECT_SOURCE_DIR}/tests/fixtures/performance/environment.schema-example.json"
)

# M7-09：方法学契约（比较器的四类结果、唯一变量强制、protected 判定、漂移降级）用
# 合成 raw JSON 驱动真实 PowerShell 脚本，避免"工具改了但没人跑过"。
add_test(
    NAME M7ComparisonMethodology
    COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m7_compare.py"
)

# M7-RESIZE-PATH：`--exercise-resize` 的三条约束（需稳定性压力、benchmark 禁止、需要真实窗口）
# 必须在创建 GPU 对象之前拒绝——只做参数层断言，端到端 resize 覆盖走发布证据。
add_test(
    NAME M7ResizeCliContract
    COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m7_resize_contract.py"
        --exe "$<TARGET_FILE:MiniEngineSandbox>"
)

# M7-DEVICE-LOST-INJECT：`--inject-device-lost-after` 的两条约束（benchmark 禁止、需要真实窗口）
# 必须在创建 GPU 对象之前拒绝——只做参数层断言，端到端注入（设备移除 → 下一帧响亮失败 → 非零退出）
# 走发布证据 `E-M7-DEVICE-LOST-001`。
add_test(
    NAME M7DeviceLostCliContract
    COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m7_device_lost_contract.py"
        --exe "$<TARGET_FILE:MiniEngineSandbox>"
)

# M7-PREREG-EVIDENCE：预注册证据链（完整性 / 未篡改 / 载荷早于数据）用合成夹具自测，
# 保证"事后补写预注册"会被工具抓出来——纪律由工具守，不靠自觉。
add_test(
    NAME M7PreregEvidence
    COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tools/evidence/check_m7_prereg.py"
        --self-test
)

# 私有账本视图检查留在源库；公共入口由 PortfolioEntry 检查。

# 发布器预注册门（`-RequirePrereg`）：完整证据链放行、篡改/事后补写在**写盘前**中止、
# 不启用开关时行为不变。夹具与临时证据库都在临时目录，不碰真实库。
add_test(
    NAME M7PreregPublishGate
    COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m7_prereg_publish_gate.py"
        --repo "${PROJECT_SOURCE_DIR}"
)
