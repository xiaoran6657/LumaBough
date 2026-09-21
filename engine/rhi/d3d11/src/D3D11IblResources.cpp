// ============================================================================
// D3D11IblResources.cpp — IBL 四资源生成管线的实现
// 里程碑：M4（04 篇「Cube resource」「Face 方向」「Readiness 与失败」）
// 职责：驱动四个生成 shader 完成 conversion → mip 链 → irradiance → prefilter →
//       LUT → finite 校验。关键契约：
//   1. Cube 资源 ArraySize=6 + MISC_TEXTURECUBE；每 (face,mip) 一个
//      TEXTURE2DARRAY RTV；整体一个 TEXTURECUBE SRV（04 篇表格）。
//   2. Face 方向固定 D3D cubemap 表（+X/-X/+Y/-Y/+Z/-Z 的 look/up），90° FOV、
//      aspect 1、eye 在原点——与 EquirectToCube.hlsl 的 UV 公式共同构成可验证契约。
//   3. mip 链生成是**确定性 full-screen face downsample pass**（PSDownsample，
//      单 tap SampleLevel(mip-1) 双线性 = 确定性 2×2 box）：每级先把上一 mip
//      CopySubresource 到内部 mipSource cube，再从 mipSource SRV 采样写 envCube
//      的 RTV——禁止同一 cube 同时绑 RTV+SRV（D3D11 hazard）。不使用
//      GenerateMips（04 篇要求确定性 pass，且 M4-03 已确立"GPU GenerateMips
//      不进正式资产"的同源纪律）。
//   4. finite 校验：irradiance 六面 + LUT 全图 staging readback，拒绝 NaN/Inf/负；
//      environment/prefilter 体积大不做全读，依赖 creation 校验与 debug layer。
//   5. 每阶段失败即丢弃并返回 false；HRESULT/stage/face/mip 全部进 error 文本。
// 关联：engine/rhi/d3d11/src/D3D11Renderer.cpp（生成入口与 pass 编排）
//       shaders/d3d11/EquirectToCube.hlsl（VSMain/PSMain/PSDownsample）
//       shaders/d3d11/IrradianceConvolution.hlsl / PrefilterEnvironment.hlsl /
//                IntegrateBrdf.hlsl
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11IblResources.h>

#include "D3D11Error.h"
#include "D3D11ShaderCompiler.h"

#include <MiniEngine/Core/Log.h>

#include <DirectXMath.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>

namespace MiniEngine::Rhi::D3D11
{
namespace
{
using Microsoft::WRL::ComPtr;

// ---- baseline profile（04 篇「固定 IBL profile」表格，禁止运行时静默更改） ----
constexpr std::uint32_t kEnvironmentSize = 512; // full chain（10 mips）
constexpr std::uint32_t kIrradianceSize = 32;   // 1 mip
constexpr std::uint32_t kPrefilterSize = 128;   // full chain（8 mips）
constexpr std::uint32_t kBrdfLutSize = 256;     // 1 mip

// full mip 链长：max(w,h) 每次减半直到 1（与 TextureFormatV2 的 IsValid 同式）。
std::uint32_t FullMipCount(const std::uint32_t size)
{
    std::uint32_t count = 1;
    for (std::uint32_t extent = size; extent > 1; extent >>= 1U)
    {
        ++count;
    }
    return count;
}

// 04 篇「Face 方向」冻结表：D3D cubemap 六面的 look/up。数组序 = SRV face 序。
struct CubeFaceBasis final
{
    float look[3];
    float up[3];
    const char* name; // RenderDoc 判读用（M4.IBL.Environment.FaceN.MipM）
};
constexpr std::array<CubeFaceBasis, 6> kCubeFaces{
    CubeFaceBasis{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, "+X"},
    CubeFaceBasis{{-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, "-X"},
    CubeFaceBasis{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, "+Y"},
    CubeFaceBasis{{0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, "-Y"},
    CubeFaceBasis{{0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}, "+Z"},
    CubeFaceBasis{{0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, "-Z"},
};

// 生成 pass 共用的 cube 几何：单位立方体 24 顶点 + 36 索引。VS 把 position 当
// 方向向量；绕序配合 CULL_NONE 光栅化（生成 pass 内无共面图元，不依赖绕序）。
struct CubeVertex final
{
    float x;
    float y;
    float z;
};
constexpr std::array<CubeVertex, 24> kCubeVertices{
    // -Z 面
    CubeVertex{-1.0F, -1.0F, -1.0F},
    CubeVertex{-1.0F, 1.0F, -1.0F},
    CubeVertex{1.0F, 1.0F, -1.0F},
    CubeVertex{1.0F, -1.0F, -1.0F},
    // +Z 面
    CubeVertex{1.0F, -1.0F, 1.0F},
    CubeVertex{1.0F, 1.0F, 1.0F},
    CubeVertex{-1.0F, 1.0F, 1.0F},
    CubeVertex{-1.0F, -1.0F, 1.0F},
    // +X 面
    CubeVertex{1.0F, -1.0F, -1.0F},
    CubeVertex{1.0F, 1.0F, -1.0F},
    CubeVertex{1.0F, 1.0F, 1.0F},
    CubeVertex{1.0F, -1.0F, 1.0F},
    // -X 面
    CubeVertex{-1.0F, -1.0F, 1.0F},
    CubeVertex{-1.0F, 1.0F, 1.0F},
    CubeVertex{-1.0F, 1.0F, -1.0F},
    CubeVertex{-1.0F, -1.0F, -1.0F},
    // +Y 面
    CubeVertex{-1.0F, 1.0F, -1.0F},
    CubeVertex{-1.0F, 1.0F, 1.0F},
    CubeVertex{1.0F, 1.0F, 1.0F},
    CubeVertex{1.0F, 1.0F, -1.0F},
    // -Y 面
    CubeVertex{-1.0F, -1.0F, 1.0F},
    CubeVertex{-1.0F, -1.0F, -1.0F},
    CubeVertex{1.0F, -1.0F, -1.0F},
    CubeVertex{1.0F, -1.0F, 1.0F},
};
constexpr std::array<std::uint16_t, 36> kCubeIndices{
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

// per-(face,mip) 的 TEXTURE2DARRAY RTV（04 篇：cube 资源的 RTV 走 array 维度）。
ComPtr<ID3D11RenderTargetView> CreateCubeFaceRtv(ID3D11Device& device, ID3D11Texture2D& cube, const std::uint32_t face,
                                                 const std::uint32_t mip, const std::string& name)
{
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    description.Texture2DArray.MipSlice = mip;
    description.Texture2DArray.FirstArraySlice = face;
    description.Texture2DArray.ArraySize = 1;

    ComPtr<ID3D11RenderTargetView> view;
    ThrowIfFailed(device.CreateRenderTargetView(&cube, &description, view.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateRenderTargetView(cube face)");
    SetDebugObjectName(view.Get(), name.c_str());
    return view;
}

// 整体 TEXTURECUBE SRV（生成 shader 与运行时 PBR 共用同一样式）。
ComPtr<ID3D11ShaderResourceView> CreateCubeSrv(ID3D11Device& device, ID3D11Texture2D& cube,
                                               const std::uint32_t mipCount, const char* name)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    description.TextureCube.MostDetailedMip = 0;
    description.TextureCube.MipLevels = mipCount;

    ComPtr<ID3D11ShaderResourceView> view;
    ThrowIfFailed(device.CreateShaderResourceView(&cube, &description, view.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateShaderResourceView(cube)");
    SetDebugObjectName(view.Get(), name);
    return view;
}

// RGBA16F cube 资源（04 篇 Cube resource 表格逐字段对应）。
ComPtr<ID3D11Texture2D> CreateCubeTexture(ID3D11Device& device, const std::uint32_t faceSize,
                                          const std::uint32_t mipCount, const char* name)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = faceSize;
    description.Height = faceSize;
    description.MipLevels = mipCount;
    description.ArraySize = 6; // 04 篇硬约束：必须为 6
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device.CreateTexture2D(&description, nullptr, texture.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateTexture2D(ibl cube)");
    SetDebugObjectName(texture.Get(), name);
    return texture;
}

// 生成期一次性上下文：shader/几何/状态与阶段顺序的全部驱动逻辑。
// 生命周期 = BuildTemporary 调用栈内（帧边界单次执行），不驻留到帧间。
struct Generator final
{
    ID3D11Device& device;
    ID3D11DeviceContext& context;
    std::filesystem::path shaderDirectory;
    std::string& error;

    // shader 与一次性资源。
    ComPtr<ID3D11VertexShader> cubeFilterVs; // cube 面 pass 共用 VS（三 shader 同源）
    ComPtr<ID3D11PixelShader> equirectPs;    // 阶段 1
    ComPtr<ID3D11PixelShader> downsamplePs;  // 阶段 2（确定性 mip 链）
    ComPtr<ID3D11PixelShader> irradiancePs;  // 阶段 3
    ComPtr<ID3D11PixelShader> prefilterPs;   // 阶段 4
    ComPtr<ID3D11VertexShader> fullscreenVs; // 阶段 5（SV_VertexID，无布局）
    ComPtr<ID3D11PixelShader> brdfLutPs;
    ComPtr<ID3DBlob> cubeFilterVsBytecode;
    ComPtr<ID3D11InputLayout> cubeInputLayout; // position-only（VS 唯一消费）
    ComPtr<ID3D11Buffer> cubeVertexBuffer;
    ComPtr<ID3D11Buffer> cubeIndexBuffer;
    ComPtr<ID3D11Buffer> faceConstantBuffer; // b0：face VP + 参数（80B DEFAULT）
    ComPtr<ID3D11RasterizerState> cullNoneRasterizer;
    ComPtr<ID3D11BlendState> blendOff;
    ComPtr<ID3D11SamplerState> linearClampSampler; // s1 语义（生成期自持一份）
    ComPtr<ID3D11Texture2D> mipSource;             // 内部 mip 中转 cube（hazard 防护）
    ComPtr<ID3D11ShaderResourceView> mipSourceSrv;

    // 编译一个生成 shader；失败把 FXC 诊断写进 error 并返回 false。
    template <typename ShaderType>
    bool Compile(const char* file, const char* entry, const char* target, ComPtr<ShaderType>& out,
                 ComPtr<ID3DBlob>* bytecodeOut = nullptr)
    {
        const std::filesystem::path path = shaderDirectory / file;
        try
        {
            const ComPtr<ID3DBlob> blob = CompileShader(path, entry, target);
            ThrowIfFailed(CreateShaderHelper(blob, target, out), "IBL shader creation failed");
            if (bytecodeOut != nullptr)
            {
                *bytecodeOut = blob;
            }
            return true;
        }
        catch (const std::exception& caught)
        {
            std::ostringstream stream;
            stream << "IBL stage 'compile' failed: " << file << " [" << entry << "/" << target
                   << "]: " << caught.what();
            error = stream.str();
            return false;
        }
    }

    // CreateShader 按 target 分派（vs_5_0/ps_5_0）；device 成员方法收字节码指针。
    HRESULT CreateShaderHelper(const ComPtr<ID3DBlob>& blob, const char* target, ComPtr<ID3D11VertexShader>& out)
    {
        (void)target;
        return device.CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                         out.ReleaseAndGetAddressOf());
    }

    HRESULT CreateShaderHelper(const ComPtr<ID3DBlob>& blob, const char* target, ComPtr<ID3D11PixelShader>& out)
    {
        (void)target;
        return device.CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                        out.ReleaseAndGetAddressOf());
    }

    bool BuildShaders()
    {
        // cube 面 pass 的 VS 只编译一次（三个 hlsl 的 VSMain 同源，取其一）。
        // downsample 入口在 IrradianceConvolution.hlsl（t0 已是 TextureCube，
        // 与本表其余 cube→cube pass 同资源语义）。
        if (!Compile("IrradianceConvolution.hlsl", "VSMain", "vs_5_0", cubeFilterVs, &cubeFilterVsBytecode) ||
            !Compile("EquirectToCube.hlsl", "PSMain", "ps_5_0", equirectPs) ||
            !Compile("IrradianceConvolution.hlsl", "PSDownsample", "ps_5_0", downsamplePs) ||
            !Compile("IrradianceConvolution.hlsl", "PSMain", "ps_5_0", irradiancePs) ||
            !Compile("PrefilterEnvironment.hlsl", "PSMain", "ps_5_0", prefilterPs) ||
            !Compile("IntegrateBrdf.hlsl", "VSMain", "vs_5_0", fullscreenVs) ||
            !Compile("IntegrateBrdf.hlsl", "PSMain", "ps_5_0", brdfLutPs))
        {
            return false;
        }
        return true;
    }

    bool CreateOneTimeResources()
    {
        // position-only 顶点/索引缓冲（IMMUTABLE，生成期常量）。
        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(sizeof(kCubeVertices));
        vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertexData{&kCubeVertices, 0, 0};
        ThrowIfFailed(device.CreateBuffer(&vertexDesc, &vertexData, cubeVertexBuffer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBuffer(ibl cube vertices)");
        SetDebugObjectName(cubeVertexBuffer.Get(), "M4.IBL.CubeVertices");

        D3D11_BUFFER_DESC indexDesc{};
        indexDesc.ByteWidth = static_cast<UINT>(sizeof(kCubeIndices));
        indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA indexData{&kCubeIndices, 0, 0};
        ThrowIfFailed(device.CreateBuffer(&indexDesc, &indexData, cubeIndexBuffer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBuffer(ibl cube indices)");
        SetDebugObjectName(cubeIndexBuffer.Get(), "M4.IBL.CubeIndices");

        // b0 face 常量（80B DEFAULT；每面 UpdateSubresource 覆盖）。
        D3D11_BUFFER_DESC faceDesc{};
        faceDesc.ByteWidth = 80;
        faceDesc.Usage = D3D11_USAGE_DEFAULT;
        faceDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ThrowIfFailed(device.CreateBuffer(&faceDesc, nullptr, faceConstantBuffer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBuffer(ibl face constants)");
        SetDebugObjectName(faceConstantBuffer.Get(), "M4.IBL.FaceConstants (b0)");

        // cube filter 输入布局（VS 只消费 POSITION，需 VS 字节码创建）。
        const std::array<D3D11_INPUT_ELEMENT_DESC, 1> cubeElements{
            D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ThrowIfFailed(device.CreateInputLayout(cubeElements.data(), static_cast<UINT>(cubeElements.size()),
                                               cubeFilterVsBytecode->GetBufferPointer(),
                                               cubeFilterVsBytecode->GetBufferSize(),
                                               cubeInputLayout.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateInputLayout(ibl cube filter)");
        SetDebugObjectName(cubeInputLayout.Get(), "M4.IBL.CubeFilter InputLayout");

        // IBL 生成光栅化：CULL_NONE——生成 pass 无共面图元，不依赖绕序判定。
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        ThrowIfFailed(device.CreateRasterizerState(&rasterDesc, cullNoneRasterizer.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateRasterizerState(ibl cull-none)");
        SetDebugObjectName(cullNoneRasterizer.Get(), "M4.IBL.CullNone Rasterizer");

        // blend off：**必须显式打开颜色写掩码**——零初始化的 D3D11_BLEND_DESC 的
        // RenderTarget[0].RenderTargetWriteMask 是 0（= 一个通道都不写），而
        // OMSetBlendState(nullptr) 的默认状态才是全写。
        // 缺陷记录（M4-05 取证发现，属 04 篇遗留）：此前用零初始化描述，导致
        // conversion/mip 链/irradiance/prefilter/LUT 五个生成 pass 全部"draw 成功
        // 但不写任何像素"，资源保持零内容；ValidateFinite 只查 NaN/Inf/负值，
        // 全零照样通过（"生成成功"是假象）。
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ThrowIfFailed(device.CreateBlendState(&blendDesc, blendOff.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateBlendState(ibl blend-off)");
        SetDebugObjectName(blendOff.Get(), "M4.IBL.BlendOff");

        // s1 线性 clamp（与运行时 IBL 采样器同语义；生成期自持避免跨对象依赖）。
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ThrowIfFailed(device.CreateSamplerState(&samplerDesc, linearClampSampler.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateSamplerState(ibl linear clamp)");
        return true;
    }

    bool CreateOutputResources(D3D11IblSet& out)
    {
        // environment cube（512² full chain）+ 内部 mip 中转 cube（同尺寸；只作
        // per-level 拷贝源，避免 RTV/SRV 同资源 hazard）。
        const std::uint32_t environmentMips = FullMipCount(kEnvironmentSize);
        out.environment = CreateCubeTexture(device, kEnvironmentSize, environmentMips, "M4.IBL.Environment");
        out.environmentSrv = CreateCubeSrv(device, *out.environment.Get(), environmentMips, "M4.IBL.Environment.SRV");
        mipSource = CreateCubeTexture(device, kEnvironmentSize, environmentMips, "M4.IBL.Internal.MipSource");
        mipSourceSrv = CreateCubeSrv(device, *mipSource.Get(), environmentMips, "M4.IBL.Internal.MipSource.SRV");

        // diffuse irradiance（32²，单 mip）。
        out.irradiance = CreateCubeTexture(device, kIrradianceSize, 1, "M4.IBL.Irradiance");
        out.irradianceSrv = CreateCubeSrv(device, *out.irradiance.Get(), 1, "M4.IBL.Irradiance.SRV");

        // prefiltered specular（128² full chain）。
        out.prefilterMipCount = FullMipCount(kPrefilterSize);
        out.prefiltered = CreateCubeTexture(device, kPrefilterSize, out.prefilterMipCount, "M4.IBL.Prefilter");
        out.prefilteredSrv =
            CreateCubeSrv(device, *out.prefiltered.Get(), out.prefilterMipCount, "M4.IBL.Prefilter.SRV");

        // BRDF LUT（256² RG16F，2D 资源）。
        D3D11_TEXTURE2D_DESC lutDesc{};
        lutDesc.Width = kBrdfLutSize;
        lutDesc.Height = kBrdfLutSize;
        lutDesc.MipLevels = 1;
        lutDesc.ArraySize = 1;
        lutDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        lutDesc.SampleDesc.Count = 1;
        lutDesc.Usage = D3D11_USAGE_DEFAULT;
        lutDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(device.CreateTexture2D(&lutDesc, nullptr, out.brdfLut.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateTexture2D(ibl brdf lut)");
        SetDebugObjectName(out.brdfLut.Get(), "M4.IBL.BrdfLut");

        D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc{};
        lutSrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        lutSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        lutSrvDesc.Texture2D.MostDetailedMip = 0;
        lutSrvDesc.Texture2D.MipLevels = 1;
        ThrowIfFailed(
            device.CreateShaderResourceView(out.brdfLut.Get(), &lutSrvDesc, out.brdfLutSrv.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(ibl brdf lut)");
        SetDebugObjectName(out.brdfLutSrv.Get(), "M4.IBL.BrdfLut.SRV");
        return true;
    }

    // ---- pass 装配与驱动 ----

    // b0（80B）：face VP（eye 在原点、look/up 按 04 篇表、90° FOV、aspect 1）+
    // 参数 float4（prefilter 阶段 x=roughness；downsample 阶段 x=source mip；
    // 其余阶段 0）。行主序转置与 PbrForward 的 CPU 常量同款约定。
    void UploadFaceConstants(const std::uint32_t face, const float paramX)
    {
        const DirectX::XMFLOAT3 look{kCubeFaces[face].look[0], kCubeFaces[face].look[1], kCubeFaces[face].look[2]};
        const DirectX::XMFLOAT3 up{kCubeFaces[face].up[0], kCubeFaces[face].up[1], kCubeFaces[face].up[2]};
        const DirectX::XMVECTOR zero = DirectX::XMVectorZero();
        const DirectX::XMMATRIX view =
            DirectX::XMMatrixLookToLH(zero, DirectX::XMLoadFloat3(&look), DirectX::XMLoadFloat3(&up));
        const DirectX::XMMATRIX projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1.0F, 0.1F, 10.0F);
        DirectX::XMFLOAT4X4 transposed{};
        DirectX::XMStoreFloat4x4(&transposed, DirectX::XMMatrixTranspose(view * projection));

        struct FaceConstants final
        {
            DirectX::XMFLOAT4X4 viewProjection; // 64B
            DirectX::XMFLOAT4 parameters;       // x = roughness / sourceMip，yzw = 0
        };
        static_assert(sizeof(FaceConstants) == 80);
        FaceConstants constants{};
        constants.viewProjection = transposed;
        constants.parameters = {paramX, 0.0F, 0.0F, 0.0F};
        context.UpdateSubresource(faceConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    }

    // cube 面 pass 装配：OM = 目标 RTV（无 DSV）、CULL_NONE、blend off、cube 几何。
    void BeginCubePass(ID3D11RenderTargetView& rtv, const std::uint32_t faceSize)
    {
        ID3D11RenderTargetView* target = &rtv;
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(faceSize), static_cast<float>(faceSize),
                                      0.0F, 1.0F};
        context.OMSetRenderTargets(1, &target, nullptr);
        context.RSSetViewports(1, &viewport);
        context.RSSetState(cullNoneRasterizer.Get());
        context.OMSetBlendState(blendOff.Get(), nullptr, 0xFFFFFFFFU);

        constexpr UINT stride = sizeof(CubeVertex);
        constexpr UINT offset = 0;
        ID3D11Buffer* vertexBuffer = cubeVertexBuffer.Get();
        ID3D11Buffer* indexBuffer = cubeIndexBuffer.Get();
        context.IASetInputLayout(cubeInputLayout.Get());
        context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context.IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
        context.VSSetShader(cubeFilterVs.Get(), nullptr, 0);
        // b0 必须同时绑到 VS 与 PS：cube filter 的 VSMain 用 FaceViewProjection
        // 把立方体顶点变换到该面的裁剪空间（IrradianceConvolution.hlsl）。
        // 缺陷记录（M4-05 取证发现，属 04 篇遗留）：此前只绑了 PS，VS 阶段 b0 未绑定
        // → 未绑定常量缓冲读出全零 → clip 位置退化，六个面一次都没光栅化 →
        // environment/mip 链/irradiance/prefilter 四份资源全部保持零内容，而
        // ValidateFinite 只查 NaN/Inf/负值，全零照样通过（"生成成功"是假象）。
        ID3D11Buffer* const faceConstants = faceConstantBuffer.Get();
        context.VSSetConstantBuffers(0, 1, &faceConstants);
        context.PSSetConstantBuffers(0, 1, &faceConstants);
        ID3D11SamplerState* sampler = linearClampSampler.Get();
        context.PSSetSamplers(1, 1, &sampler);
    }

    void DrawCube() const
    {
        context.DrawIndexed(static_cast<UINT>(kCubeIndices.size()), 0, 0);
    }

    // 阶段 1：equirect → envCube mip0（六面转换）。
    bool RenderEquirectConversion(ID3D11ShaderResourceView& bakedPanorama, D3D11IblSet& out)
    {
        context.PSSetShader(equirectPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* panoramaSrv = &bakedPanorama;
        context.PSSetShaderResources(0, 1, &panoramaSrv);

        for (std::uint32_t face = 0; face < 6; ++face)
        {
            const ComPtr<ID3D11RenderTargetView> rtv =
                CreateCubeFaceRtv(device, *out.environment.Get(), face, 0,
                                  std::string{"M4.IBL.Environment.Face"} + kCubeFaces[face].name + ".Mip0");
            BeginCubePass(*rtv.Get(), kEnvironmentSize);
            UploadFaceConstants(face, 0.0F);
            DrawCube();
        }
        EndPass();
        return true;
    }

    // 阶段 2：envCube mip 链（确定性 downsample）。每级：envCube 上一 mip →
    // CopySubresource 到 mipSource → 绑 mipSource SRV 采 (mip-1) → 写 envCube 该级。
    // envCube 的 RTV 与 SRV 永不同时绑定（hazard 契约）。
    bool RenderEnvironmentMipChain(D3D11IblSet& out)
    {
        const std::uint32_t mipCount = FullMipCount(kEnvironmentSize);
        context.PSSetShader(downsamplePs.Get(), nullptr, 0);
        ID3D11SamplerState* sampler = linearClampSampler.Get();
        context.PSSetSamplers(1, 1, &sampler);

        for (std::uint32_t mip = 1; mip < mipCount; ++mip)
        {
            const std::uint32_t mipSize = kEnvironmentSize >> mip;
            // 上一级六面拷入 mipSource（同一 subresource 布局）。
            for (std::uint32_t face = 0; face < 6; ++face)
            {
                const D3D11_BOX box{0, 0, 0, mipSize * 2U, mipSize * 2U, 1};
                context.CopySubresourceRegion(mipSource.Get(), D3D11CalcSubresource(mip - 1, face, mipCount), 0, 0, 0,
                                              out.environment.Get(), D3D11CalcSubresource(mip - 1, face, mipCount),
                                              &box);
            }

            // 从 mipSource 采 (mip-1)，写 envCube 第 mip 级（参数 x = source mip）。
            ID3D11ShaderResourceView* sourceSrv = mipSourceSrv.Get();
            context.PSSetShaderResources(0, 1, &sourceSrv);
            for (std::uint32_t face = 0; face < 6; ++face)
            {
                const ComPtr<ID3D11RenderTargetView> rtv = CreateCubeFaceRtv(
                    device, *out.environment.Get(), face, mip,
                    std::string{"M4.IBL.Environment.Face"} + kCubeFaces[face].name + ".Mip" + std::to_string(mip));
                BeginCubePass(*rtv.Get(), mipSize);
                UploadFaceConstants(face, static_cast<float>(mip - 1));
                DrawCube();
            }
            // 下一级循环前解除 SRV（envCube 下一次 CopySubresource 的源是它自己）。
            std::array<ID3D11ShaderResourceView*, 1> nullSrv{nullptr};
            context.PSSetShaderResources(0, 1, nullSrv.data());
        }
        EndPass();
        return true;
    }

    // 阶段 3：diffuse irradiance 六面（256 samples；输出 = irradiance / PI）。
    bool RenderIrradiance(D3D11IblSet& out)
    {
        context.PSSetShader(irradiancePs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* environmentSrv = out.environmentSrv.Get();
        context.PSSetShaderResources(0, 1, &environmentSrv);

        for (std::uint32_t face = 0; face < 6; ++face)
        {
            const ComPtr<ID3D11RenderTargetView> rtv = CreateCubeFaceRtv(
                device, *out.irradiance.Get(), face, 0, std::string{"M4.IBL.Irradiance.Face"} + kCubeFaces[face].name);
            BeginCubePass(*rtv.Get(), kIrradianceSize);
            UploadFaceConstants(face, 0.0F);
            DrawCube();
        }
        EndPass();
        return true;
    }

    // 阶段 4：prefiltered specular（每 (mip,face) 一 draw；roughness = mip/(N-1)）。
    bool RenderPrefilter(D3D11IblSet& out)
    {
        context.PSSetShader(prefilterPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* environmentSrv = out.environmentSrv.Get();
        context.PSSetShaderResources(0, 1, &environmentSrv);

        for (std::uint32_t mip = 0; mip < out.prefilterMipCount; ++mip)
        {
            const float roughness = out.prefilterMipCount > 1
                                        ? static_cast<float>(mip) / static_cast<float>(out.prefilterMipCount - 1)
                                        : 0.0F;
            const std::uint32_t mipSize = kPrefilterSize >> mip;
            for (std::uint32_t face = 0; face < 6; ++face)
            {
                const ComPtr<ID3D11RenderTargetView> rtv = CreateCubeFaceRtv(
                    device, *out.prefiltered.Get(), face, mip,
                    std::string{"M4.IBL.Prefilter.Face"} + kCubeFaces[face].name + ".Mip" + std::to_string(mip));
                BeginCubePass(*rtv.Get(), mipSize);
                UploadFaceConstants(face, roughness);
                DrawCube();
            }
        }
        EndPass();
        return true;
    }

    // 阶段 5：BRDF LUT（fullscreen triangle；SV_VertexID 生成，无 VB/布局）。
    bool RenderBrdfLut(D3D11IblSet& out)
    {
        context.VSSetShader(fullscreenVs.Get(), nullptr, 0);
        context.PSSetShader(brdfLutPs.Get(), nullptr, 0);
        context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.IASetInputLayout(nullptr);
        ID3D11Buffer* nullBuffer = nullptr;
        constexpr UINT zeroStride = 0;
        constexpr UINT zeroOffset = 0;
        context.IASetVertexBuffers(0, 1, &nullBuffer, &zeroStride, &zeroOffset);
        context.IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context.RSSetState(cullNoneRasterizer.Get());
        context.OMSetBlendState(blendOff.Get(), nullptr, 0xFFFFFFFFU);

        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(kBrdfLutSize), static_cast<float>(kBrdfLutSize),
                                      0.0F, 1.0F};
        context.RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* lutRtv = nullptr;
        ThrowIfFailed(device.CreateRenderTargetView(out.brdfLut.Get(), nullptr, &lutRtv),
                      "ID3D11Device::CreateRenderTargetView(ibl brdf lut)");
        SetDebugObjectName(lutRtv, "M4.IBL.BrdfLut.RTV");
        context.OMSetRenderTargets(1, &lutRtv, nullptr);
        lutRtv->Release();

        context.Draw(3, 0);
        EndPass();
        return true;
    }

    // 阶段 6：finite 校验——irradiance 六面与 LUT 全图 readback，拒绝 NaN/Inf/负，
    // 并额外拒绝"全零"——2026-09-07 的教训（见 BeginCubePass 注释）：b0 漏绑 VS 时
    // 六个面一次都没光栅化，生成结果是全零，而"只查 NaN/Inf/负值"的校验照样通过，
    // 于是日志报"生成成功"、实际资源全空。全零 = pass 没画上，属于明确缺陷。
    bool ValidateFinite(D3D11IblSet& out)
    {
        // 逐阶段定位：先 environment（转换 pass 的产物），再 irradiance、LUT。
        // 任一阶段全零都会明确报出该阶段名，不再笼统归到 irradiance。
        return CheckCubeFinite(*out.environment.Get(), kEnvironmentSize, FullMipCount(kEnvironmentSize),
                               "environment") &&
               CheckCubeFinite(*out.irradiance.Get(), kIrradianceSize, 1, "irradiance") &&
               CheckLutFinite(*out.brdfLut.Get());
    }

    // cube readback：staging 2D 逐面 CopySubresourceRegion → Map 扫描半精度位型。
    bool CheckCubeFinite(ID3D11Texture2D& cube, const std::uint32_t faceSize, const std::uint32_t mipCount,
                         const char* stage)
    {
        ComPtr<ID3D11Texture2D> staging = CreateStaging(faceSize, faceSize, DXGI_FORMAT_R16G16B16A16_FLOAT);
        bool anyNonZero = false; // 六面合计：全零意味着生成 pass 没有画上任何东西
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            const D3D11_BOX box{0, 0, 0, faceSize, faceSize, 1};
            context.CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, &cube, D3D11CalcSubresource(0, face, mipCount),
                                          &box);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            ThrowIfFailed(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "ID3D11DeviceContext::Map(ibl staging)");
            const auto* halves = static_cast<const std::uint16_t*>(mapped.pData);
            const std::size_t total = static_cast<std::size_t>(faceSize) * faceSize * 4U;
            const bool finite = HalfPixelsFinite(halves, total, true);
            for (std::size_t index = 0; index < total && !anyNonZero; ++index)
            {
                anyNonZero = (halves[index] & 0x7FFFU) != 0U; // 只看幅值位（符号位已由 finite 拒绝）
            }
            context.Unmap(staging.Get(), 0);
            if (!finite)
            {
                std::ostringstream stream;
                stream << "IBL validation failed: " << stage << " face " << face << " contains NaN/Inf/negative";
                error = stream.str();
                return false;
            }
        }
        if (!anyNonZero)
        {
            std::ostringstream stream;
            stream << "IBL validation failed: " << stage << " is entirely zero (generation pass wrote nothing)";
            error = stream.str();
            return false;
        }
        return true;
    }

    bool CheckLutFinite(ID3D11Texture2D& lut)
    {
        ComPtr<ID3D11Texture2D> staging = CreateStaging(kBrdfLutSize, kBrdfLutSize, DXGI_FORMAT_R16G16_FLOAT);
        context.CopyResource(staging.Get(), &lut);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                      "ID3D11DeviceContext::Map(ibl lut staging)");
        const bool finite = HalfPixelsFinite(static_cast<const std::uint16_t*>(mapped.pData),
                                             static_cast<std::size_t>(kBrdfLutSize) * kBrdfLutSize * 2U, false);
        context.Unmap(staging.Get(), 0);
        if (!finite)
        {
            error = "IBL validation failed: brdf lut contains NaN/Inf";
            return false;
        }
        return true;
    }

    ComPtr<ID3D11Texture2D> CreateStaging(const std::uint32_t width, const std::uint32_t height,
                                          const DXGI_FORMAT format) const
    {
        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        ThrowIfFailed(device.CreateTexture2D(&stagingDesc, nullptr, staging.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateTexture2D(ibl staging)");
        return staging;
    }

    // RGBA16F：负值（符号位）与 NaN/Inf（指数全 1）都判失败；RG16F：只查 NaN/Inf。
    static bool HalfPixelsFinite(const std::uint16_t* halves, const std::size_t total, const bool rejectNegative)
    {
        for (std::size_t index = 0; index < total; ++index)
        {
            const std::uint16_t bits = halves[index];
            const bool negative = (bits & 0x8000U) != 0U;
            const bool exponentAllOnes = ((bits >> 10U) & 0x1FU) == 0x1FU;
            if (exponentAllOnes || (rejectNegative && negative))
            {
                return false;
            }
        }
        return true;
    }

    // pass 收尾：解 RTV 与本阶段绑定的 t0 SRV（sampler/常量跨阶段复用，不清理）。
    void EndPass()
    {
        context.OMSetRenderTargets(0, nullptr, nullptr);
        std::array<ID3D11ShaderResourceView*, 1> nullSrv{nullptr};
        context.PSSetShaderResources(0, 1, nullSrv.data());
    }
};
} // namespace

bool D3D11IblResources::BuildTemporary(ID3D11Device& device, ID3D11DeviceContext& context,
                                       ID3D11ShaderResourceView& bakedPanorama,
                                       const std::filesystem::path& shaderDirectory, const std::uint64_t sourceRevision,
                                       std::string& error)
{
    m_state = IblState::Building;
    D3D11IblSet candidate{};
    candidate.sourceRevision = sourceRevision;

    Generator generator{device, context, shaderDirectory, error};
    const bool built = generator.BuildShaders() && generator.CreateOneTimeResources() &&
                       generator.CreateOutputResources(candidate) &&
                       generator.RenderEquirectConversion(bakedPanorama, candidate) &&
                       generator.RenderEnvironmentMipChain(candidate) && generator.RenderIrradiance(candidate) &&
                       generator.RenderPrefilter(candidate) && generator.RenderBrdfLut(candidate) &&
                       generator.ValidateFinite(candidate);

    if (!built)
    {
        // 失败语义（04 篇）：丢弃 temporary；active 不受影响（调用方按 State 处理）。
        candidate = {};
        m_state = m_active.environmentSrv == nullptr ? IblState::Failed : IblState::Ready;
        return false;
    }

    m_temporary = std::move(candidate);
    return true;
}

void D3D11IblResources::CommitTemporary()
{
    // ME_VERIFY 语义（仅 Building 态可提交——防御性契约，不靠调用方自觉）。
    if (m_state != IblState::Building)
    {
        throw std::runtime_error{"IBL commit requires building state"};
    }
    m_active = std::move(m_temporary);
    m_temporary = {};
    m_state = IblState::Ready;
}

void D3D11IblResources::DiscardTemporary() noexcept
{
    m_temporary = {};
    m_state = m_active.environmentSrv == nullptr ? IblState::Failed : IblState::Ready;
}

void D3D11IblResources::Release() noexcept
{
    m_temporary = {};
    m_active = {};
    m_state = IblState::Empty;
}
} // namespace MiniEngine::Rhi::D3D11
