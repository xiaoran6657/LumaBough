# ============================================================================
# CMakeLists.txt — MiniEngine 引擎库聚合入口
# 里程碑：M0
# 职责：按依赖顺序聚合引擎各子库：core（无平台依赖）、platform（平台实现）、
#   rhi（图形后端归档目录，M2 起包含 d3d11）。
# 关联：docs/architecture/README.md
# ============================================================================

# 依赖方向固定为 Sandbox → PlatformWindows → Core，且 Core 不反向依赖平台/RHI。
add_subdirectory(core)
# M7-02：观测门面。只依赖可选的 Tracy，不依赖 Core/RHI；core 等模块可以反向链接它
# （例如 Log 使用可观测锁），因此放在 core 之后、其余模块之前。
add_subdirectory(profiling)
# M7-03：任务系统（纯 CPU 调度）。只依赖 Core 与 Profiling，不依赖 RHI/assets/render，
# 因此放在平台/RHI 之前；上层（assets/render）在需要时反向链接它。
add_subdirectory(tasks)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(assets)
add_subdirectory(world)
add_subdirectory(render_graph)
add_subdirectory(render)
