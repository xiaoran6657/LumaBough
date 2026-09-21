// ============================================================================
// Profile.h — M7 观测门面（业务代码只依赖这里的 ME_PROFILE_*，不 include Tracy）
// 里程碑：M7-02（Tracy 集成与观测点）
// 职责：把 Tracy 的 C ABI 包成项目自己的最小门面：
//   * ME_ENABLE_TRACY=0 时所有宏展开为 ((void)0)，**不求值参数**、不引入任何依赖；
//   * public header 不 include Tracy；Tracy 只出现在 Profile.cpp 与 backend-private 的
//     GPU profiler 实现里；
//   * zone 名字必须是静态字符串（Tracy 保存该指针）；动态文本走 message/text API。
// 关联：docs/architecture/README.md
//       engine/profiling/CMakeLists.txt（Tracy 版本/选项冻结与链接）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#ifndef ME_ENABLE_TRACY
#define ME_ENABLE_TRACY 0
#endif

namespace MiniEngine::Profiling
{
// 与 Tracy C ABI 布局一致（Profile.cpp 用 static_assert 兜底）：这样门面可以按值
// 传递位置信息而不暴露第三方类型。
struct SourceLocation
{
    const char* name;
    const char* function;
    const char* file;
    std::uint32_t line;
    std::uint32_t color;
};

struct ZoneHandle
{
    std::uint32_t id = 0;
    std::int32_t active = 0;
};

// RAII zone：构造开始、析构结束。ME_PROFILE_ZONE* 宏在栈上创建它。
class ScopedZone final
{
  public:
    explicit ScopedZone(const SourceLocation& location) noexcept;
    ~ScopedZone() noexcept;

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

  private:
    ZoneHandle m_handle{};
};

// 帧标记（Tracy 的 frame mark）：每个主循环帧结束处调用一次。
void MarkFrame(const char* name) noexcept;
// 线程名：同时体现在 Tracy 线程列表与 SetThreadDescription（见 SetThreadName）。
void SetThreadName(const char* name) noexcept;
// 计数曲线：必须使用静态名字；值按整数采样。
void Plot(const char* name, std::int64_t value) noexcept;
// 文本消息（capture begin/end、场景身份等）：文本需在调用期间有效。
void Message(const char* text) noexcept;
// 命名内存事件：只在 allocator 边界调用一次，避免上层容器与底层 allocator 重复记账。
void MemoryAllocate(const void* pointer, std::size_t size, const char* pool) noexcept;
void MemoryFree(const void* pointer, const char* pool) noexcept;

// 事件计数器：证明"哪几类观测真的被发出"（zone/frame/plot/message/memory/lock）。
// 只在 Tracy 打开时计数；关闭时全 0。它是 M7-A04 在无离线 Tracy server 环境下的
// 可复算证据，不参与任何渲染或调度决策。
struct EventCounters final
{
    std::uint64_t zones = 0;
    std::uint64_t frames = 0;
    std::uint64_t plots = 0;
    std::uint64_t messages = 0;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t lockAcquires = 0;
};
[[nodiscard]] EventCounters Counters() noexcept;
// 锁获取计数（由 LockableMutex 实现调用；Tracy 的锁事件由 Tracy 自己发出）。
void CountLockAcquire() noexcept;

// 运行期细节级别：粗粒度（只保留帧/阶段 zone）与完整（含 detail zone）共用同一二进制，
// 便于 M7-02 的开销实验；默认完整，由组合根按 CLI 设置。
void SetDetailEnabled(bool enabled) noexcept;
[[nodiscard]] bool DetailEnabled() noexcept;
// profiler 是否已连接（on-demand 模式下只在连接期间采集）。capture 协议用它等待连接。
[[nodiscard]] bool IsConnected() noexcept;

// 带 Tracy 锁事件的可锁定对象：用法与 std::mutex 一致
// （std::lock_guard<MiniEngine::Profiling::LockableMutex>）。关掉 Tracy 时就是普通 mutex。
// name 必须是静态存储期字符串；锁名要保持稳定，禁止逐帧变化的高基数字符串。
class LockableMutex final
{
  public:
    explicit LockableMutex(const char* name) noexcept;
    ~LockableMutex() noexcept;

    LockableMutex(const LockableMutex&) = delete;
    LockableMutex& operator=(const LockableMutex&) = delete;

    void lock() noexcept;
    void unlock() noexcept;
    [[nodiscard]] bool try_lock() noexcept;

  private:
    // 实现体（std::mutex 或 Tracy 的 Lockable 包装）在 Profile.cpp 中定义，
    // public header 不出现第三方类型。
    void* m_impl = nullptr;
};
} // namespace MiniEngine::Profiling

#define ME_DETAIL_JOIN2(a, b) a##b
#define ME_DETAIL_JOIN(a, b) ME_DETAIL_JOIN2(a, b)

#if ME_ENABLE_TRACY
#define ME_PROFILE_ZONE()                                                                                             \
    static constexpr ::MiniEngine::Profiling::SourceLocation ME_DETAIL_JOIN(meProfileLocation_, __LINE__)             \
    {                                                                                                                 \
        nullptr, __FUNCTION__, __FILE__, __LINE__, 0                                                                  \
    };                                                                                                                \
    ::MiniEngine::Profiling::ScopedZone ME_DETAIL_JOIN(meProfileZone_, __LINE__)                                      \
    {                                                                                                                 \
        ME_DETAIL_JOIN(meProfileLocation_, __LINE__)                                                                  \
    }

#define ME_PROFILE_ZONE_NAMED(zoneName)                                                                               \
    static constexpr ::MiniEngine::Profiling::SourceLocation ME_DETAIL_JOIN(meProfileLocation_, __LINE__)             \
    {                                                                                                                 \
        zoneName, __FUNCTION__, __FILE__, __LINE__, 0                                                                 \
    };                                                                                                                \
    ::MiniEngine::Profiling::ScopedZone ME_DETAIL_JOIN(meProfileZone_, __LINE__)                                      \
    {                                                                                                                 \
        ME_DETAIL_JOIN(meProfileLocation_, __LINE__)                                                                  \
    }

// 只在细节级别为“完整”时进入的 zone：粗粒度构建保留阶段 zone，完整构建保留全部。
// 条件判断在运行期完成，因此两个级别共用同一二进制。
// 注意必须用 do/while 包住：ME_PROFILE_ZONE() 展开成“static 声明 + 对象声明”两条语句，
// 直接跟在 if 后面会让对象声明落到 if 之外（且引用越界声明）。
#define ME_PROFILE_ZONE_DETAIL()                                                                                      \
    do                                                                                                                \
    {                                                                                                                 \
        if (::MiniEngine::Profiling::DetailEnabled())                                                                 \
        {                                                                                                             \
            ME_PROFILE_ZONE();                                                                                        \
        }                                                                                                             \
    } while (false)

// 带名字的 detail zone（同样只在完整细节级别进入）。
#define ME_PROFILE_ZONE_NAMED_DETAIL(zoneName)                                                                        \
    do                                                                                                                \
    {                                                                                                                 \
        if (::MiniEngine::Profiling::DetailEnabled())                                                                 \
        {                                                                                                             \
            ME_PROFILE_ZONE_NAMED(zoneName);                                                                          \
        }                                                                                                             \
    } while (false)

#define ME_PROFILE_FRAME(name) ::MiniEngine::Profiling::MarkFrame(name)
#define ME_PROFILE_THREAD(name) ::MiniEngine::Profiling::SetThreadName(name)
#define ME_PROFILE_COUNTER(name, value) ::MiniEngine::Profiling::Plot(name, static_cast<std::int64_t>(value))
#define ME_PROFILE_MESSAGE(text) ::MiniEngine::Profiling::Message(text)
#define ME_PROFILE_ALLOC(pointer, size, pool) ::MiniEngine::Profiling::MemoryAllocate(pointer, size, pool)
#define ME_PROFILE_FREE(pointer, pool) ::MiniEngine::Profiling::MemoryFree(pointer, pool)
#define ME_PROFILE_IS_CONNECTED() ::MiniEngine::Profiling::IsConnected()
#else
// 关闭时必须是零副作用：不 include 第三方头、不求值任何可选参数（含函数调用）。
#define ME_PROFILE_ZONE() ((void)0)
#define ME_PROFILE_ZONE_NAMED(zoneName) ((void)0)
#define ME_PROFILE_ZONE_DETAIL() ((void)0)
#define ME_PROFILE_ZONE_NAMED_DETAIL(zoneName) ((void)0)
#define ME_PROFILE_FRAME(name) ((void)0)
#define ME_PROFILE_THREAD(name) ((void)0)
#define ME_PROFILE_COUNTER(name, value) ((void)0)
#define ME_PROFILE_MESSAGE(text) ((void)0)
#define ME_PROFILE_ALLOC(pointer, size, pool) ((void)0)
#define ME_PROFILE_FREE(pointer, pool) ((void)0)
#define ME_PROFILE_IS_CONNECTED() false
#endif
