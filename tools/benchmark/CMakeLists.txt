# M7-01：性能实验机读契约与统计原语。sandbox 用它写出 raw JSON；
# 性能契约测试（tests/performance）用它复算中位数/MAD/p95/p99。
# 依赖保持为零：不链接 RHI/World，避免把后端类型带进性能证据层。
add_library(MiniEngineBenchmark STATIC
    src/BenchmarkJson.cpp
    src/Statistics.cpp
)

target_include_directories(MiniEngineBenchmark PUBLIC include)
target_compile_features(MiniEngineBenchmark PUBLIC cxx_std_20)
set_target_properties(MiniEngineBenchmark PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineBenchmark)

# ============================================================================
# M7-08：Google Benchmark 微基准（LayoutBenchmarks）
# 默认关闭（不进入 CI 计时 Gate，也不下载依赖）；开启方式：
#   cmake --preset windows-msvc-profile -DME_BUILD_BENCHMARKS=ON
# 依赖冻结：Google Benchmark v1.9.4（归档 SHA-256 B334658EDD35EFCF06A99D9BE21E4E93E092BD5F95074C1673D5C8705D95C104，
# 记录在 docs/architecture/DECISIONS.md）。无网络机器用官方覆盖机制：
#   -DFETCHCONTENT_SOURCE_DIR_BENCHMARK=out/third_party/benchmark-1.9.4
# 夹具与 tests/performance 的等价测试共用（同一份数据，micro 与等价结论可比）。
# ============================================================================
option(ME_BUILD_BENCHMARKS "Build non-CI micro benchmarks (Google Benchmark)" OFF)

if(ME_BUILD_BENCHMARKS)
    include(FetchContent)

    # 上游默认选项与本仓库无关的尽量关掉：只要 benchmark 库本身。
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
        URL_HASH SHA256=B334658EDD35EFCF06A99D9BE21E4E93E092BD5F95074C1673D5C8705D95C104
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(benchmark)

    add_executable(MiniEngineBenchmarks
        LayoutBenchmarks.cpp
    )
    target_include_directories(MiniEngineBenchmarks
        PRIVATE
            "${CMAKE_SOURCE_DIR}/tests/performance"
    )
    target_link_libraries(MiniEngineBenchmarks
        PRIVATE
            MiniEngine::World
            MiniEngineTasks
            MiniEngineBenchmark
            benchmark::benchmark
    )
    target_compile_features(MiniEngineBenchmarks PRIVATE cxx_std_20)
    set_target_properties(MiniEngineBenchmarks PROPERTIES CXX_EXTENSIONS OFF)
    miniengine_set_project_warnings(MiniEngineBenchmarks)
endif()

# Benchmarks are manually/periodically invoked, not pass/fail CTest timing gates.
