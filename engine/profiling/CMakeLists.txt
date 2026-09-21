# ============================================================================
# engine/profiling — M7 观测门面（唯一允许 include Tracy 的公共边界）
# 里程碑：M7-02
# 约定：
#   * 业务模块链接 MiniEngineProfiling，不链接 Tracy::TracyClient；
#   * ME_ENABLE_TRACY 以 PUBLIC 传播，off 时宏为 ((void)0) 且不下载 Tracy；
#   * on/off 必须是不同 build directory（CMakePresets 的 windows-msvc-profile 与
#     windows-msvc-profile-tracy），避免 cache 混淆。
# 依赖冻结：Tracy v0.13.1（commit 05cceee0df3b8d7c6fa87e9638af311dbabc63cb），
# 归档 SHA-256 见 docs/architecture/DECISIONS.md。
# ============================================================================

option(ME_ENABLE_TRACY "Enable Tracy instrumentation (profile-tracy preset)" OFF)

add_library(MiniEngineProfiling STATIC
    src/Profile.cpp
    src/ProfileLock.cpp
)

target_include_directories(MiniEngineProfiling PUBLIC include)
target_compile_features(MiniEngineProfiling PUBLIC cxx_std_20)
set_target_properties(MiniEngineProfiling PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineProfiling)
target_compile_definitions(MiniEngineProfiling PUBLIC ME_ENABLE_TRACY=$<BOOL:${ME_ENABLE_TRACY}>)

if(ME_ENABLE_TRACY)
    include(FetchContent)

    # 推荐 Profile 默认（M7-02 文档）：按需连接、仅本机、不广播。
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)

    # 固定 tag + 归档哈希：禁止跟踪 master；归档哈希在 configure 时校验。
    # 无网络机器可用官方覆盖机制指定本地源码目录：
    #   cmake --preset windows-msvc-profile-tracy -DFETCHCONTENT_SOURCE_DIR_TRACY=<path>
    FetchContent_Declare(
        tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.tar.gz
        URL_HASH SHA256=D4EFC50EBCB0BFCFDBBA148995AEB75044C0D80F5D91223AEBFAA8FA9E563D2B
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(tracy)

    target_link_libraries(MiniEngineProfiling PUBLIC Tracy::TracyClient)
endif()
