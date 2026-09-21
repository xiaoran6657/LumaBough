# CMakeLists.txt — MiniEngineD3D12Tests 单元测试
# 里程碑：M5（01 篇架构边界与输出一致性契约）
# 职责：D3D12 后端的纯 CPU 契约测试。M5 的 GPU 侧证据走 PIX 与调试层，
#       凡能在主机侧锁定的契约（对齐算法、HRESULT 转义，以及后续加入的 fence
#       时间线、descriptor 分配与 Upload Ring 环绕规则）都在此下单测，
#       不用 GPU capture 代替——capture 只回答"这一帧画了什么"。
# 依赖：只链接 MiniEngine::D3D12 与 gtest_main——被测内容是纯值语义，不需要设备。
# 关联：docs/architecture/README.md

add_executable(
    MiniEngineD3D12Tests
    BindingContractDriftTests.cpp
    D3D12AssetCacheDeviceTests.cpp
    D3D12CameraParityTests.cpp
    D3D12CbufferDriftTests.cpp
    D3D12ConstantsTests.cpp
    D3D12ForwardPassDeviceTests.cpp
    D3D12FrameReadbackDeviceTests.cpp
    D3D12PsoFactoryDeviceTests.cpp
    PipelineKeyTests.cpp
    ParityMetadataTests.cpp
    D3D12CommonTests.cpp
    D3D12DescriptorHeapDeviceTests.cpp
    D3D12DeviceTests.cpp
    D3D12DredDiagnosticsTests.cpp
    D3D12DredLiveTests.cpp
    HlslMathParityTests.cpp
    D3D12QueueDeviceTests.cpp
    D3D12ResourceStateTrackerDeviceTests.cpp
    D3D12TrianglePassDeviceTests.cpp
    D3D12RootSignatureDeviceTests.cpp
    D3D12SwapChainDeviceTests.cpp
    D3D12UploadDeviceTests.cpp
    DeferredReleaseTests.cpp
    DescriptorAllocatorTests.cpp
    FenceTimelineTests.cpp
    ResourceStateTrackerTests.cpp
    RootBindingsContractTests.cpp
    ShaderCompileGateTests.cpp
    ShaderSemanticIdentityTests.cpp
    ShadowParityContractTests.cpp
    SwapChainConfigTests.cpp
    UploadRingTests.cpp
)

# M6 审计 DRED-E2E：真实 device-removed 故障注入取证（默认 GTEST_SKIP，仅在
# MINIENGINE_DRED_E2E=1 时由 tools/legacy/run_m612_dred_e2e.py 显式执行——TDR/页错误会复位显示驱动）。
add_executable(MiniEngineD3D12DredE2E D3D12DredE2ETests.cpp)
target_include_directories(MiniEngineD3D12DredE2E PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
target_link_libraries(MiniEngineD3D12DredE2E PRIVATE MiniEngine::D3D12 GTest::gtest_main d3dcompiler.lib)
target_compile_definitions(MiniEngineD3D12DredE2E PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(MiniEngineD3D12DredE2E PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineD3D12DredE2E)
add_test(NAME D3D12DredE2ETests COMMAND MiniEngineD3D12DredE2E)
set_tests_properties(D3D12DredE2ETests PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# ShaderCompileGateTests 复用 tools/shader_compiler 的入口清单（同一份 registry：
# "能被编译的集合" == "被 gate 验证的集合"，不让两边各抄一份清单）。
target_include_directories(
    MiniEngineD3D12Tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/tools/shader_compiler/src"
        "${PROJECT_SOURCE_DIR}/samples/common"
)
target_sources(
    MiniEngineD3D12Tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/tools/shader_compiler/src/ShaderCompileRegistry.cpp"
)

# RootBindingsContractTests 需要 D3D11 的绑定契约头做逐寄存器对照（纯枚举，无链接
# 需求）：这是"M5 与 M4 寄存器一致"这条 parity 前提的可执行断言。
target_include_directories(
    MiniEngineD3D12Tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/engine/rhi/d3d11/include
)

# D3D12Queue/D3D12FrameContext 是后端内部类型（src/），与 D3D11 测试引内部
# ShaderCompiler 头同口径：测试直连内部头，保证与运行时唯一同源。
target_include_directories(
    MiniEngineD3D12Tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src
)

target_link_libraries(
    MiniEngineD3D12Tests
    PRIVATE
        MiniEngine::D3D12
        # ShaderCompileGateTests 需要 MiniEngine::Assets 的 SHA-256（与 cooker 同口径哈希）。
        MiniEngine::Assets
        # ShaderCompileGateTests 直接调用 IDxcPdbUtils2::IsFullPDB 校验落盘 PDB 类型。
        dxcompiler
        # D3D12CameraParityTests 直接比较两份相机实现，因此**新增了对 D3D11 后端的
        # 链接依赖**（审查三-8 纠正此前"与引头同口径"的低估：此前只是 include 头，
        # 现在是链接整个静态库——D3D11Camera 的实现体在 D3D11Camera.cpp）。
        # 仅测试、PRIVATE、不进生产：这是"M5 与 M4 一致"这条 parity 前提的可执行断言。
        # ME_VERIFY（Assert.cpp）在 Core，由 D3D11/D3D12 的 PRIVATE 依赖以
        # LINK_ONLY 传递过来，故不再重复列 Core（审查小问题②）。
        MiniEngine::D3D11
        GTest::gtest_main
)

# BindingContractDriftTests 需要打开 shaders/d3d12/BindingContract.hlsli 做漂移比对；
# 目录注入方式与 D3D11 的 HLSL 门禁一致。
target_compile_definitions(
    MiniEngineD3D12Tests
    PRIVATE
        MINIENGINE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/d3d12"
        # ShaderCompileGateTests 需要仓库根（重算 sourceSha256 用）。
        MINIENGINE_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# M5-08：PSO 测试需要的 DXIL 产物由 **tools/shader_compiler 真实产出**（不在测试里
# 重新编译 shader）：这样"产物缺失/过期"会直接表现为测试失败，而不是被测试内部的
# 第二次编译掩盖。产物目标在 tools/shader_compiler 处定义，测试与 Sandbox 共用。
# ---------------------------------------------------------------------------
add_dependencies(MiniEngineD3D12Tests MiniEngineD3D12ShaderArtifacts)
target_compile_definitions(
    MiniEngineD3D12Tests
    PRIVATE
        MINIENGINE_D3D12_SHADER_DIR="${MINIENGINE_D3D12_SHADER_OUTPUT_DIR}"
)

# ctest 运行时需要 DLL 在 exe 旁（工具已由 POST_BUILD 复制；测试同样需要）。
# 告警口径与 tools/shader_compiler 对齐（审查建议）：dxc.exe 不在 PATH 时
# `MINIENGINE_DXC_BIN_DIR` 未定义 → 路径退化为 "/dxil.dll"、条件恒假而**静默跳过**，
# 测试随后以 0xC0000135（STATUS_DLL_NOT_FOUND）失败却看不到原因。因此两个分支都告警。
if(EXISTS "${MINIENGINE_DXC_BIN_DIR}/dxil.dll")
    foreach(dxcRuntimeDll IN ITEMS dxcompiler.dll dxil.dll)
        if(EXISTS "${MINIENGINE_DXC_BIN_DIR}/${dxcRuntimeDll}")
            add_custom_command(
                TARGET MiniEngineD3D12Tests
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MINIENGINE_DXC_BIN_DIR}/${dxcRuntimeDll}"
                        "$<TARGET_FILE_DIR:MiniEngineD3D12Tests>/${dxcRuntimeDll}"
                COMMENT "Copying ${dxcRuntimeDll} next to the D3D12 tests"
            )
        else()
            message(WARNING "DXC runtime DLL not found: ${MINIENGINE_DXC_BIN_DIR}/${dxcRuntimeDll}")
        endif()
    endforeach()
else()
    message(WARNING "DXC runtime DLL directory not found (MINIENGINE_DXC_BIN_DIR='${MINIENGINE_DXC_BIN_DIR}'); "
                    "the D3D12 PSO tests will fail at runtime with 0xC0000135")
endif()

target_compile_features(
    MiniEngineD3D12Tests
    PRIVATE
        cxx_std_20
)

# NOMINMAX 必须在此显式定义：被测公共头经由 d3d12.h 引入 Win32 的 min/max 宏，
# 而本测试大量使用 std::numeric_limits<...>::max()（未加括号的 max 会被宏吃掉）。
target_compile_definitions(
    MiniEngineD3D12Tests
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(
    MiniEngineD3D12Tests
    PROPERTIES
        CXX_EXTENSIONS OFF
)

miniengine_set_project_warnings(MiniEngineD3D12Tests)

# M5-10：负例隔离到独立 executable；稳定性矩阵按独立进程显式运行。
add_executable(MiniEngineD3D12ValidationNegativeTests D3D12ValidationNegativeTests.cpp)
add_executable(MiniEngineD3D12StabilityTests D3D12StabilityMain.cpp D3D12StabilityPressure.cpp)
foreach(validationTarget IN ITEMS MiniEngineD3D12ValidationNegativeTests MiniEngineD3D12StabilityTests)
    target_link_libraries(${validationTarget} PRIVATE MiniEngine::D3D12 MiniEngine::Assets MiniEngine::World MiniEngine::Core)
    target_include_directories(${validationTarget} PRIVATE "${PROJECT_SOURCE_DIR}/engine/rhi/d3d12/src")
    target_compile_features(${validationTarget} PRIVATE cxx_std_20)
    target_compile_definitions(${validationTarget} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        MINIENGINE_D3D12_SHADER_DIR="${MINIENGINE_D3D12_SHADER_OUTPUT_DIR}")
    set_target_properties(${validationTarget} PROPERTIES CXX_EXTENSIONS OFF)
    miniengine_set_project_warnings(${validationTarget})
    add_dependencies(${validationTarget} MiniEngineD3D12ShaderArtifacts)
endforeach()
target_link_libraries(MiniEngineD3D12ValidationNegativeTests PRIVATE GTest::gtest_main)
foreach(pix_target MiniEngineD3D12Tests MiniEngineD3D12ValidationNegativeTests MiniEngineD3D12StabilityTests)
    miniengine_use_pix(${pix_target})
endforeach()

# POST_BUILD 按注册顺序执行：先部署 PIX DLL，再启动 GoogleTest 枚举。
# 否则首次单目标/干净构建会在复制 DLL 前以 0xc0000135 失败。
gtest_discover_tests(
    MiniEngineD3D12Tests
    DISCOVERY_TIMEOUT 30
)

gtest_discover_tests(MiniEngineD3D12ValidationNegativeTests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "D3D12ValidationNegative" RUN_SERIAL TRUE TIMEOUT 60)
