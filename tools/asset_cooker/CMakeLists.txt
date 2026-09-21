# ============================================================================
# tools/asset_cooker/CMakeLists.txt — MiniEngineAssetCooker 可执行程序（离线烘焙）
# 里程碑：M3-04 / M3-05 / 02 篇 / 03 篇 / watch
# 职责：定义 Cooker 可执行文件：它是唯一允许链接 fastgltf、WIC 与 CNG 的目标
#      （ADR-0004 §1——运行时绝不解析 glTF/JSON/PNG/JPEG）。
#       依赖全部 PRIVATE：Cooker 是终端可执行文件，不导出头文件；
#       bcrypt / ole32 / windowscodecs 为 CNG 与 WIC 的系统库；
#       PlatformWindows 提供 watch 命令的 DirectoryWatcherWin32。
# 关联：docs/architecture/DECISIONS.md §1、§2
# ============================================================================

add_executable(MiniEngineAssetCooker
    src/AtomicPublish.cpp
    src/AtomicPublish.h
    src/BakedWriter.cpp
    src/BakedWriter.h
    src/CookSession.cpp
    src/CookSession.h
    src/GltfImportAdapter.cpp
    src/ManifestWriter.cpp
    src/ManifestWriter.h
    src/MaterialArtifactWriter.cpp
    src/MaterialArtifactWriter.h
    src/MeshArtifactWriter.cpp
    src/MeshArtifactWriter.h
    src/TextureArtifactWriter.cpp
    src/TextureArtifactWriter.h
    src/TextureMipBuilder.cpp
    src/TextureMipBuilder.h
    src/TangentGenerator.cpp
    src/TangentGenerator.h
    src/RecipeJson.cpp
    src/RecipeJson.h
    src/Sha256.cpp
    src/Sha256.h
    src/main.cpp
    src/AssetCache.cpp
    src/AssetCache.h
    src/BuildKey.cpp
    src/BuildKey.h
    src/CanonicalRecipe.cpp
    src/CanonicalRecipe.h
    src/ContentHash.cpp
    src/ContentHash.h
    src/DependencyRecord.cpp
    src/DependencyRecord.h
    src/GltfGeometry.cpp
    src/GltfGeometry.h
    src/HdrImage.cpp
    src/HdrImage.h
    src/MeworldWriter.cpp
    src/MeworldWriter.h
    src/SourceUri.cpp
    src/SourceUri.h
    src/WicImage.cpp
    src/WicImage.h
    deps/mikktspace/mikktspace.c
    deps/stb/StbImage.cpp
)

target_compile_features(MiniEngineAssetCooker PRIVATE cxx_std_20)
# vendored mikktspace.c 不在 /W4 /WX 门禁内：第三方源码保持原样编译（pin 见 deps/README.txt）。
set_source_files_properties(deps/mikktspace/mikktspace.c PROPERTIES COMPILE_OPTIONS "/w;/FS")
target_include_directories(MiniEngineAssetCooker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/mikktspace")
# stb_image：vendored（deps/stb/README.txt 的 pin 与使用约束）；/w 同 mikktspace
# ——第三方源码不套 /W4 /WX。
set_source_files_properties(deps/stb/StbImage.cpp PROPERTIES COMPILE_OPTIONS "/w;/FS")
target_include_directories(MiniEngineAssetCooker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/stb")
target_link_libraries(MiniEngineAssetCooker
    PRIVATE
        MiniEngine::Assets
        MiniEngine::Core
        MiniEngine::PlatformWindows
        fastgltf::fastgltf
        bcrypt
        ole32
        windowscodecs
)
set_target_properties(
    MiniEngineAssetCooker
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineAssetCooker)