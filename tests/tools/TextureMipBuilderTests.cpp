// ============================================================================
// TextureMipBuilderTests.cpp — 确定性 CPU mip 链生成的单元测试（G-1 缺口补齐）
// 里程碑：M4-09（02 篇「Texture v2 / Mips」的遗留覆盖缺口）
// 职责：TextureMipBuilder 自 M4-02 落地以来只有 cook e2e 间接覆盖，本文件锁定：
//         a) mip 链级数/offset/rowPitch/byteSize 的级联契约，直至 1×1 终止；
//         b) sRGB 角色（BaseColor/Emissive）在线性域滤波——结果必须区别于
//            "对已编码字节直接平均"，这是感知正确的关键；
//         c) Normal 角色解码→平均→重归一化、A 恒 255、退化回退 +Z；
//         d) 线性角色（MetallicRoughness/Occlusion）全通道直接平均；
//         e) odd 尺寸 max(1, size/2) 下取整 + 源坐标 clamp（边界 tap 采样两次）；
//         f) 非法输入拒绝（0 尺寸、顶层字节数不符、HdrEnvironment 不在 M4-02 范围）。
// 关联：tools/asset_cooker/src/TextureMipBuilder.{h,cpp}
//       docs/architecture/DECISIONS.md §1 缺口 G-1
// ============================================================================

#include "TextureMipBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
using MiniEngine::Assets::TextureUsage;
using MiniEngine::Tools::BuildTextureMipChain;
using MiniEngine::Tools::BuiltTextureMips;

// 生成 width×height 的纯色顶层（RGBA8）。
[[nodiscard]] std::vector<std::byte> SolidRgba8(const std::uint32_t width, const std::uint32_t height,
                                                const std::array<std::uint8_t, 4> rgba)
{
    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4U, std::byte{0});
    for (std::size_t index = 0; index + 3 < pixels.size(); index += 4)
    {
        pixels[index + 0] = std::byte{rgba[0]};
        pixels[index + 1] = std::byte{rgba[1]};
        pixels[index + 2] = std::byte{rgba[2]};
        pixels[index + 3] = std::byte{rgba[3]};
    }
    return pixels;
}

[[nodiscard]] std::uint8_t ByteAt(const BuiltTextureMips& mips, const std::size_t level, const std::uint32_t x,
                                  const std::uint32_t y, const std::uint32_t levelWidth, const int channel)
{
    const std::size_t base = mips.levels[level].offset + (static_cast<std::size_t>(y) * levelWidth + x) * 4U;
    return std::to_integer<std::uint8_t>(mips.pixels[base + static_cast<std::size_t>(channel)]);
}

// CPU 参考：与 TextureMipBuilder 的 sRGB 传输函数一致（02 篇 PbrMathTests 口径）。
[[nodiscard]] double SrgbToLinear(const double encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double LinearToSrgb(const double linear)
{
    return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

[[nodiscard]] std::uint8_t EncodeChannel(const double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return static_cast<std::uint8_t>(clamped * 255.0 + 0.5);
}
} // namespace

// --- 级联契约 ---------------------------------------------------------------

TEST(TextureMipBuilderTests, ChainDescendsToOneByOneWithContractOffsets)
{
    BuiltTextureMips mips;
    std::string error;
    // 8×8 纯红、线性角色（Occlusion）， mip0 在前。
    const bool ok =
        BuildTextureMipChain(8, 8, TextureUsage::Occlusion, SolidRgba8(8, 8, {200, 100, 50, 255}), mips, error);
    ASSERT_TRUE(ok) << error;

    // 8 → 4 → 2 → 1：4 级。
    ASSERT_EQ(mips.levels.size(), 4U);
    std::uint64_t expectedOffset = 0;
    std::uint32_t expectedWidth = 8;
    for (std::size_t level = 0; level < mips.levels.size(); ++level)
    {
        EXPECT_EQ(mips.levels[level].offset, expectedOffset) << "level " << level;
        EXPECT_EQ(mips.levels[level].rowPitch, expectedWidth * 4U) << "level " << level;
        EXPECT_EQ(mips.levels[level].byteSize, expectedWidth * expectedWidth * 4U) << "level " << level;
        expectedOffset += mips.levels[level].byteSize;
        expectedWidth /= 2U;
    }
    // 顶层 + 全链 = (64+16+4+1)*4 字节。
    EXPECT_EQ(mips.pixels.size(), (64U + 16U + 4U + 1U) * 4U);

    // 纯色输入逐级不变（线性平均的平凡情形），1×1 终止级就是最后一个。
    const std::size_t last = mips.levels.size() - 1;
    EXPECT_EQ(ByteAt(mips, last, 0, 0, 1, 0), 200);
    EXPECT_EQ(ByteAt(mips, last, 0, 0, 1, 1), 100);
    EXPECT_EQ(ByteAt(mips, last, 0, 0, 1, 2), 50);
    EXPECT_EQ(ByteAt(mips, last, 0, 0, 1, 3), 255);
}

TEST(TextureMipBuilderTests, NonSquareChainTerminatesAtOneByOne)
{
    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(4, 2, TextureUsage::Occlusion, SolidRgba8(4, 2, {0, 0, 0, 255}), mips, error))
        << error;

    // 4×2 → 2×1 → 1×1：3 级（每维独立 max(1, size/2)）。
    ASSERT_EQ(mips.levels.size(), 3U);
    EXPECT_EQ(mips.levels[0].rowPitch, 16U);
    EXPECT_EQ(mips.levels[1].rowPitch, 8U);
    EXPECT_EQ(mips.levels[2].rowPitch, 4U);
    EXPECT_EQ(mips.levels[2].byteSize, 4U);
}

// --- sRGB 角色：滤波必须发生在线性域 -----------------------------------------

TEST(TextureMipBuilderTests, SrgbRoleFiltersInLinearDomain)
{
    // 2×1 顶层：左像素 R=0（编码值），右像素 R=255。
    // 线性域平均 → 0.5 线性 → sRGB 编码 ≈ 188；若错误地直接平均编码字节则 ≈128。
    std::vector<std::byte> top(2U * 4U, std::byte{0});
    auto put = [&top](const std::uint32_t x, const std::array<std::uint8_t, 4> rgba)
    {
        for (int c = 0; c < 4; ++c)
        {
            top[static_cast<std::size_t>(x) * 4U + static_cast<std::size_t>(c)] = std::byte{rgba[c]};
        }
    };
    put(0, {0, 0, 0, 255});
    put(1, {255, 255, 255, 255});

    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(2, 1, TextureUsage::BaseColor, top, mips, error)) << error;
    ASSERT_EQ(mips.levels.size(), 2U);

    const std::uint8_t expectedR = EncodeChannel(LinearToSrgb((SrgbToLinear(0.0) + SrgbToLinear(1.0)) * 0.5));
    const std::uint8_t expectedA = EncodeChannel(1.0); // alpha 恒线性平均
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 0), expectedR);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 1), expectedR);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 2), expectedR);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 3), expectedA);

    // 关键反证：与"对已编码字节直接平均"（128）显著不同——滤波确实发生在线性域。
    EXPECT_NE(ByteAt(mips, 1, 0, 0, 1, 0), 128);
    // 与 CPU 参考逐字节一致（±1 容差吸收传输函数实现的浮点差）。
    EXPECT_NEAR(static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 0)), static_cast<double>(expectedR), 1.0);
}

TEST(TextureMipBuilderTests, EmissiveRoleSharesSrgbDomain)
{
    std::vector<std::byte> top(2U * 4U, std::byte{0});
    top[0] = std::byte{0};
    top[4] = std::byte{255};
    for (std::size_t index = 0; index < top.size(); index += 4)
    {
        top[index + 3] = std::byte{255};
    }

    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(2, 1, TextureUsage::Emissive, top, mips, error)) << error;

    const std::uint8_t expectedR = EncodeChannel(LinearToSrgb((SrgbToLinear(0.0) + SrgbToLinear(1.0)) * 0.5));
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 0), expectedR);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 0), EncodeChannel(LinearToSrgb(0.5)));
}

// --- Normal 角色：重归一化 / A=255 / 退化回退 ---------------------------------

TEST(TextureMipBuilderTests, NormalRoleRenormalizesAndForcesAlpha)
{
    // 2×1：+X 法线 (255,128,128) 与 +Y 法线 (128,255,128)。
    std::vector<std::byte> top(2U * 4U, std::byte{0});
    auto putNormal = [&top](const std::uint32_t x, const std::array<std::uint8_t, 3> rgb)
    {
        for (int c = 0; c < 3; ++c)
        {
            top[static_cast<std::size_t>(x) * 4U + static_cast<std::size_t>(c)] = std::byte{rgb[c]};
        }
        top[static_cast<std::size_t>(x) * 4U + 3] = std::byte{0}; // 输入 alpha 无关紧要
    };
    putNormal(0, {255, 128, 128});
    putNormal(1, {128, 255, 128});

    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(2, 1, TextureUsage::Normal, top, mips, error)) << error;

    // CPU 参考：解码到 [-1,1] → 平均 → 归一化 → 重编码；A 恒 255。
    const double decode255 = 128.0 / 255.0 * 2.0 - 1.0; // 0.00392
    const double nx = (1.0 + decode255) * 0.5;
    const double ny = (decode255 + 1.0) * 0.5;
    const double nz = (decode255 + decode255) * 0.5;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    const std::uint8_t expectedX = EncodeChannel(nx / length * 0.5 + 0.5);
    const std::uint8_t expectedY = EncodeChannel(ny / length * 0.5 + 0.5);
    EXPECT_NEAR(static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 0)), static_cast<double>(expectedX), 1.0);
    EXPECT_NEAR(static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 1)), static_cast<double>(expectedY), 1.0);
    // 归一化后 |n| = 1（2*expected-1 的重建向量长度应为 1，容差 2/255）；A 恒 255。
    const double rx = (static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 0)) / 255.0 * 2.0 - 1.0);
    const double ry = (static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 1)) / 255.0 * 2.0 - 1.0);
    const double rz = (static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 2)) / 255.0 * 2.0 - 1.0);
    EXPECT_NEAR(std::sqrt(rx * rx + ry * ry + rz * rz), 1.0, 2.0 / 255.0);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 3), 255);
}

TEST(TextureMipBuilderTests, NormalRoleFallsBackToPlusZOnDegenerateInput)
{
    // 构造精确退化的平均向量：两个 tap 解码后互为相反向量（+1,+1,+1 与 -1,-1,-1，
    // 字节 (255,255,255) 与 (0,0,0)），平均 = (0,0,0) → 长度 0 → 契约规定回退 +Z。
    // 注意字节 128 解码是 0.00392 而不是 0，用它构不成精确零向量。
    std::vector<std::byte> top(2U * 4U, std::byte{0});
    auto putNormal = [&top](const std::uint32_t x, const std::array<std::uint8_t, 3> rgb)
    {
        for (int c = 0; c < 3; ++c)
        {
            top[static_cast<std::size_t>(x) * 4U + static_cast<std::size_t>(c)] = std::byte{rgb[c]};
        }
        top[static_cast<std::size_t>(x) * 4U + 3] = std::byte{255};
    };
    putNormal(0, {255, 255, 255}); // (+1,+1,+1)
    putNormal(1, {0, 0, 0});       // (-1,-1,-1)

    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(2, 1, TextureUsage::Normal, top, mips, error)) << error;

    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 0), 128); // +Z 重编码 = 0.5*255+0.5 = 128
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 1), 128);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 2), 255);
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 3), 255);
}

// --- 线性角色与 odd 尺寸 ------------------------------------------------------

TEST(TextureMipBuilderTests, LinearRoleAveragesAllChannels)
{
    // 2×1：左 (0,0,0,0)、右 (255,255,255,255) → 全通道均值 128。
    std::vector<std::byte> top(2U * 4U, std::byte{0});
    for (int c = 0; c < 4; ++c)
    {
        top[4U + static_cast<std::size_t>(c)] = std::byte{255};
    }

    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(2, 1, TextureUsage::MetallicRoughness, top, mips, error)) << error;
    for (int c = 0; c < 4; ++c)
    {
        EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, c), 128) << "channel " << c;
    }

    // Occlusion 角色同走线性分支。
    ASSERT_TRUE(BuildTextureMipChain(2, 1, TextureUsage::Occlusion, top, mips, error)) << error;
    EXPECT_EQ(ByteAt(mips, 1, 0, 0, 1, 0), 128);
}

TEST(TextureMipBuilderTests, OddSizeClampsSourceCoordinates)
{
    // 3×1 线性：列值 0 / 128 / 255 → 目标 1×1（宽 3/2=1，高 max(1, 1/2)=1）。
    // 唯一目标像素的四个 tap 是 (0,0)(1,0)(0,1→0)(1,1→0)：高为 1 时 y+1 被 clamp
    // 回 0，同一行被采样两次 → 均值 = (0 + 128/255) / 2。
    // （clamp 只会在源维度为 1 时触发；宽度 ≥2 时 2*floor(w/2)-1 ≤ w-1 恒成立。）
    std::vector<std::byte> top(3U * 4U, std::byte{0});
    const std::array<std::uint8_t, 3> columns{0, 128, 255};
    for (std::uint32_t x = 0; x < 3; ++x)
    {
        for (int c = 0; c < 4; ++c)
        {
            top[static_cast<std::size_t>(x) * 4U + static_cast<std::size_t>(c)] = std::byte{columns[x]};
        }
    }

    BuiltTextureMips mips;
    std::string error;
    ASSERT_TRUE(BuildTextureMipChain(3, 1, TextureUsage::Occlusion, top, mips, error)) << error;
    ASSERT_EQ(mips.levels.size(), 2U);
    ASSERT_EQ(mips.levels[1].rowPitch, 4U);

    const std::uint8_t expected = EncodeChannel((0.0 + (128.0 / 255.0)) * 0.5);
    EXPECT_NEAR(static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 0)), static_cast<double>(expected), 1.0);
    EXPECT_NEAR(static_cast<double>(ByteAt(mips, 1, 0, 0, 1, 1)), static_cast<double>(expected), 1.0);
}

// --- 非法输入 ----------------------------------------------------------------

TEST(TextureMipBuilderTests, RejectsInvalidDimensionsAndPayloadAndHdrUsage)
{
    BuiltTextureMips mips;
    std::string error;

    EXPECT_FALSE(BuildTextureMipChain(0, 4, TextureUsage::Occlusion, SolidRgba8(1, 1, {0, 0, 0, 0}), mips, error));
    EXPECT_EQ(error, "texture dimensions must be non-zero");

    EXPECT_FALSE(BuildTextureMipChain(4, 0, TextureUsage::Occlusion, SolidRgba8(1, 1, {0, 0, 0, 0}), mips, error));

    error.clear();
    // 顶层字节数不符（4×2 需要 32 字节，只给 16）。
    EXPECT_FALSE(BuildTextureMipChain(4, 2, TextureUsage::Occlusion, SolidRgba8(2, 2, {0, 0, 0, 0}), mips, error));
    EXPECT_EQ(error, "top-level rgba8 size does not equal width*4*height");

    error.clear();
    // HDR 环境的 RGBA16F mip 链不在 RGBA8 生成器范围内（04 篇 IBL 接管）。
    EXPECT_FALSE(BuildTextureMipChain(4, 4, TextureUsage::HdrEnvironment, SolidRgba8(4, 4, {0, 0, 0, 0}), mips, error));
    EXPECT_EQ(error, "HDR environment mip chain is not part of M4-02");
}
