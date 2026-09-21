// ============================================================================
// D3D12ValidationNegativeTests.cpp — 独立负向验证 executable
// 职责：以实际 message ID/category 证明 Debug Layer 和 GBV 通道有效。
// 错误只存在于本测试入口；不使用 TDR，生产 sample 不提供触发 flag。
// ============================================================================
#include "D3D12DescriptorHeap.h"
#include "D3D12PsoFactory.h"
#include "D3D12Queue.h"
#include "D3D12RootSignature.h"
#include "D3D12UploadRing.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <dxgi1_6.h>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using namespace MiniEngine::Rhi::D3D12;
namespace
{
struct Fixture
{
    std::unique_ptr<D3D12Device> owner;
    D3D12Queue queue;
    Fixture(bool gbv = true, bool dred = false)
    {
        DeviceCreateOptions options;
        options.debugLayer = true;
        options.gpuValidation = gbv;
        options.dred = dred;
        owner = D3D12Device::Create(options);
        queue.Initialize(Device());
    }
    ID3D12Device& Device()
    {
        return *static_cast<ID3D12Device*>(owner->NativeDeviceHandle());
    }
};
void Expect(const ValidationReport& report, D3D12_MESSAGE_ID expected)
{
    bool found = false;
    for (const auto& msg : report.messages)
    {
        std::cout << "ID=" << msg.id << " category=" << msg.category << " severity=" << msg.severity << " "
                  << msg.description << std::endl;
        found |= msg.id == static_cast<std::uint32_t>(expected) &&
                 msg.category == static_cast<std::uint32_t>(D3D12_MESSAGE_CATEGORY_EXECUTION) &&
                 msg.severity <= static_cast<std::uint32_t>(D3D12_MESSAGE_SEVERITY_WARNING);
    }
    EXPECT_TRUE(found) << "expected ID=" << static_cast<unsigned>(expected);
}
void ExpectShadowStateMismatch(const ValidationReport& report)
{
    bool found = false;
    for (const auto& msg : report.messages)
    {
        std::cout << "ID=" << msg.id << " category=" << msg.category << " severity=" << msg.severity << " "
                  << msg.description << std::endl;
        const std::string& description = msg.description;
        const bool identifiesShadow = description.find("M5-10.Negative.Shadow.Depth") != std::string::npos;
        const bool identifiesDepthWrite = description.find("D3D12_RESOURCE_STATE_DEPTH_WRITE") != std::string::npos;
        // GBV 当前版本分开报告 table start=5 与相对 offset=3，即 heap slot 8。
        const bool identifiesShadowSlot =
            description.find("Descriptor heap index: [8]") != std::string::npos ||
            (description.find("Descriptor heap index to DescriptorTableStart: [5]") != std::string::npos &&
             description.find("Descriptor heap index FromTableStart: [3]") != std::string::npos);
        const bool identifiesGlobalTable = description.find("Root Parameter Index: [5]") != std::string::npos;
        found |=
            msg.id == static_cast<std::uint32_t>(D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_RESOURCE_STATE) &&
            msg.category == static_cast<std::uint32_t>(D3D12_MESSAGE_CATEGORY_EXECUTION) &&
            msg.severity <= static_cast<std::uint32_t>(D3D12_MESSAGE_SEVERITY_WARNING) && identifiesShadow &&
            identifiesDepthWrite && identifiesShadowSlot && identifiesGlobalTable;
    }
    EXPECT_TRUE(found) << "expected ID=942 for shadow resource DEPTH_WRITE at global t8/heap slot 8";
}
std::vector<std::byte> Shader(const char* name)
{
    std::ifstream in(std::string(MINIENGINE_D3D12_SHADER_DIR) + "/" + name, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("missing fixture shader");
    auto n = in.tellg();
    std::vector<std::byte> bytes(static_cast<std::size_t>(n));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), n))
        throw std::runtime_error("shader read failure");
    return bytes;
}
Microsoft::WRL::ComPtr<ID3D12Resource> Texture(ID3D12Device& device, bool target, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.25F, 0.5F, 0.75F, 1.0F}};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, target ? &clear : nullptr,
                                                 IID_PPV_ARGS(&resource)),
                  "fixture texture");
    return resource;
}
Microsoft::WRL::ComPtr<ID3D12Resource> CubeTexture(ID3D12Device& device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&resource)),
                  "fixture cube texture");
    return resource;
}
Microsoft::WRL::ComPtr<ID3D12Resource> LutTexture(ID3D12Device& device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&resource)),
                  "fixture BRDF LUT");
    return resource;
}
Microsoft::WRL::ComPtr<ID3D12Resource> RenderTarget(ID3D12Device& device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R16G16B16A16_FLOAT, {0.0F, 0.0F, 0.0F, 1.0F}};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 &clear, IID_PPV_ARGS(&resource)),
                  "fixture HDR render target");
    return resource;
}
Microsoft::WRL::ComPtr<ID3D12Resource> DepthResource(ID3D12Device& device, const wchar_t* name)
{
    // 严格复用生产 shadow 的 resource profile：typeless 资源配 D32 DSV 和 R32 SRV。
    // 初始状态必须是 DEPTH_WRITE，随后由 fixture 显式证明是否缺少 shader-read transition。
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0F;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                 &clear, IID_PPV_ARGS(&resource)),
                  "fixture shadow depth");
    ThrowIfFailed(resource->SetName(name), "name fixture shadow depth");
    return resource;
}
Microsoft::WRL::ComPtr<ID3D12Resource> UploadVertices(ID3D12Device& device, D3D12_VERTEX_BUFFER_VIEW& view)
{
    struct Vertex final
    {
        float position[3];
        float normal[3];
        float tangent[4];
        float uv[2];
    };
    constexpr std::array<Vertex, 3> vertices{{
        Vertex{{-0.5F, -0.5F, 0.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
        Vertex{{0.5F, -0.5F, 0.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
        Vertex{{0.0F, 0.5F, 0.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.5F, 0.0F}},
    }};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(vertices);
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&resource)),
                  "fixture shadow read vertex buffer");
    void* mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    ThrowIfFailed(resource->Map(0, &empty, &mapped), "map fixture shadow read vertex buffer");
    std::memcpy(mapped, vertices.data(), sizeof(vertices));
    resource->Unmap(0, nullptr);
    view.BufferLocation = resource->GetGPUVirtualAddress();
    view.SizeInBytes = static_cast<UINT>(sizeof(vertices));
    view.StrideInBytes = static_cast<UINT>(sizeof(Vertex));
    return resource;
}
void WriteIdentity(float* destination)
{
    constexpr std::array<float, 16> identity{
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
    };
    std::memcpy(destination, identity.data(), sizeof(identity));
}
void WritePbrConstants(D3D12UploadRing& constants, std::array<UploadAllocation, 4>& allocations)
{
    allocations[0] = constants.TryAllocateConstant(128);
    allocations[1] = constants.TryAllocateConstant(208);
    allocations[2] = constants.TryAllocateConstant(48);
    allocations[3] = constants.TryAllocateConstant(16);
    if (!allocations[0] || !allocations[1] || !allocations[2] || !allocations[3])
        throw std::runtime_error("fixture PBR constants allocation failed");

    std::memset(allocations[0].cpu, 0, allocations[0].size);
    WriteIdentity(reinterpret_cast<float*>(allocations[0].cpu));
    const float frameCamera[]{0.0F, 0.0F, -2.0F, 8.0F};
    const float frameLight[]{0.0F, 0.0F, 1.0F, 1.0F};
    const float frameColor[]{1.0F, 1.0F, 1.0F, 0.0F};
    const float frameShadowSize[]{4.0F, 4.0F, 0.0F, 0.0F};
    std::memcpy(allocations[0].cpu + 64, frameCamera, sizeof(frameCamera));
    std::memcpy(allocations[0].cpu + 80, frameLight, sizeof(frameLight));
    std::memcpy(allocations[0].cpu + 96, frameColor, sizeof(frameColor));
    std::memcpy(allocations[0].cpu + 112, frameShadowSize, sizeof(frameShadowSize));

    std::memset(allocations[1].cpu, 0, allocations[1].size);
    WriteIdentity(reinterpret_cast<float*>(allocations[1].cpu));
    WriteIdentity(reinterpret_cast<float*>(allocations[1].cpu + 64));
    WriteIdentity(reinterpret_cast<float*>(allocations[1].cpu + 128));
    const float handedness[]{1.0F, 1.0F, 0.0F, 0.0F};
    std::memcpy(allocations[1].cpu + 192, handedness, sizeof(handedness));

    const float material[]{1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F};
    std::memcpy(allocations[2].cpu, material, sizeof(material));
    const float ibl[]{1.0F, 0.0F, 0.0F, 0.0F};
    std::memcpy(allocations[3].cpu, ibl, sizeof(ibl));
}
void ShaderProbe(int fault)
{
    Fixture f;
    auto& device = f.Device();
    RootSignatureFacts facts{};
    auto root = CreateM5RootSignature(device, facts);
    D3D12PsoFactory factory;
    factory.Initialize(device, *root.Get(), 0);
    auto& pso = factory.GetOrCreate({PassKind::ToneMap, false, 0, 0},
                                    {Shader("ToneMap.VSMain.vs.dxil"), Shader("ToneMap.PSMain.ps.dxil")});
    D3D12DescriptorHeap srv, rtv;
    srv.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true, L"M5-10.Negative.SRV");
    rtv.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"M5-10.Negative.RTV");
    auto target = Texture(device, true, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto source = Texture(device, false,
                          fault == 2 ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    device.CreateRenderTargetView(target.Get(), nullptr, rtv.Cpu(0));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    for (UINT i = 1; i < 16; ++i)
        device.CreateShaderResourceView(nullptr, &view, srv.Cpu(i));
    if (fault != 0)
        device.CreateShaderResourceView(source.Get(), &view, srv.Cpu(0));
    if (fault == 1)
        source.Reset();
    D3D12UploadRing constants;
    constants.Initialize(device, 4096);
    auto cb = constants.TryAllocateConstant(256);
    std::memset(cb.cpu, 0, 256);
    struct Guard
    {
        D3D12Queue& queue;
        ~Guard()
        {
            try
            {
                queue.FlushGpu("negative cleanup");
            }
            catch (...)
            {
            }
        }
    } guard{f.queue};
    auto& frame = f.queue.BeginFrame(0);
    auto& list = f.queue.CommandList();
    ID3D12DescriptorHeap* heaps[]{&srv.Native()};
    list.SetDescriptorHeaps(1, heaps);
    list.SetGraphicsRootSignature(root.Get());
    list.SetPipelineState(&pso);
    for (UINT i = 0; i < 4; ++i)
        list.SetGraphicsRootConstantBufferView(i, cb.gpu);
    list.SetGraphicsRootDescriptorTable(4, srv.Gpu(0));
    list.SetGraphicsRootDescriptorTable(5, srv.Gpu(5));
    auto handle = rtv.Cpu(0);
    list.OMSetRenderTargets(1, &handle, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, 4, 4, 0, 1};
    D3D12_RECT sc{0, 0, 4, 4};
    list.RSSetViewports(1, &vp);
    list.RSSetScissorRects(1, &sc);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    EXPECT_FALSE(f.owner->DrainInfoQueue().HasFailure()) << "negative fixture setup must be clean";
    list.DrawInstanced(3, 1, 0, 0);
    auto fence = f.queue.ExecuteAndSignal(frame);
    constants.CommitFrame(fence);
    f.queue.WaitForSubmittedFence(fence, "negative shader");
    constants.Reclaim(f.queue.CompletedValue());
    Expect(f.owner->DrainInfoQueue(), fault == 0   ? D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_DESCRIPTOR_UNINITIALIZED
                                      : fault == 1 ? D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INVALID_RESOURCE
                                                   : D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_RESOURCE_STATE);
}
void ShadowProbe(bool insertShaderReadBarrier)
{
    Fixture f;
    auto& device = f.Device();
    RootSignatureFacts facts{};
    auto root = CreateM5RootSignature(device, facts);
    D3D12PsoFactory factory;
    factory.Initialize(device, *root.Get(), 0);
    auto& pso = factory.GetOrCreate({PassKind::PbrOpaque, false, 0, 0},
                                    {Shader("PbrForward.VSMain.vs.dxil"), Shader("PbrForward.PSMain.ps.dxil")});

    D3D12DescriptorHeap srv, rtv, dsv;
    srv.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true, L"M5-10.Negative.Shadow.SRV");
    rtv.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"M5-10.Negative.Shadow.RTV");
    dsv.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, false, L"M5-10.Negative.Shadow.DSV");

    auto materialTexture = Texture(device, false, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto iblCube = CubeTexture(device);
    auto lutTexture = LutTexture(device);
    auto target = RenderTarget(device);
    auto shadow = DepthResource(device, L"M5-10.Negative.Shadow.Depth");
    auto mainDepth = DepthResource(device, L"M5-10.Negative.Shadow.MainDepth");

    device.CreateRenderTargetView(target.Get(), nullptr, rtv.Cpu(0));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
    dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device.CreateDepthStencilView(shadow.Get(), &dsvDescription, dsv.Cpu(0));
    device.CreateDepthStencilView(mainDepth.Get(), &dsvDescription, dsv.Cpu(1));

    D3D12_SHADER_RESOURCE_VIEW_DESC textureView{};
    textureView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textureView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textureView.Texture2D.MipLevels = 1;
    for (UINT slot = 0; slot < 5; ++slot)
        device.CreateShaderResourceView(materialTexture.Get(), &textureView, srv.Cpu(slot));
    D3D12_SHADER_RESOURCE_VIEW_DESC cubeView{};
    cubeView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cubeView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    cubeView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cubeView.TextureCube.MipLevels = 1;
    device.CreateShaderResourceView(iblCube.Get(), &cubeView, srv.Cpu(5));
    device.CreateShaderResourceView(iblCube.Get(), &cubeView, srv.Cpu(6));
    D3D12_SHADER_RESOURCE_VIEW_DESC lutView{};
    lutView.Format = DXGI_FORMAT_R16G16_FLOAT;
    lutView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    lutView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lutView.Texture2D.MipLevels = 1;
    device.CreateShaderResourceView(lutTexture.Get(), &lutView, srv.Cpu(7));
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowView{};
    shadowView.Format = DXGI_FORMAT_R32_FLOAT;
    shadowView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowView.Texture2D.MipLevels = 1;
    device.CreateShaderResourceView(shadow.Get(), &shadowView, srv.Cpu(8));

    struct Guard
    {
        D3D12Queue& queue;
        ~Guard()
        {
            try
            {
                queue.FlushGpu("shadow barrier negative cleanup");
            }
            catch (...)
            {
            }
        }
    } guard{f.queue};
    auto& setupFrame = f.queue.BeginFrame(0);
    auto& setupList = f.queue.CommandList();
    ID3D12DescriptorHeap* heaps[]{&srv.Native()};
    setupList.SetDescriptorHeaps(1, heaps);

    // 先提交真实 shadow pass 的 DSV 清理，并等待 GPU 完成后再消费 setup 报告；只有
    // 这样才能排除 GPU setup 本身的消息，避免把 fixture 自身错误误判为漏 barrier。
    const auto shadowDsv = dsv.Cpu(0);
    setupList.OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    setupList.ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    const auto setupFence = f.queue.ExecuteAndSignal(setupFrame);
    f.queue.WaitForSubmittedFence(setupFence, "shadow setup clear");
    const ValidationReport setup = f.owner->DrainInfoQueue();
    ASSERT_EQ(setup.Size(), 0U) << "shadow depth setup and clear must emit zero validation messages";

    D3D12UploadRing constants;
    constants.Initialize(device, 4096);
    std::array<UploadAllocation, 4> allocations{};
    WritePbrConstants(constants, allocations);
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    const auto vertexBuffer = UploadVertices(device, vertexView);
    ASSERT_NE(vertexBuffer.Get(), nullptr) << "shadow read draw needs a live vertex buffer";

    auto& frame = f.queue.BeginFrame(0);
    auto& list = f.queue.CommandList();
    list.SetDescriptorHeaps(1, heaps);

    if (insertShaderReadBarrier)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shadow.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list.ResourceBarrier(1, &barrier);
    }

    list.SetGraphicsRootSignature(root.Get());
    list.SetPipelineState(&pso);
    for (UINT index = 0; index < 4; ++index)
        list.SetGraphicsRootConstantBufferView(index, allocations[index].gpu);
    list.SetGraphicsRootDescriptorTable(4, srv.Gpu(0));
    list.SetGraphicsRootDescriptorTable(5, srv.Gpu(5));
    const auto targetRtv = rtv.Cpu(0);
    const auto mainDsv = dsv.Cpu(1);
    list.OMSetRenderTargets(1, &targetRtv, FALSE, &mainDsv);
    const float targetClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    list.ClearRenderTargetView(targetRtv, targetClear, 0, nullptr);
    list.ClearDepthStencilView(mainDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    const D3D12_RECT scissor{0, 0, 4, 4};
    list.RSSetViewports(1, &viewport);
    list.RSSetScissorRects(1, &scissor);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.IASetVertexBuffers(0, 1, &vertexView);
    list.DrawInstanced(3, 1, 0, 0);

    const auto fence = f.queue.ExecuteAndSignal(frame);
    constants.CommitFrame(fence);
    f.queue.WaitForSubmittedFence(fence,
                                  insertShaderReadBarrier ? "shadow barrier positive" : "shadow barrier negative");
    constants.Reclaim(f.queue.CompletedValue());
    const ValidationReport report = f.owner->DrainInfoQueue();
    if (insertShaderReadBarrier)
    {
        EXPECT_FALSE(report.HasFailure()) << "explicit shadow depth shader-read barrier must be clean";
    }
    else
    {
        ExpectShadowStateMismatch(report);
    }
}
} // namespace
TEST(D3D12ValidationNegative, UninitializedDescriptor)
{
    ShaderProbe(0);
}
TEST(D3D12ValidationNegative, DeletedResourceDescriptor)
{
    ShaderProbe(1);
}
TEST(D3D12ValidationNegative, IncompatibleShaderReadState)
{
    ShaderProbe(2);
}
TEST(D3D12ValidationNegative, ShadowDepthReadWithExplicitBarrier)
{
    ShadowProbe(true);
}
TEST(D3D12ValidationNegative, MissingShadowDepthReadBarrier)
{
    ShadowProbe(false);
}
TEST(D3D12ValidationNegative, AllocatorResetBeforeFence)
{
    Fixture f(false, true);
    EXPECT_EQ(f.owner->DeviceRemovedReason(), 0);
    auto target = Texture(f.Device(), true, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12DescriptorHeap rtv;
    rtv.Initialize(f.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"M5-10.Allocator.Target");
    f.Device().CreateRenderTargetView(target.Get(), nullptr, rtv.Cpu(0));
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    ThrowIfFailed(f.Device().CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "CPU gate");
    // CPU Signal 保证断言或异常路径也能解除短暂队列等待。
    struct Guard
    {
        ID3D12Fence& gate;
        D3D12Queue& queue;
        ~Guard()
        {
            static_cast<void>(gate.Signal(1));
            try
            {
                queue.FlushGpu("gate cleanup");
            }
            catch (...)
            {
            }
        }
    } guard{*gate.Get(), f.queue};
    auto& frame = f.queue.BeginFrame(0);
    const float color[]{0.25F, 0.5F, 0.75F, 1.0F};
    f.queue.CommandList().ClearRenderTargetView(rtv.Cpu(0), color, 0, nullptr);
    ThrowIfFailed(f.queue.NativeQueue().Wait(gate.Get(), 1), "queue wait");
    auto fence = f.queue.ExecuteAndSignal(frame);
    EXPECT_LT(f.queue.CompletedValue(), fence);
    f.owner->UpdateRemovalContext(fence + 1U, fence, f.queue.CompletedValue());
    f.owner->RecordDiagnosticEvent("M5-10.Allocator.Clear");
    f.owner->UpdateDiagnosticRevisions("fixture-clear-r1", "no-shader");
    EXPECT_FALSE(f.owner->DrainInfoQueue().HasFailure()) << "negative fixture setup must be clean";
    const HRESULT reset = frame.directAllocator->Reset();
    std::cout << "allocator Reset HRESULT=" << reset << std::endl;
    static_cast<void>(gate->Signal(1));
    try
    {
        f.queue.FlushGpu("allocator validation completion");
    }
    catch (const std::exception& error)
    {
        std::cout << error.what() << std::endl;
    }
    // 当前 SDK 的 CPU 调试线程在 gate 解锁后检出 541 并逻辑移除 Device；不是 TDR。
    Expect(f.owner->DrainInfoQueue(), D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_RESET);
    EXPECT_NE(f.owner->DeviceRemovedReason(), 0) << "首次 S_OK 不得掩盖后续 removal";
    f.owner->ReportDeviceRemoved();
}
TEST(D3D12ValidationNegative, PresentOutsidePresentState)
{
    Fixture f(false);
    HWND window = CreateWindowExW(0, L"STATIC", L"M5-10 negative Present", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);
    struct Guard
    {
        HWND window;
        ~Guard()
        {
            DestroyWindow(window);
        }
    } guard{window};
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap;
    auto* factory = static_cast<IDXGIFactory7*>(f.owner->NativeFactoryHandle());
    ThrowIfFailed(factory->CreateSwapChainForHwnd(&f.queue.NativeQueue(), window, &desc, nullptr, nullptr, &swap),
                  "negative swap");
    Microsoft::WRL::ComPtr<ID3D12Resource> back;
    ThrowIfFailed(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "negative back");
    auto& frame = f.queue.BeginFrame(0);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    f.queue.CommandList().ResourceBarrier(1, &b);
    auto fence = f.queue.ExecuteAndSignal(frame);
    f.queue.WaitForSubmittedFence(fence, "negative present");
    EXPECT_FALSE(f.owner->DrainInfoQueue().HasFailure()) << "negative fixture setup must be clean";
    static_cast<void>(swap->Present(0, 0));
    f.queue.FlushGpu("present cleanup");
    Expect(f.owner->DrainInfoQueue(), D3D12_MESSAGE_ID_INVALID_SUBRESOURCE_STATE);
}
