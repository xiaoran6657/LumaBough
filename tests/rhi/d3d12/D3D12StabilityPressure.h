// ============================================================================
// D3D12StabilityPressure.h — M5-10 独立压力序列与证据结构
// 职责：向稳定性 executable 返回真实上传和描述符行为计数。
// ============================================================================
#pragma once
#include <cstdint>
namespace MiniEngine::Rhi::D3D12
{
class D3D12Device;
}
struct D3D12PressureFacts final
{
    std::uint64_t capacity{}, ringAllocations{}, wraps{}, noSpanEvents{}, waits{}, dedicated{}, comparedBytes{};
    std::uint32_t descriptorExhaustions{}, descriptorReclaims{};
};
D3D12PressureFacts RunD3D12Pressure(MiniEngine::Rhi::D3D12::D3D12Device& device);
