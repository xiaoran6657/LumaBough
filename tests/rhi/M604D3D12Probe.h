#pragma once
#include "DeviceLifetime.h"
namespace MiniEngine::Rhi::M604
{
#ifndef MINIENGINE_RHI_M606_READBACK_DEFINED
#define MINIENGINE_RHI_M606_READBACK_DEFINED
struct M606Readback final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t completion = 0;
    std::uint64_t timestampBegin = 0;
    std::uint64_t timestampEnd = 0;
    std::uint64_t timestampFrequency = 0;
    bool timestampDisjoint = false;
    std::vector<std::byte> rgba;
};
#endif

class D3D12Probe final
{
  public:
    explicit D3D12Probe(bool warp);
    ~D3D12Probe();
    D3D12Probe(const D3D12Probe&) = delete;
    D3D12Probe& operator=(const D3D12Probe&) = delete;
    std::unique_ptr<ResourcePayload> CreatePipeline(const ShaderDesc& vertex, const ShaderDesc* pixel,
                                                    const GraphicsPipelineDesc& pipeline);
    std::vector<std::byte> DrawToneMap(ResourcePayload& payload);
    // M6-06 固定 clear；返回紧密排列的 RGBA8。
    M606Readback DrawM606Clear(std::uint32_t width, std::uint32_t height);
    // M6-06 固定 Identity 208B + indexed NDC triangle 写 D32，再用 ToneMap 采样。
    M606Readback DrawM606DepthToneMap(ResourcePayload& depthPipeline, ResourcePayload& tonePipeline,
                                      std::uint32_t width, std::uint32_t height, float exposure);
    void ResetM606();
    std::uint64_t NativeCreationCount() const;
    void CheckClean();
    void CheckNoPipelineResources();
    RhiCapabilities Capabilities() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Rhi::M604
