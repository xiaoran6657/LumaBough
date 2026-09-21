#pragma once
#include <MiniEngine/Rhi/IRhiDevice.h>

namespace MiniEngine::Rhi
{
// 输入来自已完成的 backend readback；移除行 padding，结果只含紧密 RGBA8。
// 本函数不做色调映射、通道重排或 GPU 等待，也不接受截断的行。
TextureReadbackResult NormalizeRgba8Readback(std::span<const std::byte> bytes, std::uint64_t rowPitch, Extent2D extent,
                                             Format format);
// 两点须属于同一帧、同一时钟且有效；允许 validBits 范围内的一次计数回绕。
std::optional<double> TimestampDeltaSeconds(const TimestampResult& begin, const TimestampResult& end);
std::string TimestampResultJson(const std::optional<TimestampResult>& result);
std::string TextureReadbackResultJson(const std::optional<TextureReadbackResult>& result);
} // namespace MiniEngine::Rhi
