// ============================================================================
// D3D12CommonTests.cpp — AlignUp 与 HRESULT 转义的 CPU 侧契约测试
// 里程碑：M5（01 篇架构边界与输出一致性契约）
// 职责：锁定 D3D12Common 两条基元契约：按 2 的幂向上对齐（含拒绝非幂次与拒绝
//       上溢）与 HRESULT 失败转义（成功直通、失败携带原值）。这两条是后续
//       CBV 偏移、Subresource footprint、Upload Ring 环绕与所有 D3D12 调用
//       的地基；先锁死它们，03—09 篇的错误就能被本地化为"调用点用错了参数"，
//       而不是"基元可能算错偏移"。
// 为什么不用 GPU 证据：对齐与异常是 CPU 值语义，单测可穷举边界；GPU capture
//       只能事后看到结果。
// 关联：docs/architecture/README.md
// ============================================================================
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

using MiniEngine::Rhi::D3D12::AlignUp;
using MiniEngine::Rhi::D3D12::HResultError;
using MiniEngine::Rhi::D3D12::ThrowIfFailed;

namespace
{
// Hand-rolled HRESULT 值：测试不依赖 Windows 特定失败码含义，只断言"原值保留"。
constexpr HRESULT kSyntheticFailure = static_cast<HRESULT>(0x80070057L); // E_INVALIDARG 的数值形式
} // namespace

// AlignUp：CBV 所需的 256 字节对齐是 M5 最常见的调用形态，连同幂等性一起锁死。
TEST(D3D12AlignUpTests, RoundsUpToTwoPowerOfTwoAndIsIdempotent)
{
    EXPECT_EQ(AlignUp(0U, 256U), 0U) << "已对齐到边界的 0 不得偏移";
    EXPECT_EQ(AlignUp(1U, 256U), 256U) << "非零值必须向上取整到对齐粒度";
    EXPECT_EQ(AlignUp(255U, 256U), 256U);
    EXPECT_EQ(AlignUp(256U, 256U), 256U) << "对齐值本身必须保持不变";
    EXPECT_EQ(AlignUp(257U, 256U), 512U) << "越过边界一个字节即进入下一格";
    EXPECT_EQ(AlignUp(1024U, 1U), 1024U) << "粒度 1 是合法的 2 的幂，结果即原值";
}

// 非 2 的幂必须显式拒绝：按位公式在非法粒度下会给出"看起来合理"的错误结果，
// 这类错误会在 Upload Ring 里表现为偶发的跨 Allocation boundary 写。
TEST(D3D12AlignUpTests, RejectsNonPowerOfTwoAlignment)
{
    EXPECT_THROW(static_cast<void>(AlignUp(64U, 0U)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(AlignUp(64U, 3U)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(AlignUp(64U, 300U)), std::invalid_argument);
}

// 上溢必须显式拒绝而不是回绕：回绕后的"小偏移"会写到缓冲区前段另一段数据上。
TEST(D3D12AlignUpTests, RejectsOverflowInsteadOfWrapping)
{
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    EXPECT_THROW(static_cast<void>(AlignUp(max, 256U)), std::overflow_error);
    // 判据是 "value > max - (alignment - 1)"：粒度越大越容易上溢，因此同样是
    // max - 1，粒度 2 恰好还能对齐（结果回落到自身），粒度 4 就溢出了。
    EXPECT_THROW(static_cast<void>(AlignUp(max - 1U, 4U)), std::overflow_error);
    EXPECT_NO_THROW((AlignUp(max - 1U, 2U))) << "粒度 2 时 max - 1 本身向右避免上溢";

    // 恰好落在边界上的最大值不算上溢：这是对齐算法的正确上界，且结果仍 ≤ max。
    // 表达式整体加一层括号：宏参数里的逗号会被预处理器当成参数分隔符。
    EXPECT_NO_THROW((AlignUp(max - 255U, 256U))) << "max 向下对齐到 256 的倍数是合法的";
    EXPECT_EQ(AlignUp(max - 255U, 256U), max - 255U) << "已按 256 对齐的最大值必须原样返回";
}

// ThrowIfFailed：成功路径必须完全无副作用地直通（后续每个 D3D12 调用都会过它）。
TEST(D3D12ThrowIfFailedTests, PassesThroughSuccessfulResults)
{
    EXPECT_NO_THROW(ThrowIfFailed(S_OK, "synthetic success"));
    // S_FALSE 等成功码同样不得抛：能力探测常用它表达"条件成立但无数据"。
    EXPECT_NO_THROW(ThrowIfFailed(S_FALSE, "synthetic success code"));
}

// 失败路径：必须抛 HResultError，并且原值与上下文都保留，供上层判读。
TEST(D3D12ThrowIfFailedTests, ThrowsHResultErrorCarryingOriginalValueAndContext)
{
    try
    {
        ThrowIfFailed(kSyntheticFailure, "ID3D12Device::CreateCommittedResource");
        FAIL() << "失败的 HRESULT 必须抛出 HResultError";
    }
    catch (const HResultError& error)
    {
        EXPECT_EQ(error.Result(), kSyntheticFailure) << "异常必须保留原始 HRESULT，不得翻译或替换";
        const std::string_view message{error.what()};
        EXPECT_NE(message.find("ID3D12Device::CreateCommittedResource"), std::string_view::npos)
            << "异常文本必须包含可唯一定位调用点的上下文";
        EXPECT_NE(message.find("0x80070057"), std::string_view::npos) << "异常文本必须给出十六进制 HRESULT";
    }
}
