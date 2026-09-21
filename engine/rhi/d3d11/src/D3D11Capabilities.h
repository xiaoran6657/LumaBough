#pragma once
#include <MiniEngine/Rhi/RhiCapabilities.h>
#include <cstdint>
struct ID3D11Device;
struct ID3D11DeviceContext;
namespace MiniEngine::Rhi::D3D11
{
// 启动时独占 immediate context；timestamp probe 会 Flush 并有界等待，不能在帧内调用。
RhiCapabilities QueryCapabilities(ID3D11Device& device, ID3D11DeviceContext& context,
                                  std::uint32_t timeoutMilliseconds = 2000);
} // namespace MiniEngine::Rhi::D3D11
