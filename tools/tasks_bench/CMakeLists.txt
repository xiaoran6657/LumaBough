# ============================================================================
# tools/tasks_bench — M7-04 / M7-A12 调度器级微基准
# 里程碑：M7-04（workers × chunk × 版本 scaling sweep）
# 依赖：MiniEngineTasks（Core/Profiling）+ MiniEngineBenchmark（统计口径与 M7-01/M7-09 一致）；
#   刻意不链接 RHI/asset/render，保证测的是调度器而不是渲染路径。
# 关联：tools/performance/run_m7_task_sweep.ps1、tools/performance/summarize_m7_task_sweep.ps1
# ============================================================================

add_executable(MiniEngineTasksBench
    main.cpp
)

target_compile_features(MiniEngineTasksBench PRIVATE cxx_std_20)
set_target_properties(MiniEngineTasksBench PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineTasksBench)

target_link_libraries(MiniEngineTasksBench
    PRIVATE
        MiniEngineTasks
        MiniEngineBenchmark
)
