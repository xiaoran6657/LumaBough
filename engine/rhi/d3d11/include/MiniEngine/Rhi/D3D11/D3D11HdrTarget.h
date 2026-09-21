// ============================================================================
// D3D11HdrTarget.h — M4-05 scene-linear HDR 渲染目标与后处理常量
// 里程碑：M4（05 篇「HDR target」「Post constants」；手抄清单第 1 条）
// 职责：持有窗口尺寸的 `R16G16B16A16_FLOAT` scene color 资源（Texture2D + RTV +
//       SRV），并冻结 tone map pass 的常量布局。三条契约：
//   1. 格式与用法冻结：MipLevels=1、ArraySize=1、Sample.Count=1、Usage=DEFAULT、
//      BindFlags=RENDER_TARGET|SHADER_RESOURCE（05 篇「HDR target」表格）。
//      M4 固定无 MSAA——避免引入 Resolve、HDR multisample、post AA 与截图分支。
//   2. 生命周期 = 窗口尺寸：与 main depth/back buffer 同步重建；宽高为 0 时不创建
//      （Resize 中间态由渲染器跳过渲染并等待恢复）。替换语义为"先建后提交"：
//      创建失败保留旧 target，不进入半初始化状态。
//   3. 后处理常量 `PostProcessConstants` 与 shaders/d3d11/ToneMap.hlsl 的 b0
//      逐字节对应；padding 显式清零（RenderDoc 中按 16B 对齐核对）。
// 关联：docs/architecture/README.md
//       shaders/d3d11/ToneMap.hlsl（b0 的对侧声明）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（唯一消费方：pass 2/3/4）
// ============================================================================
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace MiniEngine::Rhi::D3D11
{
// 05 篇冻结的 scene color 格式：16-bit float 有足够动态范围承载 >1 的 radiance，
// 且不需要 MSAA/Resolve 分支；调试名前缀与本文资源命名一致（M4.HDR.*）。
constexpr DXGI_FORMAT kHdrSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// scene color 描述的唯一构造入口：渲染器与单元测试共用同一描述，避免"渲染端改了
// 格式、测试仍冻结旧值"的静默漂移。零尺寸由调用方拒绝（本函数不校验——D3D 会在
// CreateTexture2D 处失败，渲染器按 fatal 处理）。
constexpr D3D11_TEXTURE2D_DESC MakeHdrSceneColorDesc(const std::uint32_t width, const std::uint32_t height)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = kHdrSceneColorFormat;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    return description;
}

// tone map pass 的 b0（05 篇「Post constants」）：16B，与 ToneMap.hlsl 的
// cbuffer ToneMapConstants 逐字段对应。
//   exposureEv         —— 相对曝光控制值，着色器按 exp2(exposureEv) 缩放；
//                         固定基线取 0，范围预算 [-10, +10]（SetExposureEv 钳制）。
//   debugHdr           —— 非 0 时输出 SceneLuminance 假色（判读 HDR 中是否存在 >1
//                         的亮度），此时不做 tone map 与 sRGB encode。
//   inverseOutputSize  —— 1/outputSize（当前未参与逐像素运算，仅作核对锚点；
//                         显式清零写入常量缓冲，禁止未初始化 padding）。
struct alignas(16) PostProcessConstants final
{
    float exposureEv;
    std::uint32_t debugHdr;
    float inverseOutputSize[2];
};
static_assert(sizeof(PostProcessConstants) == 16, "PostProcessConstants must be exactly one 16-byte constant row");
static_assert(alignof(PostProcessConstants) == 16);
static_assert(offsetof(PostProcessConstants, exposureEv) == 0);
static_assert(offsetof(PostProcessConstants, debugHdr) == 4);
static_assert(offsetof(PostProcessConstants, inverseOutputSize) == 8);

// 窗口尺寸 HDR scene color 的资源组。只负责创建/持有/释放，不做任何 pass 装配——
// RTV/SRV hazard（同一资源不得同时绑为 RTV 与 SRV）由渲染器在 pass 边界显式管理。
class D3D11HdrTarget final
{
  public:
    D3D11HdrTarget() = default;
    ~D3D11HdrTarget() = default;
    D3D11HdrTarget(const D3D11HdrTarget&) = delete;
    D3D11HdrTarget& operator=(const D3D11HdrTarget&) = delete;

    // 按给定尺寸创建 scene color 资源组（Texture2D + RTV + SRV）。
    //
    // 替换语义：三个对象全部创建成功后才提交（move 进成员），任一步失败时旧资源
    // 原样保留——Resize 失败不会让渲染器进入"无 HDR target 却继续 Draw"的状态。
    //
    // 调试名：M4.HDR.SceneColor / .RTV / .SRV（05 篇 RenderDoc 检查第 1、4 项）。
    //
    // 参数：
    //   device —— 目标设备
    //   width / height —— 客户区尺寸（像素），必须非零
    // 返回：S_OK 成功；width 或 height 为 0 时 E_INVALIDARG；其余为 D3D 调用失败码。
    [[nodiscard]] HRESULT Create(ID3D11Device& device, std::uint32_t width, std::uint32_t height);

    // 释放全部资源并把尺寸归零（Resize 与 Shutdown 共用）。
    void Reset() noexcept;

    [[nodiscard]] ID3D11Texture2D* Texture() const noexcept
    {
        return m_texture.Get();
    }

    [[nodiscard]] ID3D11RenderTargetView* RenderTargetView() const noexcept
    {
        return m_renderTargetView.Get();
    }

    [[nodiscard]] ID3D11ShaderResourceView* ShaderResourceView() const noexcept
    {
        return m_shaderResourceView.Get();
    }

    [[nodiscard]] std::uint32_t Width() const noexcept
    {
        return m_width;
    }

    [[nodiscard]] std::uint32_t Height() const noexcept
    {
        return m_height;
    }

    // 资源组是否可用（RTV 与 SRV 均已创建）。渲染器据此决定是否跳过本帧渲染。
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_renderTargetView != nullptr && m_shaderResourceView != nullptr;
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};
} // namespace MiniEngine::Rhi::D3D11
