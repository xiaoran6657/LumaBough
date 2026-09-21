// ============================================================================
// ImageComparisonTests.cpp — M4-07 视觉回归比较器的单元测试
// 里程碑：M4（07 篇「先测重复性，再定阈值」；手抄清单第 3 条）
// 职责：在进入 capture 流程前锁定比较器数学与拒绝语义：
//   - MAE/RMSE/p99/max/changedRate 与手算样本一致（单像素误差、已知分布）；
//   - 空输入 / 长度不等 / 非 RGBA8 必须在计算分位数前失败（07 篇硬要求）；
//   - 截图元数据配置不一致 → INCOMPARABLE（07 篇「元数据文件」：不能给像素 PASS）。
// 比较器实现在 engine/rhi/d3d11/src/D3D11Screenshot.{h,cpp}（与 capture 同模块，
// 保证 PNG 编解码与比较器使用同一套 RGBA8 约定）。
// 关联：docs/architecture/README.md「自动化测试」
//       engine/rhi/d3d11/src/D3D11Screenshot.h（被测实现）
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11ImageComparison.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using MiniEngine::Rhi::D3D11::CompareRgba8;
using MiniEngine::Rhi::D3D11::ImageDiffMetrics;
using MiniEngine::Rhi::D3D11::MetadataIncomparableReason;
using MiniEngine::Rhi::D3D11::ScreenshotConfigIdentity;

namespace
{
// 构造 width×height 的 RGBA8 缓冲：golden 全 (16,32,48)，可选把第 index 个通道
// 拉开 delta，便于构造"已知误差分布"的用例。
std::vector<std::uint8_t> MakeImage(const std::uint32_t width, const std::uint32_t height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t index = 0; index < pixels.size(); index += 4U)
    {
        pixels[index] = 16;
        pixels[index + 1U] = 32;
        pixels[index + 2U] = 48;
        pixels[index + 3U] = 255; // alpha 契约固定 1，比较器忽略
    }
    return pixels;
}
} // namespace

// 单像素单通道差 3：MAE = 3/255/3（一个通道均摊到 3 个 sample）、RMSE = √3/255/√3、
// changedRate = 1/3（changedThreshold=2 时该通道计 1 次）。
TEST(ImageComparisonTests, ComputesKnownSinglePixelError)
{
    std::vector<std::uint8_t> golden = MakeImage(1, 1);
    std::vector<std::uint8_t> candidate = golden;
    candidate[0] += 3; // 唯一的差异通道

    const ImageDiffMetrics metrics = CompareRgba8(golden, candidate, 2);

    EXPECT_NEAR(metrics.mae, 1.0 / 255.0, 1.0e-12);
    EXPECT_NEAR(metrics.rmse, std::sqrt(3.0) / 255.0, 1.0e-12);
    EXPECT_NEAR(metrics.maxError, 3.0 / 255.0, 1.0e-12);
    EXPECT_NEAR(metrics.changedRate, 1.0 / 3.0, 1.0e-12);
    EXPECT_EQ(metrics.sampleCount, 3U); // 1 像素 × 3 通道
}

// identical 图像：全 0 指标（10× 重复性 self-consistency 的理想值锚点）。
TEST(ImageComparisonTests, IdenticalImagesYieldZeroMetrics)
{
    const std::vector<std::uint8_t> golden = MakeImage(4, 4);
    const ImageDiffMetrics metrics = CompareRgba8(golden, golden, 3);

    EXPECT_DOUBLE_EQ(metrics.mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics.rmse, 0.0);
    EXPECT_DOUBLE_EQ(metrics.p99, 0.0);
    EXPECT_DOUBLE_EQ(metrics.maxError, 0.0);
    EXPECT_DOUBLE_EQ(metrics.changedRate, 0.0);
    EXPECT_EQ(metrics.sampleCount, 4U * 4U * 3U);
}

// p99 的下标约定：300 个 sample 中恰好 3 个差 1（1%）→ ceil(0.99×300)=297 →
// 下标 296 落在"0 差异"段 → p99 = 0。差值再多 1 个（4 个 1%）→ 下标 296 仍是 0，
// 300 个里 5 个差 1（1.67%）→ 排序后下标 295..299 是 1 → p99 = 1/255。
TEST(ImageComparisonTests, P99FollowsCeilIndexConvention)
{
    std::vector<std::uint8_t> golden = MakeImage(25, 4); // 100 像素 = 300 通道
    std::vector<std::uint8_t> candidate = golden;

    // 恰好 3 个通道差 1（=1%，占满 top-1% 之外）：p99 仍应取到 0。
    for (std::size_t index = 0; index < 3U; ++index)
    {
        candidate[index * 4U] += 1;
    }
    ImageDiffMetrics metrics = CompareRgba8(golden, candidate, 3);
    EXPECT_DOUBLE_EQ(metrics.p99, 0.0);
    EXPECT_NEAR(metrics.maxError, 1.0 / 255.0, 1.0e-12);

    // 追加到 5 个通道差 1：5/300 = 1.67% > 1% → p99 = 1/255。
    candidate[4U * 4U + 1U] += 1;
    metrics = CompareRgba8(golden, candidate, 3);
    EXPECT_NEAR(metrics.p99, 1.0 / 255.0, 1.0e-12);
}

// 空输入 / 长度不等 / 非 4 倍数：必须在计算分位数前失败（07 篇硬要求）。
TEST(ImageComparisonTests, RejectsInvalidInputs)
{
    const std::vector<std::uint8_t> golden = MakeImage(2, 2);
    const std::vector<std::uint8_t> empty;

    EXPECT_THROW(static_cast<void>(CompareRgba8(empty, empty, 3)), std::invalid_argument);

    std::vector<std::uint8_t> shorter = golden;
    shorter.pop_back();
    EXPECT_THROW(static_cast<void>(CompareRgba8(golden, shorter, 3)), std::invalid_argument);

    // 非 RGBA8（大小不是 4 的倍数）。
    const std::vector<std::uint8_t> rgb{1U, 2U, 3U};
    EXPECT_THROW(static_cast<void>(CompareRgba8(rgb, rgb, 3)), std::invalid_argument);
}

// 07 篇「元数据文件」：manifest hash 不同 → INCOMPARABLE（不能给像素 PASS）；
// 配置全同 → 可比较（空 reason）。
TEST(ImageComparisonTests, MetadataComparabilityFollowsConfigKeys)
{
    ScreenshotConfigIdentity golden{};
    golden.schemaVersion = 1;
    golden.scene = "asset://tests/m4-visual-baseline";
    golden.width = 1280;
    golden.height = 720;
    golden.fixedTick = 300;
    golden.assetManifestSha256 = "aaaa";
    golden.environmentArtifactSha256 = "bbbb";
    golden.iblProfile = "baseline";
    golden.exposureEv = 0.0;
    golden.toneMapper = "reinhard";

    // 全同：可比较。
    EXPECT_EQ(MetadataIncomparableReason(golden, golden), "");

    // schemaVersion 不同 → 最先拒绝（schema 演进时旧 golden 不能进入像素比较）。
    ScreenshotConfigIdentity differentSchema = golden;
    differentSchema.schemaVersion = 2;
    const std::string schemaReason = MetadataIncomparableReason(golden, differentSchema);
    EXPECT_NE(schemaReason, "");
    EXPECT_NE(schemaReason.find("schemaVersion"), std::string::npos);

    // manifest hash 不同 → INCOMPARABLE，且原因点名 manifest。
    ScreenshotConfigIdentity differentManifest = golden;
    differentManifest.assetManifestSha256 = "cccc";
    const std::string manifestReason = MetadataIncomparableReason(golden, differentManifest);
    EXPECT_NE(manifestReason, "");
    EXPECT_NE(manifestReason.find("assetManifestSha256"), std::string::npos);

    // 曝光不同 → INCOMPARABLE（故意改曝光的负向测试依赖这一条）。
    ScreenshotConfigIdentity differentExposure = golden;
    differentExposure.exposureEv = 1.0;
    EXPECT_NE(MetadataIncomparableReason(golden, differentExposure), "");

    // 分辨率不同 → INCOMPARABLE（PNG 尺寸不同属于 FAIL，配置层先拒 INCOMPARABLE）。
    ScreenshotConfigIdentity differentSize = golden;
    differentSize.height = 480;
    EXPECT_NE(MetadataIncomparableReason(golden, differentSize), "");
}
