// ============================================================================
// HdrImage.cpp — Radiance .hdr 解码与 RGBA16F 转换的实现
// 里程碑：M4（04 篇「HDR source contract」）
// 职责：见头文件。本文件是 stb_image 在整个仓库的唯一消费方（实现宏在
//       deps/stb/StbImage.cpp 单点展开）；Cooker 之外任何目标 include 本头
//       或链接 stb 都违反 ADR-0004（runtime 不解析 source format）。
//       .hdr 的解码结果是线性光照值——与 8-bit 贴图的本质区别是不做任何
//       gamma/sRGB 处理（04 篇审查确认项 (a)），colorSpace 恒为 Linear。
// 关联：tools/asset_cooker/src/HdrImage.h（对外契约）
//       tools/asset_cooker/src/CookSession.cpp（CookEnvironment）
// ============================================================================

#include "HdrImage.h"

#include <MiniEngine/Core/Log.h>

#include <cmath>
#include <cstring>

// stb 的实现宏在 deps/stb/StbImage.cpp 唯一展开；此处只引声明。
#include "stb_image.h"

namespace MiniEngine::Tools
{
namespace
{
// 半精度转换（round-to-nearest-even 的查表法太重，直接走 IEEE 位操作：
// float 32 位 → binary16 的标准截断/舍入路径；输入已保证有限非负）。
// 参考：https://gist.github.com/rygorous/2156668 的 F16 加密流程简化版。
constexpr std::uint32_t kFloatSignMask = 0x8000'0000U;
constexpr std::uint32_t kFloatExponentMask = 0x7F80'0000U;
constexpr std::uint32_t kFloatMantissaMask = 0x007F'FFFFU;
} // namespace

std::uint16_t FloatToHalfBits(const float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const std::uint16_t sign = static_cast<std::uint16_t>((bits & kFloatSignMask) >> 16U);
    const std::int32_t exponent = static_cast<std::int32_t>((bits & kFloatExponentMask) >> 23U) - 127 + 15;
    const std::uint32_t mantissa = bits & kFloatMantissaMask;

    if ((bits & kFloatExponentMask) == kFloatExponentMask)
    {
        // Inf/NaN：输入侧已拒绝，防御性钳到 half 最大有限值并保留符号。
        return static_cast<std::uint16_t>(sign | 0x7B00U);
    }
    if (exponent >= 0x1F)
    {
        // 溢出（>65504）：钳到 half 最大有限值（HDR 亮度超出 half 域按饱和处理）。
        return static_cast<std::uint16_t>(sign | 0x7B00U);
    }
    if (exponent <= 0)
    {
        // 下溢（<2^-14）：round-to-nearest 到 subnormal/0。此处按饱和到 0 简化
        //（Sandbox/Cooker 的输入幅值远高于该阈值；真 subnormal 支持随需求再加）。
        return sign;
    }

    // 常规域：mantissa 高 10 位 + 舍入位（round-to-nearest-even 简化为就近截断：
    // 第 13 位进位判断）。为确定性采用截断 + 进位。
    const std::uint32_t halfMantissa = mantissa >> 13U;
    const std::uint32_t rounding = mantissa & 0x1FFFU;
    const bool roundUp = rounding > 0x1000U || (rounding == 0x1000U && (halfMantissa & 1U) != 0U);
    const std::uint32_t rounded = halfMantissa + (roundUp ? 1U : 0U);
    const std::uint32_t carry = rounded >> 10U; // mantissa 舍入进位到指数
    const std::uint32_t finalExponent = static_cast<std::uint32_t>(exponent) + carry;
    if (finalExponent >= 0x1F)
    {
        return static_cast<std::uint16_t>(sign | 0x7B00U); // 进位溢出钳制
    }
    return static_cast<std::uint16_t>(sign | (finalExponent << 10U) | (rounded & 0x03FFU));
}

bool DecodeHdrBytes(const std::byte* bytes, const std::size_t byteCount, DecodedHdrImage& out, std::string& error)
{
    if (bytes == nullptr || byteCount == 0)
    {
        error = "HDR source is empty";
        return false;
    }

    // 1) 先确认是 Radiance 格式（04 篇要求 stbi_is_hdr_from_memory）。
    if (stbi_is_hdr_from_memory(reinterpret_cast<stbi_uc const*>(bytes), static_cast<int>(byteCount)) == 0)
    {
        error = "HDR source is not a Radiance (.hdr) image";
        return false;
    }

    // 2) memory API 解码，desired_channels=4（RGBA），不做垂直翻转（契约：
    //    图像顶边为 +Y；进程全局 flip state 禁止触碰）。
    int width = 0;
    int height = 0;
    int channels = 0;
    float* decoded = stbi_loadf_from_memory(reinterpret_cast<stbi_uc const*>(bytes), static_cast<int>(byteCount),
                                            &width, &height, &channels, 4);
    if (decoded == nullptr)
    {
        error = std::string{"stb_image failed to decode .hdr: "} + (stbi_failure_reason() != nullptr
                                                                       ? stbi_failure_reason()
                                                                       : "unknown error");
        return false;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.rgba.assign(decoded, decoded + pixelCount * 4U);
    stbi_image_free(decoded);

    if (out.width == 0 || out.height == 0)
    {
        error = "HDR source has zero dimension";
        out = {};
        return false;
    }

    // 3) 数值校验（04 篇：拒绝 NaN、Inf、负值）。
    for (std::size_t index = 0; index < out.rgba.size(); ++index)
    {
        const float value = out.rgba[index];
        if (!std::isfinite(value))
        {
            error = "HDR source contains NaN/Inf at component index " + std::to_string(index);
            out = {};
            return false;
        }
        if (value < 0.0F)
        {
            error = "HDR source contains negative value at component index " + std::to_string(index);
            out = {};
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> LinearFloatToRgba16f(const std::vector<float>& rgba)
{
    std::vector<std::uint8_t> output(rgba.size() * sizeof(std::uint16_t), 0U);
    auto* halves = reinterpret_cast<std::uint16_t*>(output.data());
    for (std::size_t index = 0; index < rgba.size(); ++index)
    {
        halves[index] = FloatToHalfBits(rgba[index]);
    }
    return output;
}
} // namespace MiniEngine::Tools
