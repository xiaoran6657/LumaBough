// ============================================================================
// D3D12RendererPasses.cpp — Shadow、IBL 生成事务与 Skybox 的具体 D3D12 实现。
// 里程碑：M5-09。
// 职责：保持 M4 的资源 profile、面方向和 shader 数学；所有访问经 tracker 转换，
//       环境完成整个生成批并通过 fence 后回读校验，才在帧边界发布。
// 关联：docs/architecture/README.md。
// ============================================================================
#include "D3D12Events.h"
#include "D3D12IblSet.h"
#include "D3D12Renderer.h"
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
using Microsoft::WRL::ComPtr;
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

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device& device, std::uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                                : D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&resource)),
                  "Create scene buffer");
    return resource;
}
void SetViewport(ID3D12GraphicsCommandList& list, UINT size)
{
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
    list.RSSetViewports(1, &viewport);
    list.RSSetScissorRects(1, &scissor);
}
} // namespace

void D3D12Renderer::SetLightingEnabled(bool shadows, bool environment, bool skybox) noexcept
{
    m_shadowsEnabled = shadows;
    m_environmentEnabled = environment;
    m_skyboxEnabled = skybox;
}
void D3D12Renderer::SetExposure(float exposureEv) noexcept
{
    m_exposureEv = exposureEv;
}
void D3D12Renderer::SetEnvironment(Assets::AssetHandle<Assets::TextureAsset> panorama) noexcept
{
    m_environmentHandle = panorama;
}
void D3D12Renderer::RetryEnvironment() noexcept
{
    m_failedIblSource = {};
    m_iblLastError.clear();
}
bool D3D12Renderer::HasUsableIbl() const noexcept
{
    return m_environmentEnabled && m_ibl != nullptr && m_assets != nullptr &&
           m_assets->Textures().TryGet(m_ibl->source).has_value();
}
bool D3D12Renderer::IblReady() const noexcept
{
    if (!HasUsableIbl() || m_pendingIbl || m_ibl->source != m_environmentHandle ||
        m_ibl->shaderRevision != m_shaderRevision)
        return false;
    const auto source = m_assets->Textures().TryGet(m_environmentHandle);
    return source && source->revision == m_ibl->revision;
}
std::uint64_t D3D12Renderer::ShadowDrawCount() const noexcept
{
    return m_shadowDrawCount;
}
std::uint64_t D3D12Renderer::SkyboxDrawCount() const noexcept
{
    return m_skyboxDrawCount;
}

void D3D12Renderer::CreateSceneResources(ID3D12Device& device)
{
    // 静态立方体只上传一次；IBL face 与 skybox 使用同一方向几何。
    m_cubeGeometry = CreateBuffer(device, sizeof(kCubeVertices) + sizeof(kCubeIndices), D3D12_HEAP_TYPE_UPLOAD);
    void* mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    ThrowIfFailed(m_cubeGeometry->Map(0, &empty, &mapped), "Map cube geometry");
    std::memcpy(mapped, kCubeVertices.data(), sizeof(kCubeVertices));
    std::memcpy(static_cast<std::byte*>(mapped) + sizeof(kCubeVertices), kCubeIndices.data(), sizeof(kCubeIndices));
    m_cubeGeometry->Unmap(0, nullptr);
    ThrowIfFailed(m_cubeGeometry->SetName(L"M5.Cube.Geometry"), "Name cube");
    m_cubeVertices = {m_cubeGeometry->GetGPUVirtualAddress(), sizeof(kCubeVertices), sizeof(CubeVertex)};
    m_cubeIndices = {m_cubeGeometry->GetGPUVirtualAddress() + sizeof(kCubeVertices), sizeof(kCubeIndices),
                     DXGI_FORMAT_R16_UINT};

    // 固定 shadow volume 不随窗口改变尺寸，格式与 M4 的 typed views 一致。
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 2048;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                 IID_PPV_ARGS(&m_shadowMap)),
                  "Create shadow map");
    m_shadowKey =
        m_stateTracker.Register(*m_shadowMap.Get(), 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"M5.Shadow.Depth");
    m_shadowDsvHeap = std::make_unique<D3D12DescriptorHeap>();
    m_shadowDsvHeap->Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false, L"M5.Shadow.DsvHeap");
    const auto dsvSlot = m_shadowDsvHeap->Allocate(1);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device.CreateDepthStencilView(m_shadowMap.Get(), &dsv, m_shadowDsvHeap->Cpu(dsvSlot));
    const auto srvSlot = m_stagingHeap->Allocate(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_shadowSrv = m_stagingHeap->Cpu(srvSlot);
    device.CreateShaderResourceView(m_shadowMap.Get(), &srv, m_shadowSrv);
}

void D3D12Renderer::RecordShadowPass(ID3D12GraphicsCommandList& list)
{
    m_device->RecordDiagnosticEvent("M5 Pass 1 ShadowDepth");
    EventScope event(list, L"Shadow");
    m_frameReadback.BeginPass(list, m_swapChain.CurrentBackBufferIndex(), 0);
    m_stateTracker.Transition(m_shadowKey, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    static_cast<void>(m_stateTracker.FlushBarriersTo(list));
    const auto dsv = m_shadowDsvHeap->Cpu(0);
    list.OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    list.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    SetViewport(list, 2048);
    list.SetGraphicsRootSignature(m_rootSignature);
    const auto lightMatrix = TransposedForHlsl(World::BuildLightViewProjection(m_packet->directionalLight));
    if (m_shadowsEnabled)
    {
        for (const auto& draw : m_packet->shadowCasters)
        {
            if (!m_assetCache.EnsureMeshUploaded({*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()), list,
                                                  m_assetUploadManager, m_stateTracker},
                                                 *m_assets, draw.mesh))
            {
                continue;
            }
            const auto mesh = m_assetCache.TryGetMeshView(draw.mesh);
            if (!mesh)
            {
                continue;
            }
            const auto allocation = m_constantRing.TryAllocateConstant(sizeof(ObjectConstants));
            if (!allocation)
            {
                throw std::runtime_error("shadow constant ring exhausted");
            }
            ObjectConstants object{};
            object.world = TransposedForHlsl(draw.world);
            object.normalMatrix = TransposedForHlsl(draw.normal);
            object.lightWorldViewProjection = lightMatrix;
            object.handednessAndReceivesShadow = {draw.mirrored ? -1.0F : 1.0F, 1, 0, 0};
            std::memcpy(allocation.cpu, &object, sizeof(object));
            m_constantUploadBytes += sizeof(object);
            const PsoKey key{PassKind::Shadow, draw.mirrored, m_shaderRevision, m_rootSignatureRevision};
            list.SetPipelineState(&m_psoFactory.GetOrCreate(key, m_baselineShaders.at(key.pass)));
            list.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::ObjectCbv), allocation.gpu);
            list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list.IASetVertexBuffers(0, 1, &mesh->vertexBuffer);
            list.IASetIndexBuffer(&mesh->indexBuffer);
            MarkDraw(list, draw);
            list.DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
            ++m_shadowDrawCount;
        }
    }
    m_stateTracker.Transition(m_shadowKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    static_cast<void>(m_stateTracker.FlushBarriersTo(list));
    m_frameReadback.EndPass(list, m_swapChain.CurrentBackBufferIndex(), 0);
}

void D3D12Renderer::PublishGlobalTable(ID3D12Device& device, std::uint32_t frameIndex)
{
    // 本帧 fence 已完成才覆盖自己的表；旧帧保留旧 IBL descriptor 快照。
    auto sources = m_globalFallbacks;
    if (HasUsableIbl())
    {
        sources[0] = m_ibl->srvs[1];
        sources[1] = m_ibl->srvs[2];
        sources[2] = m_ibl->srvs[3];
    }
    sources[3] = m_shadowSrv;
    for (UINT index = 0; index < kGlobalSrvCount; ++index)
    {
        device.CopyDescriptorsSimple(1, m_srvHeap->Cpu(m_frameGlobalTables[frameIndex].base + index), sources[index],
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

void D3D12Renderer::RecordSkyboxPass(ID3D12GraphicsCommandList& list)
{
    if (!m_skyboxEnabled || !HasUsableIbl())
    {
        return;
    }
    m_device->RecordDiagnosticEvent("M5 Pass 3 Skybox");
    EventScope event(list, L"Skybox");
    m_frameReadback.BeginPass(list, m_swapChain.CurrentBackBufferIndex(), 2);
    const auto allocation = m_constantRing.TryAllocateConstant(sizeof(SkyboxConstants));
    if (!allocation)
    {
        throw std::runtime_error("skybox constant ring exhausted");
    }
    const SkyboxConstants constants{ViewProjectionWithoutTranslation(m_packet->view, m_packet->projection)};
    std::memcpy(allocation.cpu, &constants, sizeof(constants));
    m_constantUploadBytes += sizeof(constants);
    auto sources = m_materialFallbacks;
    sources[0] = m_ibl->srvs[0];
    PublishMaterialTable(*static_cast<ID3D12Device*>(m_device->NativeDeviceHandle()),
                         m_swapChain.CurrentBackBufferIndex(), sources);
    const PsoKey key{PassKind::Skybox, false, m_shaderRevision, m_rootSignatureRevision};
    list.SetPipelineState(&m_psoFactory.GetOrCreate(key, m_baselineShaders.at(key.pass)));
    list.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::FrameCbv), allocation.gpu);
    list.SetGraphicsRootDescriptorTable(RootIndex(RootParameter::MaterialSrvs),
                                        m_srvHeap->Gpu(m_lastMaterialTableBase));
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.IASetVertexBuffers(0, 1, &m_cubeVertices);
    list.IASetIndexBuffer(&m_cubeIndices);
    list.DrawIndexedInstanced(36, 1, 0, 0, 0);
    ++m_skyboxDrawCount;
    m_frameReadback.EndPass(list, m_swapChain.CurrentBackBufferIndex(), 2);
}

void D3D12Renderer::PrepareEnvironment(ID3D12GraphicsCommandList& list)
{
    if (!m_environmentEnabled || !m_environmentHandle.IsValid() || m_pendingIbl)
    {
        return;
    }
    const auto view = m_assets->Textures().TryGet(m_environmentHandle);
    if (!view || (m_ibl && m_ibl->source == m_environmentHandle && m_ibl->revision == view->revision &&
                  m_ibl->shaderRevision == m_shaderRevision))
    {
        return;
    }
    if (m_failedIblSource == m_environmentHandle && m_failedIblRevision == view->revision &&
        m_failedIblShaderRevision == m_shaderRevision)
        return; // 已知失败的同一输入不自动重试；旧 active 仍可使用。
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    if (!m_assetCache.EnsureTextureUploaded({device, list, m_assetUploadManager, m_stateTracker}, *m_assets,
                                            m_environmentHandle))
    {
        throw std::runtime_error("environment panorama upload failed");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE panorama{};
    if (!m_assetCache.TryGetTextureSrv(m_environmentHandle, panorama))
    {
        throw std::runtime_error("missing panorama SRV");
    }
    m_device->RecordDiagnosticEvent("M5 IBL Generation");
    EventScope event(list, L"M5 IBL Generation");
    auto candidate = std::make_unique<D3D12IblSet>();
    candidate->source = m_environmentHandle;
    candidate->revision = view->revision;
    candidate->shaderRevision = m_shaderRevision;
    candidate->staging.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, false, L"M5.IBL.Staging");
    candidate->rtvs.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, false, L"M5.IBL.RtvHeap");
    constexpr std::array<UINT, 5> sizes{512, 32, 128, 256, 512};
    constexpr std::array<UINT16, 5> mips{10, 1, 8, 1, 10};
    constexpr std::array<const wchar_t*, 5> names{L"M5.IBL.Environment", L"M5.IBL.Irradiance", L"M5.IBL.Prefilter",
                                                  L"M5.IBL.BrdfLut", L"M5.IBL.MipSource"};
    std::uint32_t generatedDraws = 0;
    try
    {
        // 全部目标与视图先创建完，创建失败不会替换当前 active 集。
        for (UINT index = 0; index < 5; ++index)
        {
            const UINT16 faces = index == 3 ? 1 : 6;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = desc.Height = sizes[index];
            desc.DepthOrArraySize = faces;
            desc.MipLevels = mips[index];
            desc.Format = index == 3 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                         IID_PPV_ARGS(&candidate->textures[index])),
                          "Create IBL target");
            candidate->keys[index] = m_stateTracker.Register(*candidate->textures[index].Get(), faces * mips[index],
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, names[index]);
            candidate->registered[index] = true;
            const auto slot = candidate->staging.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = desc.Format;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.ViewDimension = index == 3 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURECUBE;
            if (index == 3)
            {
                srv.Texture2D.MipLevels = mips[index];
            }
            else
            {
                srv.TextureCube.MipLevels = mips[index];
            }
            candidate->srvs[index] = candidate->staging.Cpu(slot);
            device.CreateShaderResourceView(candidate->textures[index].Get(), &srv, candidate->srvs[index]);
            for (UINT face = 0; face < faces; ++face)
            {
                for (UINT mip = 0; mip < mips[index]; ++mip)
                {
                    const auto target = candidate->rtvs.Allocate(1);
                    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                    rtv.Format = desc.Format;
                    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv.Texture2DArray.MipSlice = mip;
                    rtv.Texture2DArray.FirstArraySlice = face;
                    rtv.Texture2DArray.ArraySize = 1;
                    const auto cpu = candidate->rtvs.Cpu(target);
                    device.CreateRenderTargetView(candidate->textures[index].Get(), &rtv, cpu);
                    candidate->targets[index].push_back(cpu);
                }
            }
        }
        // 各次绘制的 b0 和 t0 都使用独立快照，不覆盖同一列表早先 draw 的描述符。
        const auto drawFace =
            [&](UINT target, UINT mip, UINT face, PassKind pass, float parameter, D3D12_CPU_DESCRIPTOR_HANDLE source)
        {
            PIXScopedEvent(&list, PIX_COLOR_DEFAULT, "IBL:%u/%u target=%u", face, mip, target);
            const UINT subresource = face * mips[target] + mip;
            m_stateTracker.Transition(candidate->keys[target], D3D12_RESOURCE_STATE_RENDER_TARGET, subresource);
            static_cast<void>(m_stateTracker.FlushBarriersTo(list));
            const auto rtv = candidate->targets[target][subresource];
            list.OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            SetViewport(list, std::max(1U, sizes[target] >> mip));
            struct FaceConstants
            {
                DirectX::XMFLOAT4X4 matrix;
                DirectX::XMFLOAT4 parameters;
            } constants{};
            const DirectX::XMFLOAT3 look{kCubeFaces[face].look[0], kCubeFaces[face].look[1], kCubeFaces[face].look[2]};
            const DirectX::XMFLOAT3 up{kCubeFaces[face].up[0], kCubeFaces[face].up[1], kCubeFaces[face].up[2]};
            const auto faceView = DirectX::XMMatrixLookToLH(DirectX::XMVectorZero(), DirectX::XMLoadFloat3(&look),
                                                            DirectX::XMLoadFloat3(&up));
            const auto projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1, 0.1F, 10);
            DirectX::XMStoreFloat4x4(&constants.matrix, DirectX::XMMatrixTranspose(faceView * projection));
            constants.parameters = {parameter, 0, 0, 0};
            const auto allocation = m_constantRing.TryAllocateConstant(sizeof(constants));
            if (!allocation)
            {
                throw std::runtime_error("IBL constants exhausted");
            }
            std::memcpy(allocation.cpu, &constants, sizeof(constants));
            m_constantUploadBytes += sizeof(constants);
            auto sources = m_materialFallbacks;
            sources[0] = source;
            PublishMaterialTable(device, m_swapChain.CurrentBackBufferIndex(), sources);
            list.SetGraphicsRootSignature(m_rootSignature);
            list.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::FrameCbv), allocation.gpu);
            list.SetGraphicsRootDescriptorTable(RootIndex(RootParameter::MaterialSrvs),
                                                m_srvHeap->Gpu(m_lastMaterialTableBase));
            const PsoKey key{pass, false, m_shaderRevision, m_rootSignatureRevision};
            list.SetPipelineState(&m_psoFactory.GetOrCreate(key, m_baselineShaders.at(pass)));
            list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (pass == PassKind::BrdfLut)
            {
                list.DrawInstanced(3, 1, 0, 0);
            }
            else
            {
                list.IASetVertexBuffers(0, 1, &m_cubeVertices);
                list.IASetIndexBuffer(&m_cubeIndices);
                list.DrawIndexedInstanced(36, 1, 0, 0, 0);
            }
            // 测试注入发生在真实 draw 之后，覆盖部分命令已录制的退休路径。
            if (m_iblFailureAfterDraws != 0 && ++generatedDraws == m_iblFailureAfterDraws)
            {
                m_iblFailureAfterDraws = 0;
                throw std::runtime_error("injected IBL generation failure after recorded draw");
            }
            m_stateTracker.Transition(candidate->keys[target], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource);
            static_cast<void>(m_stateTracker.FlushBarriersTo(list));
        };
        for (UINT face = 0; face < 6; ++face)
        {
            drawFace(0, 0, face, PassKind::EquirectToCube, 0, panorama);
        }
        for (UINT mip = 1; mip < 10; ++mip)
        {
            // 与 M4 同款中转 cube：先复制上一级六面，再对目标 mip 做确定性采样。
            for (UINT face = 0; face < 6; ++face)
            {
                const UINT subresource = face * 10 + mip - 1;
                m_stateTracker.Transition(candidate->keys[0], D3D12_RESOURCE_STATE_COPY_SOURCE, subresource);
                m_stateTracker.Transition(candidate->keys[4], D3D12_RESOURCE_STATE_COPY_DEST, subresource);
                static_cast<void>(m_stateTracker.FlushBarriersTo(list));
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = candidate->textures[0].Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = subresource;
                to.pResource = candidate->textures[4].Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = subresource;
                list.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                m_stateTracker.Transition(candidate->keys[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource);
                m_stateTracker.Transition(candidate->keys[4], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource);
                static_cast<void>(m_stateTracker.FlushBarriersTo(list));
            }
            for (UINT face = 0; face < 6; ++face)
            {
                drawFace(0, mip, face, PassKind::EnvironmentDownsample, static_cast<float>(mip - 1),
                         candidate->srvs[4]);
            }
        }
        for (UINT face = 0; face < 6; ++face)
        {
            drawFace(1, 0, face, PassKind::Irradiance, 0, candidate->srvs[0]);
        }
        for (UINT mip = 0; mip < 8; ++mip)
        {
            for (UINT face = 0; face < 6; ++face)
            {
                drawFace(2, mip, face, PassKind::Prefilter, static_cast<float>(mip) / 7.0F, candidate->srvs[0]);
            }
        }
        drawFace(3, 0, 0, PassKind::BrdfLut, 0, m_materialFallbacks[0]);
        // 完整 readback 检查会拒绝 NaN/Inf/负值及 RGB 全零的生成退化。
        for (UINT index = 0; index < 4; ++index)
        {
            const auto desc = candidate->textures[index]->GetDesc();
            const UINT count = desc.DepthOrArraySize * desc.MipLevels;
            auto& footprints = candidate->footprints[index];
            footprints.resize(count);
            device.GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr, nullptr,
                                         &candidate->readbackBytes[index]);
            candidate->readbacks[index] =
                CreateBuffer(device, candidate->readbackBytes[index], D3D12_HEAP_TYPE_READBACK);
            m_stateTracker.Transition(candidate->keys[index], D3D12_RESOURCE_STATE_COPY_SOURCE);
            static_cast<void>(m_stateTracker.FlushBarriersTo(list));
            for (UINT subresource = 0; subresource < count; ++subresource)
            {
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = candidate->textures[index].Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = subresource;
                to.pResource = candidate->readbacks[index].Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprints[subresource];
                list.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            m_stateTracker.Transition(candidate->keys[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            static_cast<void>(m_stateTracker.FlushBarriersTo(list));
        }
        m_pendingIbl = std::move(candidate);
    }
    catch (const std::exception& error)
    {
        // 已录制的 GPU 命令仍引用候选；持有到本帧提交 fence 完成再释放。
        // active/global table 保持旧集，当前帧可以继续渲染。
        candidate->failure = error.what();
        m_pendingIbl = std::move(candidate);
    }
}

void D3D12Renderer::ReleaseIblSet(std::unique_ptr<D3D12IblSet>& set)
{
    if (!set)
    {
        return;
    }
    for (UINT index = 0; index < 5; ++index)
    {
        if (set->registered[index])
        {
            m_stateTracker.Unregister(set->keys[index]);
        }
    }
    set.reset();
}

void D3D12Renderer::PollEnvironment(std::uint64_t completedFence)
{
    // 旧集最后一次引用的 submission fence 完成后，才撤销注册并释放资源和描述符。
    for (auto it = m_retiredIbl.begin(); it != m_retiredIbl.end();)
    {
        if (it->first <= completedFence)
        {
            ReleaseIblSet(it->second);
            it = m_retiredIbl.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (!m_pendingIbl || m_pendingIbl->fence == 0 || m_pendingIbl->fence > completedFence)
    {
        return;
    }
    const auto rejectCandidate = [this](const std::string reason)
    {
        m_failedIblSource = m_pendingIbl->source;
        m_failedIblRevision = m_pendingIbl->revision;
        m_failedIblShaderRevision = m_pendingIbl->shaderRevision;
        m_iblLastError = reason;
        ++m_iblFailureCount;
        ReleaseIblSet(m_pendingIbl);
        MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 IBL candidate rejected; active preserved: " + reason);
    };
    if (!m_pendingIbl->failure.empty())
    {
        rejectCandidate(m_pendingIbl->failure);
        return;
    }
    bool valid = true;
    for (UINT index = 0; index < 4; ++index)
    {
        void* mapped = nullptr;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(m_pendingIbl->readbackBytes[index])};
        ThrowIfFailed(m_pendingIbl->readbacks[index]->Map(0, &read, &mapped), "Map completed IBL validation");
        bool nonzero = false;
        const UINT channels = index == 3 ? 2 : 4;
        for (const auto& footprint : m_pendingIbl->footprints[index])
        {
            for (UINT row = 0; row < footprint.Footprint.Height; ++row)
            {
                const auto* half = reinterpret_cast<const std::uint16_t*>(
                    static_cast<const std::byte*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch);
                for (UINT column = 0; column < footprint.Footprint.Width; ++column)
                {
                    for (UINT channel = 0; channel < std::min(channels, 3U); ++channel)
                    {
                        const auto value = half[column * channels + channel];
                        valid =
                            valid && (value & 0x7C00U) != 0x7C00U && ((value & 0x8000U) == 0 || (value & 0x7FFFU) == 0);
                        nonzero = nonzero || (value & 0x7FFFU) != 0;
                    }
                }
            }
        }
        const D3D12_RANGE noWrite{0, 0};
        m_pendingIbl->readbacks[index]->Unmap(0, &noWrite);
        m_pendingIbl->readbacks[index].Reset();
        valid = valid && nonzero;
    }
    if (!valid)
    {
        rejectCandidate("IBL validation rejected non-finite, negative or all-zero target");
        return;
    }
    if (m_ibl)
    {
        m_retiredIbl.emplace_back(m_queue.NextFenceValue() - 1, std::move(m_ibl));
    }
    m_ibl = std::move(m_pendingIbl);
    m_iblLastError.clear();
    m_failedIblSource = {};
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 IBL ready: revision=" + std::to_string(m_ibl->revision));
}
} // namespace MiniEngine::Rhi::D3D12
