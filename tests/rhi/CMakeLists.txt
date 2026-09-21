add_subdirectory(d3d11)

# M5：D3D12 后端的 CPU 侧契约测试，与 D3D11 测试并列。
add_subdirectory(d3d12)

# 每个公共头单独成为一个 translation unit；不使用 PCH/unity build。
file(GLOB rhi_public_headers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/engine/rhi/include/MiniEngine/Rhi/*.h")
set(rhi_header_units)
foreach(header IN LISTS rhi_public_headers)
    get_filename_component(name "${header}" NAME)
    set(unit "${CMAKE_CURRENT_BINARY_DIR}/header-smoke/${name}.cpp")
    file(WRITE "${unit}" "#include <MiniEngine/Rhi/${name}>\n#if defined(_WINDOWS_) || defined(_D3D11_H_) || defined(__d3d12_h__)\n#error Native platform header leaked\n#endif\n")
    list(APPEND rhi_header_units "${unit}")
endforeach()
add_executable(RhiPublicHeaderSmoke RhiPublicHeaderSmoke.cpp ${rhi_header_units})
target_link_libraries(RhiPublicHeaderSmoke PRIVATE MiniEngineRhiPublic)
set_target_properties(RhiPublicHeaderSmoke PROPERTIES CXX_EXTENSIONS OFF UNITY_BUILD OFF)
miniengine_set_project_warnings(RhiPublicHeaderSmoke)
add_test(NAME RhiPublicHeaderSmoke COMMAND RhiPublicHeaderSmoke)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME RhiBoundaryAudit COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/validation/check_rhi_boundary.py" --root "${PROJECT_SOURCE_DIR}")
add_test(NAME RhiBoundaryAuditNegativeControls COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/validation/check_rhi_boundary.py" --self-test)

# M6-02：公共注册表、资源描述与 capability policy 的纯 CPU 单测。
add_executable(MiniEngineRhiTests HandleRegistryTests.cpp RhiDescriptorTests.cpp DeviceLifetimeTests.cpp RhiUploadValidationTests.cpp PipelineContractTests.cpp ShaderPackageTests.cpp)
target_include_directories(MiniEngineRhiTests PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/src")
target_link_libraries(MiniEngineRhiTests PRIVATE MiniEngineRhiPublic GTest::gtest_main)
set_target_properties(MiniEngineRhiTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRhiTests)
gtest_discover_tests(MiniEngineRhiTests)

# 真实能力查询单独接线，public smoke/CPU 测试不获得 native include/link。
add_executable(MiniEngineRhiBackendCapabilitiesTests RhiBackendCapabilitiesTests.cpp)
target_include_directories(MiniEngineRhiBackendCapabilitiesTests PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/rhi/d3d11/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
target_link_libraries(MiniEngineRhiBackendCapabilitiesTests PRIVATE
    MiniEngineRhiPublic MiniEngineD3D11 MiniEngineD3D12 GTest::gtest_main d3d11 d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineRhiBackendCapabilitiesTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(MiniEngineRhiBackendCapabilitiesTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRhiBackendCapabilitiesTests)
gtest_discover_tests(MiniEngineRhiBackendCapabilitiesTests)

# M6-03 原生生命周期验证独立于公共头/CPU target；负例逐个 CTest 进程隔离。
add_executable(MiniEngineRhiLifetimeDeviceTests D3D11LifetimeDeviceTests.cpp D3D12LifetimeDeviceTests.cpp)
target_include_directories(MiniEngineRhiLifetimeDeviceTests PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/rhi/src"
    "${PROJECT_SOURCE_DIR}/engine/rhi/d3d11/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
target_link_libraries(MiniEngineRhiLifetimeDeviceTests PRIVATE
    MiniEngineRhiPublic MiniEngineD3D11 MiniEngineD3D12 GTest::gtest_main d3d11 d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineRhiLifetimeDeviceTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(MiniEngineRhiLifetimeDeviceTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRhiLifetimeDeviceTests)
gtest_discover_tests(MiniEngineRhiLifetimeDeviceTests PROPERTIES TIMEOUT 60)

# M6-04 双变体包按 config 隔离；只有完整编译/reflection 校验成功才更新消费者头。
set(m604_package_dir "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>")
add_executable(MiniEngineM604PipelineDeviceTests M604PipelineDeviceTests.cpp M604D3D11Probe.cpp M604D3D12Probe.cpp)
add_dependencies(MiniEngineM604PipelineDeviceTests MiniEngineM604ShaderPackages)
target_include_directories(MiniEngineM604PipelineDeviceTests PRIVATE "${m604_package_dir}"
    "${PROJECT_SOURCE_DIR}/engine/rhi/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d11/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
target_link_libraries(MiniEngineM604PipelineDeviceTests PRIVATE MiniEngineRhiPublic MiniEngineD3D11 MiniEngineD3D12 GTest::gtest_main d3d11 d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineM604PipelineDeviceTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    M604_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/m604-native/$<CONFIG>")
set_target_properties(MiniEngineM604PipelineDeviceTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineM604PipelineDeviceTests)
gtest_discover_tests(MiniEngineM604PipelineDeviceTests PROPERTIES TIMEOUT 120)

add_test(NAME M604PackageNegativeControls COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/shader_compiler/test_rhi_package.py"
    --compiler $<TARGET_FILE:MiniEngineShaderCompiler> --root "${PROJECT_SOURCE_DIR}"
    --work "${CMAKE_BINARY_DIR}/package-negative/$<CONFIG>")
set_tests_properties(M604PackageNegativeControls PROPERTIES TIMEOUT 180)

# M6-05 无 GPU 的 IRhiDevice/command 协议替身，不链接任何 native backend。
add_library(MiniEngineTraceRhi STATIC ../trace/TraceRhi.cpp)
target_include_directories(MiniEngineTraceRhi PUBLIC "${PROJECT_SOURCE_DIR}/tests/trace" "${PROJECT_SOURCE_DIR}/engine/rhi/src")
target_link_libraries(MiniEngineTraceRhi PUBLIC MiniEngineRhiPublic)
set_target_properties(MiniEngineTraceRhi PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineTraceRhi)
add_executable(MiniEngineM605TraceTests CommandValidationTests.cpp ../trace/TraceRhiTests.cpp)
target_link_libraries(MiniEngineM605TraceTests PRIVATE MiniEngineTraceRhi GTest::gtest_main)
target_compile_definitions(MiniEngineM605TraceTests PRIVATE M605_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/m605-trace/$<CONFIG>")
set_target_properties(MiniEngineM605TraceTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineM605TraceTests)
gtest_discover_tests(MiniEngineM605TraceTests PROPERTIES TIMEOUT 60)

# M6-06 CLI parser remains CPU-only.
target_sources(MiniEngineRhiTests PRIVATE RhiSelectionTests.cpp ../../samples/rhi_sandbox/RhiSelection.cpp)

# M6-06：通过同一生产 factory 验证 native shader 所有权、回滚与帧间替换。
add_executable(MiniEngineM606AdapterDeviceTests M606AdapterDeviceTests.cpp)
target_include_directories(MiniEngineM606AdapterDeviceTests PRIVATE
    "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>" "${PROJECT_SOURCE_DIR}/engine/rhi/src")
target_link_libraries(MiniEngineM606AdapterDeviceTests PRIVATE
    MiniEngineRhiFactory MiniEngineRhiSmoke MiniEngine::PlatformWindows MiniEngine::Core GTest::gtest_main)
target_compile_definitions(MiniEngineM606AdapterDeviceTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(MiniEngineM606AdapterDeviceTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineM606AdapterDeviceTests)
add_dependencies(MiniEngineM606AdapterDeviceTests MiniEngineRhiShaderPackages)
include(MiniEnginePix)
miniengine_use_pix(MiniEngineM606AdapterDeviceTests)
gtest_discover_tests(MiniEngineM606AdapterDeviceTests PROPERTIES TIMEOUT 120)

# M6-06 初始上传失败的纯 CPU 事务回归。
target_sources(MiniEngineRhiTests PRIVATE NativeDeviceTests.cpp)

# M6-11 负向通道：公共 RHI + 测试专用开关，由 Debug Layer/GBV 按 message ID 精确捕获。
add_executable(MiniEngineM611NegativeChannelTests M611NegativeChannelTests.cpp)
target_include_directories(MiniEngineM611NegativeChannelTests PRIVATE
    "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>" "${PROJECT_SOURCE_DIR}/engine/rhi/src")
target_link_libraries(MiniEngineM611NegativeChannelTests PRIVATE
    MiniEngineRhiFactory MiniEngineRhiSmoke MiniEngine::PlatformWindows MiniEngine::Core GTest::gtest_main
    d3d11 d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineM611NegativeChannelTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(MiniEngineM611NegativeChannelTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineM611NegativeChannelTests)
add_dependencies(MiniEngineM611NegativeChannelTests MiniEngineRhiShaderPackages)
include(MiniEnginePix)
miniengine_use_pix(MiniEngineM611NegativeChannelTests)
gtest_discover_tests(MiniEngineM611NegativeChannelTests PROPERTIES TIMEOUT 120)

add_test(NAME M606CliContract COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m606_cli.py"
    --exe $<TARGET_FILE:MiniEngineSandbox> --output "${CMAKE_BINARY_DIR}/m606-cli/$<CONFIG>")
set_tests_properties(M606CliContract PROPERTIES TIMEOUT 240)

# M6-06 独立审查缺陷的实际 GPU 回归。
target_sources(MiniEngineM606AdapterDeviceTests PRIVATE M606RegressionTests.cpp)

# M6-06 旧 concrete 机制的固定输入对照；不经过 public factory。
add_executable(MiniEngineM606LegacyProbeTests M606LegacyProbeTests.cpp M604D3D11Probe.cpp M604D3D12Probe.cpp)
add_dependencies(MiniEngineM606LegacyProbeTests MiniEngineM604ShaderPackages)
target_include_directories(MiniEngineM606LegacyProbeTests PRIVATE "${m604_package_dir}"
    "${PROJECT_SOURCE_DIR}/engine/rhi/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d11/src" "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
target_link_libraries(MiniEngineM606LegacyProbeTests PRIVATE MiniEngineRhiPublic MiniEngineD3D11 MiniEngineD3D12 GTest::gtest_main d3d11 d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineM606LegacyProbeTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    M606_LEGACY_DIR="${CMAKE_BINARY_DIR}/m606-legacy/$<CONFIG>")
set_target_properties(MiniEngineM606LegacyProbeTests PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineM606LegacyProbeTests)
gtest_discover_tests(MiniEngineM606LegacyProbeTests PROPERTIES TIMEOUT 120)

# M6-10 原生环境准备回归使用独立输入，不依赖 out/m4 场景产物。
target_sources(MiniEngineM606AdapterDeviceTests PRIVATE M610EnvironmentDeviceTests.cpp)
add_dependencies(MiniEngineM606AdapterDeviceTests MiniEngineD3D12ShaderArtifacts)
target_compile_definitions(MiniEngineM606AdapterDeviceTests PRIVATE
    M610_TEST_SHADER_ROOT="$<TARGET_FILE_DIR:MiniEngineM606AdapterDeviceTests>/shaders")
add_custom_command(TARGET MiniEngineM606AdapterDeviceTests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/shaders/d3d11"
        "$<TARGET_FILE_DIR:MiniEngineM606AdapterDeviceTests>/shaders/d3d11"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${MINIENGINE_D3D12_SHADER_OUTPUT_DIR}"
        "$<TARGET_FILE_DIR:MiniEngineM606AdapterDeviceTests>/shaders/d3d12")

add_test(NAME M610PerformanceEvidence
    COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_m610_performance.py")
set_tests_properties(M610PerformanceEvidence PROPERTIES TIMEOUT 30)

# M6-10 parity evidence and matrix ordering are persistent CPU-only contracts.
add_test(NAME M610ParityCpu
    COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/test_m610_parity.py")
set_tests_properties(M610ParityCpu PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME M610CaptureIndexCpu COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/test_m610_capture_index.py")

# M6-11 稳定性诊断的正负向纯 CPU 判定。
add_test(NAME M611StabilityCpu COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/test_m611_stability.py")
set_tests_properties(M611StabilityCpu PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# M6-12 collect 汇总的 parity 字段回归（审计勘误 2026-09-16）。
add_test(NAME M612CollectSummaryCpu COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/test_m612_collect_summary.py")
set_tests_properties(M612CollectSummaryCpu PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
