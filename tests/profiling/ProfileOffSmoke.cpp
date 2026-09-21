// ============================================================================
// ProfileOffSmoke.cpp — 门面 compile-off/on 冒烟（M7-A03 证据）
// 里程碑：M7-02
// 职责：证明 ME_ENABLE_TRACY=0 时宏不求值参数、不产生第三方依赖；on 时同一份代码
//       仍能正常链接与执行（zone/消息/计数/内存事件全部走通）。
// 运行方式：Debug/Profile（off）与 profile-tracy（on）两个 build directory 各跑一次。
// ============================================================================

#include <MiniEngine/Profiling/Profile.h>

#include <cstdio>

namespace
{
int IncrementWithSideEffect(int& value)
{
    return ++value;
}
} // namespace

int main()
{
    int value = 0;

    ME_PROFILE_THREAD("ProfileOffSmoke");
    ME_PROFILE_ZONE();
    ME_PROFILE_ZONE_NAMED("ProfileOffSmoke");
    ME_PROFILE_ZONE_DETAIL();
    ME_PROFILE_FRAME("SmokeFrame");
    // 关闭时必须不求值：这些参数带副作用，off 构建下 value 必须保持 0。
    ME_PROFILE_COUNTER("MustNotEvaluate", IncrementWithSideEffect(value));
    ME_PROFILE_MESSAGE("profile facade smoke");
    ME_PROFILE_ALLOC(nullptr, static_cast<std::size_t>(IncrementWithSideEffect(value)), "SmokePool");
    ME_PROFILE_FREE(nullptr, "SmokePool");

    const bool connected = ME_PROFILE_IS_CONNECTED();
    MiniEngine::Profiling::SetDetailEnabled(true);
    const bool detail = MiniEngine::Profiling::DetailEnabled();
    MiniEngine::Profiling::SetDetailEnabled(false);
    const bool coarse = !MiniEngine::Profiling::DetailEnabled();

    std::printf("ME_ENABLE_TRACY=%d connected=%d detail=%d coarse=%d sideEffect=%d\n", ME_ENABLE_TRACY,
                connected ? 1 : 0, detail ? 1 : 0, coarse ? 1 : 0, value);

#if !ME_ENABLE_TRACY
    // off 构建：参数一个都不允许被求值。
    return value == 0 && !connected ? 0 : 1;
#else
    // on 构建：两处带副作用的参数各求值一次（counter 与 alloc 的 size），
    // 链接与调用链必须成立。
    return value == 2 && detail && coarse ? 0 : 1;
#endif
}
