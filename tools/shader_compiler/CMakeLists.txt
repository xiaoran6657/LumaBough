# ============================================================================
# tools/shader_compiler/CMakeLists.txt — MiniEngineShaderCompiler 可执行程序
# 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 1 条）
# 职责：定义 shader 离线编译工具：它是唯一允许链接 DXC 的目标（ADR-0006：运行时
#   绝不编译 shader——帧循环不创建 PSO、不碰 DXC）。产物（.dxil / PDB / manifest）
#   是 build artifact，不入库（见 .gitignore 的 *.dxil）。
# 依赖来源：DXC 取自 **Windows SDK**（`dxcompiler.lib` + 运行时 `dxcompiler.dll`），
#   不引入第三方 fetch；若日后改为 pinned 的 vendored DXC，只需替换此处的链接目标，
#   DxcCompiler 的接口与 manifest 口径不变（该决策点记录在 M5-08 记录 §6）。
# 链接：MiniEngine::Assets 仅用于 SHA-256（sourceSha256/dxilSha256 与仓库同口径）。
# 关联：docs/architecture/README.md
# ============================================================================

add_executable(MiniEngineShaderCompiler
    src/DxcCompiler.cpp
    src/DxcCompiler.h
    src/DualShaderPackage.cpp
    src/DualShaderPackage.h
    src/ShaderCompileRegistry.cpp
    src/ShaderCompileRegistry.h
    src/main.cpp
)

target_compile_features(MiniEngineShaderCompiler PRIVATE cxx_std_20)

target_link_libraries(MiniEngineShaderCompiler
    PRIVATE
        MiniEngine::Assets
        dxcompiler
        # M6-04 离线 package 仅由该工具调用 D3DCompileFromFile/D3DReflect；
        # 不把 d3dcompiler 依赖传播到运行时 target。
        d3dcompiler
)

set_target_properties(
    MiniEngineShaderCompiler
    PROPERTIES
        CXX_EXTENSIONS OFF
)

target_compile_definitions(
    MiniEngineShaderCompiler
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

miniengine_set_project_warnings(MiniEngineShaderCompiler)

# ---------------------------------------------------------------------------
# D3D12 shader 产物目标（build artifact，不入库）：测试与 Sandbox 都依赖它。
# 放在本文件而不是任一消费者侧，是为了让"编译产出 DXIL/PDB/manifest"只有一处定义，
# 两个消费者拿到的一定是同一份产物（APIVER/CMake 目标名在本仓库即交付契约）。
# ---------------------------------------------------------------------------
# CACHE INTERNAL：普通目录变量只在自身子树可见，而 samples/ 与 tests/ 是 tools/ 的
# **兄弟目录**（PARENT_SCOPE 只能回写到 tools/ 这一层，到不了根作用域）。用 cache
# 变量保证"产物路径"全局唯一，避免出现两处各写一份字符串后悄悄漂移。
set(MINIENGINE_D3D12_SHADER_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/shaders/d3d12"
    CACHE INTERNAL "Output directory of the offline D3D12 shader compilation")
add_custom_target(
    MiniEngineD3D12ShaderArtifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_D3D12_SHADER_OUTPUT_DIR}"
    COMMAND $<TARGET_FILE:MiniEngineShaderCompiler> --repo "${PROJECT_SOURCE_DIR}" --out
            "${MINIENGINE_D3D12_SHADER_OUTPUT_DIR}"
    DEPENDS MiniEngineShaderCompiler
    COMMENT "Compiling D3D12 shaders (DXIL + PDB + manifest)"
    VERBATIM
)

# DXC 运行时：dxcompiler.dll / dxil.dll 来自 Windows SDK 的 bin 目录（与 dxc.exe 同处）。
# 直接把 DLL 复制到工具输出目录，避免依赖 PATH——PATH 上的 SDK bin 在开发 shell 里
# 存在，但"能编译"不该取决于当前 shell 的环境（M5-08 实测：不复制时工具以
# STATUS_DLL_NOT_FOUND 0xC0000135 退出，而同一 shell 里 dxc.exe 可用）。
# find_program 只在配置期求值一次；找不到时给出显式告警而不是静默跳过。
find_program(MINIENGINE_DXC_EXECUTABLE NAMES dxc.exe
    HINTS "[HKEY_LOCAL_MACHINE/SOFTWARE/Microsoft/Windows Kits/Installed Roots;KitsRoot10]/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    REGISTRY_VIEW BOTH
    REQUIRED
    DOC "Windows SDK dxc.exe (used to locate the DXC runtime DLLs)")
if(MINIENGINE_DXC_EXECUTABLE)
    get_filename_component(MINIENGINE_DXC_BIN_DIR_LOCAL "${MINIENGINE_DXC_EXECUTABLE}" DIRECTORY)
    # CACHE INTERNAL（审查 P3-1）：本目录作用域的普通变量对**兄弟目录**（tests/、samples/）
    # 不可见——tests 的 DLL 复制块会因条件恒假而静默跳过（"写了但从未执行"）。
    # 提成 cache 变量后该块才真正生效；与上面的产物输出目录同一手法。
    set(MINIENGINE_DXC_BIN_DIR
        "${MINIENGINE_DXC_BIN_DIR_LOCAL}"
        CACHE INTERNAL "Directory holding the Windows SDK DXC runtime DLLs (dxcompiler.dll / dxil.dll)")
    foreach(dxcRuntimeDll IN ITEMS dxcompiler.dll dxil.dll)
        if(EXISTS "${MINIENGINE_DXC_BIN_DIR}/${dxcRuntimeDll}")
            add_custom_command(
                TARGET MiniEngineShaderCompiler
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MINIENGINE_DXC_BIN_DIR}/${dxcRuntimeDll}"
                        "$<TARGET_FILE_DIR:MiniEngineShaderCompiler>/${dxcRuntimeDll}"
                COMMENT "Copying ${dxcRuntimeDll} next to the shader compiler"
            )
        else()
            message(WARNING "DXC runtime DLL not found: ${MINIENGINE_DXC_BIN_DIR}/${dxcRuntimeDll}")
        endif()
    endforeach()
else()
    message(WARNING "dxc.exe not found on PATH; MiniEngineShaderCompiler will fail at runtime without the DXC DLLs")
endif()

# M6-06：离线双变体包也供 sandbox 使用，不依赖 BUILD_TESTING。
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(m604_package_dir "${CMAKE_BINARY_DIR}/shaders/m6-04/$<CONFIG>")
add_custom_target(MiniEngineM604ShaderPackages
    COMMAND $<TARGET_FILE:MiniEngineShaderCompiler> --package --repo "${PROJECT_SOURCE_DIR}" --out "${m604_package_dir}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/shader_compiler/generate_rhi_package.py"
        --manifest "${m604_package_dir}/manifest.json" --bindings "${PROJECT_SOURCE_DIR}/tools/shader_compiler/m6-binding-contract.json"
        --header "${m604_package_dir}/M604ShaderFixtures.h" --summary "${m604_package_dir}/logical-packages.json"
    DEPENDS MiniEngineShaderCompiler VERBATIM)

add_custom_target(MiniEngineRhiShaderPackages DEPENDS MiniEngineM604ShaderPackages)
