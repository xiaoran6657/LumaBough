// ============================================================================
// D3D11IblResources.h — M4-04 split-sum IBL 四资源的生成与 readiness 事务
// 里程碑：M4（04 篇「固定 IBL profile」「Readiness 与失败」；手抄清单第 5 条）
// 职责：从 baked panorama SRV 生成四组 GPU 资源（baseline profile 冻结，不可静默
//       降采样）：
//         Environment Cube   RGBA16F 512²×6  full chain  conversion
//         Diffuse Irradiance RGBA16F 32²×6   1 mip       256/texel（存 irradiance/PI）
//         Prefiltered Spec   RGBA16F 128²×6   full chain  512/texel（mip→roughness）
//         BRDF LUT           RG16F   256²     1 mip       1024/texel
//       事务语义（temporary → validate → commit）：任一 Create/Draw/validation
//       失败即丢弃 temporary，active set 保持可用（reload 失败不破坏旧资源）。
//       每帧只读 active；生成只在 environment revision 变化时发生一次。
// 关联：docs/architecture/README.md「Cube resource」「Readiness 与失败」
//       shaders/d3d11/EquirectToCube.hlsl 等（生成 shader 的 CPU 侧驱动方）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（消费方：State()==Ready 才绑 t5–t7）
// ============================================================================
#pragma once

#include <MiniEngine/Rhi/D3D11/D3D11RenderBindings.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace MiniEngine::Rhi::D3D11
{
// IBL 生成状态机：Empty（无环境）→ Building（temporary 生成中）→ Ready（可消费）；
// 生成失败：无 active 时 Failed、有 active 时回 Ready（reload 失败保留旧资源）。
enum class IblState
{
    Empty,
    Building,
    Ready,
    Failed
};

// 一套完整的 IBL GPU 资源（active 或 temporary）。默认构造 = 空 set（SRV 判空）。
struct D3D11IblSet final
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> environment;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> environmentSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> irradiance;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> irradianceSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> prefiltered;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prefilteredSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> brdfLut;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> brdfLutSrv;
    std::uint32_t prefilterMipCount = 0; // roughness→lod 的运行时换算基准（PbrForward b3）
    std::uint64_t sourceRevision = 0;    // 生成所用的 panorama revision（reload 去重依据）
};

class D3D11IblResources final
{
  public:
    D3D11IblResources() = default;
    ~D3D11IblResources() = default;
    D3D11IblResources(const D3D11IblResources&) = delete;
    D3D11IblResources& operator=(const D3D11IblResources&) = delete;

    // 全量生成到 temporary set：conversion → mip 链 → irradiance → prefilter →
    // LUT → finite 校验。任一步失败返回 false 并写 error（含 stage/face/mip 与
    // FXC 诊断），temporary 已被内部丢弃；调用方保证 active 不受影响。
    // shaderDirectory 是生成 shader 的解析根（与 Renderer 同源）；sourceRevision
    // 仅作为记录与去重依据（调用方负责跳过相同 revision）。
    bool BuildTemporary(ID3D11Device& device, ID3D11DeviceContext& context, ID3D11ShaderResourceView& bakedPanorama,
                        const std::filesystem::path& shaderDirectory, std::uint64_t sourceRevision, std::string& error);

    // 校验通过后整体提交：temporary → active，状态置 Ready（构建期逐帧不可中断，
    // 由调用方在帧边界触发 BuildTemporary 而非渲染中段）。
    void CommitTemporary();

    // 丢弃 temporary；有 active 则回 Ready，否则 Failed（首次失败语义：阻止
    // baseline capture 依赖 IBL 的部分，渲染端绑定 fallback 继续绘制）。
    void DiscardTemporary() noexcept;

    // 显式释放全部资源（Shutdown 顺序契约：先于 device 释放）。
    void Release() noexcept;

    [[nodiscard]] IblState State() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] const D3D11IblSet* Active() const noexcept
    {
        return m_active.environmentSrv == nullptr ? nullptr : &m_active;
    }

    [[nodiscard]] const D3D11IblSet* Temporary() const noexcept
    {
        return m_temporary.environmentSrv == nullptr ? nullptr : &m_temporary;
    }

  private:
    D3D11IblSet m_active;
    D3D11IblSet m_temporary;
    IblState m_state = IblState::Empty;
};
} // namespace MiniEngine::Rhi::D3D11
