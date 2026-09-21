#pragma once
#include <MiniEngine/Rhi/RhiCapabilities.h>
namespace MiniEngine::Rhi::D3D12
{
class D3D12Device;
// 使用已创建 wrapper 的实际模式记录，不接受可伪造的 requested-debug 参数。
RhiCapabilities QueryCapabilities(const D3D12Device& device);
} // namespace MiniEngine::Rhi::D3D12
