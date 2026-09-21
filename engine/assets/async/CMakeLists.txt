# ============================================================================
# engine/assets/async/CMakeLists.txt — MiniEngineAssetsAsync 静态库（M7-06）
# 里程碑：M7-06（异步资产加载流水线）
# 职责：I/O 线程 + decode task + 上传队列的三段流水线。依赖方向：
#         MiniEngine::Assets（M3 的 Registry/BakedReader/解码原语，PUBLIC）
#         MiniEngine::Tasks（decode 提交，PRIVATE：public header 只前向声明 TaskSystem）
#         MiniEngine::Profiling（线程名与 zone，PRIVATE）
#       禁止依赖 World / RHI / 平台头（tools/validation/check_rhi_boundary.py 覆盖 engine/assets）。
# 关联：docs/architecture/README.md
# ============================================================================

add_library(MiniEngineAssetsAsync STATIC
    include/MiniEngine/Assets/AssetLoadPayload.h
    include/MiniEngine/Assets/AssetLoadRequest.h
    include/MiniEngine/Assets/AssetLoadState.h
    include/MiniEngine/Assets/AssetUploadCoordinator.h
    include/MiniEngine/Assets/AsyncAssetLoader.h
    include/MiniEngine/Assets/CookedFileSource.h
    include/MiniEngine/Assets/TaskSystemDecodeDispatcher.h
    include/MiniEngine/Assets/UploadBudget.h
    include/MiniEngine/Assets/UploadSink.h
    src/AssetUploadCoordinator.cpp
    src/AsyncAssetLoader.cpp
    src/BoundedQueue.h
    src/CookedFileSource.cpp
)
add_library(MiniEngine::AssetsAsync ALIAS MiniEngineAssetsAsync)

target_include_directories(MiniEngineAssetsAsync
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(MiniEngineAssetsAsync PUBLIC cxx_std_20)
target_link_libraries(MiniEngineAssetsAsync
    PUBLIC
        MiniEngine::Assets
    PRIVATE
        MiniEngineTasks
        MiniEngineProfiling
)
set_target_properties(
    MiniEngineAssetsAsync
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineAssetsAsync)
