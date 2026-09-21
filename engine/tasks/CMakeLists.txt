# ============================================================================
# engine/tasks — M7 任务系统（M7-03 起：契约 + 全局有锁队列基线）
# 里程碑：M7-03（任务系统契约、所有权与生命周期）
# 职责：Task/TaskGroup 生命周期、wait-help、shutdown 状态机与统计。
# 依赖纪律（tools/validation/check_rhi_boundary.py 静态检查 + 本文件 manifest）：
#   * 只链接 MiniEngineCore（断言/日志）与 MiniEngineProfiling（线程名/zone）；
#   * 不得链接 RHI 后端、assets 或 render 模块：任务库是纯 CPU 调度，
#     GPU 上传/资源 commit/RenderGraph execute 只能发生在 render thread（ADR-0008）。
# 关联：docs/architecture/README.md
#       engine/tasks/src/TaskSystem.cpp、tests/tasks/CMakeLists.txt
# ============================================================================

add_library(MiniEngineTasks STATIC
    src/TaskSystem.cpp
)

target_include_directories(MiniEngineTasks
    PUBLIC include
    PRIVATE src
)

target_compile_features(MiniEngineTasks PUBLIC cxx_std_20)
set_target_properties(MiniEngineTasks PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineTasks)

# Core 是 PUBLIC（公共契约使用其断言/日志约定）；Profiling 是 PRIVATE：
# 需要的只是线程名/zone，不让消费者经由 tasks 拿到 Tracy 边界。
target_link_libraries(MiniEngineTasks
    PUBLIC MiniEngineCore
    PRIVATE MiniEngineProfiling
)

# TaskSystem must not link an RHI backend or the asset/render modules.
