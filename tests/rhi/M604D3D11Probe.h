#pragma once

#include "DeviceLifetime.h"
#include <MiniEngine/Rhi/RhiCapabilities.h>
#include <MiniEngine/Rhi/RhiDescriptors.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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

// M6-04 原生 smoke probe 的 D3D11 侧接口。
// 头文件只暴露 RHI descriptor 与 ResourcePayload；COM/native 类型留在 cpp 的 pimpl 内。
class D3D11Probe final
{
  public:
    explicit D3D11Probe(bool warp);
    ~D3D11Probe();

    D3D11Probe(const D3D11Probe&) = delete;
    D3D11Probe& operator=(const D3D11Probe&) = delete;

    // bytecode 必须由调用方提供离线 DXBC；此入口不编译、不保存借用的 span。
    std::unique_ptr<ResourcePayload> CreatePipeline(const ShaderDesc& vertex, const ShaderDesc* pixel,
                                                    const GraphicsPipelineDesc& pipeline);
    // 仅用于已识别的 ToneMap pipeline；返回紧密排列的 4x4 RGBA8 回读数据。
    std::vector<std::byte> DrawToneMap(ResourcePayload& payload);
    // M6-06 固定输入：只清理 R8G8B8A8 target，返回紧密排列的 RGBA8。
    M606Readback DrawM606Clear(std::uint32_t width, std::uint32_t height);
    // M6-06 固定输入：Identity 208B + indexed NDC triangle 写 D32，再用 ToneMap 采样。
    // depthPipeline 必须由 ShadowDepthVSMain 创建，tonePipeline 必须是 ToneMap pipeline。
    M606Readback DrawM606DepthToneMap(ResourcePayload& depthPipeline, ResourcePayload& tonePipeline,
                                      std::uint32_t width, std::uint32_t height, float exposure);
    void ResetM606();
    // 统计 probe 生命周期内成功的 native 创建调用；DrawToneMap 不应增加该计数。
    std::uint64_t NativeCreationCount() const;
    // 读取并清空 D3D11 InfoQueue；WARNING/ERROR/CORRUPTION 均视为失败。
    void CheckClean();
    void CheckNoPipelineResources();
    // 构造时由真实 device/context 查询并缓存的能力；不会在 DrawToneMap 中查询。
    RhiCapabilities Capabilities() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine::Rhi::M604
