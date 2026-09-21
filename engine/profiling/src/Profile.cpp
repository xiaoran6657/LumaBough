// ============================================================================
// Profile.cpp — 观测门面实现（Tracy C ABI 适配）
// 里程碑：M7-02
// 职责：把 ME_PROFILE_* 映射到 Tracy 的 C ABI；static_assert 防第三方升级破坏布局。
//       业务模块只链接 MiniEngineProfiling，不直接链接 Tracy::TracyClient。
// 关联：engine/profiling/include/MiniEngine/Profiling/Profile.h
//       docs/architecture/README.md（依赖冻结与选项）
// ============================================================================

#include <MiniEngine/Profiling/Profile.h>

#include <atomic>
#include <cstring>

#if ME_ENABLE_TRACY
#include <tracy/TracyC.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif
#endif

namespace MiniEngine::Profiling
{
namespace
{
// 默认完整细节；组合根可切到粗粒度（M7-02 开销实验的第 3/4 组共用一个二进制）。
std::atomic<bool> g_detailEnabled{true};

// 事件计数器（relaxed 自增；只在 Tracy 打开时被调用）。用于验收证据，不参与控制流。
std::atomic<std::uint64_t> g_zoneCount{0};
std::atomic<std::uint64_t> g_frameCount{0};
std::atomic<std::uint64_t> g_plotCount{0};
std::atomic<std::uint64_t> g_messageCount{0};
std::atomic<std::uint64_t> g_allocCount{0};
std::atomic<std::uint64_t> g_freeCount{0};
std::atomic<std::uint64_t> g_lockCount{0};

#if ME_ENABLE_TRACY && defined(_WIN32)
// 线程名同时写入系统层，便于 PIX/RenderDoc/WPA/crash dump 与 Tracy 对应同一线程。
void ApplySystemThreadName(const char* name) noexcept
{
    wchar_t wide[64]{};
    const int written = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, static_cast<int>(std::size(wide)));
    if (written > 0)
        SetThreadDescription(GetCurrentThread(), wide);
}
#else
void ApplySystemThreadName(const char*) noexcept
{
}
#endif
} // namespace

#if ME_ENABLE_TRACY
// 布局守护：门面按值持有位置信息与 zone 句柄，Tracy 升级改变布局必须在这里失败，
// 而不是在运行期静默写坏 Tracy 的队列。
static_assert(sizeof(SourceLocation) == sizeof(___tracy_source_location_data),
              "SourceLocation must match Tracy C ABI");
static_assert(alignof(SourceLocation) == alignof(___tracy_source_location_data),
              "SourceLocation alignment must match Tracy C ABI");
static_assert(sizeof(ZoneHandle) == sizeof(TracyCZoneCtx), "ZoneHandle must match TracyCZoneCtx");
static_assert(alignof(ZoneHandle) == alignof(TracyCZoneCtx), "ZoneHandle alignment must match TracyCZoneCtx");
#endif

ScopedZone::ScopedZone(const SourceLocation& location) noexcept
{
#if ME_ENABLE_TRACY
    const auto* tracyLocation = reinterpret_cast<const ___tracy_source_location_data*>(&location);
    const TracyCZoneCtx context = ___tracy_emit_zone_begin(tracyLocation, 1);
    std::memcpy(&m_handle, &context, sizeof(m_handle));
    g_zoneCount.fetch_add(1, std::memory_order_relaxed);
#else
    (void)location;
#endif
}

ScopedZone::~ScopedZone() noexcept
{
#if ME_ENABLE_TRACY
    TracyCZoneCtx context{};
    std::memcpy(&context, &m_handle, sizeof(context));
    ___tracy_emit_zone_end(context);
#endif
}

void MarkFrame(const char* name) noexcept
{
#if ME_ENABLE_TRACY
    g_frameCount.fetch_add(1, std::memory_order_relaxed);
    ___tracy_emit_frame_mark(name);
#else
    (void)name;
#endif
}

void SetThreadName(const char* name) noexcept
{
    if (name == nullptr)
        return;
    ApplySystemThreadName(name);
#if ME_ENABLE_TRACY
    ___tracy_set_thread_name(name);
#endif
}

void Plot(const char* name, std::int64_t value) noexcept
{
#if ME_ENABLE_TRACY
    g_plotCount.fetch_add(1, std::memory_order_relaxed);
    ___tracy_emit_plot_int(name, value);
#else
    (void)name;
    (void)value;
#endif
}

void Message(const char* text) noexcept
{
    if (text == nullptr)
        return;
#if ME_ENABLE_TRACY
    g_messageCount.fetch_add(1, std::memory_order_relaxed);
    ___tracy_emit_message(text, std::strlen(text), 0);
#else
    (void)text;
#endif
}

void MemoryAllocate(const void* pointer, std::size_t size, const char* pool) noexcept
{
    if (pointer == nullptr)
        return;
#if ME_ENABLE_TRACY
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    ___tracy_emit_memory_alloc_named(pointer, size, 0, pool);
#else
    (void)size;
    (void)pool;
#endif
}

void MemoryFree(const void* pointer, const char* pool) noexcept
{
    if (pointer == nullptr)
        return;
#if ME_ENABLE_TRACY
    g_freeCount.fetch_add(1, std::memory_order_relaxed);
    ___tracy_emit_memory_free_named(pointer, 0, pool);
#else
    (void)pool;
#endif
}

void CountLockAcquire() noexcept
{
#if ME_ENABLE_TRACY
    g_lockCount.fetch_add(1, std::memory_order_relaxed);
#endif
}

EventCounters Counters() noexcept
{
    EventCounters counters;
#if ME_ENABLE_TRACY
    counters.zones = g_zoneCount.load(std::memory_order_relaxed);
    counters.frames = g_frameCount.load(std::memory_order_relaxed);
    counters.plots = g_plotCount.load(std::memory_order_relaxed);
    counters.messages = g_messageCount.load(std::memory_order_relaxed);
    counters.allocations = g_allocCount.load(std::memory_order_relaxed);
    counters.frees = g_freeCount.load(std::memory_order_relaxed);
    counters.lockAcquires = g_lockCount.load(std::memory_order_relaxed);
#endif
    return counters;
}

void SetDetailEnabled(bool enabled) noexcept
{
    g_detailEnabled.store(enabled, std::memory_order_relaxed);
}

bool DetailEnabled() noexcept
{
    return g_detailEnabled.load(std::memory_order_relaxed);
}

bool IsConnected() noexcept
{
#if ME_ENABLE_TRACY
    return ___tracy_connected() != 0;
#else
    return false;
#endif
}
} // namespace MiniEngine::Profiling

// 本适配层有意只使用 Tracy 的 C ABI 与静态断言；升级 Tracy 时必须重跑 off/on 两组测试。
