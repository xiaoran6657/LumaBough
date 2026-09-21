#pragma once
#include <MiniEngine/Render/SmokePasses.h>
#include <MiniEngine/Rhi/IRhiDevice.h>
#include <functional>
#include <map>
#include <optional>

namespace MiniEngine::Sandbox
{
struct SmokeShaders final
{
    Rhi::ShaderDesc toneVertex;
    Rhi::ShaderDesc tonePixel;
    Rhi::ShaderDesc depthVertex;
};
struct SmokeFrameResult final
{
    Rhi::FrameToken frame;
    Rhi::BufferHandle readback;
    Rhi::TimestampQueryHandle beginQuery;
    Rhi::TimestampQueryHandle endQuery;
};
// 这是 adapter 阶梯的手工 executor；pass 只在 MiniEngineRenderer 中使用 command 接口。
// M6-07 的图编译尚未接入，资源导入/转换在此显式声明且两 backend 共享同一路径。
class SmokeScene final
{
  public:
    SmokeScene(Rhi::IRhiDevice& device, const SmokeShaders& shaders, Rhi::Extent2D extent, std::uint32_t level);
    ~SmokeScene();
    SmokeScene(const SmokeScene&) = delete;
    SmokeScene& operator=(const SmokeScene&) = delete;
    SmokeFrameResult Render(Rhi::SwapChainHandle chain, bool capture);
    void Resize(Rhi::Extent2D extent);
    void ReloadExposure(float exposure);
    void ReloadToneShaders(const Rhi::ShaderDesc& vertex, const Rhi::ShaderDesc& pixel);
    Rhi::GraphicsPipelineHandle TonePipeline() const
    {
        return m_tone.pipeline;
    }
    Rhi::TextureHandle Source() const
    {
        return m_source;
    }
    Rhi::ResourceSetHandle ToneSet() const
    {
        return m_tone.sets[0];
    }
    float Exposure() const
    {
        return m_exposure;
    }

  private:
    struct Resources
    {
        explicit Resources(Rhi::IRhiDevice& d) : device(d)
        {
        }
        ~Resources();
        template <class Handle> Handle Own(Handle handle)
        {
            cleanup.emplace_back([this, handle] { device.Destroy(handle); });
            return handle;
        }
        Rhi::IRhiDevice& device;
        std::vector<std::function<void()>> cleanup;
    };
    void Transition(Rhi::IRhiGraphCommandSink& sink, Rhi::TextureHandle handle, Rhi::ResourceAccess after);
    void Transition(Rhi::IRhiGraphCommandSink& sink, Rhi::BufferHandle handle, Rhi::ResourceAccess after);
    void BuildSizeResources();
    void BuildToneSet();
    Rhi::IRhiDevice& m_device;
    Resources m_resources;
    Rhi::Extent2D m_extent;
    const std::uint32_t m_level;
    Render::SmokeBindings m_tone;
    Render::SmokeBindings m_depth;
    Rhi::ResourceSetLayoutHandle m_toneLayout;
    Rhi::GraphicsPipelineDesc m_toneDescriptor;
    Rhi::ShaderHandle m_toneVertex, m_tonePixel;
    Rhi::ResourceSetLayoutHandle m_drawLayout;
    Rhi::TextureHandle m_source;
    Rhi::SamplerHandle m_sampler;
    Rhi::BufferHandle m_constants;
    Rhi::BufferHandle m_vertices;
    Rhi::BufferHandle m_indices;
    std::array<Rhi::BufferHandle, 3> m_readbacks{};
    std::array<Rhi::TimestampQueryHandle, 3> m_beginQueries{}, m_endQueries{};
    std::map<Rhi::TextureHandle, Rhi::ResourceAccess> m_textureAccess;
    std::map<Rhi::BufferHandle, Rhi::ResourceAccess> m_bufferAccess;
    std::array<float, 52> m_object{};
    float m_exposure = 0;
};
} // namespace MiniEngine::Sandbox
