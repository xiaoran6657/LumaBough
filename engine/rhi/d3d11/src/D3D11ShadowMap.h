// ============================================================================
// D3D11ShadowMap.h — M4-03 单张方向光 shadow map 的资源与固定状态（内部类）
// 里程碑：M4（03 篇 Directional Shadow）
// 职责：一次创建 03 篇冻结的 shadow 资源组：2048² R32_TYPELESS 纹理（DSV+SRV
//       双绑定）、D32_FLOAT DSV、R32_FLOAT SRV、s2 comparison sampler（LESS_EQUAL、
//       border=1 = volume 外 fully lit）与 shadow rasterizer（depth bias 挡 acne；
//       normal/mirrored 两个正面绕序状态，与主 pass 同款约定；bias 全 0 时直接
//       复用主 pass 状态对象，见 Create 注释）。
//       只创建与持有资源，不做任何 pass 装配——Shadow pass 的绑定/解绑顺序是
//       D3D11Renderer 的职责（hazard 语义在调用侧显式管理）。
// 能力检查口径（M4-03 教训，ADR-0005 决策 3 修订）：typeless 格式的
//       CheckFormatSupport 结果不可靠，必须按用途查带类型视图（D32_FLOAT 查
//       DEPTH_STENCIL、R32_FLOAT 查 SHADER_SAMPLE_COMPARISON）。R32F comparison
//       为可选能力，缺失设备走清晰失败（不偷偷退化）。
// bias 说明：DepthBias=1000 对 D32_FLOAT 是 ULP 单位；SlopeScaledDepthBias=1.5、
//       Clamp=0 为模板起点；03 篇禁止抄"万能值"——最终值必须经
//       acne/peter-panning fixture 校准后写入验收记录（用户操作项）。
// 关联：docs/architecture/README.md「D3D11 resource/views」「Rasterizer bias」
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（shadow pass / forward pass 消费方）
// ============================================================================
#pragma once

#include <MiniEngine/Rhi/D3D11/D3D11RenderBindings.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace MiniEngine::Rhi::D3D11
{
// 单张固定分辨率 shadow map 的全部 GPU 资源。非拷贝、非移动（ComPtr 持有）。
// 创建失败（格式不支持/D3D 调用失败）抛 std::runtime_error，不静默退化。
class D3D11ShadowMap final
{
  public:
    D3D11ShadowMap() = default;
    ~D3D11ShadowMap() = default;
    D3D11ShadowMap(const D3D11ShadowMap&) = delete;
    D3D11ShadowMap& operator=(const D3D11ShadowMap&) = delete;

    // 创建 03 篇冻结的资源组并做 CheckFormatSupport 前置检查。
    //
    // sharedNormal/sharedMirrored 是主 pass 的 rasterizer 状态对象：当 shadow 描述
    // 与其**逐字节相同**（GetDesc + memcmp 校验，而非条件推断）时，D3D11 runtime
    // 会把两次 CreateRasterizerState 去重为同一 COM 对象——对同一对象设置不同长度
    // 的 debug 名会触发 SETPRIVATEDATA_CHANGINGPARAMS WARNING（M4-03 审查定位）。
    // 此时直接共享主 pass 状态对象（语义相同，debug 名沿用主 pass 的唯一命名）；
    // 描述不同时本就无去重撞车，独立创建并命名。
    //
    // 失败：设备不支持 R32_TYPELESS 的 depth-stencil + shader-resource 绑定或任何
    // D3D 调用失败时抛 std::runtime_error（含操作名），不偷偷退化。
    void Create(ID3D11Device& device, ID3D11RasterizerState* sharedNormal = nullptr,
                ID3D11RasterizerState* sharedMirrored = nullptr);

    // shadow pass 视口（0,0,2048,2048,0..1）。
    [[nodiscard]] const D3D11_VIEWPORT& Viewport() const noexcept
    {
        return m_viewport;
    }

    [[nodiscard]] ID3D11DepthStencilView* DepthStencilView() const noexcept
    {
        return m_depthStencilView.Get();
    }

    // forward pass 绑定 t8 用（R32_FLOAT 视图）。
    [[nodiscard]] ID3D11ShaderResourceView* ShaderResourceView() const noexcept
    {
        return m_shaderResourceView.Get();
    }

    // s2 comparison sampler（LESS_EQUAL + border=1）。
    [[nodiscard]] ID3D11SamplerState* ComparisonSampler() const noexcept
    {
        return m_comparisonSampler.Get();
    }

    // shadow pass 的光栅化状态：mirrored=false 用与主 pass 相同的 CCW 正面约定，
    // mirrored=true 用相反绕序（03 篇"mirrored 与 normal object 使用匹配
    // front-face state"——错配会导致 mirrored caster 缺面，属必测故障）。
    [[nodiscard]] ID3D11RasterizerState* Rasterizer(bool mirrored) const noexcept
    {
        return mirrored ? m_mirroredRasterizer.Get() : m_rasterizer.Get();
    }

    // shadow map 边长（PCF texel 步长与 viewport 共用此值）。
    static constexpr std::uint32_t kShadowSize = 2048;

    // 显式释放全部资源（Shutdown 顺序契约：先于 device 释放，live-object 口径）。
    void Release() noexcept
    {
        m_mirroredRasterizer.Reset();
        m_rasterizer.Reset();
        m_comparisonSampler.Reset();
        m_shaderResourceView.Reset();
        m_depthStencilView.Reset();
        m_texture.Reset();
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_comparisonSampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_mirroredRasterizer;
    D3D11_VIEWPORT m_viewport{};
};
} // namespace MiniEngine::Rhi::D3D11
