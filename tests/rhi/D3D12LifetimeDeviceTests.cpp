#include "D3D12Capabilities.h"
#include "DeviceLifetime.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <Windows.h>
#include <array>
#include <cstring>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <wrl/client.h>

using namespace MiniEngine::Rhi;
using namespace MiniEngine::Rhi::D3D12;
using Microsoft::WRL::ComPtr;
namespace
{
struct Run
{
    const char* name;
    bool warp;
    bool gbv;
};
void PrintTo(const Run& run, std::ostream* stream)
{
    *stream << run.name;
}
ComPtr<ID3D12Resource> Resource(ID3D12Device& device, D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
                                D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(
        device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
        "M6-03 CreateCommittedResource");
    return resource;
}
D3D12_RESOURCE_DESC BufferDescription(UINT64 size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
void Wait(ID3D12Fence& fence, UINT64 value)
{
    const auto deadline = GetTickCount64() + 5000;
    while (fence.GetCompletedValue() < value)
    {
        if (GetTickCount64() >= deadline)
        {
            throw std::runtime_error("M6-03 fence timeout");
        }
        Sleep(1);
    }
    if (fence.GetCompletedValue() == std::numeric_limits<UINT64>::max())
    {
        throw std::runtime_error("M6-03 device removed during fence wait");
    }
}
void Transition(ID3D12GraphicsCommandList& list, ID3D12Resource& resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {&resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list.ResourceBarrier(1, &barrier);
}
struct NativePayload final : ResourcePayload
{
    NativePayload(ComPtr<ID3D12Resource> value, int& count) : resource(std::move(value)), live(count)
    {
        ++live;
    }
    ~NativePayload() override
    {
        resource.Reset();
        --live;
    }
    ComPtr<ID3D12Resource> resource;
    int& live;
};
struct Rig final
{
    std::unique_ptr<D3D12Device> owner;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> done;
    explicit Rig(Run run)
    {
        DeviceCreateOptions options;
        options.debugLayer = true;
        options.gpuValidation = run.gbv;
        options.warp = run.warp;
        owner = D3D12Device::Create(options);
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(Device().CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "create queue");
        ThrowIfFailed(Device().CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                      "allocator");
        ThrowIfFailed(Device().CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&list)),
                      "command list");
        ThrowIfFailed(Device().CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "completion fence");
    }
    ID3D12Device& Device()
    {
        return *static_cast<ID3D12Device*>(owner->NativeDeviceHandle());
    }
    void Submit(UINT64 serial)
    {
        ThrowIfFailed(list->Close(), "close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ThrowIfFailed(queue->Signal(done.Get(), serial), "signal");
    }
    void CheckCleanResources()
    {
        auto report = owner->DrainInfoQueue();
        for (const auto& message : report.messages)
        {
            std::cout << "D12_NORMAL id=" << message.id << " " << message.description << '\n';
        }
        EXPECT_FALSE(report.HasFailure());
        ComPtr<ID3D12DebugDevice> debug;
        ASSERT_HRESULT_SUCCEEDED(Device().QueryInterface(IID_PPV_ARGS(&debug)));
        ASSERT_HRESULT_SUCCEEDED(debug->ReportLiveDeviceObjects(
            static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)));
        auto live = owner->DrainInfoQueue();
        bool reportedDevice = false;
        for (const auto& message : live.messages)
        {
            reportedDevice |= message.id == static_cast<std::uint32_t>(D3D12_MESSAGE_ID_LIVE_DEVICE);
            EXPECT_NE(message.id, static_cast<std::uint32_t>(D3D12_MESSAGE_ID_LIVE_RESOURCE)) << message.description;
        }
        EXPECT_TRUE(reportedDevice);
    }
};
class D3D12LifetimeDeviceTest : public testing::TestWithParam<Run>
{
};
TEST_P(D3D12LifetimeDeviceTest, BufferUploadBeforeFirstFrameWaitsForActualFence)
{
    const auto run = GetParam();
    Rig rig(run);
    ASSERT_TRUE(rig.owner->Metadata().debugLayerActive);
    ASSERT_EQ(rig.owner->Metadata().gpuValidationActive, run.gbv);
    int live = 0;
    DeviceLifetime lifetime(QueryCapabilities(*rig.owner));
    BufferDesc desc{256, BufferUsage::Vertex | BufferUsage::CopySource | BufferUsage::CopyDestination,
                    MemoryDomain::GpuOnly, "M6-03.buffer"};
    bool allocated = false;
    EXPECT_THROW(lifetime.CreateBuffer({},
                                       [&]
                                       {
                                           allocated = true;
                                           return std::make_unique<NativePayload>(ComPtr<ID3D12Resource>{}, live);
                                       }),
                 RhiValidationError);
    EXPECT_FALSE(allocated);
    auto buffer = lifetime.CreateBuffer(
        desc,
        [&]
        {
            return std::make_unique<NativePayload>(
                Resource(rig.Device(), D3D12_HEAP_TYPE_DEFAULT, BufferDescription(256), D3D12_RESOURCE_STATE_COPY_DEST),
                live);
        });
    auto upload =
        Resource(rig.Device(), D3D12_HEAP_TYPE_UPLOAD, BufferDescription(256), D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback =
        Resource(rig.Device(), D3D12_HEAP_TYPE_READBACK, BufferDescription(256), D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<std::byte, 256> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        expected[i] = static_cast<std::byte>((i * 17 + 3) & 255);
    }
    ComPtr<ID3D12Fence> gate;
    ASSERT_HRESULT_SUCCEEDED(rig.Device().CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    // 即使断言失败也解除 CPU gate，避免将等待留给共享 GPU 队列。
    struct GateRelease
    {
        ComPtr<ID3D12Fence> value;
        ~GateRelease()
        {
            value->Signal(1);
        }
    } release{gate};
    auto serial = lifetime.UploadBuffer(
        buffer, 0, expected,
        [&](ResourcePayload& payload, std::uint64_t offset, std::span<const std::byte> data, std::uint64_t ticket)
        {
            void* mapped = nullptr;
            D3D12_RANGE none{0, 0};
            ThrowIfFailed(upload->Map(0, &none, &mapped), "upload map");
            std::memcpy(mapped, data.data(), data.size());
            upload->Unmap(0, nullptr);
            auto& target = *static_cast<NativePayload&>(payload).resource.Get();
            rig.list->CopyBufferRegion(&target, offset, upload.Get(), 0, data.size());
            Transition(*rig.list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            rig.list->CopyBufferRegion(readback.Get(), 0, &target, 0, data.size());
            ThrowIfFailed(rig.queue->Wait(gate.Get(), 1), "queue gate");
            rig.Submit(ticket);
        });
    EXPECT_EQ(rig.done->GetCompletedValue(), 0U);
    lifetime.Destroy(buffer);
    EXPECT_THROW(lifetime.ValidateAlive(buffer), RhiValidationError);
    EXPECT_THROW(lifetime.Destroy(buffer), RhiValidationError);
    lifetime.Collect(rig.done->GetCompletedValue());
    EXPECT_EQ(live, 1);
    EXPECT_EQ(lifetime.Stats().retiring, 1U);
    ASSERT_HRESULT_SUCCEEDED(gate->Signal(1));
    Wait(*rig.done.Get(), serial);
    lifetime.Collect(serial);
    EXPECT_EQ(live, 0);
    void* mapped = nullptr;
    D3D12_RANGE range{0, 256};
    ASSERT_HRESULT_SUCCEEDED(readback->Map(0, &range, &mapped));
    EXPECT_EQ(std::memcmp(mapped, expected.data(), expected.size()), 0);
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    auto reused = lifetime.CreateBuffer(
        desc,
        [&]
        {
            return std::make_unique<NativePayload>(
                Resource(rig.Device(), D3D12_HEAP_TYPE_DEFAULT, BufferDescription(256), D3D12_RESOURCE_STATE_COPY_DEST),
                live);
        });
    EXPECT_NE(reused, buffer);
    EXPECT_EQ(reused.Index(), buffer.Index());
    lifetime.Destroy(reused);
    lifetime.Collect(serial);
    EXPECT_NO_THROW(lifetime.CheckShutdown());
    EXPECT_EQ(live, 0);
    upload.Reset();
    readback.Reset();
    rig.list.Reset();
    rig.allocator.Reset();
    rig.CheckCleanResources();
    std::cout << "M6_D12_LIFETIME run=" << run.name << " bytes=256 pendingBeforeGate=1 completed=" << serial
              << " alive=0 retiring=0 liveResources=0\n";
}

TEST_P(D3D12LifetimeDeviceTest, TextureRowsCrossNativeFootprintsAndRetireAfterCopy)
{
    const auto run = GetParam();
    Rig rig(run);
    int live = 0;
    DeviceLifetime lifetime(QueryCapabilities(*rig.owner));
    D3D12_RESOURCE_DESC native{};
    native.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    native.Width = native.Height = 4;
    native.DepthOrArraySize = native.MipLevels = 1;
    native.SampleDesc.Count = 1;
    native.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureDesc desc{TextureDimension::Texture2D,
                     {4, 4},
                     1,
                     1,
                     1,
                     Format::Rgba8Unorm,
                     TextureUsage::Sampled | TextureUsage::CopySource | TextureUsage::CopyDestination,
                     "M6-03.texture"};
    auto texture = lifetime.CreateTexture(
        desc,
        [&]
        {
            return std::make_unique<NativePayload>(
                Resource(rig.Device(), D3D12_HEAP_TYPE_DEFAULT, native, D3D12_RESOURCE_STATE_COPY_DEST), live);
        });
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    rig.Device().GetCopyableFootprints(&native, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto upload =
        Resource(rig.Device(), D3D12_HEAP_TYPE_UPLOAD, BufferDescription(total), D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback =
        Resource(rig.Device(), D3D12_HEAP_TYPE_READBACK, BufferDescription(total), D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<std::byte, 64> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        expected[i] = static_cast<std::byte>((i * 5 + 19) & 255);
    }
    TextureSubresourceData sub{expected, 16, 64};
    auto serial = lifetime.UploadTexture(
        texture, std::span(&sub, 1),
        [&](ResourcePayload& payload, std::span<const TextureSubresourceData> data, std::uint64_t ticket)
        {
            void* mapped = nullptr;
            D3D12_RANGE none{0, 0};
            ThrowIfFailed(upload->Map(0, &none, &mapped), "texture upload map");
            std::memset(mapped, 0, static_cast<std::size_t>(total));
            for (std::size_t row = 0; row < 4; ++row)
            {
                std::memcpy(static_cast<std::byte*>(mapped) + row * footprint.Footprint.RowPitch,
                            data[0].bytes.data() + row * data[0].rowPitch, 16);
            }
            upload->Unmap(0, nullptr);
            auto& target = *static_cast<NativePayload&>(payload).resource.Get();
            D3D12_TEXTURE_COPY_LOCATION packed{};
            packed.pResource = upload.Get();
            packed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            packed.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION tex{};
            tex.pResource = &target;
            tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rig.list->CopyTextureRegion(&tex, 0, 0, 0, &packed, nullptr);
            Transition(*rig.list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            packed.pResource = readback.Get();
            rig.list->CopyTextureRegion(&packed, 0, 0, 0, &tex, nullptr);
            Transition(*rig.list.Get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            rig.Submit(ticket);
        });
    lifetime.Destroy(texture);
    lifetime.Collect(0);
    EXPECT_EQ(live, 1);
    Wait(*rig.done.Get(), serial);
    lifetime.Collect(serial);
    EXPECT_EQ(live, 0);
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    ASSERT_HRESULT_SUCCEEDED(readback->Map(0, &range, &mapped));
    for (std::size_t row = 0; row < 4; ++row)
    {
        EXPECT_EQ(std::memcmp(static_cast<std::byte*>(mapped) + row * footprint.Footprint.RowPitch,
                              expected.data() + row * 16, 16),
                  0);
    }
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    EXPECT_NO_THROW(lifetime.CheckShutdown());
    upload.Reset();
    readback.Reset();
    rig.list.Reset();
    rig.allocator.Reset();
    rig.CheckCleanResources();
    std::cout << "M6_D12_TEXTURE run=" << run.name
              << " bytes=64 engineRowPitch=16 nativeRowPitch=" << footprint.Footprint.RowPitch
              << " alive=0 retiring=0 liveResources=0\n";
}
INSTANTIATE_TEST_SUITE_P(M6, D3D12LifetimeDeviceTest,
                         testing::Values(Run{"HardwareGBV", false, true}, Run{"WarpDebug", true, false}),
                         [](const testing::TestParamInfo<Run>& info) { return info.param.name; });

// 此 SDK 对已删除 bundle 资源可抛 0x87d validation SEH；仅捕获该已知验证异常。
// 访问违例等其他异常继续传播；通过条件仍必须包含精确 native message ID。
unsigned ValidateDeletedBundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle)
{
    __try
    {
        list->ExecuteBundle(bundle);
        return 0;
    }
    __except (GetExceptionCode() == 0x87d ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return 0x87d;
    }
}
// ExecuteBundle 的 CPU debug 校验识别已删除的资源；不向 GPU 提交该非法 bundle。
TEST(D3D12LifetimeNegative, DeletedBundleResourceIsDiagnosedBeforeGpuSubmission)
{
    Rig rig({"WarpDebug", true, false});
    ComPtr<ID3D12InfoQueue> info;
    ASSERT_HRESULT_SUCCEEDED(rig.Device().QueryInterface(IID_PPV_ARGS(&info)));
    ASSERT_HRESULT_SUCCEEDED(info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE));
    ASSERT_HRESULT_SUCCEEDED(info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE));
    ASSERT_HRESULT_SUCCEEDED(info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> bundle;
    ASSERT_HRESULT_SUCCEEDED(
        rig.Device().CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&allocator)));
    ASSERT_HRESULT_SUCCEEDED(rig.Device().CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, allocator.Get(), nullptr,
                                                            IID_PPV_ARGS(&bundle)));
    auto resource =
        Resource(rig.Device(), D3D12_HEAP_TYPE_UPLOAD, BufferDescription(256), D3D12_RESOURCE_STATE_GENERIC_READ);
    ASSERT_HRESULT_SUCCEEDED(resource->SetName(L"M6-03.Deleted.BundleVertexBuffer"));
    D3D12_VERTEX_BUFFER_VIEW view{resource->GetGPUVirtualAddress(), 256, 16};
    bundle->IASetVertexBuffers(0, 1, &view);
    ASSERT_HRESULT_SUCCEEDED(bundle->Close());
    ASSERT_FALSE(rig.owner->DrainInfoQueue().HasFailure());
    resource.Reset();
    const auto validationException = ValidateDeletedBundle(rig.list.Get(), bundle.Get());
    std::cout << "M6_D12_DELETED validationException=" << validationException << std::endl;
    // 无 ExecuteCommandLists；Close 仅收束失败录制，返回值由实际 debug validation 决定。
    const HRESULT closed = rig.list->Close();
    auto report = rig.owner->DrainInfoQueue();
    bool deleted = false;
    for (const auto& message : report.messages)
    {
        std::cout << "M6_D12_DELETED id=" << message.id << " severity=" << message.severity << " "
                  << message.description << '\n';
        deleted |= message.id == static_cast<std::uint32_t>(D3D12_MESSAGE_ID_SET_VERTEX_BUFFERS_INVALID) &&
                   message.severity == static_cast<std::uint32_t>(D3D12_MESSAGE_SEVERITY_ERROR) &&
                   message.description.find("ExecuteBundle") != std::string::npos &&
                   message.description.find("IASetVertexBuffers") != std::string::npos &&
                   message.description.find("does not belong to any existing Resource or Heap") != std::string::npos;
    }
    std::cout << "M6_D12_DELETED gpuSubmitted=false close=" << static_cast<unsigned>(closed) << '\n';
    EXPECT_TRUE(deleted);
}
} // namespace
