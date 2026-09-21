// ============================================================================
// D3D11Renderer.cpp — 具体 D3D11 渲染器实现
// 里程碑：M2 / M3 / M4-02（RenderPacket 消费 + direct-light PBR）/ M4-05（HDR 与后处理）
// 职责：在 PImpl 内完成设备/调试队列/交换链/管线/资源/每帧绘制与 Resize 的全流程：
//   Debug 设备强制开启且缺失时不静默降级，flip-discard 双缓冲交换链 + VSync，安全顺序
//   Resize，WRITE_DISCARD 上传 b0/b1/b2 常量缓冲，PbrForward 48B 顶点管线，材质语义由
//   `.memat` payload 驱动（t0–t4 缺省槽位绑定真实 fallback SRV），device removal 只
//   诊断不恢复。M4-05 起场景写入 `R16G16B16A16_FLOAT` HDR target，随后 skybox 与
//   tone map 两个 pass 把它变成 UNORM 后备缓冲上的 sRGB 图像（显式编码一次）。
//   所有 D3D11 具体类型仅存在于本实现文件。
// 关联：docs/architecture/DECISIONS.md、docs/architecture/README.md
//       shaders/d3d11/PbrForward.hlsl（常量布局与 slot 绑定的对侧契约）
// ============================================================================
#include <MiniEngine/Rhi/D3D11/D3D11Renderer.h>

#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/PbrVertex.h>
#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D11/D3D11AssetCache.h>
#include <MiniEngine/Rhi/D3D11/D3D11GpuTimer.h>
#include <MiniEngine/Rhi/D3D11/D3D11HdrTarget.h>
#include <MiniEngine/Rhi/D3D11/D3D11IblResources.h>
#include <MiniEngine/Rhi/D3D11/D3D11RenderBindings.h>
#include <MiniEngine/World/RenderPacket.h>
#include <MiniEngine/World/RenderQueueBuilder.h>

#include "D3D11Screenshot.h"
#include "D3D11ShadowMap.h"

#include "D3D11Error.h"
#include "D3D11ShaderCompiler.h"

// 实现文件内允许出现 D3D11/DXGI 具体类型；这些类型不会泄漏到 PUBLIC 头（PImpl 隔离）。
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace MiniEngine
{
namespace
{
using Microsoft::WRL::ComPtr;

// binding table 的枚举与转换函数位于 Rhi::D3D11（01 篇 slot 契约）；
// 本文件在 MiniEngine 命名空间内，统一 using 引入避免散落的限定。
using Rhi::D3D11::ConstantBufferSlot;
using Rhi::D3D11::PbrSrvSlot;
using Rhi::D3D11::SamplerSlot;
using Rhi::D3D11::Slot;

// 每帧上传到 b0 的场景数据，与 PbrForward.hlsl 的 cbuffer FrameConstants 逐字段对应。
// 全部字段 float4：fxc 对 float3/float2 混合字段的隐式打包与 CPU struct 错位
// （M4-03 实测 DebugMode 读到 0 的事故），float4-only 是布局唯一可靠形态（b2 同款）。
struct alignas(16) FrameConstants
{
    DirectX::XMFLOAT4X4 viewProjection;           // 64B（已转置的行主序）
    DirectX::XMFLOAT4 cameraPositionAndDebugMode; // xyz = 相机世界位置，w = debug view 模式
    DirectX::XMFLOAT4 directionAndIntensity;      // xyz = DirectionToLight（L），w = LightIntensity
    DirectX::XMFLOAT4 lightColorAndPadding;       // xyz = 线性 RGB，w = 0
    DirectX::XMFLOAT4 shadowMapSizeAndPadding;    // xy = 分辨率（2048²），zw = 0
};
static_assert(sizeof(FrameConstants) == 128);
static_assert(sizeof(FrameConstants) % 16 == 0);

// 每 Draw 上传到 b1 的对象数据，与 cbuffer ObjectConstants 逐字段对应；
// 与 ShadowDepth.hlsl 的 b1 声明逐字节一致（03 篇硬约束）；float4-only 同上。
struct alignas(16) ObjectConstants
{
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 normalMatrix;              // 3×3 inverse-transpose（非均匀缩放下 ≠ world）
    DirectX::XMFLOAT4X4 lightWorldViewProjection;  // 光空间 VP（固定 shadow volume）
    DirectX::XMFLOAT4 handednessAndReceivesShadow; // x = WorldHandedness，y = ReceivesShadow，zw = 0
};
static_assert(sizeof(ObjectConstants) == 208);
static_assert(sizeof(ObjectConstants) % 16 == 0);

// 每 Draw 上传到 b2 的材质数据：48B（02 篇「Material constants」契约，
// 不用 C++ bool 或未初始化 padding 写常量缓冲）。
struct alignas(16) MaterialConstants
{
    DirectX::XMFLOAT4 baseColorFactor;
    DirectX::XMFLOAT4 emissiveAndMetallic;           // rgb = emissiveFactor，a = metallicFactor
    DirectX::XMFLOAT4 roughnessNormalOcclusionFlags; // x=roughness，y=normalScale，z=occlusion，w=flags(0)
};
static_assert(sizeof(MaterialConstants) == 48);
static_assert(sizeof(MaterialConstants) % 16 == 0);

// 引擎 row-major row-vector Matrix4 → HLSL mul(vector, matrix) 所需的转置行主序。
DirectX::XMFLOAT4X4 TransposedForHlsl(const World::Matrix4& matrix)
{
    const auto& engine = *reinterpret_cast<const DirectX::XMFLOAT4X4*>(matrix.values.data());
    DirectX::XMFLOAT4X4 result{};
    DirectX::XMStoreFloat4x4(&result, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&engine)));
    return result;
}

// 曝光控制值的范围预算（05 篇「手动曝光」）：相对 EV，不宣称完整摄影 EV100。
// 固定 baseline 取 0（场景亮度靠 light/environment 数据校准，不靠后期调曝光）。
constexpr float kMinExposureEv = -10.0F;
constexpr float kMaxExposureEv = 10.0F;

// debug view 模式表（b0 DebugMode）新增 05 篇一项：10 = SceneLuminance 假色。
// 1..9 由 02/03/04 篇冻结（PbrForward.hlsl），10 只在 tone map pass 生效。
constexpr std::uint32_t kDebugModeSceneLuminance = 10;

// skybox 单位立方体：24 顶点（POSITION-only，顶点即方向）+ 36 索引（R16_UINT）。
// 与 D3D11IblResources.cpp 的生成立方体同款数据，但两者生命周期与用途独立
// （生成 pass 在内部、skybox 在渲染器），不共享头文件以免为省几行引入耦合；
// 若将来统一，应抽到公共头并同步两侧绕序。
constexpr std::array<float, 72> kSkyboxVertices{
    // -Z 面
    -1.0F,
    -1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    1.0F,
    1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    // +Z 面
    1.0F,
    -1.0F,
    1.0F,
    1.0F,
    1.0F,
    1.0F,
    -1.0F,
    1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    // +X 面
    1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    1.0F,
    -1.0F,
    1.0F,
    1.0F,
    1.0F,
    1.0F,
    -1.0F,
    1.0F,
    // -X 面
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    1.0F,
    1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    -1.0F,
    -1.0F,
    // +Y 面
    -1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    1.0F,
    1.0F,
    1.0F,
    1.0F,
    1.0F,
    1.0F,
    -1.0F,
    // -Y 面
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    1.0F,
};
constexpr std::array<std::uint16_t, 36> kSkyboxIndices{
    0,  1,  2,  0,  2,  3,  // -Z
    4,  5,  6,  4,  6,  7,  // +Z
    8,  9,  10, 8,  10, 11, // +X
    12, 13, 14, 12, 14, 15, // -X
    16, 17, 18, 16, 18, 19, // +Y
    20, 21, 22, 20, 22, 23, // -Y
};

// skybox 专用 VP：把 view 的平移行清零后再乘 projection（05 篇"去 Camera 平移"）。
// 引擎矩阵是 row-major row-vector：平移位于第 4 行（r[3]），故只清 r[3] 的 xyz。
DirectX::XMFLOAT4X4 ViewProjectionWithoutTranslation(const World::Matrix4& view, const World::Matrix4& projection)
{
    const auto engineView = *reinterpret_cast<const DirectX::XMFLOAT4X4*>(view.values.data());
    const auto engineProjection = *reinterpret_cast<const DirectX::XMFLOAT4X4*>(projection.values.data());

    DirectX::XMMATRIX viewMatrix = DirectX::XMLoadFloat4x4(&engineView);
    viewMatrix.r[3] = DirectX::XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F); // 去掉相机平移

    const DirectX::XMMATRIX result = DirectX::XMMatrixMultiply(viewMatrix, DirectX::XMLoadFloat4x4(&engineProjection));
    DirectX::XMFLOAT4X4 transposed{};
    DirectX::XMStoreFloat4x4(&transposed, DirectX::XMMatrixTranspose(result));
    return transposed;
}

// GPU 标记（ID3DUserDefinedAnnotation）的 RAII 作用域：构造 BeginEvent、析构 EndEvent，
// 异常/早退时也能保证配对，便于在 RenderDoc 中按事件层级阅读。
class AnnotationScope
{
  public:
    AnnotationScope(ID3DUserDefinedAnnotation* annotation, const wchar_t* name) : m_annotation{annotation}
    {
        if (m_annotation != nullptr)
        {
            m_annotation->BeginEvent(name);
        }
    }

    ~AnnotationScope()
    {
        if (m_annotation != nullptr)
        {
            m_annotation->EndEvent();
        }
    }

    AnnotationScope(const AnnotationScope&) = delete;
    AnnotationScope& operator=(const AnnotationScope&) = delete;

  private:
    ID3DUserDefinedAnnotation* m_annotation;
};

// 把 HRESULT 转成大写十六进制字符串，用于错误日志/异常文本。
std::string HResultHex(const HRESULT value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(value);
    return stream.str();
}
} // namespace

class D3D11Renderer::Impl
{
  public:
    // 完成渲染器初始化：先校验窗口/尺寸，再按 设备→调试队列→交换链→管线→尺寸相关资源
    // 的顺序创建全部 D3D 对象，最后清空初始化期产生的调试消息。
    Impl(void* nativeWindow, const std::uint32_t width, const std::uint32_t height,
         std::filesystem::path shaderDirectory, const bool enableDebugLayer)
        : m_window{static_cast<HWND>(nativeWindow)}, m_shaderDirectory{std::move(shaderDirectory)}
    {
        // 校验窗口句柄与初始客户区尺寸，非法输入在创建任何 D3D 对象前拒绝。
        if (m_window == nullptr || IsWindow(m_window) == FALSE)
        {
            throw std::invalid_argument{"D3D11Renderer requires a valid HWND"};
        }

        if (width == 0 || height == 0)
        {
            throw std::invalid_argument{"D3D11Renderer requires a non-zero initial client size"};
        }

        // 初始化顺序：设备 → 调试队列 → 交换链 → 不依赖尺寸的管线资源 → 依赖尺寸的资源。
        CreateDevice(enableDebugLayer);
        ConfigureDebugQueue();
        CreateSwapChain();
        CreatePipelineResources();
        CreateSizeDependentResources(width, height);
        DrainDebugMessages("initialization");
    }

    ~Impl()
    {
        Shutdown();
    }

    // 按新客户区尺寸重建尺寸相关资源；零尺寸或尺寸未变化时忽略，避免无意义重建。
    void Resize(const std::uint32_t width, const std::uint32_t height)
    {
        // 忽略零尺寸（拖拽中间态）与尺寸未变化的重建请求。
        if (width == 0 || height == 0 || (width == m_width && height == m_height))
        {
            return;
        }

        // Resize 顺序契约：先解绑并清理状态，再释放尺寸相关资源，避免它们被引用。
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->ClearState();
        // HDR target 必须在 ResizeBuffers 之前释放：05 篇"old HDR SRV 在新 RTV 创建
        // 前解除"——ClearState 已解绑全部绑定，此处释放资源本身。
        m_hdrTarget.Reset();
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();
        m_depthTexture.Reset();

        // BufferCount=0 表示保持原缓冲数，Format=UNKNOWN 表示保持原格式，只改尺寸。
        const HRESULT result = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        CheckDeviceResult(result, "IDXGISwapChain::ResizeBuffers");

        // 重建后备缓冲 RTV、深度纹理/DSV 与 Viewport。
        CreateSizeDependentResources(width, height);
        DrainDebugMessages("resize");
    }

    // 绘制一帧烘焙场景并呈现；处理遮挡待机、逐 RenderDraw 上传/绑定与 Present。
    // M4-02：直接消费 RenderPacket（mainOpaque 已按 AssetId 稳定排序），材质语义
    // 全部来自 `.memat` payload（b2 常量 + t0–t4 槽位），缺省槽位绑定 fallback SRV。
    [[nodiscard]] bool Render(const World::RenderPacket& packet, const Assets::AssetManager& assets)
    {
        // 上次呈现被遮挡时，用 DXGI_PRESENT_TEST 低频探测恢复；待机时不做完整渲染。
        if (m_occluded)
        {
            const HRESULT testResult = m_swapChain->Present(0, DXGI_PRESENT_TEST);
            if (testResult == DXGI_STATUS_OCCLUDED)
            {
                DrainDebugMessages("occlusion test");
                return false;
            }

            CheckDeviceResult(testResult, "IDXGISwapChain::Present(DXGI_PRESENT_TEST)");
            m_occluded = false;
            DrainDebugMessages("occlusion recovery");
        }

        // P1-2：仅在真正渲染时淘汰已 Removed/Unload 的 GPU entry（map 不得只增不减）。
        // 遮挡待机不做完整渲染，无需维护缓存；条目增多后可改由 CommitReport::removed 精确驱动。
        m_assetCache.PruneStale(assets);

        // 每帧一次的光空间 VP（固定 shadow volume，M4-01 数学）。前置保证：
        // packet 由 BuildRenderPacket 构建（同一 light 输入已在 culling 阶段成功
        // 运行过本函数），此处对退化输入的 throw 属于不可达防御路径。
        m_lightViewProjection = World::BuildLightViewProjection(packet.directionalLight);

        // M4 固定帧序列（01 篇）：Shadow → ForwardOpaqueHDR → Skybox → ToneMap →
        // Screenshot → Present。本步（M4-01）只冻结 pass 边界与显式 state 所有权：
        // shadow map、HDR target、IBL 与 tone map 资源分别在 M4-02..05/07 篇落地，
        // 届时逐 pass 显式设置 RTV/DSV/viewport/状态并在退出时解绑 hazard 资源。
        AnnotationScope frame{m_annotation.Get(), L"Frame"};

        // 08 篇 GPU 计时：whole-frame begin（shadow..tone map 全覆盖）；ring 满时
        // 本帧不计时并计 skipped（benchmark 据此判 BLOCKED，不覆盖 pending 查询）。
        m_gpuFrameActive = m_gpuTimer.BeginFrame(*m_context.Get());
        if (!m_gpuFrameActive)
        {
            ++m_gpuSkippedFrames;
        }

        { // Pass 1/5：ShadowDepth —— 单张 2048² R32_TYPELESS depth-only（03 篇）。
            AnnotationScope shadowPass{m_annotation.Get(), L"M4 Pass 1 ShadowDepth"};
            if (m_gpuFrameActive)
            {
                m_gpuTimer.BeginPass(*m_context.Get(), Rhi::D3D11::GpuPass::Shadow);
            }

            // 入口先解绑 t8（03 篇 hazard 清单：任何 DSV/SRV 同时绑定都是缺陷）。
            ID3D11ShaderResourceView* const nullShadowSrv = nullptr;
            m_context->PSSetShaderResources(Slot(PbrSrvSlot::ShadowMap), 1, &nullShadowSrv);

            // depth-only：无 RTV、PS=null、只绑 DSV；2048² 视口；clear=1（越远越大）。
            // 不继承上一帧 ToneMap/IBL 的 depth-disabled 状态；每帧显式恢复深度写入。
            m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
            m_context->OMSetRenderTargets(0, nullptr, m_shadowMap.DepthStencilView());
            m_context->RSSetViewports(1, &m_shadowMap.Viewport());
            m_context->ClearDepthStencilView(m_shadowMap.DepthStencilView(), D3D11_CLEAR_DEPTH, 1.0F, 0);

            constexpr UINT shadowStride = sizeof(Assets::PbrVertex); // POSITION-only layout，stride 仍 48
            constexpr UINT shadowOffset = 0;
            m_context->IASetInputLayout(m_shadowInputLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_shadowVertexShader.Get(), nullptr, 0);
            m_context->PSSetShader(nullptr, nullptr, 0); // opaque depth-only：无 PS
            ID3D11Buffer* const shadowObjectBuffer = m_objectConstantBuffer.Get();
            m_context->VSSetConstantBuffers(Slot(ConstantBufferSlot::Object), 1, &shadowObjectBuffer);

            // 逐 shadow caster：packet.shadowCasters 已由 BuildRenderPacket 完成光视锥
            // culling 与 castsShadow 过滤；不绑材质 SRV（03 篇）。
            for (const World::RenderDraw& draw : packet.shadowCasters)
            {
                if (!m_assetCache.EnsureUploaded(*m_device.Get(), assets, draw.mesh))
                {
                    continue; // stale/不可上传：跳过本帧
                }
                const auto meshView = m_assetCache.TryGetUploadedView(draw.mesh);
                if (!meshView.has_value())
                {
                    continue;
                }

                UpdateObjectConstants(draw); // b1 携带 LightWorldViewProjection
                m_context->RSSetState(m_shadowMap.Rasterizer(draw.mirrored));

                m_context->IASetVertexBuffers(0, 1, &meshView->vertexBuffer, &shadowStride, &shadowOffset);
                m_context->IASetIndexBuffer(meshView->indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                m_context->DrawIndexed(meshView->indexCount, 0, 0);
            }

            // 结束：先解绑 DSV，forward pass 才能把同一资源绑为 SRV（hazard 规则）。
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            if (m_gpuFrameActive)
            {
                m_gpuTimer.EndPass(*m_context.Get(), Rhi::D3D11::GpuPass::Shadow);
            }
        }

        { // Pass 2/5：ForwardOpaqueHDR —— 线性 radiance 写入 RGBA16F scene target（05 篇）。
            AnnotationScope opaquePass{m_annotation.Get(), L"M4 Pass 2 ForwardOpaqueHdr"};

            // HDR target 的不变量：CreateSizeDependentResources 用 ThrowIfFailed 创建，
            // Resize 忽略零尺寸（拖拽/最小化中间态保留旧尺寸），因此此处必然可用——
            // 与后备缓冲 RTV/DSV 同款"创建期失败即抛出"口径，不做静默半初始化渲染。

            // 绑定 HDR RTV + 主深度 DSV 与视口；HDR clear 恒为 (0,0,0,0)——
            // 显示背景色由 skybox pass 提供（背景也属于场景 radiance，不能在
            // HDR 阶段预置"已经像显示色"的值，否则 tone map 的输入不再纯线性）。
            constexpr float clearColor[4] = {0.0F, 0.0F, 0.0F, 0.0F};
            ID3D11RenderTargetView* const hdrRtv = m_hdrTarget.RenderTargetView();
            m_context->OMSetRenderTargets(1, &hdrRtv, m_depthStencilView.Get());
            if (m_gpuFrameActive)
            {
                m_gpuTimer.BeginPass(*m_context.Get(), Rhi::D3D11::GpuPass::OpaquePbr);
            }
            m_context->RSSetViewports(1, &m_viewport);
            m_context->ClearRenderTargetView(hdrRtv, clearColor);
            m_context->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0F,
                                             0);

            // 装配共享阶段：输入布局、三角形列表拓扑、VS/PS/常量缓冲、采样器与深度状态。
            // stride 48 = PbrVertex（.memesh v2 磁盘契约与 input layout 的共同真源）。
            constexpr UINT stride = sizeof(Assets::PbrVertex);
            constexpr UINT offset = 0;
            m_context->IASetInputLayout(m_inputLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
            m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

            // 常量缓冲按 binding table（D3D11RenderBindings）挂槽：b0 frame、b1 object、
            // b2 material；VS 需要 b0/b1，PS 需要 b0/b1/b2。
            ID3D11Buffer* const frameBuffer = m_frameConstantBuffer.Get();
            ID3D11Buffer* const objectBuffer = m_objectConstantBuffer.Get();
            ID3D11Buffer* const materialBuffer = m_materialConstantBuffer.Get();
            m_context->VSSetConstantBuffers(Slot(ConstantBufferSlot::Frame), 1, &frameBuffer);
            m_context->VSSetConstantBuffers(Slot(ConstantBufferSlot::Object), 1, &objectBuffer);
            m_context->PSSetConstantBuffers(Slot(ConstantBufferSlot::Frame), 1, &frameBuffer);
            m_context->PSSetConstantBuffers(Slot(ConstantBufferSlot::Object), 1, &objectBuffer);
            m_context->PSSetConstantBuffers(Slot(ConstantBufferSlot::Material), 1, &materialBuffer);
            ID3D11Buffer* const iblBuffer = m_iblConstantBuffer.Get();
            m_context->PSSetConstantBuffers(Slot(ConstantBufferSlot::Ibl), 1, &iblBuffer);
            m_context->PSSetSamplers(Slot(SamplerSlot::Material), 1, m_samplerState.GetAddressOf());
            m_context->PSSetSamplers(Slot(SamplerSlot::IblAndPost), 1, m_iblSamplerState.GetAddressOf());
            ID3D11SamplerState* const shadowSampler = m_shadowMap.ComparisonSampler();
            m_context->PSSetSamplers(Slot(SamplerSlot::ShadowComparison), 1, &shadowSampler);
            m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);

            // t8 shadow SRV：shadow pass 已结束并解绑 DSV（hazard 规则），可安全绑定。
            ID3D11ShaderResourceView* const shadowSrv = m_shadowMap.ShaderResourceView();
            m_context->PSSetShaderResources(Slot(PbrSrvSlot::ShadowMap), 1, &shadowSrv);

            // t5–t7 IBL SRV：active set 就绪用真实资源，否则 fallback（黑 cube ×2 +
            // 中性 LUT，indirect 退化为 0）。generation 是一次性阶段（revision 变化时
            // 在帧边界执行），steady-state 每帧只有绑定与 b3 上传。
            const Rhi::D3D11::D3D11IblSet* ibl = m_iblResources.Active();
            ID3D11ShaderResourceView* irradianceSrv =
                ibl != nullptr ? ibl->irradianceSrv.Get()
                               : m_fallbackIblSrvs[Slot(PbrSrvSlot::DiffuseIrradiance)].Get();
            ID3D11ShaderResourceView* prefilteredSrv =
                ibl != nullptr ? ibl->prefilteredSrv.Get()
                               : m_fallbackIblSrvs[Slot(PbrSrvSlot::PrefilteredSpecular)].Get();
            ID3D11ShaderResourceView* brdfLutSrv =
                ibl != nullptr ? ibl->brdfLutSrv.Get() : m_fallbackIblSrvs[Slot(PbrSrvSlot::BrdfLut)].Get();
            ID3D11ShaderResourceView* const iblSrvs[3]{irradianceSrv, prefilteredSrv, brdfLutSrv};
            m_context->PSSetShaderResources(Slot(PbrSrvSlot::DiffuseIrradiance), 3, iblSrvs);
            UpdateIblConstants(ibl != nullptr ? ibl->prefilterMipCount : 1U);

            // 每帧一次的 b0：相机 VP、相机位置、固定方向光与 debug 模式。
            UpdateFrameConstants(packet);

            // 逐 RenderDraw：上传（revision 驱动，no-op 优先）→ 解析材质 → 绑定
            // VB/IB/t0–t4 → 写 b1/b2 → 单次 DrawIndexed。
            for (const World::RenderDraw& draw : packet.mainOpaque)
            {
                if (!m_assetCache.EnsureUploaded(*m_device.Get(), assets, draw.mesh))
                {
                    continue; // stale/不可上传：跳过本帧
                }
                const auto meshView = m_assetCache.TryGetUploadedView(draw.mesh);
                if (!meshView.has_value())
                {
                    continue;
                }

                // 材质 payload：Handle 失效（空槽/热重载竞态）时退回默认材质继续绘制，
                // 不因单物体材质缺失丢整个 draw（失败语义 = 保守可视化而非崩溃）。
                static const Assets::MaterialAsset kDefaultMaterial{};
                const Assets::MaterialAsset* material = &kDefaultMaterial;
                if (draw.material.IsValid())
                {
                    const auto materialView = assets.Materials().TryGet(draw.material);
                    if (materialView.has_value())
                    {
                        material = &*materialView->asset;
                    }
                }

                // t0–t4 五个贴图角色槽位：材质引用的 AssetId 逐槽上传并取 SRV，
                // 未引用/失效/占位的槽位绑定真实 fallback SRV（禁止留空残留）。
                // t8 由 pass 级绑定（本循环之后仍保持），因此这里只绑 t0–t4。
                ID3D11ShaderResourceView* srvs[Slot(PbrSrvSlot::Emissive) + 1]{};
                srvs[Slot(PbrSrvSlot::BaseColor)] = m_fallbackSrvs[Slot(PbrSrvSlot::BaseColor)].Get();
                srvs[Slot(PbrSrvSlot::Normal)] = m_fallbackSrvs[Slot(PbrSrvSlot::Normal)].Get();
                srvs[Slot(PbrSrvSlot::MetallicRoughness)] = m_fallbackSrvs[Slot(PbrSrvSlot::MetallicRoughness)].Get();
                srvs[Slot(PbrSrvSlot::Occlusion)] = m_fallbackSrvs[Slot(PbrSrvSlot::Occlusion)].Get();
                srvs[Slot(PbrSrvSlot::Emissive)] = m_fallbackSrvs[Slot(PbrSrvSlot::Emissive)].Get();

                const Assets::AssetId* const slotIds[Slot(PbrSrvSlot::Emissive) + 1]{
                    &material->baseColorTexture, &material->normalTexture, &material->metallicRoughnessTexture,
                    &material->occlusionTexture, &material->emissiveTexture};
                for (std::size_t slot = 0; slot < 5; ++slot)
                {
                    // `.memat` 只引用 AssetId：先在纹理池解析 Handle，再走 revision 驱动上传。
                    const Assets::AssetId& id = *slotIds[slot];
                    const auto textureHandle = id.IsValid() ? assets.Textures().TryFind(id) : std::nullopt;
                    if (!textureHandle.has_value() ||
                        !m_assetCache.EnsureTextureUploaded(*m_device.Get(), assets, *textureHandle))
                    {
                        continue; // 保持 fallback SRV
                    }
                    const auto texView = m_assetCache.TryGetTextureView(*textureHandle);
                    if (texView.has_value() && texView->srv != nullptr)
                    {
                        srvs[slot] = texView->srv;
                    }
                }

                UpdateObjectConstants(draw);
                UpdateMaterialConstants(*material);
                m_context->RSSetState(draw.mirrored ? m_mirroredRasterizerState.Get() : m_rasterizerState.Get());

                m_context->IASetVertexBuffers(0, 1, &meshView->vertexBuffer, &stride, &offset);
                m_context->IASetIndexBuffer(meshView->indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                m_context->PSSetShaderResources(0, Slot(PbrSrvSlot::Emissive) + 1, srvs); // 仅 t0–t4
                m_context->DrawIndexed(meshView->indexCount, 0, 0);
            }

            if (m_gpuFrameActive)
            {
                m_gpuTimer.EndPass(*m_context.Get(), Rhi::D3D11::GpuPass::OpaquePbr);
            }
        }

        { // Pass 3/5：Skybox —— 环境 cubemap 写入同一 HDR target（05 篇「PBR 和 skybox」）。
            AnnotationScope skyboxPass{m_annotation.Get(), L"M4 Pass 3 Skybox"};
            if (m_gpuFrameActive)
            {
                m_gpuTimer.BeginPass(*m_context.Get(), Rhi::D3D11::GpuPass::Skybox);
            }

            const Rhi::D3D11::D3D11IblSet* const ibl = m_iblResources.Active();
            if (ibl != nullptr && ibl->environmentSrv != nullptr)
            {
                // 与 forward 同一个 HDR RTV + 主深度 DSV：skybox 是场景 radiance 的
                // 一部分，必须与几何一起进入 tone map（05 篇 RenderDoc 检查第 3 项）。
                ID3D11RenderTargetView* const hdrRtv = m_hdrTarget.RenderTargetView();
                m_context->OMSetRenderTargets(1, &hdrRtv, m_depthStencilView.Get());
                m_context->RSSetViewports(1, &m_viewport);

                // 远深度 + 深度只读：skybox 只在无几何覆盖（depth==1）的像素通过，
                // 且不写深度（否则后续 tone map 与下一帧的深度语义被污染）。
                // CULL_NONE：相机位于立方体内部，绕序无关且必须画到内表面。
                m_context->RSSetState(m_noCullRasterizerState.Get());
                m_context->OMSetDepthStencilState(m_skyboxDepthState.Get(), 0);

                m_context->IASetInputLayout(m_skyboxInputLayout.Get());
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                constexpr UINT skyboxStride = sizeof(float) * 3U; // POSITION-only
                constexpr UINT skyboxOffset = 0;
                m_context->IASetVertexBuffers(0, 1, m_skyboxVertexBuffer.GetAddressOf(), &skyboxStride, &skyboxOffset);
                m_context->IASetIndexBuffer(m_skyboxIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
                m_context->VSSetShader(m_skyboxVertexShader.Get(), nullptr, 0);
                m_context->PSSetShader(m_skyboxPixelShader.Get(), nullptr, 0);

                // b0 = 去平移 VP；t0 = environment cube；s1 = 线性 clamp（与 IBL 同槽位）。
                ID3D11Buffer* const skyboxConstants = m_skyboxConstantBuffer.Get();
                m_context->VSSetConstantBuffers(Slot(ConstantBufferSlot::Frame), 1, &skyboxConstants);
                UpdateSkyboxConstants(packet);
                ID3D11ShaderResourceView* const environmentSrv = ibl->environmentSrv.Get();
                m_context->PSSetShaderResources(Slot(PbrSrvSlot::BaseColor), 1, &environmentSrv);
                m_context->PSSetSamplers(Slot(SamplerSlot::IblAndPost), 1, m_iblSamplerState.GetAddressOf());

                m_context->DrawIndexed(static_cast<UINT>(kSkyboxIndices.size()), 0, 0);
            }
            // 无环境（IblState 非 Ready）时跳过：HDR 保持 clear 值，tone map 后为黑。
            if (m_gpuFrameActive)
            {
                m_gpuTimer.EndPass(*m_context.Get(), Rhi::D3D11::GpuPass::Skybox);
            }
        }

        { // Pass 4/5：ToneMap —— HDR SRV → 手动曝光 + Reinhard → 显式 sRGB → UNORM 后备缓冲。
            AnnotationScope toneMapPass{m_annotation.Get(), L"M4 Pass 4 ToneMap"};
            if (m_gpuFrameActive)
            {
                m_gpuTimer.BeginPass(*m_context.Get(), Rhi::D3D11::GpuPass::ToneMap);
            }

            // 入口先换绑后备缓冲 RTV 并解绑 DSV：HDR SRV 与 HDR RTV 不得同时绑定
            // （05 篇 RenderDoc 检查第 7 项）。深度关闭、混合关闭、剔除关闭。
            ID3D11RenderTargetView* const backBufferRtv = m_renderTargetView.Get();
            m_context->OMSetRenderTargets(1, &backBufferRtv, nullptr);
            m_context->RSSetViewports(1, &m_viewport);
            m_context->OMSetDepthStencilState(m_postDepthState.Get(), 0);
            m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            m_context->RSSetState(m_noCullRasterizerState.Get());

            // 无 VB 全屏三角形：IA 不需要顶点/索引缓冲与输入布局（SV_VertexID）。
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_toneMapVertexShader.Get(), nullptr, 0);
            m_context->PSSetShader(m_toneMapPixelShader.Get(), nullptr, 0);

            // b0 = PostProcessConstants（覆盖 forward 的 frame 常量——tone map 的
            // VS/PS 只声明自己的 b0，不读 frame 常量）；t0 = HDR SRV；s1 = 线性 clamp。
            ID3D11Buffer* const postBuffer = m_postConstantBuffer.Get();
            m_context->PSSetConstantBuffers(Slot(ConstantBufferSlot::Frame), 1, &postBuffer);
            UpdatePostConstants();
            ID3D11ShaderResourceView* const hdrSrv = m_hdrTarget.ShaderResourceView();
            m_context->PSSetShaderResources(Slot(PbrSrvSlot::BaseColor), 1, &hdrSrv);
            m_context->PSSetSamplers(Slot(SamplerSlot::IblAndPost), 1, m_iblSamplerState.GetAddressOf());

            m_context->Draw(3, 0);

            // 结束立刻解绑 t0：下一帧 pass 2 会把同一资源重新绑为 RTV，
            // 不能依赖"帧末统一解绑"这一层保险之外的残留绑定。
            ID3D11ShaderResourceView* const nullSrv = nullptr;
            m_context->PSSetShaderResources(Slot(PbrSrvSlot::BaseColor), 1, &nullSrv);
            if (m_gpuFrameActive)
            {
                m_gpuTimer.EndPass(*m_context.Get(), Rhi::D3D11::GpuPass::ToneMap);
            }
        }

        // 08 篇：whole-frame end（在截图入队之前——GPU 帧覆盖到 tone map 为止）。
        if (m_gpuFrameActive)
        {
            m_gpuTimer.EndFrame(*m_context.Get());
        }

        // Pass 5/5：ScreenshotCopy —— 占位（M4-07 接入 staging readback ring；禁止每帧 Flush）。

        // 帧末显式解除本帧使用过的 PS SRV 槽位（t0..t8），避免跨帧残留绑定；
        // 01 篇契约：不用 ClearState 掩盖 state 泄漏，只解绑实际用过的范围。
        std::array<ID3D11ShaderResourceView*, 9> nullSrvs{};
        m_context->PSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());

        // 07 篇截图：tone-map 已写完后备缓冲，在 Present 前复制进 staging ring。
        EnqueuePendingScreenshot();

        // vsync 默认 1 锁定 60Hz 教学节奏；0 用于 capture/RenderDoc（07 篇 CLI --vsync 0）。
        // 08 篇 presentCpu：CPU 在 Present 中的耗时（阻塞于节流时反映的是节流，
        // 不等于 GPU frame——报告侧与 GPU sample 分开统计）。
        LARGE_INTEGER presentBegin{};
        QueryPerformanceCounter(&presentBegin);
        const HRESULT presentResult = m_swapChain->Present(m_presentInterval, 0);
        if (presentResult == DXGI_STATUS_OCCLUDED)
        {
            m_occluded = true;
            DrainDebugMessages("present occluded");
            return false;
        }

        LARGE_INTEGER presentEnd{};
        QueryPerformanceCounter(&presentEnd);
        m_presentCpuMicroseconds = static_cast<double>(presentEnd.QuadPart - presentBegin.QuadPart) * 1.0e6 /
                                   static_cast<double>(m_qpcFrequency);

        CheckDeviceResult(presentResult, "IDXGISwapChain::Present");
        PollCompletedScreenshot(); // Present 之后轮询：GPU 大概率已完成 1-2 帧前的拷贝
        PollGpuSampleInternal();
        DrainDebugMessages("present");
        return true;
    }

    // 08 篇：每帧排空所有已完成的 GPU sample（PRESENT 之后执行；排空最多 8 个 slot，
    // 每个都会触发一次轻量 flush——发生在帧管线路径之外，不干扰被测 pass）。
    void PollGpuSampleInternal()
    {
        for (std::uint32_t drained = 0; drained < Rhi::D3D11::D3D11GpuTimer::kQueryFrameCount; ++drained)
        {
            std::optional<Rhi::D3D11::GpuFrameTiming> sample = m_gpuTimer.PollOldest(*m_context.Get());
            if (!sample.has_value())
            {
                break; // 最老 slot 尚未就绪：本帧停止排空（绝不 busy-wait）
            }

            if (sample->disjoint)
            {
                ++m_gpuCounters.disjoint;
            }
            else if (sample->valid)
            {
                ++m_gpuCounters.valid;
                m_gpuSample = *sample;
                m_gpuSampleReady = true;
            }
            else
            {
                ++m_gpuCounters.missing; // FAILED（HRESULT 在 timer 内）或缺时间戳
            }
        }
        m_gpuCounters.missingTimestamps = m_gpuTimer.MissingTimestampCount();
        m_gpuCounters.lastHresult = m_gpuTimer.LastQueryHresult();
    }

    // 校准/RenderDoc 取证的 debug view 切换入口（b0 DebugMode）。
    void SetDebugMode(const std::uint32_t debugMode) noexcept
    {
        m_debugMode = debugMode;
    }

    // ---- 07 篇截图与基线采集 ----

    void RequestScreenshot(const std::filesystem::path& pngPath) noexcept
    {
        m_pendingScreenshotPath = pngPath;
    }

    // 轮询并取走完成结果（取走后 m_screenshotCompleted 复位，下一次 Poll 返回 false）。
    bool PollScreenshot(ScreenshotResult& out) noexcept
    {
        if (!m_screenshotCompleted)
        {
            return false;
        }
        out.pngPath = m_completedScreenshot.pngPath;
        out.width = m_completedScreenshot.width;
        out.height = m_completedScreenshot.height;
        out.pngSha256 = m_completedScreenshot.pngSha256;
        m_screenshotCompleted = false;
        return true;
    }

    [[nodiscard]] bool IblReady() const noexcept
    {
        return m_iblResources.State() == Rhi::D3D11::IblState::Ready;
    }

    void SetPresentInterval(const std::uint32_t interval) noexcept
    {
        m_presentInterval = interval;
    }

    [[nodiscard]] std::string GpuDescription() const noexcept
    {
        return m_gpuDescription;
    }

    [[nodiscard]] std::uint32_t DebugLayerMessageCount() const noexcept
    {
        return m_debugLayerMessageCount;
    }

    // ---- 08 篇性能基线 ----

    [[nodiscard]] GpuInfo GetGpuInfo() const noexcept
    {
        return m_gpuInfo;
    }

    [[nodiscard]] double LastPresentCpuMicroseconds() const noexcept
    {
        return m_presentCpuMicroseconds;
    }

    bool PollGpuSample(Rhi::D3D11::GpuFrameTiming& out) noexcept
    {
        if (!m_gpuSampleReady)
        {
            return false;
        }
        out = m_gpuSample;
        m_gpuSampleReady = false;
        return true;
    }

    [[nodiscard]] Rhi::D3D11::GpuSampleCounters GpuSampleCounters() const noexcept
    {
        return m_gpuCounters;
    }

    [[nodiscard]] std::uint32_t GpuSkippedFrameCount() const noexcept
    {
        return m_gpuSkippedFrames;
    }

    // 固定曝光（05 篇「手动曝光」）：相对 EV，钳到 [-10,+10] 的预算范围。
    // 基线固定 0（场景亮度靠 light/environment 校准），fixed screenshot 禁止
    // 键盘临时值——本入口只用于校准与性能报告记录的实际值。
    void SetExposureEv(const float exposureEv) noexcept
    {
        m_exposureEv = std::clamp(exposureEv, kMinExposureEv, kMaxExposureEv);
    }

    [[nodiscard]] float ExposureEv() const noexcept
    {
        return m_exposureEv;
    }

    [[nodiscard]] std::uint32_t Width() const noexcept
    {
        return m_width;
    }

    [[nodiscard]] std::uint32_t Height() const noexcept
    {
        return m_height;
    }

  private:
    // 创建设备与即时上下文：BGRA 支持、功能级别回退、Debug 层缺失即失败，并查询调试接口。
    void CreateDevice(const bool enableDebugLayer)
    {
        // BGRA 支持用于交换链/D2D 互操作；Debug 构建额外启用调试层，且不静默降级。
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (enableDebugLayer)
        {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        }

        // 优先请求 11_1，旧 runtime 不识别时回退到 11_0。
        constexpr std::array requestedLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

        HRESULT result =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, requestedLevels.data(),
                              static_cast<UINT>(requestedLevels.size()), D3D11_SDK_VERSION,
                              m_device.ReleaseAndGetAddressOf(), &m_featureLevel, m_context.ReleaseAndGetAddressOf());

        // 部分旧 runtime 不支持含 11_1 的数组（返回 E_INVALIDARG），只用 11_0 重试。
        if (result == E_INVALIDARG)
        {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &requestedLevels[1], 1,
                                       D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(), &m_featureLevel,
                                       m_context.ReleaseAndGetAddressOf());
        }

        // Debug 构建缺调试层组件时明确失败并提示安装 Graphics Tools，绝不静默回退。
        if (enableDebugLayer && result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            throw std::runtime_error{
                "D3D11 Debug Layer is unavailable. Install the Windows Graphics Tools optional feature."};
        }

        ThrowIfFailed(result, "D3D11CreateDevice");

        // 实际功能级别低于 11_0 时不接受。
        if (m_featureLevel < D3D_FEATURE_LEVEL_11_0)
        {
            throw std::runtime_error{"D3D feature level 11_0 or newer is required"};
        }

        // 仅 Debug 设备才有 InfoQueue 与 ID3D11Debug，用于消息读取与 live-object 报告。
        if (enableDebugLayer)
        {
            ThrowIfFailed(m_device.As(&m_infoQueue), "Query ID3D11InfoQueue");
            ThrowIfFailed(m_device.As(&m_debug), "Query ID3D11Debug");
        }

        // GPU 标记接口来自设备上下文，Debug/Release 均可用。
        ThrowIfFailed(m_context.As(&m_annotation), "Query ID3DUserDefinedAnnotation");
    }

    // 配置 InfoQueue：错误级到达即断点，丢弃 INFO/MESSAGE 只保留 WARNING 及以上供读取。
    void ConfigureDebugQueue()
    {
        // 非 Debug 设备没有 InfoQueue，跳过配置。
        if (m_infoQueue == nullptr)
        {
            return;
        }

        // 严重错误与错误级别到达即触发断点，便于立即定位；警告级别不打断。
        ThrowIfFailed(m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE),
                      "InfoQueue::SetBreakOnSeverity(CORRUPTION)");
        ThrowIfFailed(m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE),
                      "InfoQueue::SetBreakOnSeverity(ERROR)");

        // 存储过滤丢弃 INFO 与 MESSAGE，只保留 WARNING/ERROR/CORRUPTION 供 Drain 读取。
        std::array deniedSeverities{D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
        D3D11_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = static_cast<UINT>(deniedSeverities.size());
        filter.DenyList.pSeverityList = deniedSeverities.data();
        ThrowIfFailed(m_infoQueue->AddStorageFilterEntries(&filter), "InfoQueue::AddStorageFilterEntries");
    }

    // 创建 flip-discard 双缓冲交换链：上溯获取工厂、填充描述、为窗口建链并禁 Alt+Enter。
    void CreateSwapChain()
    {
        // 沿 device → IDXGIDevice → adapter → IDXGIFactory2 上溯，拿到创建交换链所需的工厂。
        ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(m_device.As(&dxgiDevice), "Query IDXGIDevice");

        ComPtr<IDXGIAdapter> adapter;
        ThrowIfFailed(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf()), "IDXGIDevice::GetAdapter");

        // 诊断日志：适配器与 feature level 是 shadow 能力定性（R32_TYPELESS
        // comparison 采样在 11_0 为可选、11_1 起强制）的关键输入（M4-03）。
        DXGI_ADAPTER_DESC adapterDescription{};
        ThrowIfFailed(adapter->GetDesc(&adapterDescription), "IDXGIAdapter::GetDesc");
        {
            std::ostringstream adapterStream;
            for (const wchar_t character : adapterDescription.Description)
            {
                if (character == L'\0')
                {
                    break;
                }
                adapterStream << static_cast<char>(character); // 适配器描述仅用于稳定身份文本
            }
            const std::string adapterName = adapterStream.str();
            std::ostringstream stream;
            stream << "D3D11 device: featureLevel=0x" << std::hex << m_featureLevel << ", adapter=" << adapterName;
            // benchmark 的 adapter 使用原生 DXGI description，避免混入诊断前缀。
            m_gpuDescription = stream.str();
            m_gpuInfo.adapter = adapterName;
            m_gpuInfo.vendorId = adapterDescription.VendorId;
            m_gpuInfo.deviceId = adapterDescription.DeviceId;
            LARGE_INTEGER driverVersion{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion)))
            {
                std::ostringstream version;
                version << HIWORD(driverVersion.HighPart) << '.' << LOWORD(driverVersion.HighPart) << '.'
                        << HIWORD(driverVersion.LowPart) << '.' << LOWORD(driverVersion.LowPart);
                m_gpuInfo.driverVersion = version.str();
            }
            else
            {
                m_gpuInfo.driverVersion = "unknown";
            }
            WriteLog(LogLevel::Info, m_gpuDescription);
        }

        ComPtr<IDXGIFactory2> factory;
        ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
                      "IDXGIAdapter::GetParent(IDXGIFactory2)");

        // flip-discard 双缓冲交换链：现代推荐路径，需至少 2 个缓冲，不启用 MSAA。
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        // 为指定 HWND 创建交换链；fullscreenDesc 与 restrictedToOutput 均置空。
        ThrowIfFailed(factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &desc, nullptr, nullptr,
                                                      m_swapChain.ReleaseAndGetAddressOf()),
                      "IDXGIFactory2::CreateSwapChainForHwnd");
        // 禁止 Alt+Enter 切换 DXGI 独占全屏（M2 只做窗口化）。
        ThrowIfFailed(factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER),
                      "IDXGIFactory::MakeWindowAssociation");

        // 给交换链命名，供 RenderDoc 与 live-object 报告识别。
        constexpr char name[] = "MiniEngine FlipDiscard SwapChain";
        ThrowIfFailed(m_swapChain->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(name) - 1, name),
                      "IDXGISwapChain::SetPrivateData");
    }

    // 编译着色器并创建着色器对象、输入布局，随后创建常量缓冲、fallback 纹理与固定状态。
    void CreatePipelineResources()
    {
        // 运行时基于 executable directory 解析 HLSL，缺失即失败（M4 起为 PBR forward）。
        const std::filesystem::path shaderFile = m_shaderDirectory / L"PbrForward.hlsl";
        if (!std::filesystem::is_regular_file(shaderFile))
        {
            throw std::runtime_error{"Shader file not found: " + shaderFile.string()};
        }

        const ComPtr<ID3DBlob> vertexBytecode = CompileShader(shaderFile, "VSMain", "vs_5_0");
        const ComPtr<ID3DBlob> pixelBytecode = CompileShader(shaderFile, "PSMain", "ps_5_0");

        // 从字节码创建顶点/像素着色器。
        ThrowIfFailed(m_device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                                   nullptr, m_vertexShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateVertexShader");
        ThrowIfFailed(m_device->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
                                                  nullptr, m_pixelShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreatePixelShader");

        // 输入布局：与 PbrVertex（48B，.memesh v2 磁盘契约）一一对应——
        // POSITION@0、NORMAL@12、TANGENT@24（RGBA32）、TEXCOORD0@40。
        const std::array inputElements{
            D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA,
                                     0},
            D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };

        // 输入布局必须与顶点着色器字节码匹配，故用 VS 字节码创建。
        ThrowIfFailed(m_device->CreateInputLayout(inputElements.data(), static_cast<UINT>(inputElements.size()),
                                                  vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                                  m_inputLayout.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateInputLayout");

        // shadow pass 的 depth-only VS（ShadowDepth.hlsl）：b1 与 PbrForward 逐字节
        // 一致；input layout 只含 POSITION（VS 唯一消费的属性），stride 仍 48。
        const std::filesystem::path shadowShaderFile = m_shaderDirectory / L"ShadowDepth.hlsl";
        if (!std::filesystem::is_regular_file(shadowShaderFile))
        {
            throw std::runtime_error{"Shader file not found: " + shadowShaderFile.string()};
        }
        const ComPtr<ID3DBlob> shadowVertexBytecode = CompileShader(shadowShaderFile, "VSMain", "vs_5_0");
        ThrowIfFailed(m_device->CreateVertexShader(shadowVertexBytecode->GetBufferPointer(),
                                                   shadowVertexBytecode->GetBufferSize(), nullptr,
                                                   m_shadowVertexShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateVertexShader(shadow)");
        const std::array shadowInputElements{
            D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ThrowIfFailed(
            m_device->CreateInputLayout(shadowInputElements.data(), static_cast<UINT>(shadowInputElements.size()),
                                        shadowVertexBytecode->GetBufferPointer(), shadowVertexBytecode->GetBufferSize(),
                                        m_shadowInputLayout.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateInputLayout(shadow)");
        SetDebugObjectName(m_shadowVertexShader.Get(), "ShadowDepth VS");
        SetDebugObjectName(m_shadowInputLayout.Get(), "ShadowDepth InputLayout");

        // 主 pass 固定状态先行创建（shadow 共享其 rasterizer，见 Create 注释）。
        CreateFixedStates();

        // shadow 资源组（03 篇：typeless 纹理 / D32_FLOAT DSV / R32_FLOAT SRV /
        // comparison sampler / bias rasterizer），含 CheckFormatSupport 前置检查。
        m_shadowMap.Create(*m_device.Get(), m_rasterizerState.Get(), m_mirroredRasterizerState.Get());

        // 05 篇后处理两个 pass 的静态资源（不依赖窗口尺寸，故只创建一次）。
        CreateSkyboxResources();
        CreateToneMapResources();

        // 07 篇截图：3 个 EVENT 查询（staging 纹理按需创建，尺寸随窗口）。
        m_screenshot.Create(*m_device.Get());

        // 08 篇 GPU 计时：11×8 个 timestamp/disjoint 查询（预创建，帧内只提交 End）。
        m_gpuTimer.Create(*m_device.Get());

        // 08 篇 CPU timing：QPC 频率启动时缓存一次。
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        m_qpcFrequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

        CreateConstantBuffers();
        CreateFallbackTextures();
        CreateIblResources();

        // 为着色器与输入布局命名，便于 RenderDoc 与 live-object 报告定位。
        SetDebugObjectName(m_vertexShader.Get(), "PbrForward VS");
        SetDebugObjectName(m_pixelShader.Get(), "PbrForward PS");
        SetDebugObjectName(m_inputLayout.Get(), "PbrForward InputLayout");
    }

    // 创建 b0/b1/b2 三条动态常量缓冲（WRITE_DISCARD 每帧/每 Draw 覆盖写）。
    void CreateConstantBuffers()
    {
        const auto createDynamicCb = [this](const UINT byteWidth, const char* name, ComPtr<ID3D11Buffer>& destination)
        {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = byteWidth;
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ThrowIfFailed(m_device->CreateBuffer(&description, nullptr, destination.ReleaseAndGetAddressOf()), name);
            SetDebugObjectName(destination.Get(), name);
        };

        // b0 frame：每帧一次（112B）。
        createDynamicCb(sizeof(FrameConstants), "Frame Constant Buffer (b0)", m_frameConstantBuffer);
        // b1 object：每 Draw 一次（144B）。
        createDynamicCb(sizeof(ObjectConstants), "Object Constant Buffer (b1)", m_objectConstantBuffer);
        // b2 material：每 Draw 一次（48B）。
        createDynamicCb(sizeof(MaterialConstants), "Material Constant Buffer (b2)", m_materialConstantBuffer);

        // 线性过滤 + 三个方向 wrap 的材质采样器（s0）。
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX; // 完整 mip 链可用（.metex v2）
        ThrowIfFailed(m_device->CreateSamplerState(&samplerDesc, m_samplerState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateSamplerState");
        SetDebugObjectName(m_samplerState.Get(), "LinearWrap Sampler");
    }

    // 创建 5 个 1×1 fallback 纹理（02 篇「默认纹理」）：缺省贴图槽位必须绑定真实
    // fallback SRV——禁止留空后依赖上一材质的绑定残留。颜色与颜色空间按下表固定。
    void CreateFallbackTextures()
    {
        struct FallbackSpec final
        {
            const char* name;
            std::array<std::uint8_t, 4> rgba;
            bool srgb;
        };
        // 次序 = PbrSrvSlot 0..4（BaseColor/Normal/MetallicRoughness/Occlusion/Emissive）。
        constexpr std::array<FallbackSpec, 5> kFallbacks{
            FallbackSpec{"Fallback BaseColor SRV (white sRGB)", {255U, 255U, 255U, 255U}, true},
            FallbackSpec{"Fallback Normal SRV (+Z linear)", {128U, 128U, 255U, 255U}, false},
            FallbackSpec{"Fallback MetallicRoughness SRV (G=B=1 linear)", {255U, 255U, 255U, 255U}, false},
            FallbackSpec{"Fallback Occlusion SRV (white linear)", {255U, 255U, 255U, 255U}, false},
            FallbackSpec{"Fallback Emissive SRV (black sRGB)", {0U, 0U, 0U, 255U}, true},
        };

        for (std::size_t index = 0; index < kFallbacks.size(); ++index)
        {
            const FallbackSpec& spec = kFallbacks[index];

            // 资源 TYPELESS + 视图按颜色空间选 UNORM/UNORM_SRGB，与资产纹理上传同一约定。
            D3D11_TEXTURE2D_DESC textureDesc{};
            textureDesc.Width = 1;
            textureDesc.Height = 1;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            const std::array<std::uint8_t, 4> pixel = spec.rgba;
            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = pixel.data();
            initialData.SysMemPitch = 4;

            ComPtr<ID3D11Texture2D> texture;
            ThrowIfFailed(m_device->CreateTexture2D(&textureDesc, &initialData, texture.ReleaseAndGetAddressOf()),
                          "ID3D11Device::CreateTexture2D(fallback)");

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = spec.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MostDetailedMip = 0;
            viewDesc.Texture2D.MipLevels = 1;
            ThrowIfFailed(m_device->CreateShaderResourceView(texture.Get(), &viewDesc,
                                                             m_fallbackSrvs[index].ReleaseAndGetAddressOf()),
                          "ID3D11Device::CreateShaderResourceView(fallback)");

            SetDebugObjectName(texture.Get(), spec.name);
            SetDebugObjectName(m_fallbackSrvs[index].Get(), spec.name);
        }
    }

    // 创建 IBL 相关资源（04 篇）：b3 常量缓冲、s1 线性 clamp 采样器与 t5/t6/t7 的
    // fallback（黑 cube ×2 + 中性 LUT）。IblState 未 Ready 时渲染端绑定这些真实
    // fallback——indirect 退化为 0 而非 NaN，不因缺环境丢绘制（02 篇同款失败语义）。
    void CreateIblResources()
    {
        // s1：IBL 线性 clamp（与生成 shader 的 s1 同语义；wrap 采样对 cube/LUT 会
        // 越界混绕，clamp 是 04 篇契约）。
        D3D11_SAMPLER_DESC iblSamplerDesc{};
        iblSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        iblSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        iblSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        iblSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        iblSamplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        iblSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ThrowIfFailed(m_device->CreateSamplerState(&iblSamplerDesc, m_iblSamplerState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateSamplerState(ibl clamp)");
        SetDebugObjectName(m_iblSamplerState.Get(), "LinearClamp Sampler (s1)");

        // b3：environment 变化时更新（16B 动态 CB；prefilterMipCount 每帧写入）。
        D3D11_BUFFER_DESC iblCbDesc{};
        iblCbDesc.ByteWidth = 16;
        iblCbDesc.Usage = D3D11_USAGE_DYNAMIC;
        iblCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        iblCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(m_device->CreateBuffer(&iblCbDesc, nullptr, m_iblConstantBuffer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBuffer(ibl constants)");
        SetDebugObjectName(m_iblConstantBuffer.Get(), "Ibl Constant Buffer (b3)");

        // fallback 黑 cube（1×1×6 RGBA16F，t5/t6 共用内容、各建一份资源便于命名）。
        const auto createBlackCubeSrv = [this](const char* name, ComPtr<ID3D11ShaderResourceView>& destination)
        {
            D3D11_TEXTURE2D_DESC cubeDesc{};
            cubeDesc.Width = 1;
            cubeDesc.Height = 1;
            cubeDesc.MipLevels = 1;
            cubeDesc.ArraySize = 6;
            cubeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            cubeDesc.SampleDesc.Count = 1;
            cubeDesc.Usage = D3D11_USAGE_IMMUTABLE;
            cubeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            cubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

            const std::array<std::uint16_t, 4> blackPixel{0U, 0U, 0U, 0x3C00U}; // (0,0,0,1) 半精度
            D3D11_SUBRESOURCE_DATA faceData{blackPixel.data(), 4, 0};
            const std::array<D3D11_SUBRESOURCE_DATA, 6> faces{faceData, faceData, faceData,
                                                              faceData, faceData, faceData};
            ComPtr<ID3D11Texture2D> texture;
            ThrowIfFailed(m_device->CreateTexture2D(&cubeDesc, faces.data(), texture.ReleaseAndGetAddressOf()),
                          "ID3D11Device::CreateTexture2D(fallback black cube)");

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.MipLevels = 1;
            ThrowIfFailed(
                m_device->CreateShaderResourceView(texture.Get(), &srvDesc, destination.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(fallback black cube)");
            SetDebugObjectName(texture.Get(), name);
            SetDebugObjectName(destination.Get(), name);
        };
        createBlackCubeSrv("Fallback DiffuseIrradiance SRV (black cube)",
                           m_fallbackIblSrvs[Slot(PbrSrvSlot::DiffuseIrradiance)]);
        createBlackCubeSrv("Fallback PrefilteredSpecular SRV (black cube)",
                           m_fallbackIblSrvs[Slot(PbrSrvSlot::PrefilteredSpecular)]);

        // fallback 中性 BRDF LUT（1×1 RG16F = (1,0)）：specularIbl =
        // prefiltered * (F0 * 1 + 0) —— LUT 本身不改变 prefiltered 的值。
        {
            D3D11_TEXTURE2D_DESC lutDesc{};
            lutDesc.Width = 1;
            lutDesc.Height = 1;
            lutDesc.MipLevels = 1;
            lutDesc.ArraySize = 1;
            lutDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
            lutDesc.SampleDesc.Count = 1;
            lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
            lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            // 半精度 1.0 = 0x3C00、0.0 = 0x0000。
            const std::array<std::uint16_t, 2> neutralPixel{0x3C00U, 0x0000U};
            D3D11_SUBRESOURCE_DATA initialData{neutralPixel.data(), 4, 0};
            ComPtr<ID3D11Texture2D> texture;
            ThrowIfFailed(m_device->CreateTexture2D(&lutDesc, &initialData, texture.ReleaseAndGetAddressOf()),
                          "ID3D11Device::CreateTexture2D(fallback neutral lut)");

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            ThrowIfFailed(
                m_device->CreateShaderResourceView(
                    texture.Get(), &srvDesc, m_fallbackIblSrvs[Slot(PbrSrvSlot::BrdfLut)].ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(fallback neutral lut)");
            SetDebugObjectName(texture.Get(), "Fallback BrdfLut SRV (neutral)");
            SetDebugObjectName(m_fallbackIblSrvs[Slot(PbrSrvSlot::BrdfLut)].Get(), "Fallback BrdfLut SRV (neutral)");
        }
    }

    // 创建 skybox pass 的资源（05 篇）：VS/PS、POSITION-only 输入布局、单位立方体
    // VB/IB 与 b0 常量缓冲。几何是常量数据（顶点即方向），不随帧变化。
    void CreateSkyboxResources()
    {
        const std::filesystem::path shaderFile = m_shaderDirectory / L"Skybox.hlsl";
        if (!std::filesystem::is_regular_file(shaderFile))
        {
            throw std::runtime_error{"Shader file not found: " + shaderFile.string()};
        }

        const ComPtr<ID3DBlob> vertexBytecode = CompileShader(shaderFile, "VSMain", "vs_5_0");
        const ComPtr<ID3DBlob> pixelBytecode = CompileShader(shaderFile, "PSMain", "ps_5_0");
        ThrowIfFailed(m_device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                                   nullptr, m_skyboxVertexShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateVertexShader(skybox)");
        ThrowIfFailed(m_device->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
                                                  nullptr, m_skyboxPixelShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreatePixelShader(skybox)");

        const std::array inputElements{
            D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ThrowIfFailed(m_device->CreateInputLayout(inputElements.data(), static_cast<UINT>(inputElements.size()),
                                                  vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                                  m_skyboxInputLayout.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateInputLayout(skybox)");

        const auto createImmutableBuffer = [this](const void* data, const UINT byteWidth, const UINT bindFlags,
                                                  const char* name, ComPtr<ID3D11Buffer>& destination)
        {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = byteWidth;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = bindFlags;
            D3D11_SUBRESOURCE_DATA initialData{data, 0, 0};
            ThrowIfFailed(m_device->CreateBuffer(&description, &initialData, destination.ReleaseAndGetAddressOf()),
                          name);
            SetDebugObjectName(destination.Get(), name);
        };
        createImmutableBuffer(kSkyboxVertices.data(), static_cast<UINT>(kSkyboxVertices.size() * sizeof(float)),
                              D3D11_BIND_VERTEX_BUFFER, "Skybox Vertex Buffer", m_skyboxVertexBuffer);
        createImmutableBuffer(kSkyboxIndices.data(), static_cast<UINT>(kSkyboxIndices.size() * sizeof(std::uint16_t)),
                              D3D11_BIND_INDEX_BUFFER, "Skybox Index Buffer", m_skyboxIndexBuffer);

        // b0：去平移 VP（64B 动态 CB，每帧写入）。
        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = sizeof(DirectX::XMFLOAT4X4);
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(m_device->CreateBuffer(&constantDesc, nullptr, m_skyboxConstantBuffer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBuffer(skybox constants)");
        SetDebugObjectName(m_skyboxConstantBuffer.Get(), "Skybox Constant Buffer (b0)");

        SetDebugObjectName(m_skyboxVertexShader.Get(), "Skybox VS");
        SetDebugObjectName(m_skyboxPixelShader.Get(), "Skybox PS");
        SetDebugObjectName(m_skyboxInputLayout.Get(), "Skybox InputLayout");
    }

    // 创建 tone map pass 的资源（05 篇）：fullscreen triangle 的 VS/PS（无 VB、
    // 无输入布局）与 b0 `PostProcessConstants`（16B）。
    void CreateToneMapResources()
    {
        const std::filesystem::path shaderFile = m_shaderDirectory / L"ToneMap.hlsl";
        if (!std::filesystem::is_regular_file(shaderFile))
        {
            throw std::runtime_error{"Shader file not found: " + shaderFile.string()};
        }

        const ComPtr<ID3DBlob> vertexBytecode = CompileShader(shaderFile, "VSMain", "vs_5_0");
        const ComPtr<ID3DBlob> pixelBytecode = CompileShader(shaderFile, "PSMain", "ps_5_0");
        ThrowIfFailed(m_device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                                   nullptr, m_toneMapVertexShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateVertexShader(tonemap)");
        ThrowIfFailed(m_device->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
                                                  nullptr, m_toneMapPixelShader.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreatePixelShader(tonemap)");

        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = sizeof(Rhi::D3D11::PostProcessConstants);
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(m_device->CreateBuffer(&constantDesc, nullptr, m_postConstantBuffer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBuffer(post constants)");
        SetDebugObjectName(m_postConstantBuffer.Get(), "PostProcess Constant Buffer (b0)");

        SetDebugObjectName(m_toneMapVertexShader.Get(), "ToneMap VS");
        SetDebugObjectName(m_toneMapPixelShader.Get(), "ToneMap PS");
    }

    // 创建固定的光栅化（背面剔除）与深度模板（LESS）状态对象。
    void CreateFixedStates()
    {
        // 光栅化：实心填充、背面剔除、不翻转顺时针正面、启用深度裁剪。
        D3D11_RASTERIZER_DESC rasterizerDesc{};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_BACK;
        // M3 烘焙网格正面为 CCW（glTF CCW → x 镜像 → 索引交换），故 CCW 为正面。
        rasterizerDesc.FrontCounterClockwise = TRUE;
        rasterizerDesc.DepthClipEnable = TRUE;
        ThrowIfFailed(m_device->CreateRasterizerState(&rasterizerDesc, m_rasterizerState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateRasterizerState");

        // 镜像（负 determinant world）对象用相反正面绕序的第二个光栅化状态。
        D3D11_RASTERIZER_DESC mirroredDesc = rasterizerDesc;
        mirroredDesc.FrontCounterClockwise = FALSE;
        ThrowIfFailed(
            m_device->CreateRasterizerState(&mirroredDesc, m_mirroredRasterizerState.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState(mirrored)");

        // 深度模板：启用深度、写全部深度、LESS 比较、不使用模板。
        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
        depthDesc.StencilEnable = FALSE;
        ThrowIfFailed(m_device->CreateDepthStencilState(&depthDesc, m_depthStencilState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateDepthStencilState");

        // 无剔除光栅化（skybox 与 tone map 共用）：相机位于立方体内部、全屏三角形
        // 的绕序也不携带语义——CULL_NONE 让这两类 pass 与绕序约定解耦。
        D3D11_RASTERIZER_DESC noCullDesc = rasterizerDesc;
        noCullDesc.CullMode = D3D11_CULL_NONE;
        ThrowIfFailed(m_device->CreateRasterizerState(&noCullDesc, m_noCullRasterizerState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateRasterizerState(no cull)");

        // skybox 深度：测试开启 + **不写深度** + LESS_EQUAL。
        // LESS_EQUAL 是远深度技巧的前提——skybox 的 ndc.z 恰为 1，只有 LESS 会被
        // 清屏深度（1）判为失败而整屏消失；不写深度则保证它不影响任何后续 pass。
        D3D11_DEPTH_STENCIL_DESC skyboxDepthDesc{};
        skyboxDepthDesc.DepthEnable = TRUE;
        skyboxDepthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        skyboxDepthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        skyboxDepthDesc.StencilEnable = FALSE;
        ThrowIfFailed(m_device->CreateDepthStencilState(&skyboxDepthDesc, m_skyboxDepthState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateDepthStencilState(skybox)");

        // tone map 深度：05 篇"depth off"——全屏 pass 不参与深度测试也不写深度。
        D3D11_DEPTH_STENCIL_DESC postDepthDesc{};
        postDepthDesc.DepthEnable = FALSE;
        postDepthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        postDepthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        postDepthDesc.StencilEnable = FALSE;
        ThrowIfFailed(m_device->CreateDepthStencilState(&postDepthDesc, m_postDepthState.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateDepthStencilState(post)");

        SetDebugObjectName(m_rasterizerState.Get(), "Opaque Rasterizer State");
        SetDebugObjectName(m_mirroredRasterizerState.Get(), "Opaque Rasterizer State (mirrored front-face)");
        SetDebugObjectName(m_depthStencilState.Get(), "DepthLess State");
        SetDebugObjectName(m_noCullRasterizerState.Get(), "NoCull Rasterizer State (skybox/post)");
        SetDebugObjectName(m_skyboxDepthState.Get(), "Skybox Depth State (no write, LESS_EQUAL)");
        SetDebugObjectName(m_postDepthState.Get(), "Post Depth State (depth off)");
    }

    // 按给定尺寸创建后备缓冲 RTV、深度纹理/DSV、HDR scene target 与等大 Viewport，
    // 并记录新尺寸（05 篇：HDR 与 main depth/back buffer 同步重建）。
    void CreateSizeDependentResources(const std::uint32_t width, const std::uint32_t height)
    {
        // 从交换链取后备缓冲，并创建其渲染目标视图。
        ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.ReleaseAndGetAddressOf())),
                      "IDXGISwapChain::GetBuffer");
        SetDebugObjectName(backBuffer.Get(), "BackBuffer Texture");

        ThrowIfFailed(
            m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_renderTargetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView");

        // 深度纹理与后备缓冲同尺寸，D24_UNORM_S8_UINT 提供 24 位深度 + 8 位模板。
        D3D11_TEXTURE2D_DESC depthDesc{};
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ThrowIfFailed(m_device->CreateTexture2D(&depthDesc, nullptr, m_depthTexture.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateTexture2D(depth)");
        ThrowIfFailed(m_device->CreateDepthStencilView(m_depthTexture.Get(), nullptr,
                                                       m_depthStencilView.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateDepthStencilView");

        // HDR scene target 与深度/后备缓冲同尺寸、同生命周期（05 篇「Resize 与前后台」）。
        // 创建失败按 fatal 抛出：半初始化（有 back buffer 无 HDR）会让 pass 2
        // 静默画到错误目标上。
        ThrowIfFailed(m_hdrTarget.Create(*m_device.Get(), width, height), "D3D11HdrTarget::Create");

        SetDebugObjectName(m_renderTargetView.Get(), "BackBuffer RTV");
        SetDebugObjectName(m_depthTexture.Get(), "Depth Texture");
        SetDebugObjectName(m_depthStencilView.Get(), "Depth DSV");

        // 记录尺寸并建立与客户区等大的 Viewport（深度范围 0..1）。
        m_width = width;
        m_height = height;
        m_viewport = {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F};
    }

    // ---- 07 篇截图 ----

    // 在 tone-map 写完后、Present 前把后备缓冲复制进 staging ring：flip-discard 模型
    // 下 Present 之后后备缓冲内容不再有定义，因此截图必须挂在帧内这个点。
    void EnqueuePendingScreenshot()
    {
        if (m_pendingScreenshotPath.empty())
        {
            return;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.ReleaseAndGetAddressOf())),
                      "IDXGISwapChain::GetBuffer(screenshot)");
        std::string error;
        if (m_screenshot.Enqueue(*m_device.Get(), *m_context.Get(), *backBuffer.Get(), m_pendingScreenshotPath, error))
        {
            WriteLog(LogLevel::Info, "screenshot enqueued: " + m_pendingScreenshotPath.string());
        }
        else
        {
            WriteLog(LogLevel::Error, "screenshot enqueue failed: " + error);
        }
        m_pendingScreenshotPath.clear();
    }

    // Present 之后轮询 ring：GPU 通常已完成 1-2 帧前的拷贝，Map 不阻塞。
    void PollCompletedScreenshot()
    {
        std::string error;
        const Rhi::D3D11::D3D11Screenshot::PollResult result =
            m_screenshot.Poll(*m_context.Get(), m_completedScreenshot, error);
        if (result == Rhi::D3D11::D3D11Screenshot::PollResult::Completed)
        {
            m_screenshotCompleted = true;
            WriteLog(LogLevel::Info, "screenshot written: " + m_completedScreenshot.pngPath.string() +
                                         " sha256=" + m_completedScreenshot.pngSha256);
        }
        else if (result == Rhi::D3D11::D3D11Screenshot::PollResult::Failed)
        {
            WriteLog(LogLevel::Error, "screenshot failed: " + error);
        }
    }

    // 每帧一次的 b0：相机 VP、相机位置、固定方向光、shadow map 尺寸与 debug 模式。
    void UpdateFrameConstants(const World::RenderPacket& packet)
    {
        FrameConstants constants{};
        constants.viewProjection = TransposedForHlsl(packet.viewProjection);
        constants.cameraPositionAndDebugMode = {packet.cameraWorldPosition.x, packet.cameraWorldPosition.y,
                                                packet.cameraWorldPosition.z, static_cast<float>(m_debugMode)};
        constants.directionAndIntensity = {
            packet.directionalLight.directionToLight[0], packet.directionalLight.directionToLight[1],
            packet.directionalLight.directionToLight[2], packet.directionalLight.illuminanceScale};
        constants.lightColorAndPadding = {packet.directionalLight.colorLinear[0],
                                          packet.directionalLight.colorLinear[1],
                                          packet.directionalLight.colorLinear[2], 0.0F};
        constants.shadowMapSizeAndPadding = {static_cast<float>(Rhi::D3D11::D3D11ShadowMap::kShadowSize),
                                             static_cast<float>(Rhi::D3D11::D3D11ShadowMap::kShadowSize), 0.0F, 0.0F};

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(m_context->Map(m_frameConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "ID3D11DeviceContext::Map(frame constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_frameConstantBuffer.Get(), 0);
    }

    // 每 Draw 的 b1：world / normal matrix / 光空间 VP / 手性 / receiver 开关。
    // b1 同时供 forward（PbrForward.hlsl）与 shadow pass（ShadowDepth.hlsl）消费，
    // 两侧声明逐字节一致（03 篇硬约束）。
    void UpdateObjectConstants(const World::RenderDraw& draw)
    {
        ObjectConstants object{};
        object.world = TransposedForHlsl(draw.world);
        object.normalMatrix = TransposedForHlsl(draw.normal);
        object.lightWorldViewProjection = TransposedForHlsl(m_lightViewProjection);
        object.handednessAndReceivesShadow = {draw.mirrored ? -1.0F : 1.0F,      // x = WorldHandedness
                                              draw.receivesShadow ? 1.0F : 0.0F, // y = ReceivesShadow
                                              0.0F, // zw 显式清零，不写未初始化字节进常量缓冲
                                              0.0F};

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(m_context->Map(m_objectConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "ID3D11DeviceContext::Map(object constants)");
        std::memcpy(mapped.pData, &object, sizeof(object));
        m_context->Unmap(m_objectConstantBuffer.Get(), 0);
    }

    // 每 Draw 的 b2：材质因子（48B，02 篇 MaterialConstants 契约；不用未初始化 padding）。
    void UpdateMaterialConstants(const Assets::MaterialAsset& material)
    {
        MaterialConstants constants{};
        constants.baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1],
                                     material.baseColorFactor[2], material.baseColorFactor[3]};
        constants.emissiveAndMetallic = {material.emissiveFactor[0], material.emissiveFactor[1],
                                         material.emissiveFactor[2], material.metallicFactor};
        constants.roughnessNormalOcclusionFlags = {material.roughnessFactor, material.normalScale,
                                                   material.occlusionStrength, 0.0F};

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(m_context->Map(m_materialConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "ID3D11DeviceContext::Map(material constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_materialConstantBuffer.Get(), 0);
    }

    // skybox b0：去平移的 view * projection（64B）。平移清零后立方体始终以相机为
    // 中心——相机移动时天空不产生视差，这是 skybox 与"放置得很远的盒子"的区别。
    void UpdateSkyboxConstants(const World::RenderPacket& packet)
    {
        const DirectX::XMFLOAT4X4 viewProjection = ViewProjectionWithoutTranslation(packet.view, packet.projection);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(m_context->Map(m_skyboxConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "ID3D11DeviceContext::Map(skybox constants)");
        std::memcpy(mapped.pData, &viewProjection, sizeof(viewProjection));
        m_context->Unmap(m_skyboxConstantBuffer.Get(), 0);
    }

    // tone map b0：`PostProcessConstants`（16B，与 ToneMap.hlsl 逐字段对应）。
    // debugMode 10 = SceneLuminance 假色（05 篇"确认亮部 >1"）；padding 显式清零。
    void UpdatePostConstants()
    {
        Rhi::D3D11::PostProcessConstants constants{};
        constants.exposureEv = m_exposureEv;
        constants.debugHdr = m_debugMode == kDebugModeSceneLuminance ? 1U : 0U;
        constants.inverseOutputSize[0] = m_width > 0 ? 1.0F / static_cast<float>(m_width) : 0.0F;
        constants.inverseOutputSize[1] = m_height > 0 ? 1.0F / static_cast<float>(m_height) : 0.0F;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(m_context->Map(m_postConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "ID3D11DeviceContext::Map(post constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_postConstantBuffer.Get(), 0);
    }

    // b3：prefilter mip 数（roughness→lod 换算基准；16B float4-only）。
    void UpdateIblConstants(const std::uint32_t prefilterMipCount)
    {
        const DirectX::XMFLOAT4 constants{static_cast<float>(prefilterMipCount), 0.0F, 0.0F, 0.0F};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(m_context->Map(m_iblConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "ID3D11DeviceContext::Map(ibl constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_iblConstantBuffer.Get(), 0);
    }

    // 环境 panorama 上传与 IBL 生成事务（04 篇「Readiness 与失败」）：revision 变化
    // 时在帧边界一次性完成 上传 → BuildTemporary → Commit/Discard；失败保留旧 active。
    // （public：对外 UpdateEnvironmentPanorama 薄转发的落点。）
  public:
    bool UpdateEnvironmentPanoramaImpl(const std::uint32_t width, const std::uint32_t height,
                                       const std::uint16_t* rgba16HalfPixels, const std::uint64_t revision)
    {
        if (width == 0 || height == 0 || rgba16HalfPixels == nullptr)
        {
            // 移除环境：释放 IBL set 与 panorama，回到 direct-only（State=Empty）。
            m_iblResources.Release();
            m_environmentSrv.Reset();
            m_environmentTexture.Reset();
            m_environmentRevision = 0;
            return true;
        }

        if (revision == m_environmentRevision)
        {
            return false; // 无变化：steady-state 快速路径（不逐帧重建）
        }

        // panorama 2D 纹理（RGBA16F，单 mip——equirect 转换只采 LOD0）。
        D3D11_TEXTURE2D_DESC panoramaDesc{};
        panoramaDesc.Width = width;
        panoramaDesc.Height = height;
        panoramaDesc.MipLevels = 1;
        panoramaDesc.ArraySize = 1;
        panoramaDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        panoramaDesc.SampleDesc.Count = 1;
        panoramaDesc.Usage = D3D11_USAGE_IMMUTABLE;
        panoramaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{rgba16HalfPixels, width * 8U, 0};
        ComPtr<ID3D11Texture2D> newTexture;
        ComPtr<ID3D11ShaderResourceView> newSrv;
        ThrowIfFailed(m_device->CreateTexture2D(&panoramaDesc, &initialData, newTexture.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateTexture2D(environment panorama)");
        SetDebugObjectName(newTexture.Get(), "M4.IBL.Panorama");
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        ThrowIfFailed(m_device->CreateShaderResourceView(newTexture.Get(), &srvDesc, newSrv.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateShaderResourceView(environment panorama)");
        SetDebugObjectName(newSrv.Get(), "M4.IBL.Panorama.SRV");

        // 生成事务：成功换新 panorama + commit；失败丢弃 temporary 并按 04 篇语义
        // 保留旧 active（本调用把新 panorama 也一并丢弃，保持"整个 set 原子换"）。
        std::string error;
        if (!m_iblResources.BuildTemporary(*m_device.Get(), *m_context.Get(), *newSrv.Get(), m_shaderDirectory,
                                           revision, error))
        {
            WriteLog(LogLevel::Error, "IBL generation failed: " + error);
            m_iblResources.DiscardTemporary();
            return false;
        }
        m_iblResources.CommitTemporary();

        m_environmentTexture = std::move(newTexture);
        m_environmentSrv = std::move(newSrv);
        m_environmentRevision = revision;
        WriteLog(LogLevel::Info, "IBL generation completed (revision " + std::to_string(revision) + ")");
        return true;
    }

    // 统一检查 Present/Resize 等设备相关 HRESULT；设备移除类错误附带 removal reason 抛出。
    void CheckDeviceResult(const HRESULT result, const char* operation)
    {
        if (SUCCEEDED(result))
        {
            return;
        }

        // 设备移除/重置/挂起：先取 removal reason 一并报出，再失败退出，不做自动恢复。
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
            result == DXGI_ERROR_DEVICE_HUNG)
        {
            const HRESULT reason = m_device->GetDeviceRemovedReason();
            throw std::runtime_error{std::string{operation} + " failed with " + HResultHex(result) +
                                     "; removal reason=" + HResultHex(reason)};
        }

        // 其余失败按普通 HRESULT 处理。
        ThrowIfFailed(result, operation);
    }

    // 读取并清空 InfoQueue 消息：WARNING 记录、ERROR/CORRUPTION 计为失败并抛异常。
    void DrainDebugMessages(const char* phase)
    {
        // 非 Debug 设备没有 InfoQueue，无消息可读。
        if (m_infoQueue == nullptr)
        {
            return;
        }

        const UINT64 count = m_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        if (count == 0)
        {
            return;
        }

        // 验收口径是"Debug Layer 无错误"：WARNING 只记录并清空（WARNING 类别
        // 包含大量良性消息，当 fatal 处理会让正确代码直接崩溃）；
        // ERROR/CORRUPTION 才视为失败。这与 ConfigureDebugQueue 的
        // SetBreakOnSeverity(ERROR/CORRUPTION) 语义一致。
        bool hasError = false;
        for (UINT64 index = 0; index < count; ++index)
        {
            // 先查单条消息字节长度，再分配缓冲并读取完整 D3D11_MESSAGE。
            SIZE_T byteLength = 0;
            ThrowIfFailed(m_infoQueue->GetMessage(index, nullptr, &byteLength), "ID3D11InfoQueue::GetMessage(size)");

            std::vector<std::byte> storage(byteLength);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            ThrowIfFailed(m_infoQueue->GetMessage(index, message, &byteLength), "ID3D11InfoQueue::GetMessage(data)");

            // ERROR/CORRUPTION 记入失败标志，WARNING 只记录；统一写入日志。
            const bool isError = message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                                 message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION;
            const std::string text = std::string{"D3D11 ["} + phase + "] message " + std::to_string(message->ID) +
                                     ": " + message->pDescription;
            WriteLog(isError ? LogLevel::Error : LogLevel::Warning, text);
            hasError = hasError || isError;
            ++m_debugLayerMessageCount; // 07 篇：截图元数据 debugLayerMessages 字段
        }

        // 读后清空队列，避免下次重复计数。
        m_infoQueue->ClearStoredMessages();
        if (hasError)
        {
            throw std::runtime_error{std::string{"D3D11 Debug Layer reported error/corruption during "} + phase};
        }
    }

    // 释放全部 D3D 资源并输出 live-object 报告；按 上下文→子资源→交换链→调试接口→设备 顺序。
    void Shutdown() noexcept
    {
        // 先清理即时上下文状态并排空命令，再逐个释放资源，最后释放 device。
        if (m_context != nullptr)
        {
            m_context->ClearState();
            m_context->Flush();
        }

        // 资产缓存持有的 GPU 资源必须先于 device 释放（live-object 报告口径）。
        m_assetCache.ReleaseAll();
        m_shadowMap.Release();
        m_iblResources.Release();
        m_hdrTarget.Reset(); // 窗口尺寸资源：与 back buffer/depth 同批释放
        m_screenshot.Release();
        m_gpuTimer.Release();

        // 05 篇两个后处理 pass 的静态资源（不依赖尺寸，随 Impl 生命周期释放）。
        m_postConstantBuffer.Reset();
        m_toneMapPixelShader.Reset();
        m_toneMapVertexShader.Reset();
        m_skyboxConstantBuffer.Reset();
        m_skyboxIndexBuffer.Reset();
        m_skyboxVertexBuffer.Reset();
        m_skyboxInputLayout.Reset();
        m_skyboxPixelShader.Reset();
        m_skyboxVertexShader.Reset();
        m_noCullRasterizerState.Reset();
        m_skyboxDepthState.Reset();
        m_postDepthState.Reset();

        // 按"子资源 → 交换链 → 调试接口 → 设备"的顺序释放。
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();
        m_depthTexture.Reset();
        m_samplerState.Reset();
        m_iblSamplerState.Reset();
        for (ComPtr<ID3D11ShaderResourceView>& srv : m_fallbackSrvs)
        {
            srv.Reset();
        }
        for (ComPtr<ID3D11ShaderResourceView>& srv : m_fallbackIblSrvs)
        {
            srv.Reset();
        }
        m_environmentSrv.Reset();
        m_environmentTexture.Reset();
        m_iblConstantBuffer.Reset();
        m_materialConstantBuffer.Reset();
        m_objectConstantBuffer.Reset();
        m_frameConstantBuffer.Reset();
        m_inputLayout.Reset();
        m_pixelShader.Reset();
        m_vertexShader.Reset();
        m_swapChain.Reset();
        m_annotation.Reset();
        m_infoQueue.Reset();
        m_context.Reset();

        // 保留 ID3D11Debug 临时引用，待 device 释放后再输出 live-object 报告。
        ComPtr<ID3D11Debug> debug = m_debug;
        m_debug.Reset();
        m_device.Reset();

        if (debug != nullptr)
        {
            // DETAIL 级别输出各对象引用计数，用于核对应用 child 资源是否全部释放。
            debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        }
    }

    HWND m_window = nullptr;
    std::filesystem::path m_shaderDirectory;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_9_1;
    std::uint32_t m_debugMode = 0; // b0 DebugMode（SetDebugMode 设置；0 = 最终着色）

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11InfoQueue> m_infoQueue;
    ComPtr<ID3D11Debug> m_debug;
    ComPtr<ID3DUserDefinedAnnotation> m_annotation;
    ComPtr<IDXGISwapChain1> m_swapChain;

    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11Texture2D> m_depthTexture;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11VertexShader> m_shadowVertexShader; // ShadowDepth.hlsl（depth-only）
    ComPtr<ID3D11InputLayout> m_shadowInputLayout;   // POSITION-only（stride 48）
    Rhi::D3D11::D3D11ShadowMap m_shadowMap;
    Rhi::D3D11::D3D11HdrTarget m_hdrTarget; // 05 篇：窗口尺寸 RGBA16F scene color
    ComPtr<ID3D11VertexShader> m_skyboxVertexShader;
    ComPtr<ID3D11PixelShader> m_skyboxPixelShader;
    ComPtr<ID3D11InputLayout> m_skyboxInputLayout;    // POSITION-only（stride 12）
    ComPtr<ID3D11Buffer> m_skyboxVertexBuffer;        // 单位立方体 24 顶点（顶点即方向）
    ComPtr<ID3D11Buffer> m_skyboxIndexBuffer;         // 36 × R16_UINT
    ComPtr<ID3D11Buffer> m_skyboxConstantBuffer;      // skybox b0：去平移 VP（64B）
    ComPtr<ID3D11VertexShader> m_toneMapVertexShader; // fullscreen triangle（无 VB）
    ComPtr<ID3D11PixelShader> m_toneMapPixelShader;
    ComPtr<ID3D11Buffer> m_postConstantBuffer;     // tone map b0：PostProcessConstants（16B）
    Rhi::D3D11::D3D11IblResources m_iblResources;  // 04 篇：IBL 四资源生成与 readiness 事务
    World::Matrix4 m_lightViewProjection{};        // 帧初由 BuildLightViewProjection 计算
    ComPtr<ID3D11Buffer> m_frameConstantBuffer;    // b0：每帧一次（112B）
    ComPtr<ID3D11Buffer> m_objectConstantBuffer;   // b1：每 Draw 一次（144B）
    ComPtr<ID3D11Buffer> m_materialConstantBuffer; // b2：每 Draw 一次（48B）
    ComPtr<ID3D11Buffer> m_iblConstantBuffer;      // b3：environment 变化时（16B）
    // 1×1 fallback SRV×5：次序 = PbrSrvSlot 0..4（02 篇「默认纹理」）。
    std::array<ComPtr<ID3D11ShaderResourceView>, Slot(PbrSrvSlot::Count)> m_fallbackSrvs;
    // IBL fallback（t5/t6 黑 cube、t7 中性 LUT）：IblState 未 Ready 时的真实绑定。
    std::array<ComPtr<ID3D11ShaderResourceView>, Slot(PbrSrvSlot::Count)> m_fallbackIblSrvs;
    ComPtr<ID3D11Texture2D> m_environmentTexture; // 当前 panorama（RGBA16F 2D）
    ComPtr<ID3D11ShaderResourceView> m_environmentSrv;
    std::uint64_t m_environmentRevision = 0;      // 0 = 无环境（UpdateEnvironmentPanorama 语义）
    ComPtr<ID3D11SamplerState> m_iblSamplerState; // s1：IBL 线性 clamp
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11RasterizerState> m_mirroredRasterizerState;
    ComPtr<ID3D11RasterizerState> m_noCullRasterizerState; // skybox / tone map 共用
    ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    ComPtr<ID3D11DepthStencilState> m_skyboxDepthState; // 测试开启、不写深度、LESS_EQUAL
    ComPtr<ID3D11DepthStencilState> m_postDepthState;   // 深度关闭（tone map）
    Rhi::D3D11::D3D11AssetCache m_assetCache;
    float m_exposureEv = 0.0F; // 05 篇固定曝光基线（相对 EV，默认 0）

    // 07 篇截图：staging ring、待处理路径、最近完成结果与元数据辅助字段。
    Rhi::D3D11::D3D11Screenshot m_screenshot;
    std::filesystem::path m_pendingScreenshotPath;
    Rhi::D3D11::ScreenshotResult m_completedScreenshot;
    bool m_screenshotCompleted = false;
    std::uint32_t m_presentInterval = 1; // Present 间隔（0 = 关闭垂直同步）
    std::string m_gpuDescription;        // 适配器 + feature level（截图元数据 gpu 字段）
    std::uint32_t m_debugLayerMessageCount = 0;

    // 08 篇性能基线：GPU 计时 ring、sample 计数与 Present CPU 耗时。
    Rhi::D3D11::D3D11GpuTimer m_gpuTimer;
    GpuInfo m_gpuInfo;
    Rhi::D3D11::GpuSampleCounters m_gpuCounters;
    Rhi::D3D11::GpuFrameTiming m_gpuSample;
    bool m_gpuSampleReady = false;
    bool m_gpuFrameActive = false; // 本帧是否成功占用了 query ring（ring 满时为 false）
    std::uint32_t m_gpuSkippedFrames = 0;
    double m_presentCpuMicroseconds = 0.0;
    LONGLONG m_qpcFrequency = 1; // 启动时缓存（08 篇：QPC 频率只查一次）

    D3D11_VIEWPORT m_viewport{};
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    bool m_occluded = false;
};

// 构造：委托给内部 Impl 完成全部初始化，异常会向上传播。
D3D11Renderer::D3D11Renderer(void* nativeWindow, const std::uint32_t width, const std::uint32_t height,
                             std::filesystem::path shaderDirectory, const bool enableDebugLayer)
    : m_impl{std::make_unique<Impl>(nativeWindow, width, height, std::move(shaderDirectory), enableDebugLayer)}
{
}

D3D11Renderer::~D3D11Renderer() = default;

// 对外方法均为薄转发，真正的逻辑在 Impl 内。
void D3D11Renderer::Resize(const std::uint32_t width, const std::uint32_t height)
{
    m_impl->Resize(width, height);
}

bool D3D11Renderer::Render(const World::RenderPacket& packet, const Assets::AssetManager& assets)
{
    return m_impl->Render(packet, assets);
}

void D3D11Renderer::SetDebugMode(const std::uint32_t debugMode)
{
    m_impl->SetDebugMode(debugMode);
}

void D3D11Renderer::SetExposureEv(const float exposureEv)
{
    m_impl->SetExposureEv(exposureEv);
}

float D3D11Renderer::ExposureEv() const noexcept
{
    return m_impl->ExposureEv();
}

bool D3D11Renderer::UpdateEnvironmentPanorama(const std::uint32_t width, const std::uint32_t height,
                                              const std::uint16_t* rgba16HalfPixels, const std::uint64_t revision)
{
    return m_impl->UpdateEnvironmentPanoramaImpl(width, height, rgba16HalfPixels, revision);
}

std::uint32_t D3D11Renderer::Width() const noexcept
{
    return m_impl->Width();
}

std::uint32_t D3D11Renderer::Height() const noexcept
{
    return m_impl->Height();
}

void D3D11Renderer::RequestScreenshot(const std::filesystem::path& pngPath)
{
    m_impl->RequestScreenshot(pngPath);
}

bool D3D11Renderer::PollScreenshot(ScreenshotResult& out)
{
    return m_impl->PollScreenshot(out);
}

bool D3D11Renderer::IblReady() const noexcept
{
    return m_impl->IblReady();
}

void D3D11Renderer::SetPresentInterval(const std::uint32_t interval) noexcept
{
    m_impl->SetPresentInterval(interval);
}

std::string D3D11Renderer::GpuDescription() const noexcept
{
    return m_impl->GpuDescription();
}

std::uint32_t D3D11Renderer::DebugLayerMessageCount() const noexcept
{
    return m_impl->DebugLayerMessageCount();
}

D3D11Renderer::GpuInfo D3D11Renderer::GetGpuInfo() const noexcept
{
    return m_impl->GetGpuInfo();
}

double D3D11Renderer::LastPresentCpuMicroseconds() const noexcept
{
    return m_impl->LastPresentCpuMicroseconds();
}

bool D3D11Renderer::PollGpuSample(Rhi::D3D11::GpuFrameTiming& out) noexcept
{
    return m_impl->PollGpuSample(out);
}

Rhi::D3D11::GpuSampleCounters D3D11Renderer::GpuSampleCounters() const noexcept
{
    return m_impl->GpuSampleCounters();
}

std::uint32_t D3D11Renderer::GpuSkippedFrameCount() const noexcept
{
    return m_impl->GpuSkippedFrameCount();
}
} // namespace MiniEngine
