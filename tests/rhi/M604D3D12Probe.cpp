#include "M604D3D12Probe.h"
#include "D3D12Capabilities.h"
#include "D3D12RootSignature.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <limits>
#include <stdexcept>
#include <wrl/client.h>

namespace MiniEngine::Rhi::M604
{
using D3D12::ThrowIfFailed;
using Microsoft::WRL::ComPtr;
namespace
{
DXGI_FORMAT NativeFormat(Format format)
{
    switch (format)
    {
    case Format::Unknown:
        return DXGI_FORMAT_UNKNOWN;
    case Format::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8UnormSrgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Rg16Float:
        return DXGI_FORMAT_R16G16_FLOAT;
    case Format::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case Format::D24UnormS8Uint:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32Float:
        return DXGI_FORMAT_D32_FLOAT;
    default:
        throw std::runtime_error("unknown texture format");
    }
}
DXGI_FORMAT NativeVertex(VertexFormat format)
{
    switch (format)
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
const char* Semantic(VertexSemantic semantic)
{
    switch (semantic)
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
    throw std::runtime_error("unknown vertex semantic");
}
D3D12_COMPARISON_FUNC Compare(CompareOp value)
{
    switch (value)
    {
    case CompareOp::Never:
        return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less:
        return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::LessEqual:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Equal:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::GreaterEqual:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::Always:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    throw std::runtime_error("unknown compare function");
}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
D3D12_RESOURCE_DESC Texture2D(UINT width, UINT height, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Format = format;
    d.Flags = flags;
    return d;
}
D3D12_RESOURCE_DESC Texture(UINT size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    return Texture2D(size, size, format, flags);
}
void Transition(ID3D12GraphicsCommandList& list, ID3D12Resource& resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {&resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list.ResourceBarrier(1, &barrier);
}
void Wait(ID3D12Fence& fence, UINT64 value)
{
    const auto deadline = GetTickCount64() + 5000;
    while (fence.GetCompletedValue() < value)
    {
        if (GetTickCount64() > deadline)
            throw std::runtime_error("M6-04 D3D12 fence timeout");
        Sleep(1);
    }
    if (fence.GetCompletedValue() == std::numeric_limits<UINT64>::max())
        throw std::runtime_error("M6-04 device removed");
}
} // namespace
struct D3D12Probe::Impl
{
    struct Pipeline final : ResourcePayload
    {
        ComPtr<ID3D12PipelineState> state;
        ComPtr<ID3D12Resource> hdr, target, readback, constants;
        ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 readbackBytes = 0;
        bool toneMap = false;
    };
    struct M606Resources final
    {
        UINT width = 0;
        UINT height = 0;
        std::uint64_t completion = 0;
        ComPtr<ID3D12Resource> depth;
        ComPtr<ID3D12Resource> target;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12Resource> toneConstants;
        ComPtr<ID3D12Resource> depthConstants;
        ComPtr<ID3D12Resource> vertices;
        ComPtr<ID3D12Resource> indices;
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12DescriptorHeap> srvHeap;
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> timestampReadback;
        UINT64 timestampFrequency = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 readbackBytes = 0;
    };
    std::unique_ptr<D3D12::D3D12Device> owner;
    RhiCapabilities capabilities;
    M606Resources m606;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial = 0;
    std::uint64_t created = 0;
    explicit Impl(bool warp)
    {
        D3D12::DeviceCreateOptions options;
        options.warp = warp;
        options.debugLayer = true;
        options.gpuValidation = !warp;
        owner = D3D12::D3D12Device::Create(options);
        capabilities = D3D12::QueryCapabilities(*owner);
        if (!owner->Metadata().debugLayerActive || owner->Metadata().gpuValidationActive != !warp)
            throw std::runtime_error("requested native validation inactive");
        D3D12::RootSignatureFacts facts;
        root = D3D12::CreateM5RootSignature(Device(), facts);
        ++created;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(Device().CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "M6-04 queue");
        ++created;
        ThrowIfFailed(Device().CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                      "M6-04 allocator");
        ++created;
        ThrowIfFailed(Device().CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&commands)),
                      "M6-04 list");
        ++created;
        ThrowIfFailed(commands->Close(), "initial close");
        ThrowIfFailed(Device().CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "M6-04 fence");
        ++created;
    }
    ID3D12Device& Device()
    {
        return *static_cast<ID3D12Device*>(owner->NativeDeviceHandle());
    }
    ComPtr<ID3D12Resource> Resource(D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
                                    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clearValue = nullptr)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        ComPtr<ID3D12Resource> value;
        ThrowIfFailed(Device().CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clearValue,
                                                       IID_PPV_ARGS(&value)),
                      "M6-04 resource");
        ++created;
        return value;
    }
    void Begin()
    {
        Wait(*fence.Get(), serial);
        ThrowIfFailed(allocator->Reset(), "reset allocator");
        ThrowIfFailed(commands->Reset(allocator.Get(), nullptr), "reset commands");
    }
    void Submit()
    {
        ThrowIfFailed(commands->Close(), "close commands");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        ThrowIfFailed(queue->Signal(fence.Get(), ++serial), "signal fence");
        Wait(*fence.Get(), serial);
    }
    void CheckClean()
    {
        const auto report = owner->DrainInfoQueue();
        if (report.HasFailure())
            throw std::runtime_error("M6-04 normal D3D12 diagnostic failure");
    }
    void Census()
    {
        CheckClean();
        Begin();
        ThrowIfFailed(commands->Close(), "census reset close");
        ComPtr<ID3D12InfoQueue> info;
        ComPtr<ID3D12DebugDevice> debug;
        ThrowIfFailed(Device().QueryInterface(IID_PPV_ARGS(&info)), "census InfoQueue");
        ThrowIfFailed(Device().QueryInterface(IID_PPV_ARGS(&debug)), "census debug device");
        ThrowIfFailed(debug->ReportLiveDeviceObjects(
                          static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)),
                      "D3D12 live census");
        bool sawDevice = false;
        std::string leaks;
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &size), "census size");
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            ThrowIfFailed(info->GetMessage(i, message, &size), "census message");
            sawDevice |= message->ID == D3D12_MESSAGE_ID_LIVE_DEVICE;
            if (message->ID == D3D12_MESSAGE_ID_LIVE_PIPELINESTATE || message->ID == D3D12_MESSAGE_ID_LIVE_RESOURCE ||
                message->ID == D3D12_MESSAGE_ID_LIVE_DESCRIPTORHEAP)
                leaks += std::string(message->pDescription, message->DescriptionByteLength) + "\n";
        }
        info->ClearStoredMessages();
        if (!sawDevice || !leaks.empty())
            throw std::runtime_error("D3D12 pipeline census missing or leaking: " + leaks);
    }
    void PrepareToneMap(Pipeline& p)
    {
        auto sourceDesc = Texture(1, DXGI_FORMAT_R32G32B32A32_FLOAT);
        p.hdr = Resource(D3D12_HEAP_TYPE_DEFAULT, sourceDesc, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sourceFootprint{};
        UINT64 sourceBytes = 0;
        Device().GetCopyableFootprints(&sourceDesc, 0, 1, 0, &sourceFootprint, nullptr, nullptr, &sourceBytes);
        auto upload = Resource(D3D12_HEAP_TYPE_UPLOAD, Buffer(std::max<UINT64>(sourceBytes, 256)),
                               D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        ThrowIfFailed(upload->Map(0, &none, &mapped), "map hdr upload");
        const std::array<float, 4> hdr{4, 2, 1, 1};
        std::memcpy(mapped, hdr.data(), sizeof(hdr));
        upload->Unmap(0, nullptr);
        Begin();
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = p.hdr.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = sourceFootprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(*commands.Get(), *p.hdr.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Submit();
        // upload 到此已获真实 fence 完成证明，离开作用域可释放。
        const auto targetDesc = Texture(4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clear{};
        clear.Format = targetDesc.Format;
        p.target = Resource(D3D12_HEAP_TYPE_DEFAULT, targetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
        Device().GetCopyableFootprints(&targetDesc, 0, 1, 0, &p.footprint, nullptr, nullptr, &p.readbackBytes);
        p.readback = Resource(D3D12_HEAP_TYPE_READBACK, Buffer(p.readbackBytes), D3D12_RESOURCE_STATE_COPY_DEST);
        p.constants = Resource(D3D12_HEAP_TYPE_UPLOAD, Buffer(256), D3D12_RESOURCE_STATE_GENERIC_READ);
        ThrowIfFailed(p.constants->Map(0, &none, &mapped), "map ToneMap constants");
        std::memset(mapped, 0, 256);
        const std::array<float, 4> constants{0, 0, 0.25F, 0.25F};
        std::memcpy(mapped, constants.data(), sizeof(constants));
        p.constants->Unmap(0, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        ThrowIfFailed(Device().CreateDescriptorHeap(&heap, IID_PPV_ARGS(&p.rtvHeap)), "RTV heap");
        ++created;
        Device().CreateRenderTargetView(p.target.Get(), nullptr, p.rtvHeap->GetCPUDescriptorHandleForHeapStart());
        ++created;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 9;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(Device().CreateDescriptorHeap(&heap, IID_PPV_ARGS(&p.srvHeap)), "SRV heap");
        ++created;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = sourceDesc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        auto handle = p.srvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto increment = Device().GetDescriptorHandleIncrementSize(heap.Type);
        for (unsigned i = 0; i < 9; ++i)
        {
            Device().CreateShaderResourceView(i == 0 ? p.hdr.Get() : nullptr, &srv, handle);
            ++created;
            handle.ptr += increment;
        }
    }
    void EnsureM606(UINT width, UINT height)
    {
        if (width == 0 || height == 0)
            throw std::invalid_argument("M606 D3D12 extent must be positive");
        if (m606.width == width && m606.height == height && m606.target && m606.readback)
            return;

        // All M606 draws wait for their fence before this point; release only after
        // the previous submission has completed, then rebuild the size-dependent set.
        Wait(*fence.Get(), serial);
        const auto completion = m606.completion;
        m606 = {};
        m606.completion = completion;
        m606.width = width;
        m606.height = height;

        const auto targetDesc =
            Texture2D(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE targetClear{};
        targetClear.Format = targetDesc.Format;
        targetClear.Color[0] = 0.04F;
        targetClear.Color[1] = 0.08F;
        targetClear.Color[2] = 0.14F;
        targetClear.Color[3] = 1.0F;
        m606.target = Resource(D3D12_HEAP_TYPE_DEFAULT, targetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &targetClear);
        Device().GetCopyableFootprints(&targetDesc, 0, 1, 0, &m606.footprint, nullptr, nullptr, &m606.readbackBytes);
        m606.readback = Resource(D3D12_HEAP_TYPE_READBACK, Buffer(m606.readbackBytes), D3D12_RESOURCE_STATE_COPY_DEST);

        const auto depthDesc =
            Texture2D(width, height, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0F;
        depthClear.DepthStencil.Stencil = 0;
        m606.depth = Resource(D3D12_HEAP_TYPE_DEFAULT, depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(Device().CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m606.rtvHeap)), "M606 D3D12 RTV heap");
        ++created;
        Device().CreateRenderTargetView(m606.target.Get(), nullptr, m606.rtvHeap->GetCPUDescriptorHandleForHeapStart());
        ++created;

        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(Device().CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m606.dsvHeap)), "M606 D3D12 DSV heap");
        ++created;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        Device().CreateDepthStencilView(m606.depth.Get(), &dsv, m606.dsvHeap->GetCPUDescriptorHandleForHeapStart());
        ++created;

        D3D12_QUERY_HEAP_DESC timestampDesc{};
        timestampDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        timestampDesc.Count = 2;
        ThrowIfFailed(Device().CreateQueryHeap(&timestampDesc, IID_PPV_ARGS(&m606.timestamps)),
                      "M606 D3D12 timestamp heap");
        ++created;
        ThrowIfFailed(queue->GetTimestampFrequency(&m606.timestampFrequency), "M606 D3D12 timestamp frequency");
        m606.timestampReadback =
            Resource(D3D12_HEAP_TYPE_READBACK, Buffer(sizeof(std::uint64_t) * 2), D3D12_RESOURCE_STATE_COPY_DEST);

        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 9;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(Device().CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m606.srvHeap)), "M606 D3D12 SRV heap");
        ++created;
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1;
        Device().CreateShaderResourceView(m606.depth.Get(), &depthSrv,
                                          m606.srvHeap->GetCPUDescriptorHandleForHeapStart());
        ++created;
        const auto increment = Device().GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = depthSrv;
        for (UINT index = 1; index < 9; ++index)
        {
            auto handle = m606.srvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * increment;
            Device().CreateShaderResourceView(nullptr, &nullSrv, handle);
            ++created;
        }

        D3D12_RANGE none{0, 0};
        m606.toneConstants = Resource(D3D12_HEAP_TYPE_UPLOAD, Buffer(256), D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        ThrowIfFailed(m606.toneConstants->Map(0, &none, &mapped), "M606 tone constants map");
        std::memset(mapped, 0, 256);
        m606.toneConstants->Unmap(0, nullptr);

        m606.depthConstants = Resource(D3D12_HEAP_TYPE_UPLOAD, Buffer(256), D3D12_RESOURCE_STATE_GENERIC_READ);
        ThrowIfFailed(m606.depthConstants->Map(0, &none, &mapped), "M606 depth constants map");
        std::array<float, 52> identity{};
        for (std::size_t matrix = 0; matrix < 3; ++matrix)
            for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
                identity[matrix * 16 + diagonal * 5] = 1.0F;
        identity[48] = 1.0F;
        std::memcpy(mapped, identity.data(), sizeof(identity));
        m606.depthConstants->Unmap(0, nullptr);

        const std::array<float, 9> vertices{-0.75F, -0.75F, 0.25F, 0.0F, 0.75F, 0.25F, 0.75F, -0.75F, 0.25F};
        m606.vertices = Resource(D3D12_HEAP_TYPE_UPLOAD, Buffer(sizeof(vertices)), D3D12_RESOURCE_STATE_GENERIC_READ);
        ThrowIfFailed(m606.vertices->Map(0, &none, &mapped), "M606 vertices map");
        std::memcpy(mapped, vertices.data(), sizeof(vertices));
        m606.vertices->Unmap(0, nullptr);

        const std::array<std::uint16_t, 3> indices{0, 1, 2};
        m606.indices = Resource(D3D12_HEAP_TYPE_UPLOAD, Buffer(sizeof(indices)), D3D12_RESOURCE_STATE_GENERIC_READ);
        ThrowIfFailed(m606.indices->Map(0, &none, &mapped), "M606 indices map");
        std::memcpy(mapped, indices.data(), sizeof(indices));
        m606.indices->Unmap(0, nullptr);
        CheckClean();
    }
    M606Readback ReadM606()
    {
        void* data = nullptr;
        D3D12_RANGE read{0, static_cast<SIZE_T>(m606.readbackBytes)};
        D3D12_RANGE timestampRead{0, sizeof(std::uint64_t) * 2};
        void* timestampData = nullptr;
        ThrowIfFailed(m606.timestampReadback->Map(0, &timestampRead, &timestampData),
                      "M606 D3D12 timestamp readback map");
        std::array<std::uint64_t, 2> timestamps{};
        std::memcpy(timestamps.data(), timestampData, sizeof(timestamps));
        D3D12_RANGE timestampNone{0, 0};
        m606.timestampReadback->Unmap(0, &timestampNone);
        ThrowIfFailed(m606.readback->Map(0, &read, &data), "M606 D3D12 readback map");
        const std::size_t rowBytes = static_cast<std::size_t>(m606.width) * 4;
        M606Readback result;
        result.width = m606.width;
        result.height = m606.height;
        result.completion = m606.completion;
        result.timestampBegin = timestamps[0];
        result.timestampEnd = timestamps[1];
        result.timestampFrequency = m606.timestampFrequency;
        result.timestampDisjoint = false;
        result.rgba.resize(rowBytes * m606.height);
        for (UINT row = 0; row < m606.height; ++row)
            std::memcpy(result.rgba.data() + static_cast<std::size_t>(row) * rowBytes,
                        static_cast<const std::byte*>(data) +
                            static_cast<std::size_t>(row) * m606.footprint.Footprint.RowPitch,
                        rowBytes);
        D3D12_RANGE none{0, 0};
        m606.readback->Unmap(0, &none);
        CheckClean();
        return result;
    }
    void CopyM606Target()
    {
        Transition(*commands.Get(), *m606.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = m606.target.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m606.readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = m606.footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(*commands.Get(), *m606.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    M606Readback DrawM606Clear(UINT width, UINT height)
    {
        EnsureM606(width, height);
        const auto before = created;
        Begin();
        commands->EndQuery(m606.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const auto rtv = m606.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        const float clear[]{0.04F, 0.08F, 0.14F, 1.0F};
        commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
        CopyM606Target();
        commands->EndQuery(m606.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        commands->ResolveQueryData(m606.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                   m606.timestampReadback.Get(), 0);
        Submit();
        ++m606.completion;
        auto result = ReadM606();
        if (created != before)
            throw std::runtime_error("native creation in M606 D3D12 clear");
        return result;
    }
    M606Readback DrawM606DepthToneMap(ResourcePayload& depthPayload, ResourcePayload& tonePayload, UINT width,
                                      UINT height, float exposure)
    {
        EnsureM606(width, height);
        auto& depth = dynamic_cast<Pipeline&>(depthPayload);
        auto& tone = dynamic_cast<Pipeline&>(tonePayload);
        if (!depth.state || depth.toneMap || !tone.state || !tone.toneMap)
            throw std::runtime_error("M606 D3D12 pipeline roles are invalid");
        const auto before = created;
        Begin();
        commands->EndQuery(m606.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            commands->RSSetViewports(1, &viewport);
            commands->RSSetScissorRects(1, &scissor);
            commands->SetGraphicsRootSignature(root.Get());
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            commands->SetPipelineState(depth.state.Get());
            commands->SetGraphicsRootConstantBufferView(1, m606.depthConstants->GetGPUVirtualAddress());
            D3D12_VERTEX_BUFFER_VIEW vertexView{m606.vertices->GetGPUVirtualAddress(),
                                                static_cast<UINT>(sizeof(float) * 9), sizeof(float) * 3};
            D3D12_INDEX_BUFFER_VIEW indexView{m606.indices->GetGPUVirtualAddress(),
                                              static_cast<UINT>(sizeof(std::uint16_t) * 3), DXGI_FORMAT_R16_UINT};
            commands->IASetVertexBuffers(0, 1, &vertexView);
            commands->IASetIndexBuffer(&indexView);
            const auto dsv = m606.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            commands->OMSetRenderTargets(0, nullptr, TRUE, &dsv);
            commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);
            commands->DrawIndexedInstanced(3, 1, 0, 0, 0);

            Transition(*commands.Get(), *m606.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->SetPipelineState(tone.state.Get());
            D3D12_RANGE none{0, 0};
            void* mapped = nullptr;
            ThrowIfFailed(m606.toneConstants->Map(0, &none, &mapped), "M606 tone constants update");
            const std::array<float, 4> toneConstants{exposure, 0.0F, 1.0F / static_cast<float>(width),
                                                     1.0F / static_cast<float>(height)};
            std::memset(mapped, 0, 256);
            std::memcpy(mapped, toneConstants.data(), sizeof(toneConstants));
            m606.toneConstants->Unmap(0, nullptr);
            for (UINT rootIndex = 0; rootIndex < 4; ++rootIndex)
                commands->SetGraphicsRootConstantBufferView(rootIndex, m606.toneConstants->GetGPUVirtualAddress());
            ID3D12DescriptorHeap* heaps[]{m606.srvHeap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetGraphicsRootDescriptorTable(4, m606.srvHeap->GetGPUDescriptorHandleForHeapStart());
            auto global = m606.srvHeap->GetGPUDescriptorHandleForHeapStart();
            global.ptr += 5ULL * Device().GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            commands->SetGraphicsRootDescriptorTable(5, global);
            const auto rtv = m606.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float clear[]{0.04F, 0.08F, 0.14F, 1.0F};
            commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
            commands->DrawInstanced(3, 1, 0, 0);
            CopyM606Target();
            Transition(*commands.Get(), *m606.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        commands->EndQuery(m606.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        commands->ResolveQueryData(m606.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                   m606.timestampReadback.Get(), 0);
        Submit();
        ++m606.completion;
        auto result = ReadM606();
        if (created != before)
            throw std::runtime_error("native creation in M606 D3D12 depth/tone");
        return result;
    }
    void ResetM606()
    {
        m606 = {};
    }
    std::unique_ptr<ResourcePayload> Create(const ShaderDesc& vs, const ShaderDesc* ps, const GraphicsPipelineDesc& p)
    {
        auto payload = std::make_unique<Pipeline>();
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        for (const auto& a : p.vertexAttributes)
            elements.push_back({Semantic(a.semantic), a.semanticIndex, NativeVertex(a.format), a.bufferSlot, a.offset,
                                a.instanceStepRate ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                   : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                                a.instanceStepRate});
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.VS = {vs.bytecode.data(), vs.bytecode.size()};
        if (ps)
            d.PS = {ps->bytecode.data(), ps->bytecode.size()};
        d.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.SampleMask = UINT_MAX;
        d.SampleDesc.Count = p.sampleCount;
        d.NumRenderTargets = p.colorAttachmentCount;
        for (unsigned i = 0; i < p.colorAttachmentCount; ++i)
            d.RTVFormats[i] = NativeFormat(p.colorFormats[i]);
        d.DSVFormat = NativeFormat(p.depthFormat);
        auto& r = d.RasterizerState;
        r.FillMode = D3D12_FILL_MODE_SOLID;
        r.CullMode = p.cullMode == CullMode::None    ? D3D12_CULL_MODE_NONE
                     : p.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                     : D3D12_CULL_MODE_BACK;
        r.FrontCounterClockwise = p.frontFace == FrontFace::CounterClockwise;
        r.DepthClipEnable = p.depthClip;
        r.DepthBias = p.depthBias;
        r.DepthBiasClamp = p.depthBiasClamp;
        r.SlopeScaledDepthBias = p.slopeScaledDepthBias;
        for (auto& blend : d.BlendState.RenderTarget)
        {
            blend.BlendEnable = p.alphaBlend;
            blend.SrcBlend = p.alphaBlend ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
            blend.DestBlend = p.alphaBlend ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = p.alphaBlend ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        auto& depth = d.DepthStencilState;
        depth.DepthEnable = p.depthTest;
        depth.DepthWriteMask = p.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = Compare(p.depthCompare);
        depth.StencilReadMask = depth.StencilWriteMask = 255;
        depth.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                           D3D12_COMPARISON_FUNC_ALWAYS};
        depth.BackFace = depth.FrontFace;
        ThrowIfFailed(Device().CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&payload->state)), "M6-04 PSO");
        ++created;
        payload->toneMap = p.vertexAttributes.empty() && p.colorAttachmentCount == 1 &&
                           p.colorFormats[0] == Format::Rgba8Unorm && p.depthFormat == Format::Unknown;
        if (payload->toneMap)
            PrepareToneMap(*payload);
        CheckClean();
        return payload;
    }
    std::vector<std::byte> Draw(ResourcePayload& resource)
    {
        auto& p = dynamic_cast<Pipeline&>(resource);
        if (!p.toneMap)
            throw std::runtime_error("fixed smoke only draws ToneMap");
        const auto before = created;
        Begin();
        commands->SetPipelineState(p.state.Get());
        commands->SetGraphicsRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{p.srvHeap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        for (UINT i = 0; i < 4; ++i)
            commands->SetGraphicsRootConstantBufferView(i, p.constants->GetGPUVirtualAddress());
        auto gpu = p.srvHeap->GetGPUDescriptorHandleForHeapStart();
        commands->SetGraphicsRootDescriptorTable(4, gpu);
        gpu.ptr += 5ULL * Device().GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        commands->SetGraphicsRootDescriptorTable(5, gpu);
        const auto rtv = p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
        const D3D12_RECT scissor{0, 0, 4, 4};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        const float clear[4]{};
        commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        Transition(*commands.Get(), *p.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = p.target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = p.readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = p.footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(*commands.Get(), *p.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        Submit();
        void* data = nullptr;
        D3D12_RANGE read{0, static_cast<SIZE_T>(p.readbackBytes)};
        ThrowIfFailed(p.readback->Map(0, &read, &data), "ToneMap readback");
        std::vector<std::byte> bytes(64);
        for (std::size_t row = 0; row < 4; ++row)
            std::memcpy(bytes.data() + row * 16,
                        static_cast<const std::byte*>(data) + row * p.footprint.Footprint.RowPitch, 16);
        D3D12_RANGE none{0, 0};
        p.readback->Unmap(0, &none);
        CheckClean();
        if (created != before)
            throw std::runtime_error("native creation occurred in draw");
        return bytes;
    }
};
D3D12Probe::D3D12Probe(bool warp) : m_impl(std::make_unique<Impl>(warp))
{
}
D3D12Probe::~D3D12Probe() = default;
std::unique_ptr<ResourcePayload> D3D12Probe::CreatePipeline(const ShaderDesc& vs, const ShaderDesc* ps,
                                                            const GraphicsPipelineDesc& pipeline)
{
    return m_impl->Create(vs, ps, pipeline);
}
std::vector<std::byte> D3D12Probe::DrawToneMap(ResourcePayload& payload)
{
    return m_impl->Draw(payload);
}
std::uint64_t D3D12Probe::NativeCreationCount() const
{
    return m_impl->created;
}
void D3D12Probe::CheckClean()
{
    m_impl->CheckClean();
}
M606Readback D3D12Probe::DrawM606Clear(std::uint32_t width, std::uint32_t height)
{
    return m_impl->DrawM606Clear(width, height);
}
M606Readback D3D12Probe::DrawM606DepthToneMap(ResourcePayload& depthPipeline, ResourcePayload& tonePipeline,
                                              std::uint32_t width, std::uint32_t height, float exposure)
{
    return m_impl->DrawM606DepthToneMap(depthPipeline, tonePipeline, width, height, exposure);
}
void D3D12Probe::ResetM606()
{
    m_impl->ResetM606();
}
void D3D12Probe::CheckNoPipelineResources()
{
    m_impl->Census();
}
RhiCapabilities D3D12Probe::Capabilities() const
{
    return m_impl->capabilities;
}
} // namespace MiniEngine::Rhi::M604
