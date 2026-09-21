# ============================================================================
# CMakeLists.txt — 单元测试聚合入口
# 里程碑：M0
# 职责：拉取 GoogleTest 并聚合各模块测试子目录（core、platform、rhi/d3d11、
#   assets、world、tools）。gtest 以 FetchContent 引入，配置时可用本地源码
#   目录覆盖下载（详见 M2 验收记录环境限制）。
# 关联：docs/architecture/README.md
# ============================================================================

include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# 聚合各模块测试目标。
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(render_graph)
add_subdirectory(assets)
add_subdirectory(world)
add_subdirectory(rendering)
add_subdirectory(tools)
add_subdirectory(performance)
add_subdirectory(profiling)
add_subdirectory(tasks)

# 公共入口与导入安全契约；不包含联网发布检查。
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME PortfolioEntry COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/portfolio/validate_publication.py" --stage entry)
add_test(NAME PortfolioImportContracts COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tests/tools/contracts/test_import.py")
