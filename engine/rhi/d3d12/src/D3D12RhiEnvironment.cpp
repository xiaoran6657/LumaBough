// M6-10：保留 M5 固定 IBL profile 的 revision 边界准备。
// face/mip 状态仍是 backend-private；公开 graph 只收到已完成、已验证的整纹理。
#include "D3D12RhiBackend.h"
#include <DirectXMath.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <pix3.h>

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

ComPtr<ID3D12Resource> CreateIblBuffer(ID3D12Device& device, std::uint64_t bytes, D3D12_HEAP_TYPE type)
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

std::vector<std::byte> LoadIblShader(const std::filesystem::path& root, const char* stem, const char* entry,
                                     const char* stage)
{
    const auto path = root / "d3d12" / (std::string(stem) + "." + entry + "." + stage + ".dxil");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0)
        throw std::runtime_error("missing IBL DXIL: " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read IBL DXIL: " + path.string());
    return bytes;
}
} // namespace
std::array<std::unique_ptr<ResourcePayload>, 4> D3D12RhiBackend::PrepareEnvironment(
    ResourcePayload& panorama, std::string_view shaderRoot, std::uint64_t revision,
    const std::array<TextureDesc, 4>& descriptors)
{
    RequireAttached("PrepareEnvironment");
    if (m_activeFrame)
        throw std::logic_error("IBL preparation requires a frame boundary");
    auto& source = RequirePayload(panorama, PayloadKind::Texture);
    auto& device = *static_cast<ID3D12Device*>(m_device->NativeDeviceHandle());
    const std::filesystem::path root(shaderRoot);
    std::map<PassKind, PsoShaderSet> shaders;
    shaders[PassKind::EquirectToCube] = {LoadIblShader(root, "EquirectToCube", "VSMain", "vs"),
                                         LoadIblShader(root, "EquirectToCube", "PSMain", "ps")};
    shaders[PassKind::EnvironmentDownsample] = {LoadIblShader(root, "IrradianceConvolution", "VSMain", "vs"),
                                                LoadIblShader(root, "IrradianceConvolution", "PSDownsample", "ps")};
    shaders[PassKind::Irradiance] = {LoadIblShader(root, "IrradianceConvolution", "VSMain", "vs"),
                                     LoadIblShader(root, "IrradianceConvolution", "PSMain", "ps")};
    shaders[PassKind::Prefilter] = {LoadIblShader(root, "PrefilterEnvironment", "VSMain", "vs"),
                                    LoadIblShader(root, "PrefilterEnvironment", "PSMain", "ps")};
    shaders[PassKind::BrdfLut] = {LoadIblShader(root, "IntegrateBrdf", "VSMain", "vs"),
                                  LoadIblShader(root, "IntegrateBrdf", "PSMain", "ps")};
    D3D12PsoFactory pipelines;
    pipelines.Initialize(device, *m_m5RootSignature.Get(), 1);
    for (const auto& [pass, code] : shaders)
        (void)pipelines.GetOrCreate({pass, false, revision, 1}, code);

    // Prepare 的资源全部在提交前创建；candidate 失败不会触及旧 PreparedEnvironment。
    std::array<std::unique_ptr<ResourcePayload>, 5> targets;
    std::array<Payload*, 5> payloads{};
    constexpr std::array<UINT, 5> sizes{512, 32, 128, 256, 512};
    constexpr std::array<UINT16, 5> mips{10, 1, 8, 1, 10};
    D3D12DescriptorHeap rtvs, visible;
    rtvs.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, false, L"M6.IBL.FaceTargets");
    visible.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true, L"M6.IBL.DrawSnapshots");
    std::array<std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>, 5> faces;
    for (std::size_t i = 0; i < 5; ++i)
    {
        auto desc = descriptors[i == 4 ? 0 : i];
        desc.usage = desc.usage | TextureUsage::ColorAttachment;
        if (i == 4)
            desc.debugName = "IBL.MipSource";
        targets[i] = CreateTexture(desc);
        payloads[i] = &AsPayload(*targets[i]);
        const UINT faceCount = i == 3 ? 1 : 6;
        for (UINT face = 0; face < faceCount; ++face)
            for (UINT mip = 0; mip < mips[i]; ++mip)
            {
                D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                rtv.Format = ToNativeFormat(desc.format);
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.MipSlice = mip;
                rtv.Texture2DArray.FirstArraySlice = face;
                rtv.Texture2DArray.ArraySize = 1;
                const auto slot = rtvs.Allocate(1);
                const auto cpu = rtvs.Cpu(slot);
                device.CreateRenderTargetView(payloads[i]->resource.Get(), &rtv, cpu);
                faces[i].push_back(cpu);
            }
    }
    auto vertices = CreateIblBuffer(device, sizeof(kCubeVertices), D3D12_HEAP_TYPE_UPLOAD);
    auto indices = CreateIblBuffer(device, sizeof(kCubeIndices), D3D12_HEAP_TYPE_UPLOAD);
    auto constants = CreateIblBuffer(device, 256 * 128, D3D12_HEAP_TYPE_UPLOAD);
    const auto upload = [](ID3D12Resource& buffer, const void* input, std::size_t bytes)
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        ThrowIfFailed(buffer.Map(0, &noRead, &mapped), "Map IBL initialization");
        std::memcpy(mapped, input, bytes);
        const D3D12_RANGE written{0, bytes};
        buffer.Unmap(0, &written);
    };
    upload(*vertices.Get(), kCubeVertices.data(), sizeof(kCubeVertices));
    upload(*indices.Get(), kCubeIndices.data(), sizeof(kCubeIndices));
    void* constantMemory = nullptr;
    const D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(constants->Map(0, &noRead, &constantMemory), "Map IBL constants");
    D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(), sizeof(kCubeVertices), sizeof(CubeVertex)};
    D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(), sizeof(kCubeIndices), DXGI_FORMAT_R16_UINT};
    std::array<ComPtr<ID3D12Resource>, 4> readbacks;
    std::array<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>, 4> footprints;
    std::array<UINT64, 4> readbackBytes{};
    for (UINT i = 0; i < 4; ++i)
    {
        const auto desc = payloads[i]->resource->GetDesc();
        const UINT count = desc.DepthOrArraySize * desc.MipLevels;
        footprints[i].resize(count);
        device.GetCopyableFootprints(&desc, 0, count, 0, footprints[i].data(), nullptr, nullptr, &readbackBytes[i]);
        readbacks[i] = CreateIblBuffer(device, readbackBytes[i], D3D12_HEAP_TYPE_READBACK);
    }
    // 私有初始化队列只在 revision 边界工作；等待完成后结果才进入公共 owner。
    D3D12Queue queue;
    queue.Initialize(device);
    auto& context = queue.BeginFrame(0);
    auto& list = queue.CommandList();
    m_stateTracker.BeginRecording();
    struct RecordingGuard final
    {
        D3D12ResourceStateTracker& tracker;
        ~RecordingGuard()
        {
            if (tracker.IsRecording())
                tracker.Rollback();
        }
    } recordingGuard{m_stateTracker};
    PIXBeginEvent(&list, PIX_COLOR_DEFAULT, "M6 IBL Generation");
    ID3D12DescriptorHeap* heaps[]{&visible.Native()};
    list.SetDescriptorHeaps(1, heaps);
    m_stateTracker.Transition(source.stateKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (auto* payload : payloads)
        m_stateTracker.Transition(payload->stateKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_barriers += m_stateTracker.FlushBarriersTo(list);
    UINT drawIndex = 0;
    const auto drawFace =
        [&](UINT target, UINT mip, UINT face, PassKind pass, float parameter, D3D12_CPU_DESCRIPTOR_HANDLE sampled)
    {
        PIXScopedEvent(&list, PIX_COLOR_DEFAULT, "IBL:%u/%u target=%u", face, mip, target);
        const UINT subresource = face * mips[target] + mip;
        m_stateTracker.Transition(payloads[target]->stateKey, D3D12_RESOURCE_STATE_RENDER_TARGET, subresource);
        m_barriers += m_stateTracker.FlushBarriersTo(list);
        const auto rtv = faces[target][subresource];
        list.OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        SetViewport(list, std::max(1U, sizes[target] >> mip));
        struct FaceConstants
        {
            DirectX::XMFLOAT4X4 matrix;
            DirectX::XMFLOAT4 parameters;
        } data{};
        const DirectX::XMFLOAT3 look{kCubeFaces[face].look[0], kCubeFaces[face].look[1], kCubeFaces[face].look[2]};
        const DirectX::XMFLOAT3 up{kCubeFaces[face].up[0], kCubeFaces[face].up[1], kCubeFaces[face].up[2]};
        const auto view = DirectX::XMMatrixLookToLH(DirectX::XMVectorZero(), DirectX::XMLoadFloat3(&look),
                                                    DirectX::XMLoadFloat3(&up));
        const auto projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1, 0.1F, 10);
        DirectX::XMStoreFloat4x4(&data.matrix, DirectX::XMMatrixTranspose(view * projection));
        data.parameters = {parameter, 0, 0, 0};
        if (drawIndex >= 128)
            throw std::runtime_error("IBL draw constants exceeded fixed profile");
        std::memcpy(static_cast<std::byte*>(constantMemory) + drawIndex * 256, &data, sizeof(data));
        const auto range = visible.Allocate(5);
        for (UINT i = 0; i < 5; ++i)
            device.CopyDescriptorsSimple(1, visible.Cpu(range.base + i), sampled,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list.SetGraphicsRootSignature(m_m5RootSignature.Get());
        list.SetGraphicsRootConstantBufferView(RootIndex(RootParameter::FrameCbv),
                                               constants->GetGPUVirtualAddress() + drawIndex * 256);
        list.SetGraphicsRootDescriptorTable(RootIndex(RootParameter::MaterialSrvs), visible.Gpu(range));
        list.SetPipelineState(&pipelines.GetOrCreate({pass, false, revision, 1}, shaders.at(pass)));
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (pass == PassKind::BrdfLut)
            list.DrawInstanced(3, 1, 0, 0);
        else
        {
            list.IASetVertexBuffers(0, 1, &vb);
            list.IASetIndexBuffer(&ib);
            list.DrawIndexedInstanced(36, 1, 0, 0, 0);
        }
        ++drawIndex;
        m_stateTracker.Transition(payloads[target]->stateKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource);
        m_barriers += m_stateTracker.FlushBarriersTo(list);
    };

    for (UINT face = 0; face < 6; ++face)
        drawFace(0, 0, face, PassKind::EquirectToCube, 0, source.srvCpu);
    for (UINT mip = 1; mip < 10; ++mip)
    {
        for (UINT face = 0; face < 6; ++face)
        {
            const UINT subresource = face * 10 + mip - 1;
            m_stateTracker.Transition(payloads[0]->stateKey, D3D12_RESOURCE_STATE_COPY_SOURCE, subresource);
            m_stateTracker.Transition(payloads[4]->stateKey, D3D12_RESOURCE_STATE_COPY_DEST, subresource);
            m_barriers += m_stateTracker.FlushBarriersTo(list);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = payloads[0]->resource.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = subresource;
            to.pResource = payloads[4]->resource.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = subresource;
            list.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            m_stateTracker.Transition(payloads[0]->stateKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource);
            m_stateTracker.Transition(payloads[4]->stateKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource);
            m_barriers += m_stateTracker.FlushBarriersTo(list);
        }
        for (UINT face = 0; face < 6; ++face)
            drawFace(0, mip, face, PassKind::EnvironmentDownsample, static_cast<float>(mip - 1), payloads[4]->srvCpu);
    }
    for (UINT face = 0; face < 6; ++face)
        drawFace(1, 0, face, PassKind::Irradiance, 0, payloads[0]->srvCpu);
    for (UINT mip = 0; mip < 8; ++mip)
        for (UINT face = 0; face < 6; ++face)
            drawFace(2, mip, face, PassKind::Prefilter, static_cast<float>(mip) / 7.0F, payloads[0]->srvCpu);
    drawFace(3, 0, 0, PassKind::BrdfLut, 0, source.srvCpu);
    for (UINT index = 0; index < 4; ++index)
    {
        m_stateTracker.Transition(payloads[index]->stateKey, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_barriers += m_stateTracker.FlushBarriersTo(list);
        for (UINT subresource = 0; subresource < footprints[index].size(); ++subresource)
        {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = payloads[index]->resource.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = subresource;
            to.pResource = readbacks[index].Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = footprints[index][subresource];
            list.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        m_stateTracker.Transition(payloads[index]->stateKey, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_barriers += m_stateTracker.FlushBarriersTo(list);
    }
    PIXEndEvent(&list);
    const D3D12_RANGE written{0, drawIndex * 256};
    constants->Unmap(0, &written);
    const auto fence = queue.ExecuteAndSignal(context);
    m_stateTracker.CommitExecuted();
    queue.WaitForSubmittedFence(fence, "IBL revision initialization");
    ++m_submittedBatches;
    bool valid = true;
    for (UINT index = 0; index < 4; ++index)
    {
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(readbackBytes[index])};
        ThrowIfFailed(readbacks[index]->Map(0, &range, &mapped), "Map completed IBL validation");
        bool nonzero = false;
        const UINT channels = index == 3 ? 2 : 4;
        for (const auto& footprint : footprints[index])
            for (UINT row = 0; row < footprint.Footprint.Height; ++row)
            {
                const auto* half = reinterpret_cast<const std::uint16_t*>(
                    static_cast<const std::byte*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch);
                for (UINT column = 0; column < footprint.Footprint.Width; ++column)
                    for (UINT channel = 0; channel < std::min(channels, 3U); ++channel)
                    {
                        const auto value = half[column * channels + channel];
                        valid =
                            valid && (value & 0x7C00U) != 0x7C00U && ((value & 0x8000U) == 0 || (value & 0x7FFFU) == 0);
                        nonzero = nonzero || (value & 0x7FFFU) != 0;
                    }
            }
        const D3D12_RANGE noWrite{0, 0};
        readbacks[index]->Unmap(0, &noWrite);
        valid = valid && nonzero;
    }
    if (!valid)
        throw std::runtime_error("IBL validation rejected non-finite, negative or all-zero target");
    std::array<std::unique_ptr<ResourcePayload>, 4> result;
    for (UINT index = 0; index < 4; ++index)
        result[index] = std::move(targets[index]);
    return result;
}
} // namespace MiniEngine::Rhi::D3D12
