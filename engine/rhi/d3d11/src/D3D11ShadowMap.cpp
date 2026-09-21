// ============================================================================
// D3D11ShadowMap.cpp — shadow 资源组创建的实现
// 里程碑：M4（03 篇 Directional Shadow）
// 职责：按 03 篇冻结口径创建 typeless 纹理（DSV+SRV 双绑定）、D32_FLOAT DSV、
//       R32_FLOAT SRV、comparison sampler（border=1）与两个 shadow rasterizer。
//       创建前先 CheckFormatSupport 验证设备能力，不满足即清晰失败——绝不偷偷
//       退化到不可比较格式（comparison 采样在不支持的格式上会静默画错）。
// 关联：D3D11ShadowMap.h（契约与 bias 校准说明）
//       docs/architecture/README.md「D3D11 resource/views」
// ============================================================================
#include "D3D11ShadowMap.h"

#include "D3D11Error.h"

#include <MiniEngine/Core/Log.h>

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D11
{
using Microsoft::WRL::ComPtr;

namespace
{
// 读 bias 校准环境变量（03 篇"Debug UI/CLI 可调"入口）；缺失/非法时用默认值，
// 非法值打日志便于发现手误。golden capture 不设这些变量，走默认固定值。
template <typename T> T ReadBiasEnvironmentVariable(const char* name, const T& defaultValue)
{
    char buffer[64]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length == 0 || length >= sizeof(buffer))
    {
        return defaultValue;
    }
    if constexpr (std::is_same_v<T, INT>)
    {
        try
        {
            return std::stoi(buffer);
        }
        catch (const std::exception&)
        {
            WriteLog(LogLevel::Warning, std::string{"D3D11ShadowMap: invalid "} + name + " value: " + buffer);
            return defaultValue;
        }
    }
    else
    {
        try
        {
            return std::stof(buffer);
        }
        catch (const std::exception&)
        {
            WriteLog(LogLevel::Warning, std::string{"D3D11ShadowMap: invalid "} + name + " value: " + buffer);
            return defaultValue;
        }
    }
}
} // namespace

void D3D11ShadowMap::Create(ID3D11Device& device, ID3D11RasterizerState* sharedNormal,
                            ID3D11RasterizerState* sharedMirrored)
{
    // 能力前置检查（03 篇"不满足时清晰失败，不偷偷退化"）。教训（M4-03，已入
    // ADR-0005 决策 3 修订）：**typeless 格式的 CheckFormatSupport 结果不可靠**
    // （驱动不保证覆盖其带类型视图，本机 R32_TYPELESS 返回 0x1210F0 两位皆缺，
    // 而带类型视图均支持）——必须按用途查带类型格式：DSV 查 D32_FLOAT、
    // comparison 查 R32_FLOAT。R32F 的 comparison 采样在 D3D11 为可选能力，
    // 缺失设备走清晰失败路径；本机（RX 9070/FL 11_1）实测两者均支持。
    // 探针日志为一次性设备能力取证（含 D24 家族对照），默认关闭——设置
    // MINIENGINE_SHADOW_PROBE=1 打开（避免常驻运行路径的 Info 噪声）。
    const struct Probe final
    {
        const char* name;
        DXGI_FORMAT format;
    } kProbes[]{
        {"R32_TYPELESS", DXGI_FORMAT_R32_TYPELESS},
        {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT},
        {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT},
        {"R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS},
        {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT},
        {"R24_UNORM_X8_TYPELESS", DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
    };
    char probeEnabledBuffer[8] = {};
    const bool probeEnabled = GetEnvironmentVariableA("MINIENGINE_SHADOW_PROBE", probeEnabledBuffer,
                                                      static_cast<DWORD>(sizeof(probeEnabledBuffer))) > 0;
    for (const Probe& probe : kProbes)
    {
        if (!probeEnabled)
        {
            continue;
        }
        UINT support = 0;
        if (SUCCEEDED(device.CheckFormatSupport(probe.format, &support)))
        {
            std::ostringstream stream;
            stream << "D3D11ShadowMap probe: " << probe.name << " support=0x" << std::hex << support
                   << " (depthStencil=" << ((support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0U)
                   << ", comparison=" << ((support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON) != 0U) << ")";
            WriteLog(LogLevel::Info, stream.str());
        }
    }

    UINT dsvSupport = 0;
    ThrowIfFailed(device.CheckFormatSupport(DXGI_FORMAT_D32_FLOAT, &dsvSupport),
                  "ID3D11Device::CheckFormatSupport(D32_FLOAT)");
    if ((dsvSupport & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) == 0U)
    {
        std::ostringstream stream;
        stream << "D3D11ShadowMap: device lacks DEPTH_STENCIL support for D32_FLOAT (support=0x" << std::hex
               << dsvSupport << ")";
        throw std::runtime_error{stream.str()};
    }
    UINT srvSupport = 0;
    ThrowIfFailed(device.CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &srvSupport),
                  "ID3D11Device::CheckFormatSupport(R32_FLOAT)");
    if ((srvSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON) == 0U)
    {
        std::ostringstream stream;
        stream << "D3D11ShadowMap: device lacks SHADER_SAMPLE_COMPARISON for R32_FLOAT (support=0x" << std::hex
               << srvSupport << "); comparison shadow mapping is unavailable";
        throw std::runtime_error{stream.str()};
    }

    // typeless 资源（03 篇冻结 R32）：DSV 以 D32_FLOAT 解释（深度写入），SRV 以
    // R32_FLOAT 解释（forward pass 采样）；双绑定格式不匹配是 hazard 的常见来源。
    // 能力检查已按带类型格式验证（见上），typeless 本身不参与能力查询。
    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = kShadowSize;
    textureDescription.Height = kShadowSize;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device.CreateTexture2D(&textureDescription, nullptr, m_texture.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateTexture2D(shadow)");

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
    dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDescription.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ThrowIfFailed(
        device.CreateDepthStencilView(m_texture.Get(), &dsvDescription, m_depthStencilView.ReleaseAndGetAddressOf()),
        "ID3D11Device::CreateDepthStencilView(shadow)");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R32_FLOAT;
    srvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Texture2D.MostDetailedMip = 0;
    srvDescription.Texture2D.MipLevels = 1;
    ThrowIfFailed(device.CreateShaderResourceView(m_texture.Get(), &srvDescription,
                                                  m_shaderResourceView.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateShaderResourceView(shadow)");

    // s2 comparison sampler：LESS_EQUAL + border=(1,1,1,1)——volume 外按 fully lit
    // 处理（与 SampleDirectionalShadow 的 receiver policy 同一语义，双保险）。
    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDescription.BorderColor[0] = 1.0F;
    samplerDescription.BorderColor[1] = 1.0F;
    samplerDescription.BorderColor[2] = 1.0F;
    samplerDescription.BorderColor[3] = 1.0F;
    samplerDescription.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDescription.MinLOD = 0.0F;
    samplerDescription.MaxLOD = 0.0F;
    ThrowIfFailed(device.CreateSamplerState(&samplerDescription, m_comparisonSampler.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateSamplerState(shadow comparison)");

    // shadow rasterizer（独立于主 pass）：depth bias 挡 acne。D32_FLOAT 下
    // DepthBias 按 ULP 计（Bias = DepthBias * 2^(exponent(maxZ) - 23)），
    // 1000 ≈ z=1 附近 [0,1] 深度范围的 6e-5——禁止套用 UNORM 式的 0.005。
    // 校准入口（03 篇"Debug UI/CLI 可调，golden capture 必须使用固定值"）：
    // 环境变量 MINIENGINE_SHADOW_DEPTH_BIAS / MINIENGINE_SHADOW_SLOPE_BIAS /
    // MINIENGINE_SHADOW_BIAS_CLAMP 覆盖默认值，供 03 篇校准流程逐项调节。
    // 校准记录（2026-09-07，box 自阴影判据 + RenderDoc mode8 逐像素取证，
    // 见 out/m4-03-calib/scan-*.rdc）：slope 0→0.1 阴影带（factor 0.29–0.65
    // PCF 渐变）与 lit 区（factor=1，无 acne）均不变；slope≥0.2 开始侵蚀阴影
    // （0.42→0.71）；起点值 1000/1.5 把自阴影完全推掉（factor 全 1，peter-
    // panning 方向）。最终值 DepthBias=0 / SlopeBias=0.1 / Clamp=0；07 篇
    // 固定场景（地面+球阵列）验收时按同流程复核。
    constexpr INT kDefaultDepthBias = 0;
    constexpr FLOAT kDefaultSlopeBias = 0.1F;
    constexpr FLOAT kDefaultBiasClamp = 0.0F;
    const INT depthBias = ReadBiasEnvironmentVariable("MINIENGINE_SHADOW_DEPTH_BIAS", kDefaultDepthBias);
    const FLOAT slopeBias = ReadBiasEnvironmentVariable("MINIENGINE_SHADOW_SLOPE_BIAS", kDefaultSlopeBias);
    const FLOAT biasClamp = ReadBiasEnvironmentVariable("MINIENGINE_SHADOW_BIAS_CLAMP", kDefaultBiasClamp);
    // bias 配置日志按规则 8 收口：默认值（校准终值）时静默——校准会话通过 env
    // 覆盖后才会输出，保证"校准入口生效"的取证可见性同时不产生常态噪音。
    if (depthBias != kDefaultDepthBias || slopeBias != kDefaultSlopeBias || biasClamp != kDefaultBiasClamp)
    {
        std::ostringstream biasStream;
        biasStream << "D3D11ShadowMap: depthBias=" << depthBias << ", slopeScaledDepthBias=" << slopeBias
                   << ", depthBiasClamp=" << biasClamp << " (non-default; calibration override active)";
        WriteLog(LogLevel::Info, biasStream.str());
    }
    // shadow 描述与主 pass rasterizer 的构建（frontCounterClockwise 是唯一分叉字段）。
    const auto makeShadowDescription = [depthBias, slopeBias, biasClamp](const BOOL frontCounterClockwise)
    {
        D3D11_RASTERIZER_DESC description{};
        description.FillMode = D3D11_FILL_SOLID;
        description.CullMode = D3D11_CULL_BACK;
        // 与主 pass 同款正面约定（M3 冻结：烘焙网格 CCW 为正面），保证 shadow
        // pass 剔除的面与 forward pass 一致；mirrored 物体用相反绕序状态。
        description.FrontCounterClockwise = frontCounterClockwise;
        description.DepthClipEnable = TRUE;
        description.DepthBias = depthBias;
        description.SlopeScaledDepthBias = slopeBias;
        description.DepthBiasClamp = biasClamp;
        return description;
    };
    const auto createShadowRasterizer =
        [&device, &makeShadowDescription](const BOOL frontCounterClockwise, ComPtr<ID3D11RasterizerState>& destination)
    {
        const D3D11_RASTERIZER_DESC description = makeShadowDescription(frontCounterClockwise);
        ThrowIfFailed(device.CreateRasterizerState(&description, destination.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateRasterizerState(shadow)");
    };
    // 能否共享主 pass 状态由 runtime 的实际描述决定（GetDesc 逐字节比较），而不是
    // 由"我猜两边描述一样"的条件推断——主 pass 描述日后任何字段变化（CullMode/
    // MultisampleEnable 等）都会让共享自动失效并走独立创建路径（M4-03 审查）。
    // 共享的动机：描述逐字节相同时 D3D11 runtime 会把两次 CreateRasterizerState
    // 去重为同一 COM 对象，之后对同一对象再设不同长度的 debug 名会触发
    // SETPRIVATEDATA_CHANGINGPARAMS WARNING（M4-03 审查定位）；描述不同时本就
    // 无去重撞车，独立创建并命名。D3D11_RASTERIZER_DESC 为 10×4B 无 padding，
    // memcmp 逐字节比较安全。
    const auto canShareRasterizer =
        [&makeShadowDescription](ID3D11RasterizerState* shared, const BOOL frontCounterClockwise)
    {
        if (shared == nullptr)
        {
            return false;
        }
        D3D11_RASTERIZER_DESC mainDescription{};
        shared->GetDesc(&mainDescription); // GetDesc 返回 void，无失败路径
        const D3D11_RASTERIZER_DESC shadowDescription = makeShadowDescription(frontCounterClockwise);
        return std::memcmp(&shadowDescription, &mainDescription, sizeof(D3D11_RASTERIZER_DESC)) == 0;
    };
    if (canShareRasterizer(sharedNormal, TRUE) && canShareRasterizer(sharedMirrored, FALSE))
    {
        m_rasterizer = sharedNormal;
        m_mirroredRasterizer = sharedMirrored;
        WriteLog(LogLevel::Info, "D3D11ShadowMap: descriptions identical; sharing main-pass rasterizer states");
    }
    else
    {
        createShadowRasterizer(TRUE, m_rasterizer);
        createShadowRasterizer(FALSE, m_mirroredRasterizer);
        SetDebugObjectName(m_rasterizer.Get(), "M4.Shadow.Rasterizer");
        SetDebugObjectName(m_mirroredRasterizer.Get(), "M4.Shadow.Rasterizer (mirrored front-face)");
    }

    // shadow pass 视口（与资源同尺寸，深度 0..1）。
    m_viewport = {0.0F, 0.0F, static_cast<float>(kShadowSize), static_cast<float>(kShadowSize), 0.0F, 1.0F};

    // 03 篇要求的可读 debug name（RenderDoc / live-object 报告口径）。
    SetDebugObjectName(m_texture.Get(), "M4.Shadow.Depth");
    SetDebugObjectName(m_depthStencilView.Get(), "M4.Shadow.DSV");
    SetDebugObjectName(m_shaderResourceView.Get(), "M4.Shadow.SRV");
    SetDebugObjectName(m_comparisonSampler.Get(), "M4.Shadow.ComparisonSampler");
}
} // namespace MiniEngine::Rhi::D3D11
