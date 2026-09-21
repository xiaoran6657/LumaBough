#include <MiniEngine/Rhi/RhiFactory.h>
namespace MiniEngine::Rhi
{
RhiBackend ParseRhiBackend(std::string_view text)
{
    if (text == "d3d11")
        return RhiBackend::D3D11;
    if (text == "d3d12")
        return RhiBackend::D3D12;
    throw RhiException(
        {RhiErrorCode::InvalidArgument, "ParseRhiBackend", "CLI", "", "",
         "invalid --rhi value '" + std::string(text) + "'; allowed values: d3d11, d3d12 (lowercase only)"});
}
} // namespace MiniEngine::Rhi
