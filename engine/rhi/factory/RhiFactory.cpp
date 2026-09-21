#include "D3D11RhiBackend.h"
#include "D3D12RhiBackend.h"
#include "NativeDevice.h"
#include <MiniEngine/Rhi/RhiFactory.h>

namespace MiniEngine::Rhi
{
std::unique_ptr<IRhiDevice> CreateRhiDevice(RhiBackend backend, const RhiDeviceCreateInfo& info)
{
    try
    {
        std::unique_ptr<NativeRhiBackend> native;
        switch (backend)
        {
        case RhiBackend::D3D11:
            if (info.enableGpuValidation)
                throw RhiException(
                    {RhiErrorCode::Unsupported, "CreateRhiDevice", "Device", "", "d3d11",
                     "d3d11 does not support GPU-based validation; requires d3d12; no backend fallback is permitted"});
            native = D3D11::CreateD3D11RhiBackend(info);
            break;
        case RhiBackend::D3D12:
            native = D3D12::CreateD3D12RhiBackend(info);
            break;
        default:
            throw RhiException({RhiErrorCode::Unsupported, "CreateRhiDevice", "Device", "", "",
                                "unsupported backend; allowed values: d3d11, d3d12"});
        }
        if (!native || native->Capabilities().backend != backend)
            throw RhiException({RhiErrorCode::BackendFailure, "CreateRhiDevice", "Device", "",
                                std::string(ToString(backend)), "backend identity mismatch"});
        return std::make_unique<NativeDevice>(std::move(native), info);
    }
    catch (const RhiException&)
    {
        throw;
    }
    catch (const std::bad_alloc&)
    {
        throw RhiException({RhiErrorCode::OutOfMemory, "CreateRhiDevice", "Device", "", std::string(ToString(backend)),
                            "device allocation failed"});
    }
    catch (const std::exception& error)
    {
        throw RhiException({RhiErrorCode::BackendFailure, "CreateRhiDevice", "Device", "",
                            std::string(ToString(backend)), error.what()});
    }
}
PreparedEnvironment PrepareRhiEnvironment(IRhiDevice& device, TextureHandle panorama, std::string_view shaderRoot,
                                          std::uint64_t revision)
{
    auto* native = dynamic_cast<NativeDevice*>(&device);
    if (!native)
        throw std::invalid_argument("environment preparation requires a production RHI device");
    return native->PrepareEnvironment(panorama, shaderRoot, revision);
}
} // namespace MiniEngine::Rhi
