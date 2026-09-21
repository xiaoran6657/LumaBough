// ============================================================================
// D3D12Png.h — 紧密 RGBA8 的 WIC PNG 输出。
// 里程碑：M5-09。
// 职责：复用 M4 编码与哈希口径，输入不含 GPU 类型，失败不发布半成品。
// 关联：engine/rhi/d3d11/src/D3D11Screenshot.cpp。
// ============================================================================
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
namespace MiniEngine::Rhi::D3D12
{
// 原子编码并返回最终 PNG SHA-256；尺寸与载荷不匹配时抛异常。
[[nodiscard]] std::string WritePng(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
                                   const std::filesystem::path& target);
} // namespace MiniEngine::Rhi::D3D12
