// ============================================================================
// HdrImage.h — Radiance RGBE (.hdr) panorama 解码与校验（Cooker 专用）
// 里程碑：M4（04 篇「HDR source contract」；手抄清单/资产链）
// 职责：从内存字节解码 .hdr 为线性 float RGBA（stbi_loadf_from_memory，
//       desired_channels=4），并执行篇目硬校验：
//         1. stbi_is_hdr_from_memory 先确认是 Radiance 格式；
//         2. 拒绝 NaN/Inf/负值（HDR 源的物理光照值不允许越界输入）；
//         3. 尺寸预算（maxTextureDimension）与 file budget 由调用方先行校验，
//            此处兜底 0 尺寸拒绝。
//       垂直方向契约：stbi_set_flip_vertically_on_load 保持 0（进程全局状态，
//       04 篇禁止）——stb 原生输出顶行对应图像顶行，与「顶边为 +Y、Cooker 不
//       做垂直翻转」契约一致。
// 关联：tools/asset_cooker/deps/stb/README.txt（版本 pin）
//       tools/asset_cooker/src/CookSession.cpp（CookEnvironment 消费方）
// ============================================================================
#pragma once

#include <MiniEngine/Assets/TextureFormatV2.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// 解码后的 HDR panorama：线性光照值（每像素 4 float，行主序，顶行 = 图像顶行）。
struct DecodedHdrImage final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba; // width*height*4，线性值（可为 >1 的 HDR 亮度）
};

// 从内存解码 Radiance .hdr。失败返回 false 且 error 携带可读原因
//（非 Radiance / stb 解码错误 / NaN / Inf / 负值 / 零尺寸）。
[[nodiscard]] bool DecodeHdrBytes(const std::byte* bytes, std::size_t byteCount, DecodedHdrImage& out,
                                  std::string& error);

// 把线性 float4 像素转换为 RGBA16F 半精度字节流（.metex HdrEnvironment 的 DATA
// 载荷，4 半精度/像素 = 8B/像素）。NaN/Inf/负在 DecodeHdrBytes 已拒绝，此处是
// 纯数值转换（round-to-nearest-even 由 half 转换实现承担）。
[[nodiscard]] std::vector<std::uint8_t> LinearFloatToRgba16f(const std::vector<float>& rgba);

// float → IEEE 754 binary16（round-to-nearest-even；NaN/Inf/溢出钳到 0x7C00 域）。
[[nodiscard]] std::uint16_t FloatToHalfBits(float value);
} // namespace MiniEngine::Tools
