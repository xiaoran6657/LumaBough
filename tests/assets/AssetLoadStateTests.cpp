// ============================================================================
// AssetLoadStateTests.cpp — M7-06 状态机的合法边与终态不变量
// 里程碑：M7-06（异步资产加载流水线）
// 职责：正向边逐条断言、非法边逐条拒绝、终态不可离开、失败类终态谓词与名称稳定。
//       状态机是所有阶段实现共用的唯一契约：这里漏一条非法边，运行时就会出现
//       "旧 Ready 被半完成状态覆盖"的静默错误。
// 关联：engine/assets/async/include/MiniEngine/Assets/AssetLoadState.h
//       docs/architecture/README.md「状态机」
// ============================================================================

#include <MiniEngine/Assets/AssetLoadState.h>

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace
{
using namespace MiniEngine::Assets;

constexpr std::array<AssetLoadState, 11> kAllStates{
    AssetLoadState::Unloaded, AssetLoadState::Queued,       AssetLoadState::Reading,   AssetLoadState::Decoding,
    AssetLoadState::CpuReady, AssetLoadState::UploadQueued, AssetLoadState::Uploading, AssetLoadState::Ready,
    AssetLoadState::Failed,   AssetLoadState::Cancelled,    AssetLoadState::Stale};
} // namespace

TEST(AssetLoadStateTests, AcceptsOnlyForwardPipelineEdges)
{
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::Unloaded, AssetLoadState::Queued));
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::Queued, AssetLoadState::Reading));
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::Reading, AssetLoadState::Decoding));
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::Decoding, AssetLoadState::CpuReady));
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::CpuReady, AssetLoadState::UploadQueued));
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::UploadQueued, AssetLoadState::Uploading));
    EXPECT_TRUE(IsLegalTransition(AssetLoadState::Uploading, AssetLoadState::Ready));

    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Queued, AssetLoadState::Ready));
    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Decoding, AssetLoadState::Reading));
    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Ready, AssetLoadState::Queued));
    // 跳阶段（例如 Reading 直接到 UploadQueued）同样非法：旧资源替换必须走完整流水线。
    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Reading, AssetLoadState::UploadQueued));
    EXPECT_FALSE(IsLegalTransition(AssetLoadState::CpuReady, AssetLoadState::Ready));
    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Queued, AssetLoadState::Decoding));
}

TEST(AssetLoadStateTests, AllowsTerminalFailureOnlyFromNonTerminalState)
{
    for (const AssetLoadState from :
         {AssetLoadState::Unloaded, AssetLoadState::Queued, AssetLoadState::Reading, AssetLoadState::Decoding,
          AssetLoadState::CpuReady, AssetLoadState::UploadQueued, AssetLoadState::Uploading})
    {
        EXPECT_TRUE(IsLegalTransition(from, AssetLoadState::Failed));
        EXPECT_TRUE(IsLegalTransition(from, AssetLoadState::Cancelled));
        EXPECT_TRUE(IsLegalTransition(from, AssetLoadState::Stale));
    }

    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Ready, AssetLoadState::Failed));
    EXPECT_FALSE(IsLegalTransition(AssetLoadState::Failed, AssetLoadState::Cancelled));
}

TEST(AssetLoadStateTests, TerminalStatesAreAbsorbing)
{
    for (const AssetLoadState terminal :
         {AssetLoadState::Ready, AssetLoadState::Failed, AssetLoadState::Cancelled, AssetLoadState::Stale})
    {
        EXPECT_TRUE(IsTerminal(terminal));
        EXPECT_FALSE(IsActive(terminal));
        for (const AssetLoadState target : kAllStates)
        {
            EXPECT_FALSE(IsLegalTransition(terminal, target))
                << "terminal " << ToString(terminal) << " must not transition to " << ToString(target);
        }
    }
}

TEST(AssetLoadStateTests, ActiveAndFailurePredicatesMatchTheMachine)
{
    for (const AssetLoadState state : kAllStates)
    {
        EXPECT_NE(IsTerminal(state), IsActive(state)) << ToString(state);
    }

    EXPECT_TRUE(IsFailureOrCancellation(AssetLoadState::Failed));
    EXPECT_TRUE(IsFailureOrCancellation(AssetLoadState::Cancelled));
    EXPECT_TRUE(IsFailureOrCancellation(AssetLoadState::Stale));
    EXPECT_FALSE(IsFailureOrCancellation(AssetLoadState::Ready));
    EXPECT_FALSE(IsFailureOrCancellation(AssetLoadState::Reading));
}

// 名称是 raw JSON / 报告 / 本目录其它测试共用的稳定字符串，改名即破坏可读证据。
TEST(AssetLoadStateTests, NamesAreStableAndUnique)
{
    std::array<const char*, 11> names{};
    for (std::size_t index = 0; index < kAllStates.size(); ++index)
    {
        names[index] = ToString(kAllStates[index]);
        EXPECT_STRNE(names[index], "unknown") << "state " << static_cast<int>(kAllStates[index]) << " lacks a name";
    }
    for (std::size_t left = 0; left < names.size(); ++left)
    {
        for (std::size_t right = left + 1; right < names.size(); ++right)
        {
            EXPECT_STRNE(names[left], names[right]);
        }
    }
}
