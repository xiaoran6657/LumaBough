#include "HandleRegistry.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>

using namespace MiniEngine::Rhi;
namespace
{
template <class F> void ExpectHandleError(F&& call, RhiErrorCode code)
{
    try
    {
        call();
        FAIL() << "expected structured handle error";
    }
    catch (const RhiValidationError& e)
    {
        EXPECT_EQ(e.Error().code, code);
        EXPECT_FALSE(e.Error().operation.empty());
        EXPECT_FALSE(e.Error().objectType.empty());
        EXPECT_FALSE(e.Error().message.empty());
    }
}
using Registry = HandleRegistry<TextureHandle, std::string>;
static_assert(!std::is_invocable_v<decltype(&Registry::Retire), Registry&, BufferHandle>);
static_assert(!std::is_convertible_v<TextureHandle, BufferHandle>);
static_assert(!std::is_copy_constructible_v<Registry::Retirement>);
} // namespace
TEST(HandleRegistryTests, CanonicalInvalidEncoding)
{
    EXPECT_EQ(TextureHandle(0, 0, 7), TextureHandle{});
    EXPECT_EQ(TextureHandle(0, 1, 0), TextureHandle{});
    EXPECT_EQ(TextureHandle(TextureHandle::kInvalidIndex, 1, 7), TextureHandle{});
    EXPECT_EQ(TextureHandle{}.Index(), TextureHandle::kInvalidIndex);
    EXPECT_EQ(TextureHandle{}.Generation(), 0U);
    EXPECT_EQ(TextureHandle{}.Owner(), 0U);
}
TEST(HandleRegistryTests, RetireInvalidatesBeforeRecycleAndReusesWithNewGeneration)
{
    Registry registry("Texture", "trace");
    const auto first = registry.Create("first", "hdr");
    const Registry& readOnly = registry;
    EXPECT_EQ(readOnly.Get(first), "first");
    auto retired = registry.Retire(first);
    EXPECT_EQ(retired.payload, "first");
    EXPECT_EQ(registry.AliveCount(), 0U);
    EXPECT_EQ(registry.RetiringCount(), 1U);
    ExpectHandleError([&] { static_cast<void>(registry.Get(first)); }, RhiErrorCode::InvalidHandle);
    try
    {
        static_cast<void>(registry.Get(first));
    }
    catch (const RhiValidationError& e)
    {
        EXPECT_EQ(e.Error().objectName, "hdr");
        EXPECT_EQ(e.Error().backend, "trace");
    }
    const auto second = registry.Create("second");
    EXPECT_NE(second.Index(), first.Index());
    registry.Recycle(retired.ticket);
    const auto reused = registry.Create("reused");
    EXPECT_EQ(reused.Index(), first.Index());
    EXPECT_EQ(reused.Generation(), first.Generation() + 1);
    EXPECT_EQ(registry.Get(reused), "reused");
    ExpectHandleError([&] { static_cast<void>(registry.Get(first)); }, RhiErrorCode::InvalidHandle);
}
TEST(HandleRegistryTests, RejectsDoubleRetireAndLiveRecycle)
{
    Registry registry;
    const auto h = registry.Create("value");
    ExpectHandleError([&] { registry.Recycle(h); }, RhiErrorCode::InvalidState);
    auto retired = registry.Retire(h);
    ExpectHandleError([&] { static_cast<void>(registry.Retire(h)); }, RhiErrorCode::InvalidHandle);
    registry.Recycle(retired.ticket);
    ExpectHandleError([&] { registry.Recycle(retired.ticket); }, RhiErrorCode::InvalidState);
}
TEST(HandleRegistryTests, RejectsForeignOwnerOutOfBoundsAndForgedGeneration)
{
    Registry a, b;
    const auto ha = a.Create("a");
    const auto hb = b.Create("b");
    ASSERT_EQ(ha.Index(), hb.Index());
    ASSERT_EQ(ha.Generation(), hb.Generation());
    EXPECT_NE(ha.Owner(), hb.Owner());
    ExpectHandleError([&] { static_cast<void>(b.Get(ha)); }, RhiErrorCode::InvalidHandle);
    ExpectHandleError([&] { static_cast<void>(b.Retire(ha)); }, RhiErrorCode::InvalidHandle);
    ExpectHandleError([&] { b.Recycle(ha); }, RhiErrorCode::InvalidHandle);
    for (auto bad : {TextureHandle{}, TextureHandle(100, ha.Generation(), ha.Owner()),
                     TextureHandle(ha.Index(), ha.Generation() + 1, ha.Owner())})
    {
        ExpectHandleError([&] { static_cast<void>(a.Get(bad)); }, RhiErrorCode::InvalidHandle);
    }
    EXPECT_EQ(a.Get(ha), "a");
    EXPECT_EQ(b.Get(hb), "b");
}
TEST(HandleRegistryTests, StaleRetirementTicketCannotRecycleNextLifetime)
{
    Registry registry;
    auto first = registry.Create("first");
    auto old = registry.Retire(first);
    registry.Recycle(old.ticket);
    auto second = registry.Create("second");
    auto pending = registry.Retire(second);
    ExpectHandleError([&] { registry.Recycle(old.ticket); }, RhiErrorCode::InvalidState);
    EXPECT_EQ(registry.RetiringCount(), 1U);
    registry.Recycle(pending.ticket);
}
TEST(HandleRegistryTests, ExhaustedGenerationNeverWrapsOrReusesSlot)
{
    HandleRegistry<BufferHandle, int, 2> registry;
    const auto first = registry.Create(1);
    auto r1 = registry.Retire(first);
    registry.Recycle(r1.ticket);
    const auto second = registry.Create(2);
    EXPECT_EQ(second.Index(), first.Index());
    EXPECT_EQ(second.Generation(), 2U);
    auto r2 = registry.Retire(second);
    registry.Recycle(r2.ticket);
    EXPECT_EQ(registry.ExhaustedCount(), 1U);
    const auto third = registry.Create(3);
    EXPECT_NE(third.Index(), first.Index());
    EXPECT_EQ(third.Generation(), 1U);
    ExpectHandleError([&] { static_cast<void>(registry.Get(first)); }, RhiErrorCode::InvalidHandle);
    ExpectHandleError([&] { static_cast<void>(registry.Get(second)); }, RhiErrorCode::InvalidHandle);
}
TEST(HandleRegistryTests, SupportsMoveOnlyNonDefaultPayloadAndTransfersOwnership)
{
    struct Payload final
    {
        explicit Payload(int v) : value(std::make_unique<int>(v))
        {
        }
        Payload(Payload&&) noexcept = default;
        Payload& operator=(Payload&&) = delete;
        std::unique_ptr<int> value;
    };
    HandleRegistry<BufferHandle, Payload> registry;
    const auto h = registry.Create(Payload(42));
    for (int i = 0; i < 100; ++i)
    {
        static_cast<void>(registry.Create(Payload(i)));
    }
    EXPECT_EQ(*registry.Get(h).value, 42);
    auto retired = registry.Retire(h);
    registry.Recycle(retired.ticket);
    EXPECT_EQ(*retired.payload.value, 42);
}
TEST(HandleRegistryTests, RepeatedRecycleHasBoundedSlotsAndStaleReferences)
{
    HandleRegistry<BufferHandle, int> registry;
    std::unordered_set<BufferHandle> seen;
    for (int i = 0; i < 10000; ++i)
    {
        const auto h = registry.Create(i);
        EXPECT_TRUE(seen.insert(h).second);
        auto retired = registry.Retire(h);
        registry.Recycle(retired.ticket);
        EXPECT_EQ(retired.payload, i);
    }
    EXPECT_EQ(registry.SlotCount(), 1U);
    EXPECT_EQ(registry.AliveCount(), 0U);
    EXPECT_EQ(registry.RetiringCount(), 0U);
}
TEST(HandleRegistryTests, DestroyingRegistryDoesNotReuseOwnerIdentity)
{
    TextureHandle old;
    {
        Registry a;
        old = a.Create("old");
    }
    Registry b;
    auto now = b.Create("now");
    EXPECT_NE(old.Owner(), now.Owner());
    ExpectHandleError([&] { static_cast<void>(b.Get(old)); }, RhiErrorCode::InvalidHandle);
}
