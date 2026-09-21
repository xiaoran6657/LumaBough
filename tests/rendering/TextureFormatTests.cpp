// ============================================================================
// TextureFormatTests.cpp — .metex v2 语义契约（usage / 像素格式 / 颜色空间 / mip）
// 里程碑：M4（02 篇「Texture v2」）
// 职责：锁定 TextureHeaderV2::IsValid 的语义校验：baseColor/emissive 必须 sRGB、
//       normal/occlusion/metallicRoughness 必须 linear、HDR 环境必须 RGBA16F
//       linear、mipCount 不超过全链上界——非法组合在 Reader 入口即拒绝，
//       不允许按文件名或扩展名猜测（Khronos 传输函数契约）。
// 关联：engine/assets/include/MiniEngine/Assets/TextureFormatV2.h（被测实现）
//       docs/architecture/README.md「Texture v2」「自动化测试」
// ============================================================================

#include <MiniEngine/Assets/TextureFormatV2.h>

#include <gtest/gtest.h>

using namespace MiniEngine::Assets;

TEST(TextureFormatTests, AcceptsKnownColorSpaceContracts)
{
    // baseColor：RGBA8 + sRGB（Khronos：base color 使用 sRGB transfer）。
    EXPECT_TRUE(IsValid(TextureHeaderV2{256, 128, 9, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Srgb,
                                        TextureUsage::BaseColor}));
    // HDR 环境：RGBA16F + linear（04 篇 IBL 输入，语义已在 v2 契约中预留）。
    EXPECT_TRUE(IsValid(TextureHeaderV2{1024, 512, 11, TexturePixelFormat::Rgba16Float, TextureColorSpace::Linear,
                                        TextureUsage::HdrEnvironment}));
    // normal / occlusion / metallicRoughness：RGBA8 + linear。
    EXPECT_TRUE(IsValid(
        TextureHeaderV2{4, 4, 1, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear, TextureUsage::Normal}));
    EXPECT_TRUE(IsValid(
        TextureHeaderV2{4, 4, 3, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear, TextureUsage::Occlusion}));
}

TEST(TextureFormatTests, RejectsUnknownUsageAndImpossibleMipCount)
{
    // 未知 usage（wire 枚举外的值）拒绝。
    EXPECT_FALSE(IsValid(TextureHeaderV2{4, 4, 1, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear,
                                         static_cast<TextureUsage>(255)}));
    // 2×2 的 mip 全链上界是 2（2×2 → 1×1），mipCount=4 物理不可达。
    EXPECT_FALSE(IsValid(
        TextureHeaderV2{4, 4, 4, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear, TextureUsage::Normal}));
    // 零尺寸拒绝。
    EXPECT_FALSE(IsValid(
        TextureHeaderV2{0, 4, 1, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear, TextureUsage::Normal}));
}

TEST(TextureFormatTests, RejectsIncorrectSemanticColorSpace)
{
    // normal 贴图标 sRGB：违反 Khronos 传输函数契约，Reader 入口即拒绝。
    EXPECT_FALSE(IsValid(TextureHeaderV2{4, 4, 1, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Srgb,
                                         TextureUsage::MetallicRoughness}));
    // baseColor 用 linear 视图同样非法（会跳过硬件 sRGB 解码）。
    EXPECT_FALSE(IsValid(
        TextureHeaderV2{4, 4, 1, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear, TextureUsage::BaseColor}));
    // HDR 环境用 RGBA8/非 float 格式非法。
    EXPECT_FALSE(IsValid(TextureHeaderV2{4, 4, 1, TexturePixelFormat::Rgba8Unorm, TextureColorSpace::Linear,
                                         TextureUsage::HdrEnvironment}));
}
