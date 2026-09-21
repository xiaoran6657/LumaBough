# ============================================================================
# engine/assets/CMakeLists.txt — MiniEngineAssets 静态库（资产管线运行时）
# 里程碑：M3（7-A）
# 职责：定义 MiniEngineAssets：AssetId / AssetHandle / AssetPool / AssetRegistry /
#       AssetManager、Baked 格式与 Reader、可移植 SHA-256。依赖只到 Core，
#       不依赖 World / RHI / fastgltf，也不含平台 API——因此引擎侧 SHA-256
#       是纯 C++ 实现（与 Cooker 的 CNG 版并存，见 ADR-0004「影响」）。
# 关联：docs/architecture/DECISIONS.md「影响」
# ============================================================================

# 资产管线运行时库：头文件大量使用 Core 的 FileSystem / Assert，故 Core 为 PUBLIC；
# include 目录 PUBLIC 导出，供 World / D3D11 / Cooker 消费。
add_library(MiniEngineAssets STATIC
    include/MiniEngine/Assets/AssetHandle.h
    include/MiniEngine/Assets/AssetId.h
    include/MiniEngine/Assets/AssetManager.h
    include/MiniEngine/Assets/AssetPool.h
    include/MiniEngine/Assets/AssetRegistry.h
    include/MiniEngine/Assets/BakedFormat.h
    include/MiniEngine/Assets/BakedReader.h
    include/MiniEngine/Assets/Internal/AssetDecode.h
    include/MiniEngine/Assets/Internal/BakedReadUtil.h
    include/MiniEngine/Assets/MaterialAsset.h
    include/MiniEngine/Assets/MeshAsset.h
    include/MiniEngine/Assets/PbrVertex.h
    include/MiniEngine/Assets/Sha256.h
    include/MiniEngine/Assets/TextureAsset.h
    include/MiniEngine/Assets/TextureFormatV2.h
    src/AssetDecode.cpp
    src/AssetId.cpp
    src/AssetManager.cpp
    src/AssetRegistry.cpp
    src/BakedReader.cpp
    src/Sha256.cpp
)
add_library(MiniEngine::Assets ALIAS MiniEngineAssets)


target_include_directories(MiniEngineAssets
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(MiniEngineAssets PUBLIC cxx_std_20)
target_link_libraries(MiniEngineAssets PUBLIC MiniEngine::Core)
set_target_properties(
    MiniEngineAssets
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineAssets)

# M7-06：异步加载流水线独立成库（要有 Tasks 依赖，不让 M3 的 Assets 反向背上线程依赖）。
add_subdirectory(async)
