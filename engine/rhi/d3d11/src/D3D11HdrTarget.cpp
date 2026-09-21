// ============================================================================
// D3D11HdrTarget.cpp — scene-linear HDR 渲染目标的实现
// 里程碑：M4（05 篇「HDR target」）
// 职责：按 MakeHdrSceneColorDesc 创建 RGBA16F scene color 的 Texture2D/RTV/SRV，
//       先建后提交（创建失败保留旧资源），并为三个对象设置 05 篇 RenderDoc 检查
//       依赖的调试名。零尺寸请求直接 E_INVALIDARG——窗口最小化/拖拽中间态由
//       渲染器拒绝调用，不在此静默兜底。
// 关联：engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11HdrTarget.h
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（Resize 与 pass 2/3/4 消费方）
// ============================================================================
#include <MiniEngine/Rhi/D3D11/D3D11HdrTarget.h>

#include "D3D11Error.h"

#include <utility>

namespace MiniEngine::Rhi::D3D11
{
HRESULT D3D11HdrTarget::Create(ID3D11Device& device, const std::uint32_t width, const std::uint32_t height)
{
    // 零尺寸是调用方缺陷（05 篇：窗口宽高为 0 时不创建、跳过渲染并等待恢复）。
    if (width == 0 || height == 0)
    {
        return E_INVALIDARG;
    }

    const D3D11_TEXTURE2D_DESC description = MakeHdrSceneColorDesc(width, height);

    // 先建后提交：三个对象全部成功才 move 进成员，失败时旧资源完整保留。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT result = device.CreateTexture2D(&description, nullptr, texture.GetAddressOf());
    if (FAILED(result))
    {
        return result;
    }

    // RTV/SRV 用 nullptr 描述（整资源、mip 0、与纹理同格式），与冻结描述一致。
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView;
    result = device.CreateRenderTargetView(texture.Get(), nullptr, renderTargetView.GetAddressOf());
    if (FAILED(result))
    {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
    result = device.CreateShaderResourceView(texture.Get(), nullptr, shaderResourceView.GetAddressOf());
    if (FAILED(result))
    {
        return result;
    }

    SetDebugObjectName(texture.Get(), "M4.HDR.SceneColor");
    SetDebugObjectName(renderTargetView.Get(), "M4.HDR.SceneColor.RTV");
    SetDebugObjectName(shaderResourceView.Get(), "M4.HDR.SceneColor.SRV");

    m_texture = std::move(texture);
    m_renderTargetView = std::move(renderTargetView);
    m_shaderResourceView = std::move(shaderResourceView);
    m_width = width;
    m_height = height;
    return S_OK;
}

void D3D11HdrTarget::Reset() noexcept
{
    // 释放顺序 SRV → RTV → 纹理：先解引用视图，再释放底层资源。
    m_shaderResourceView.Reset();
    m_renderTargetView.Reset();
    m_texture.Reset();
    m_width = 0;
    m_height = 0;
}
} // namespace MiniEngine::Rhi::D3D11
