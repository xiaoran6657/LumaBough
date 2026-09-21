// ============================================================================
// ProfileLock.cpp — 可观测锁的实现（Tracy 锁事件）
// 里程碑：M7-02
// 职责：实现 Profiling::LockableMutex。Tracy 的锁事件只有 C++ API（TracyLock.hpp），
//       因此单独一个 TU include Tracy.hpp，避免与 C ABI 适配层混用。
//       关闭 Tracy 时退化为 std::mutex，调用方代码不变。
// 关联：engine/profiling/include/MiniEngine/Profiling/Profile.h
// ============================================================================

#include <MiniEngine/Profiling/Profile.h>

#include <mutex>

#if ME_ENABLE_TRACY
#include <tracy/Tracy.hpp>
#endif

namespace MiniEngine::Profiling
{
namespace
{
#if ME_ENABLE_TRACY
// Tracy 的锁身份在构造时注册（LockAnnounce）；名字必须静态存储期。
struct Impl final
{
    tracy::SourceLocationData location;
    tracy::Lockable<std::mutex> lock;

    explicit Impl(const char* name) : location{name, "LockableMutex", nullptr, 0, 0}, lock(&location)
    {
    }
};
#else
struct Impl final
{
    std::mutex mutex;
};
#endif
} // namespace

LockableMutex::LockableMutex(const char* name) noexcept
{
    try
    {
#if ME_ENABLE_TRACY
        m_impl = new Impl(name == nullptr ? "unnamed" : name);
#else
        (void)name;
        m_impl = new Impl();
#endif
    }
    catch (...)
    {
        // 观测设施不允许让业务初始化失败；退化为无锁保护并在 Tracy 时间线上缺失该锁。
        m_impl = nullptr;
    }
}

LockableMutex::~LockableMutex() noexcept
{
    delete static_cast<Impl*>(m_impl);
    m_impl = nullptr;
}

void LockableMutex::lock() noexcept
{
#if ME_ENABLE_TRACY
    if (m_impl != nullptr)
        static_cast<Impl*>(m_impl)->lock.lock();
#else
    if (m_impl != nullptr)
        static_cast<Impl*>(m_impl)->mutex.lock();
#endif
    // 锁事件计数（relaxed）；Tracy 的锁事件由 Lockable 自身发出。
    CountLockAcquire();
}

void LockableMutex::unlock() noexcept
{
#if ME_ENABLE_TRACY
    if (m_impl != nullptr)
        static_cast<Impl*>(m_impl)->lock.unlock();
#else
    if (m_impl != nullptr)
        static_cast<Impl*>(m_impl)->mutex.unlock();
#endif
}

bool LockableMutex::try_lock() noexcept
{
#if ME_ENABLE_TRACY
    return m_impl == nullptr || static_cast<Impl*>(m_impl)->lock.try_lock();
#else
    return m_impl == nullptr || static_cast<Impl*>(m_impl)->mutex.try_lock();
#endif
}
} // namespace MiniEngine::Profiling
