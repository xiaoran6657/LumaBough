// ============================================================================
// SwapChainConfigTests.cpp — 交换链配置决策的纯 CPU 契约测试
// 里程碑：M5（04 篇 Command List、SwapChain 与 Resize）
// 职责：锁定两条可脱离 GPU 验证的决策：Present flag 的 resolving（tearing 只在
//       "驱动支持 + vsync=0 + 窗口化"三者同时成立时才生效）与 0×0（minimized）
//       的挂起判定。这两条是 04 篇最容易"看起来生效其实没生效"的地方
//       （传了 tearing flag 但被 DXGI 拒绝、或对 0×0 调 ResizeBuffers）。
// 关联：docs/architecture/README.md（可选 tearing / Resize）
// ============================================================================
#include "D3D12SwapChain.h"

#include <gtest/gtest.h>

#include <cstdint>

using MiniEngine::Rhi::D3D12::IsSuspendedSize;
using MiniEngine::Rhi::D3D12::ResolvePresentFlags;
using MiniEngine::Rhi::D3D12::ResolveTearingEnabled;

// tearing flag 的三前提：任何一条不满足都必须回落到 0（普通 Present）。
TEST(SwapChainConfigTests, TearingFlagRequiresSupportedVsyncOffAndWindowed)
{
    // 唯一允许的组合：允许 tearing 的交换链 + vsync=0 + 窗口化。
    EXPECT_EQ(ResolvePresentFlags(false, true, true), DXGI_PRESENT_ALLOW_TEARING);

    // vsync=1：必须不带 tearing flag（带 vsync 的 tearing 无意义，且被 DXGI 拒绝）。
    EXPECT_EQ(ResolvePresentFlags(true, true, true), 0U);

    // 交换链本身没有 ALLOW_TEARING 标志（驱动不支持或用户关闭）：绝不传该 flag。
    EXPECT_EQ(ResolvePresentFlags(false, false, true), 0U);

    // 非窗口化（独占全屏）：该 flag 不被允许。
    EXPECT_EQ(ResolvePresentFlags(false, true, false), 0U);
}

// tearing 的"实际启用"还要求交换链创建时**带上了** ALLOW_TEARING 标志：
// 以 vsync=1 创建的交换链之后切到 vsync=0 也不能传 tearing flag（DXGI 会拒绝）。
TEST(SwapChainConfigTests, TearingEnabledAlsoRequiresTheSwapChainFlag)
{
    constexpr std::uint32_t kAllowTearing = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // 支持 + 标志在 + vsync=0 → 启用。
    EXPECT_TRUE(ResolveTearingEnabled(true, false, kAllowTearing));

    // 支持但创建时没带标志（以 vsync=1 建链）→ 运行期切到 vsync=0 也不得启用。
    EXPECT_FALSE(ResolveTearingEnabled(true, false, 0U))
        << "缺 ALLOW_TEARING 标志时传 tearing flag 会被 DXGI 拒绝，因此必须为 false";

    // vsync=1 或驱动不支持 → 不启用。
    EXPECT_FALSE(ResolveTearingEnabled(true, true, kAllowTearing));
    EXPECT_FALSE(ResolveTearingEnabled(false, false, kAllowTearing));
}

// 0×0 是 minimized 的挂起信号：既不渲染也不 ResizeBuffers。
TEST(SwapChainConfigTests, SuspendedSizeCoversAnyZeroDimension)
{
    EXPECT_TRUE(IsSuspendedSize(0U, 0U));
    EXPECT_TRUE(IsSuspendedSize(0U, 720U));
    EXPECT_TRUE(IsSuspendedSize(1280U, 0U));

    EXPECT_FALSE(IsSuspendedSize(1U, 1U)) << "1x1 是最小的合法尺寸，不是挂起";
    EXPECT_FALSE(IsSuspendedSize(1280U, 720U));
}
