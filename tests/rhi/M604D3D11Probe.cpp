#include "M604D3D11Probe.h"
#include "D3D11Capabilities.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <sstream>
#include <stdexcept>
#include <wrl/client.h>

namespace MiniEngine::Rhi::M604
{
using Microsoft::WRL::ComPtr;
namespace
{
void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        std::ostringstream message;
        message << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result);
        throw std::runtime_error(message.str());
    }
}
DXGI_FORMAT VertexType(VertexFormat f)
{
    switch (f)
    {
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    throw std::runtime_error("unknown vertex format");
}
const char* Semantic(VertexSemantic s)
{
    switch (s)
    {
    case VertexSemantic::Position:
        return "POSITION";
    case VertexSemantic::Normal:
        return "NORMAL";
    case VertexSemantic::Tangent:
        return "TANGENT";
    case VertexSemantic::TexCoord:
        return "TEXCOORD";
    case VertexSemantic::Color:
        return "COLOR";
    }
    throw std::runtime_error("unknown semantic");
}
D3D11_COMPARISON_FUNC Comparison(CompareOp op)
{
    switch (op)
    {
    case CompareOp::Never:
        return D3D11_COMPARISON_NEVER;
    case CompareOp::Less:
        return D3D11_COMPARISON_LESS;
    case CompareOp::LessEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOp::Equal:
        return D3D11_COMPARISON_EQUAL;
    case CompareOp::GreaterEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOp::Greater:
        return D3D11_COMPARISON_GREATER;
    case CompareOp::Always:
        return D3D11_COMPARISON_ALWAYS;
    }
    throw std::runtime_error("unknown comparison");
}
} // namespace
struct D3D11Probe::Impl
{
    struct Pipeline : ResourcePayload
    {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> input;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11Texture2D> hdr, target, staging;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11SamplerState> sampler;
        bool tone = false;
    };
    struct M606Resources final
    {
        UINT width = 0;
        UINT height = 0;
        std::uint64_t completion = 0;
        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> dsv;
        ComPtr<ID3D11ShaderResourceView> depthSrv;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Buffer> toneConstants;
        ComPtr<ID3D11Buffer> depthConstants;
        ComPtr<ID3D11Buffer> vertices;
        ComPtr<ID3D11Buffer> indices;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> timestampBegin;
        ComPtr<ID3D11Query> timestampEnd;
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> messages;
    ComPtr<ID3D11Query> event;
    RhiCapabilities capabilities;
    M606Resources m606;
    std::uint64_t created = 0;
    explicit Impl(bool warp)
    {
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual{};
        Check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                D3D11_CREATE_DEVICE_DEBUG, requested, 1, D3D11_SDK_VERSION, &device, &actual, &context),
              "create D3D11 probe device");
        if (actual < D3D_FEATURE_LEVEL_11_0 || !(device->GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG))
            throw std::runtime_error("requested D3D11 debug validation unavailable");
        Check(device.As(&messages), "D3D11 InfoQueue");
        // 无人值守的 legacy probe 必须先收集真实诊断；关闭 break 不会过滤消息，
        // CheckClean 仍会把 WARNING/ERROR/CORRUPTION 全部视为失败。
        Check(messages->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE),
              "D3D11 InfoQueue break corruption");
        Check(messages->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE), "D3D11 InfoQueue break error");
        Check(messages->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE), "D3D11 InfoQueue break warning");
        capabilities = D3D11::QueryCapabilities(*device.Get(), *context.Get());
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        Check(device->CreateQuery(&query, &event), "D3D11 completion EVENT");
        ++created;
        CheckClean();
    }
    ~Impl()
    {
        if (context)
            context->ClearState();
    }
    void CheckClean()
    {
        std::string failures;
        const auto count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            Check(messages->GetMessage(i, nullptr, &size), "InfoQueue size");
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            Check(messages->GetMessage(i, message, &size), "InfoQueue message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
                failures += std::to_string(message->ID) + ": " +
                            std::string(message->pDescription, message->DescriptionByteLength) + "\n";
        }
        messages->ClearStoredMessages();
        if (!failures.empty())
            throw std::runtime_error("M604 D3D11 normal diagnostics: " + failures);
    }
    void Census()
    {
        CheckClean();
        context->ClearState();
        context->Flush();
        ComPtr<ID3D11Debug> debug;
        Check(device.As(&debug), "D3D11 debug device");
        Check(debug->ReportLiveDeviceObjects(
                  static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL)),
              "D3D11 live-object census");
        bool sawDevice = false;
        std::string leaks;
        const std::array ids{D3D11_MESSAGE_ID_LIVE_BUFFER,
                             D3D11_MESSAGE_ID_LIVE_TEXTURE2D,
                             D3D11_MESSAGE_ID_LIVE_SHADERRESOURCEVIEW,
                             D3D11_MESSAGE_ID_LIVE_RENDERTARGETVIEW,
                             D3D11_MESSAGE_ID_LIVE_VERTEXSHADER,
                             D3D11_MESSAGE_ID_LIVE_PIXELSHADER,
                             D3D11_MESSAGE_ID_LIVE_INPUTLAYOUT,
                             D3D11_MESSAGE_ID_LIVE_SAMPLER,
                             D3D11_MESSAGE_ID_LIVE_BLENDSTATE,
                             D3D11_MESSAGE_ID_LIVE_DEPTHSTENCILSTATE,
                             D3D11_MESSAGE_ID_LIVE_RASTERIZERSTATE,
                             D3D11_MESSAGE_ID_LIVE_BUFFER_WIN7,
                             D3D11_MESSAGE_ID_LIVE_TEXTURE2D_WIN7,
                             D3D11_MESSAGE_ID_LIVE_SHADERRESOURCEVIEW_WIN7,
                             D3D11_MESSAGE_ID_LIVE_RENDERTARGETVIEW_WIN7,
                             D3D11_MESSAGE_ID_LIVE_VERTEXSHADER_WIN7,
                             D3D11_MESSAGE_ID_LIVE_PIXELSHADER_WIN7,
                             D3D11_MESSAGE_ID_LIVE_INPUTLAYOUT_WIN7,
                             D3D11_MESSAGE_ID_LIVE_SAMPLER_WIN7,
                             D3D11_MESSAGE_ID_LIVE_BLENDSTATE_WIN7,
                             D3D11_MESSAGE_ID_LIVE_DEPTHSTENCILSTATE_WIN7,
                             D3D11_MESSAGE_ID_LIVE_RASTERIZERSTATE_WIN7};
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0;
            Check(messages->GetMessage(i, nullptr, &size), "census size");
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            Check(messages->GetMessage(i, message, &size), "census message");
            sawDevice |=
                message->ID == D3D11_MESSAGE_ID_LIVE_DEVICE || message->ID == D3D11_MESSAGE_ID_LIVE_DEVICE_WIN7;
            if (std::find(ids.begin(), ids.end(), message->ID) != ids.end())
                leaks += std::string(message->pDescription, message->DescriptionByteLength) + "\n";
        }
        messages->ClearStoredMessages();
        if (!sawDevice || !leaks.empty())
            throw std::runtime_error("D3D11 pipeline census missing or leaking: " + leaks);
    }
    void PrepareTone(Pipeline& p)
    {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = 1;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_IMMUTABLE;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::array<float, 4> hdr{4, 2, 1, 1};
        const D3D11_SUBRESOURCE_DATA data{hdr.data(), 16, 16};
        Check(device->CreateTexture2D(&texture, &data, &p.hdr), "HDR texture");
        ++created;
        Check(device->CreateShaderResourceView(p.hdr.Get(), nullptr, &p.srv), "HDR SRV");
        ++created;
        texture.Width = texture.Height = 4;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&texture, nullptr, &p.target), "ToneMap target");
        ++created;
        Check(device->CreateRenderTargetView(p.target.Get(), nullptr, &p.rtv), "ToneMap RTV");
        ++created;
        texture.Usage = D3D11_USAGE_STAGING;
        texture.BindFlags = 0;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&texture, nullptr, &p.staging), "ToneMap staging");
        ++created;
        const std::array<float, 4> constants{0, 0, 0.25F, 0.25F};
        const D3D11_SUBRESOURCE_DATA cbData{constants.data(), 0, 0};
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 16;
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&buffer, &cbData, &p.constants), "ToneMap CB");
        ++created;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MinLOD = 0;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sampler, &p.sampler), "ToneMap sampler s1");
        ++created;
    }
    void EnsureM606(UINT width, UINT height)
    {
        if (width == 0 || height == 0)
            throw std::invalid_argument("M606 D3D11 extent must be positive");
        if (m606.width == width && m606.height == height && m606.target && m606.staging)
            return;

        // 每次 M606 绘制均等待真实 EVENT；先解除 context 引用，再释放尺寸相关对象，
        // 避免 resize 后仍绑定旧 view。
        context->ClearState();
        context->Flush();
        const auto completion = m606.completion;
        m606 = {};
        m606.completion = completion;
        m606.width = width;
        m606.height = height;

        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = width;
        depth.Height = height;
        depth.MipLevels = depth.ArraySize = 1;
        depth.Format = DXGI_FORMAT_R32_TYPELESS;
        depth.SampleDesc.Count = 1;
        depth.Usage = D3D11_USAGE_DEFAULT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&depth, nullptr, &m606.depth), "M606 D3D11 depth");
        ++created;

        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsv.Texture2D.MipSlice = 0;
        Check(device->CreateDepthStencilView(m606.depth.Get(), &dsv, &m606.dsv), "M606 D3D11 DSV");
        ++created;

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        Check(device->CreateShaderResourceView(m606.depth.Get(), &srv, &m606.depthSrv), "M606 D3D11 depth SRV");
        ++created;

        D3D11_TEXTURE2D_DESC target{};
        target.Width = width;
        target.Height = height;
        target.MipLevels = target.ArraySize = 1;
        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target.SampleDesc.Count = 1;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&target, nullptr, &m606.target), "M606 D3D11 target");
        ++created;
        Check(device->CreateRenderTargetView(m606.target.Get(), nullptr, &m606.rtv), "M606 D3D11 RTV");
        ++created;

        target.Usage = D3D11_USAGE_STAGING;
        target.BindFlags = 0;
        target.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&target, nullptr, &m606.staging), "M606 D3D11 staging");
        ++created;

        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 16;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&constants, nullptr, &m606.toneConstants), "M606 D3D11 tone constants");
        ++created;

        constants.ByteWidth = 208;
        const std::array<float, 52> identity = []
        {
            std::array<float, 52> value{};
            for (std::size_t matrix = 0; matrix < 3; ++matrix)
                for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
                    value[matrix * 16 + diagonal * 5] = 1.0F;
            value[48] = 1.0F;
            return value;
        }();
        const D3D11_SUBRESOURCE_DATA objectData{identity.data(), 0, 0};
        constants.Usage = D3D11_USAGE_IMMUTABLE;
        Check(device->CreateBuffer(&constants, &objectData, &m606.depthConstants), "M606 D3D11 depth constants");
        ++created;

        const std::array<float, 9> vertices{-0.75F, -0.75F, 0.25F, 0.0F, 0.75F, 0.25F, 0.75F, -0.75F, 0.25F};
        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(float));
        vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA vertexData{vertices.data(), 0, 0};
        Check(device->CreateBuffer(&vertexDesc, &vertexData, &m606.vertices), "M606 D3D11 vertices");
        ++created;

        const std::array<std::uint16_t, 3> indices{0, 1, 2};
        D3D11_BUFFER_DESC indexDesc{};
        indexDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA indexData{indices.data(), 0, 0};
        Check(device->CreateBuffer(&indexDesc, &indexData, &m606.indices), "M606 D3D11 indices");
        ++created;

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MinLOD = 0;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sampler, &m606.sampler), "M606 D3D11 sampler");
        ++created;

        D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC timestampDesc{D3D11_QUERY_TIMESTAMP, 0};
        Check(device->CreateQuery(&disjointDesc, &m606.disjoint), "M606 D3D11 timestamp disjoint");
        ++created;
        Check(device->CreateQuery(&timestampDesc, &m606.timestampBegin), "M606 D3D11 timestamp begin");
        ++created;
        Check(device->CreateQuery(&timestampDesc, &m606.timestampEnd), "M606 D3D11 timestamp end");
        ++created;
        CheckClean();
    }
    void WaitM606Event()
    {
        const auto deadline = GetTickCount64() + 5000;
        BOOL complete = FALSE;
        while (true)
        {
            const auto result =
                context->GetData(event.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            Check(result, "M606 D3D11 EVENT result");
            if (result == S_OK && complete)
                return;
            if (GetTickCount64() > deadline)
                throw std::runtime_error("M606 D3D11 EVENT timeout");
            Sleep(1);
        }
    }
    void ReadM606Timestamps(M606Readback& result)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        Check(context->GetData(m606.disjoint.Get(), &disjoint, sizeof(disjoint), 0), "M606 D3D11 disjoint read");
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        Check(context->GetData(m606.timestampBegin.Get(), &begin, sizeof(begin), 0), "M606 D3D11 timestamp begin read");
        Check(context->GetData(m606.timestampEnd.Get(), &end, sizeof(end), 0), "M606 D3D11 timestamp end read");
        result.timestampBegin = begin;
        result.timestampEnd = end;
        result.timestampFrequency = disjoint.Frequency;
        result.timestampDisjoint = disjoint.Disjoint != FALSE;
    }
    M606Readback ReadM606()
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(m606.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "M606 D3D11 readback map");
        const std::size_t rowBytes = static_cast<std::size_t>(m606.width) * 4;
        M606Readback result;
        result.width = m606.width;
        result.height = m606.height;
        result.completion = m606.completion;
        result.rgba.resize(rowBytes * m606.height);
        for (UINT row = 0; row < m606.height; ++row)
            std::memcpy(result.rgba.data() + static_cast<std::size_t>(row) * rowBytes,
                        static_cast<const std::byte*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                        rowBytes);
        context->Unmap(m606.staging.Get(), 0);
        context->ClearState();
        CheckClean();
        return result;
    }
    M606Readback DrawM606Clear(UINT width, UINT height)
    {
        EnsureM606(width, height);
        const auto before = created;
        context->ClearState();
        context->Begin(m606.disjoint.Get());
        context->End(m606.timestampBegin.Get());
        const float clear[]{0.04F, 0.08F, 0.14F, 1.0F};
        ID3D11RenderTargetView* rtv = m606.rtv.Get();
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetViewports(1, &viewport);
        context->ClearRenderTargetView(rtv, clear);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(m606.staging.Get(), m606.target.Get());
        context->End(m606.timestampEnd.Get());
        context->End(m606.disjoint.Get());
        context->End(event.Get());
        context->Flush();
        WaitM606Event();
        ++m606.completion;
        auto result = ReadM606();
        ReadM606Timestamps(result);
        if (created != before)
            throw std::runtime_error("native creation in M606 D3D11 clear");
        return result;
    }
    M606Readback DrawM606DepthToneMap(ResourcePayload& depthPayload, ResourcePayload& tonePayload, UINT width,
                                      UINT height, float exposure)
    {
        EnsureM606(width, height);
        auto& depth = dynamic_cast<Pipeline&>(depthPayload);
        auto& tone = dynamic_cast<Pipeline&>(tonePayload);
        if (!depth.vs || !depth.input || depth.ps || !tone.tone || !tone.vs || !tone.ps)
            throw std::runtime_error("M606 D3D11 pipeline roles are invalid");
        const auto before = created;
        context->ClearState();
        context->Begin(m606.disjoint.Get());
        context->End(m606.timestampBegin.Get());
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->IASetInputLayout(depth.input.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride = static_cast<UINT>(sizeof(float) * 3);
            UINT offset = 0;
            ID3D11Buffer* vertex = m606.vertices.Get();
            context->IASetVertexBuffers(0, 1, &vertex, &stride, &offset);
            context->IASetIndexBuffer(m606.indices.Get(), DXGI_FORMAT_R16_UINT, 0);
            context->VSSetShader(depth.vs.Get(), nullptr, 0);
            ID3D11Buffer* depthConstants = m606.depthConstants.Get();
            context->VSSetConstantBuffers(1, 1, &depthConstants);
            context->PSSetShader(nullptr, nullptr, 0);
            context->RSSetState(depth.raster.Get());
            context->OMSetDepthStencilState(depth.depth.Get(), 0);
            const float factors[]{1, 1, 1, 1};
            context->OMSetBlendState(depth.blend.Get(), factors, 0xFFFFFFFFU);
            ID3D11DepthStencilView* dsv = m606.dsv.Get();
            context->OMSetRenderTargets(0, nullptr, dsv);
            context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0F, 0);
            context->DrawIndexed(3, 0, 0);

            // 采样同一资源前显式解除输出绑定，避免依赖 runtime 隐式处理 hazard。
            context->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11Buffer* nullConstant = nullptr;
            context->VSSetConstantBuffers(1, 1, &nullConstant);
            context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
            context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
            context->IASetInputLayout(tone.input.Get());
            context->VSSetShader(tone.vs.Get(), nullptr, 0);
            context->PSSetShader(tone.ps.Get(), nullptr, 0);
            context->RSSetState(tone.raster.Get());
            context->OMSetDepthStencilState(tone.depth.Get(), 0);
            context->OMSetBlendState(tone.blend.Get(), factors, 0xFFFFFFFFU);
            const std::array<float, 4> toneConstants{exposure, 0.0F, 1.0F / static_cast<float>(width),
                                                     1.0F / static_cast<float>(height)};
            context->UpdateSubresource(m606.toneConstants.Get(), 0, nullptr, toneConstants.data(), 0, 0);
            ID3D11Buffer* cb = m606.toneConstants.Get();
            context->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView* srv = m606.depthSrv.Get();
            context->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState* sampler = m606.sampler.Get();
            context->PSSetSamplers(1, 1, &sampler);
            ID3D11RenderTargetView* rtv = m606.rtv.Get();
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->ClearRenderTargetView(rtv, factors);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            context->PSSetShaderResources(0, 1, &nullSrv);
        }
        context->CopyResource(m606.staging.Get(), m606.target.Get());
        context->End(m606.timestampEnd.Get());
        context->End(m606.disjoint.Get());
        context->End(event.Get());
        context->Flush();
        WaitM606Event();
        ++m606.completion;
        auto result = ReadM606();
        ReadM606Timestamps(result);
        if (created != before)
            throw std::runtime_error("native creation in M606 D3D11 depth/tone");
        return result;
    }
    void ResetM606()
    {
        if (!m606.target && !m606.depth)
            return;
        context->ClearState();
        context->Flush();
        m606 = {};
    }
    std::unique_ptr<ResourcePayload> Create(const ShaderDesc& vs, const ShaderDesc* ps, const GraphicsPipelineDesc& p)
    {
        auto result = std::make_unique<Pipeline>();
        Check(device->CreateVertexShader(vs.bytecode.data(), vs.bytecode.size(), nullptr, &result->vs),
              "M604 vertex shader");
        ++created;
        if (ps)
        {
            Check(device->CreatePixelShader(ps->bytecode.data(), ps->bytecode.size(), nullptr, &result->ps),
                  "M604 pixel shader");
            ++created;
        }
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        for (const auto& a : p.vertexAttributes)
            elements.push_back({Semantic(a.semantic), a.semanticIndex, VertexType(a.format), a.bufferSlot, a.offset,
                                a.instanceStepRate ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA,
                                a.instanceStepRate});
        if (!elements.empty())
        {
            Check(device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), vs.bytecode.data(),
                                            vs.bytecode.size(), &result->input),
                  "M604 input layout");
            ++created;
        }
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = p.cullMode == CullMode::None    ? D3D11_CULL_NONE
                          : p.cullMode == CullMode::Front ? D3D11_CULL_FRONT
                                                          : D3D11_CULL_BACK;
        raster.FrontCounterClockwise = p.frontFace == FrontFace::CounterClockwise;
        raster.DepthBias = p.depthBias;
        raster.DepthBiasClamp = p.depthBiasClamp;
        raster.SlopeScaledDepthBias = p.slopeScaledDepthBias;
        raster.DepthClipEnable = p.depthClip;
        Check(device->CreateRasterizerState(&raster, &result->raster), "M604 raster state");
        ++created;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = p.depthTest;
        depth.DepthWriteMask = p.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = Comparison(p.depthCompare);
        depth.StencilReadMask = depth.StencilWriteMask = 255;
        depth.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                           D3D11_COMPARISON_ALWAYS};
        depth.BackFace = depth.FrontFace;
        Check(device->CreateDepthStencilState(&depth, &result->depth), "M604 depth state");
        ++created;
        D3D11_BLEND_DESC blend{};
        for (auto& rt : blend.RenderTarget)
        {
            rt.BlendEnable = p.alphaBlend;
            rt.SrcBlend = p.alphaBlend ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_ONE;
            rt.DestBlend = p.alphaBlend ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_ZERO;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = p.alphaBlend ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        Check(device->CreateBlendState(&blend, &result->blend), "M604 blend state");
        ++created;
        result->tone = p.vertexAttributes.empty() && p.colorAttachmentCount == 1 &&
                       p.colorFormats[0] == Format::Rgba8Unorm && p.depthFormat == Format::Unknown;
        if (result->tone)
            PrepareTone(*result);
        CheckClean();
        return result;
    }
    std::vector<std::byte> Draw(ResourcePayload& value)
    {
        auto& p = dynamic_cast<Pipeline&>(value);
        if (!p.tone)
            throw std::runtime_error("fixed smoke only draws ToneMap");
        const auto before = created;
        context->ClearState();
        context->IASetInputLayout(p.input.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(p.vs.Get(), nullptr, 0);
        context->PSSetShader(p.ps.Get(), nullptr, 0);
        context->RSSetState(p.raster.Get());
        context->OMSetDepthStencilState(p.depth.Get(), 0);
        const float factors[]{1, 1, 1, 1};
        context->OMSetBlendState(p.blend.Get(), factors, 0xFFFFFFFFU);
        ID3D11Buffer* cb = p.constants.Get();
        context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* srv = p.srv.Get();
        context->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* sampler = p.sampler.Get();
        context->PSSetSamplers(1, 1, &sampler);
        ID3D11RenderTargetView* rtv = p.rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
        context->RSSetViewports(1, &viewport);
        const float clear[4]{};
        context->ClearRenderTargetView(rtv, clear);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(p.staging.Get(), p.target.Get());
        context->End(event.Get());
        context->Flush();
        const auto deadline = GetTickCount64() + 5000;
        BOOL complete = FALSE;
        while (true)
        {
            const auto result =
                context->GetData(event.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            Check(result, "EVENT result");
            if (result == S_OK && complete)
                break;
            if (GetTickCount64() > deadline)
                throw std::runtime_error("D3D11 EVENT timeout");
            Sleep(1);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(p.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "ToneMap staging read");
        std::vector<std::byte> pixels(64);
        for (std::size_t row = 0; row < 4; ++row)
            std::memcpy(pixels.data() + row * 16, static_cast<const std::byte*>(mapped.pData) + row * mapped.RowPitch,
                        16);
        context->Unmap(p.staging.Get(), 0);
        context->ClearState();
        CheckClean();
        if (created != before)
            throw std::runtime_error("native creation in D3D11 Draw");
        return pixels;
    }
};
D3D11Probe::D3D11Probe(bool warp) : m_impl(std::make_unique<Impl>(warp))
{
}
D3D11Probe::~D3D11Probe() = default;
std::unique_ptr<ResourcePayload> D3D11Probe::CreatePipeline(const ShaderDesc& vs, const ShaderDesc* ps,
                                                            const GraphicsPipelineDesc& p)
{
    return m_impl->Create(vs, ps, p);
}
std::vector<std::byte> D3D11Probe::DrawToneMap(ResourcePayload& payload)
{
    return m_impl->Draw(payload);
}
std::uint64_t D3D11Probe::NativeCreationCount() const
{
    return m_impl->created;
}
void D3D11Probe::CheckClean()
{
    m_impl->CheckClean();
}
M606Readback D3D11Probe::DrawM606Clear(std::uint32_t width, std::uint32_t height)
{
    return m_impl->DrawM606Clear(width, height);
}
M606Readback D3D11Probe::DrawM606DepthToneMap(ResourcePayload& depthPipeline, ResourcePayload& tonePipeline,
                                              std::uint32_t width, std::uint32_t height, float exposure)
{
    return m_impl->DrawM606DepthToneMap(depthPipeline, tonePipeline, width, height, exposure);
}
void D3D11Probe::ResetM606()
{
    m_impl->ResetM606();
}
void D3D11Probe::CheckNoPipelineResources()
{
    m_impl->Census();
}
RhiCapabilities D3D11Probe::Capabilities() const
{
    return m_impl->capabilities;
}
} // namespace MiniEngine::Rhi::M604
