#pragma once

// M6-01 公共契约草稿；运行时实现与验证在后续篇目完成。
// 所有调用限渲染线程；句柄不拥有对象，span/string_view 仅在调用期间借用。

#include <MiniEngine/Rhi/IRhiDevice.h>

#include <memory>
#include <string_view>

namespace MiniEngine::Rhi
{
struct RhiDeviceCreateInfo final
{
    // 仅 composition 注入平台窗口；不返回任何图形 native 对象。
    // 窗口由调用者持有，寿命必须覆盖 device 和 swap chain；M6 单窗口。
    void* nativeWindow = nullptr;
    bool enableDebugLayer = false;
    bool enableGpuValidation = false;
    bool useWarp = false;
};

// IBL 属于 revision 边界的资源准备，whole-resource graph 只消费完成后的四组纹理。
// 顺序为 environment / irradiance / prefilter / BRDF LUT。调用者按普通纹理句柄销毁。
// 准备同步完成并校验完整内容后才发布；失败不替换调用者的上一 revision。
struct PreparedEnvironment final
{
    std::array<TextureHandle, 4> textures{};
    std::array<TextureDesc, 4> descriptors{};
    std::uint64_t sourceRevision = 0;
};
PreparedEnvironment PrepareRhiEnvironment(IRhiDevice& device, TextureHandle panorama, std::string_view shaderRoot,
                                          std::uint64_t revision);
// M6-01 仅声明；工厂和 CLI 解析在 M6-06 接线。
RhiBackend ParseRhiBackend(std::string_view text);

std::unique_ptr<IRhiDevice> CreateRhiDevice(RhiBackend backend, const RhiDeviceCreateInfo& createInfo);
} // namespace MiniEngine::Rhi
