#pragma once

#include "../../src/NativeRhiBackend.h"

#include <memory>

namespace MiniEngine::Rhi::D3D11
{
// 创建严格选定的 D3D11 hardware/WARP adapter；失败不会切换设备或后端。
std::unique_ptr<NativeRhiBackend> CreateD3D11RhiBackend(const RhiDeviceCreateInfo& createInfo);
} // namespace MiniEngine::Rhi::D3D11
