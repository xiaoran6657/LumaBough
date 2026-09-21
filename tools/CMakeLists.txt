include(FetchContent)

# fastgltf 会 find_package(simdjson CONFIG)（非 REQUIRED）；本机命中的 simdjson 是
# 共享 DLL 版，从不随 exe 交付 → 运行时 STATUS_DLL_NOT_FOUND(0xC0000135)。
# 旧策略只毒化 simdjson_DIR 指向不存在的目录——但 CONFIG 模式在 _DIR 无效时仍会
# 搜索其他前缀并把 _DIR 覆盖回真实位置（2026-09-06 实锤：cache 里出现过
# C:/ProgramData/miniconda3/Library/...，fastgltf 动态链接 conda 的 simdjson.dll，
# ABI 不匹配 → GltfImportAdapterTests SEH 0xc0000005 空栈崩溃）。
# CMAKE_DISABLE_FIND_PACKAGE_simdjson 使 find_package 无条件 NOT FOUND，
# fastgltf 稳定回退到自带 single-header simdjson（deps/simdjson，静态编入 fastgltf.lib）。
set(CMAKE_DISABLE_FIND_PACKAGE_simdjson ON)

FetchContent_Declare(
    fastgltf
    URL https://github.com/spnda/fastgltf/archive/refs/tags/v0.9.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(fastgltf)

add_subdirectory(asset_cooker)

# M5-08：D3D12 shader 的离线编译工具（唯一链接 DXC 的目标）。
add_subdirectory(shader_compiler)

# M7-01：性能实验的机读 schema、统计与 raw JSON 序列化（纯 CPU，无第三方依赖）。
add_subdirectory(benchmark)

# M7-04：调度器级微基准（workers × chunk × 版本 scaling sweep，A12）。
add_subdirectory(tasks_bench)