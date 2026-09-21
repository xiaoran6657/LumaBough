// M6-01：只链接公共契约，禁止通过 include 顺序掩盖头文件缺失。
#include <MiniEngine/Rhi/IRhiDevice.h>
#include <MiniEngine/Rhi/RhiFactory.h>

#include <tuple>
#include <type_traits>

using namespace MiniEngine::Rhi;
using Handles =
    std::tuple<BufferHandle, TextureHandle, SamplerHandle, ShaderHandle, ResourceSetLayoutHandle, ResourceSetHandle,
               PipelineLayoutHandle, GraphicsPipelineHandle, SwapChainHandle, TimestampQueryHandle>;

template <class Left, class... Right> consteval bool IsIsolated(std::tuple<Right...>)
{
    return ((std::is_same_v<Left, Right> || !std::is_convertible_v<Left, Right>) && ...);
}

template <class... Types> consteval bool CheckHandles(std::tuple<Types...> values)
{
    return ((sizeof(Types) == 16 && std::is_trivially_copyable_v<Types> && !Types{}.IsValid() &&
             IsIsolated<Types>(values)) &&
            ...);
}
static_assert(CheckHandles(Handles{}));
static_assert(RhiCapabilities::kFormatCount == 8);
static_assert(static_cast<unsigned>(Format::D24UnormS8Uint) + 1 == RhiCapabilities::kFormatCount);
static_assert(std::is_abstract_v<IRhiDevice> && std::is_abstract_v<IRhiCommandList>);
static_assert(std::is_abstract_v<IRhiGraphCommandSink>);
static_assert(!std::is_same_v<VertexFormat, Format>);

int main()
{
    RhiCapabilities caps;
    caps.formatSupport.fill(0xff);
    if (caps.SupportsSampled(Format::Unknown) || caps.SupportsSampled(Format::Count) ||
        caps.SupportsSampled(static_cast<Format>(255)))
    {
        return 1;
    }
    return caps.SupportsSampled(Format::Rgba8Unorm) ? 0 : 2;
}
