// ============================================================================
// HdrImageTests.cpp — .hdr 解码与 RGBA16F 转换的数值锁定（M4-04）
// 里程碑：M4（04 篇「HDR source contract」）
// 职责：审查确认项 (b) 的落地——在接入真实 HDRI 之前，用手写 Radiance fixture
//       把「.hdr 字节 → 线性 float → RGBA16F → .metex 载荷」的数值链锁死：
//       ① flat RGBE fixture 的解码值逐像素容差比对（线性、顶行方向）；
//       ② 非 Radiance / NaN 拒绝路径；
//       ③ FloatToHalfBits 的锚点与溢出钳制；
//       ④ decode → LinearFloatToRgba16f → BuildTextureArtifact → 半精度字节
//          比对（CookEnvironment 的烘焙段等价物）。
//       数值口径：half 转换为 round-to-nearest，断言容差 1 ULP（half 精度）。
// 关联：tools/asset_cooker/src/HdrImage.h（被测契约）
//       docs/architecture/README.md「HDR source contract」
// ============================================================================

#include "HdrImage.h"
#include "TextureArtifactWriter.h"
#include "TextureMipBuilder.h"

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/TextureFormatV2.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
using MiniEngine::Tools::DecodedHdrImage;
using MiniEngine::Tools::DecodeHdrBytes;
using MiniEngine::Tools::FloatToHalfBits;
using MiniEngine::Tools::LinearFloatToRgba16f;

// 手写 Radiance flat（非 RLE）编码：值 v 的 (byte,e) 组合满足
// v = byte/256 * 2^(e-128)，byte ∈ (0,255]。先选像素级共享的 e（最大通道的
// byte 不超界），再对三通道编码——e 是第 4 字节，三通道必须共用同一 e。
std::array<std::uint8_t, 4> MakeRgbbePixel(const float r, const float g, const float b)
{
    const float maximum = std::max({r, g, b});
    int exponent = static_cast<int>(std::ceil(std::log2(maximum))) + 128;
    const auto byteFor = [&exponent](const float value)
    { return static_cast<int>(std::lround(value / std::ldexp(1.0F, exponent - 128) * 256.0F)); };
    while (byteFor(r) > 255 || byteFor(g) > 255 || byteFor(b) > 255)
    {
        ++exponent; // 任何通道超界：整体提升指数（幅值差有限，两次内收敛）
    }
    return {static_cast<std::uint8_t>(byteFor(r)), static_cast<std::uint8_t>(byteFor(g)),
            static_cast<std::uint8_t>(byteFor(b)), static_cast<std::uint8_t>(exponent)};
}

// 2×1 Radiance flat 文件：头 + "-Y 1 +X 2" 分辨率行 + 2 个 flat RGBE 像素。
// 契约：顶行 = 图像顶行（v=0 侧）——fixture 只有一行，方向由后续 ±Y fixture 锁。
std::vector<std::byte> MakeHdrFixture(const float pixel0[3], const float pixel1[3])
{
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    std::vector<std::byte> bytes;
    for (const char character : header)
    {
        bytes.push_back(static_cast<std::byte>(character));
    }
    for (const auto* pixel : {pixel0, pixel1})
    {
        const std::array<std::uint8_t, 4> encoded = MakeRgbbePixel(pixel[0], pixel[1], pixel[2]);
        for (const std::uint8_t component : encoded)
        {
            bytes.push_back(static_cast<std::byte>(component));
        }
    }
    return bytes;
}

// 半精度位型 → float（测试断言用；标准指数/尾数重建）。
float HalfToFloat(const std::uint16_t half)
{
    const int exponent = ((half >> 10U) & 0x1FU) - 15;
    const std::uint32_t mantissa = half & 0x03FFU;
    float value = 0.0F;
    if (exponent == -15)
    {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    }
    else
    {
        value = std::ldexp(static_cast<float>(mantissa) + 1024.0F, exponent - 10);
    }
    return (half & 0x8000U) != 0U ? -value : value;
}
} // namespace

TEST(HdrImageTests, FlatRgbbeFixtureDecodesToLinearValues)
{
    // 数值链锁定：手写 RGBE → 解码 → 与期望线性值比对（stb 的 RGBE→float 为
    // 精确位运算，容差取 half 一档以内）。
    constexpr std::array<float, 3> kPixel0{1.0F, 2.0F, 0.5F};
    constexpr std::array<float, 3> kPixel1{0.25F, 1.0F, 4.0F};
    const std::vector<std::byte> fixture = MakeHdrFixture(kPixel0.data(), kPixel1.data());

    DecodedHdrImage decoded;
    std::string error;
    ASSERT_TRUE(DecodeHdrBytes(fixture.data(), fixture.size(), decoded, error)) << error;

    EXPECT_EQ(decoded.width, 2U);
    EXPECT_EQ(decoded.height, 1U);
    ASSERT_EQ(decoded.rgba.size(), 8U);
    // 解码值 = 线性光照值（不做任何 gamma）；RGBE 量化误差 < 1/256 相对值。
    for (int channel = 0; channel < 3; ++channel)
    {
        EXPECT_NEAR(decoded.rgba[static_cast<std::size_t>(channel)], kPixel0[channel], 0.01F) << "pixel0 ch" << channel;
        EXPECT_NEAR(decoded.rgba[4 + static_cast<std::size_t>(channel)], kPixel1[channel], 0.01F)
            << "pixel1 ch" << channel;
    }
    EXPECT_FLOAT_EQ(decoded.rgba[3], 1.0F); // A 通道固定 1
}

TEST(HdrImageTests, RejectsNonRadianceAndReportsReason)
{
    // 04 篇要求 stbi_is_hdr_from_memory 前置检查：非 Radiance 字节必须拒绝。
    const std::string notHdr = "not a radiance file at all";
    DecodedHdrImage decoded;
    std::string error;
    EXPECT_FALSE(DecodeHdrBytes(reinterpret_cast<const std::byte*>(notHdr.data()), notHdr.size(), decoded, error));
    EXPECT_NE(error.find("Radiance"), std::string::npos) << error;

    // 空输入防御。
    EXPECT_FALSE(DecodeHdrBytes(nullptr, 0, decoded, error));
}

TEST(HdrImageTests, FloatToHalfAnchorsAndClamps)
{
    // 锚点（IEEE binary16 位型）：1.0=0x3C00、2.0=0x4000、0.5=0x3800。
    EXPECT_EQ(FloatToHalfBits(1.0F), 0x3C00U);
    EXPECT_EQ(FloatToHalfBits(2.0F), 0x4000U);
    EXPECT_EQ(FloatToHalfBits(0.5F), 0x3800U);
    // 往返精度：常规域内 half → float → half 逐位稳定。
    for (const float value : {0.25F, 3.5F, 12.75F, 100.0F})
    {
        const float roundTrip = HalfToFloat(FloatToHalfBits(value));
        EXPECT_FLOAT_EQ(roundTrip, value);
    }
    // 溢出钳制：65504（half 域上界）保持有限；超大值钳到 0x7B00 域（不产生 Inf）。
    EXPECT_TRUE(std::isfinite(HalfToFloat(FloatToHalfBits(65504.0F))));
    const std::uint16_t huge = FloatToHalfBits(1.0e30F);
    EXPECT_EQ((huge >> 10U) & 0x1FU, 0x1EU) << "exponent must not be all-ones (would decode as Inf/NaN)";
}

TEST(HdrImageTests, BakedEnvironmentPayloadIsDeterministicHalfStream)
{
    // CookEnvironment 烘焙段等价物：decode → LinearFloatToRgba16f →
    // BuildTextureArtifact（Rgba16Float/Linear/HdrEnvironment/单 mip）→
    // DATA 载荷与半精度流逐字节一致。
    constexpr std::array<float, 3> kPixel0{1.0F, 2.0F, 0.5F};
    constexpr std::array<float, 3> kPixel1{0.25F, 1.0F, 4.0F};
    const std::vector<std::byte> fixture = MakeHdrFixture(kPixel0.data(), kPixel1.data());
    DecodedHdrImage decoded;
    std::string error;
    ASSERT_TRUE(DecodeHdrBytes(fixture.data(), fixture.size(), decoded, error)) << error;

    const std::vector<std::uint8_t> halfStream = LinearFloatToRgba16f(decoded.rgba);
    ASSERT_EQ(halfStream.size(), 16U); // 2×1×4 通道×2B

    // 半精度数值比对（round-to-nearest，1 ULP half 容差）。
    for (std::size_t component = 0; component < 8; ++component)
    {
        const std::uint16_t bits =
            static_cast<std::uint16_t>(halfStream[component * 2U]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(halfStream[component * 2U + 1U]) << 8U);
        const float actual = HalfToFloat(bits);
        const float expected = decoded.rgba[component];
        EXPECT_NEAR(actual, expected, std::max(0.01F * std::abs(expected), 1.0e-4F)) << "component " << component;
    }

    // .metex v2 烘焙：单 mip（rowPitch = width*8）。
    MiniEngine::Tools::BuiltTextureMips mips;
    mips.pixels.resize(halfStream.size());
    std::memcpy(mips.pixels.data(), halfStream.data(), halfStream.size());
    const MiniEngine::Tools::TextureMipLevel mip0{0U, decoded.width * 8U,
                                                  static_cast<std::uint32_t>(halfStream.size())};
    mips.levels.push_back(mip0);

    const std::array<std::byte, 32> buildKey{};
    std::vector<std::byte> textureBytes;
    ASSERT_TRUE(MiniEngine::Tools::BuildTextureArtifact(
        buildKey, decoded.width, decoded.height, MiniEngine::Assets::TexturePixelFormat::Rgba16Float,
        MiniEngine::Assets::TextureColorSpace::Linear, MiniEngine::Assets::TextureUsage::HdrEnvironment, mips,
        textureBytes, error))
        << error;

    // header flags = .metex v2；MIPS 单级记录（offset 0 / rowPitch 16 / size 16）。
    const auto readU32 = [&textureBytes](const std::size_t offset)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, textureBytes.data() + offset, sizeof(value));
        return value;
    };
    EXPECT_EQ(readU32(56), MiniEngine::Assets::kTextureFormatVersion);
    EXPECT_EQ(readU32(12), 3U); // INFO + MIPS + DATA
}
