# M7-02：门面 compile-off smoke。
# 它必须在 off（windows-msvc-profile）与 on（windows-msvc-profile-tracy）两个构建里
# 都通过；off 构建的返回值证明宏参数未被求值。
add_executable(MiniEngineProfileOffSmoke ProfileOffSmoke.cpp)
target_link_libraries(MiniEngineProfileOffSmoke PRIVATE MiniEngineProfiling)
target_compile_features(MiniEngineProfileOffSmoke PRIVATE cxx_std_20)
set_target_properties(
    MiniEngineProfileOffSmoke
    PROPERTIES
        CXX_EXTENSIONS OFF
)
miniengine_set_project_warnings(MiniEngineProfileOffSmoke)

add_test(NAME ProfileOffSmoke COMMAND MiniEngineProfileOffSmoke)
