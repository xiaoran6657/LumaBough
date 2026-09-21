#pragma once

// M6-01 公共契约草稿；运行时实现与验证在后续篇目完成。
// 所有调用限渲染线程；句柄不拥有对象，span/string_view 仅在调用期间借用。

#include <MiniEngine/Rhi/RhiHandle.h>
#include <MiniEngine/Rhi/RhiTypes.h>

#include <span>
#include <string_view>

namespace MiniEngine::Rhi
{
struct GraphResourceImport final
{
    TextureHandle texture;
    BufferHandle buffer;
};

struct AccessTransition final
{
    TextureHandle texture;
    BufferHandle buffer;
    ResourceAccess before = ResourceAccess::None;
    ResourceAccess after = ResourceAccess::None;
    ShaderStage stages = ShaderStage::Pixel;
    std::string_view debugName;
};

// texture/buffer 恰好一个有效；只允许图执行器调用，pass 不持有此 sink。
// before/after 是 whole-resource 逻辑访问，由后端解释为解绑或状态转换。

class IRhiGraphCommandSink
{
  public:
    virtual ~IRhiGraphCommandSink() = default;
    // 图执行器在本帧先导入资源，裸 handle 不替代声明。
    virtual void ImportResources(std::span<const GraphResourceImport> resources) = 0;
    // 新 logical owner 只撤销内容定义性，保留 access；必须已 import 且在 rendering 外。
    virtual void ResetTransientContents(std::span<const GraphResourceImport> resources) = 0;
    virtual void ApplyTransitions(std::span<const AccessTransition> transitions) = 0;
};
} // namespace MiniEngine::Rhi
