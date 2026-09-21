# ============================================================================
# tests/tools/CMakeLists.txt — Cooker 侧工具测试
# 里程碑：M3-04 / M3-05 / M3-02(glTF 导入)
# 职责：聚合 MiniEngineToolTests，直接编译 Cooker 的纯工具源文件。M3-05 起纳入
#       ContentHash/DependencyRecord/CanonicalRecipe/BuildKey/AssetCache；glTF 导入
#       G1 纳入 SourceUri（URI 安全边界）。链接 MiniEngine::Assets（AssetCache 命中
#       判定依赖 BakedReader）与 bcrypt（Cooker 侧 SHA-256 走 CNG）；不把 tools 变成库。
# 关联：docs/architecture/README.md
#       docs/architecture/README.md
#       docs/architecture/README.md
# ============================================================================
add_executable(MiniEngineToolTests
    Sha256BuilderTests.cpp
    RecipeJsonTests.cpp
    BuildKeyTests.cpp
    CanonicalRecipeTests.cpp
    AssetCacheTests.cpp
    SourceUriTests.cpp
    GltfGeometryTests.cpp
    GltfImportAdapterTests.cpp
    HdrImageTests.cpp
    ArtifactWriterTests.cpp
    TextureMipBuilderTests.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/BakedWriter.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/BakedWriter.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/ManifestWriter.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/ManifestWriter.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/MeshArtifactWriter.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/MeshArtifactWriter.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/MaterialArtifactWriter.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/MaterialArtifactWriter.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/TextureArtifactWriter.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/TextureArtifactWriter.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/TextureMipBuilder.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/TextureMipBuilder.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/TangentGenerator.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/TangentGenerator.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/HdrImage.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/HdrImage.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/deps/mikktspace/mikktspace.c
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/deps/stb/StbImage.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/RecipeJson.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/RecipeJson.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/Sha256.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/Sha256.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/ContentHash.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/ContentHash.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/DependencyRecord.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/DependencyRecord.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/CanonicalRecipe.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/CanonicalRecipe.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/BuildKey.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/BuildKey.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/AssetCache.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/AssetCache.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/SourceUri.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/SourceUri.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/GltfGeometry.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/GltfGeometry.h
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/GltfImportAdapter.cpp
    ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src/GltfImportAdapter.h
)

target_link_libraries(
    MiniEngineToolTests
    PRIVATE
        GTest::gtest_main
        MiniEngine::Assets
        fastgltf::fastgltf
        bcrypt
        ole32
        windowscodecs
)

target_include_directories(
    MiniEngineToolTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/asset_cooker/src
        ${PROJECT_SOURCE_DIR}/tools/asset_cooker/deps/mikktspace
        ${PROJECT_SOURCE_DIR}/tools/asset_cooker/deps/stb
)

# vendored mikktspace.c 不在 /W4 /WX 门禁内（与 Cooker 同款处理），
# /FS 规避并发 PDB 写入冲突。stb_image 实现同款（第三方源码原样编译）。
set_source_files_properties(${PROJECT_SOURCE_DIR}/tools/asset_cooker/deps/mikktspace/mikktspace.c
    PROPERTIES COMPILE_OPTIONS "/w;/FS")
set_source_files_properties(${PROJECT_SOURCE_DIR}/tools/asset_cooker/deps/stb/StbImage.cpp
    PROPERTIES COMPILE_OPTIONS "/w;/FS")

target_compile_features(MiniEngineToolTests PRIVATE cxx_std_20)
set_target_properties(
    MiniEngineToolTests
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineToolTests)

gtest_discover_tests(
    MiniEngineToolTests
    DISCOVERY_TIMEOUT 30
)
