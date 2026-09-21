// M6-08：独立 EXE 的 CPU/内存取证；全局 allocator 不得接入其他测试目标。
#include "D3D12Queue.h"
#include "D3D12ResourceStateTracker.h"
#include "D3D12SwapChain.h"
#include "NativeDevice.h"
#include "RenderGraphTestFixtures.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef M608_COST_ARTIFACT_DIR
#define M608_COST_ARTIFACT_DIR "m608-cost"
#endif

namespace M608Allocation
{
// Header 跟随分配到释放；scope 外或其他线程的 delete 仍归还原计数器。
// 进程寿命计数器不借用栈对象；只覆盖 C++ new，不包括驱动 malloc/native heap。
struct Counters
{
    std::atomic<std::uint64_t> allocations{0}, requested{0}, live{0}, peak{0};
};
struct Snapshot
{
    std::uint64_t allocations = 0, requested = 0, live = 0, peak = 0;
};
Counters cpu, legacy, build, compile, execute, cleanup, selfTest;
thread_local Counters* active = nullptr;
Snapshot Read(const Counters& value)
{
    return {value.allocations.load(), value.requested.load(), value.live.load(), value.peak.load()};
}
class Scope final
{
  public:
    explicit Scope(Counters* value) noexcept : m_previous(active)
    {
        active = value;
    }
    ~Scope()
    {
        active = m_previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    Counters* m_previous;
};
struct alignas(std::max_align_t) Header
{
    void* base;
    Counters* owner;
    std::size_t bytes;
};
void* Allocate(std::size_t bytes, std::size_t alignment)
{
    alignment = (std::max)(alignment, alignof(Header));
    const auto occupied = (std::max)(bytes, std::size_t{1});
    if (alignment > (std::numeric_limits<std::size_t>::max)() - sizeof(Header) ||
        occupied > (std::numeric_limits<std::size_t>::max)() - sizeof(Header) - alignment)
        throw std::bad_alloc();
    void* base = nullptr;
    while ((base = std::malloc(occupied + sizeof(Header) + alignment - 1)) == nullptr)
    {
        const auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto address = (start + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    auto* header = reinterpret_cast<Header*>(address) - 1;
    ::new (static_cast<void*>(header)) Header{base, active, bytes};
    if (auto* owner = header->owner)
    {
        owner->allocations.fetch_add(1, std::memory_order_relaxed);
        owner->requested.fetch_add(bytes, std::memory_order_relaxed);
        const auto live = owner->live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        auto peak = owner->peak.load(std::memory_order_relaxed);
        while (peak < live && !owner->peak.compare_exchange_weak(peak, live, std::memory_order_relaxed))
        {
        }
    }
    return reinterpret_cast<void*>(address);
}
void Release(void* value) noexcept
{
    if (!value)
        return;
    auto* header = reinterpret_cast<Header*>(value) - 1;
    if (header->owner)
        header->owner->live.fetch_sub(header->bytes, std::memory_order_relaxed);
    std::free(header->base);
}
} // namespace M608Allocation

void* operator new(std::size_t bytes)
{
    return M608Allocation::Allocate(bytes, alignof(std::max_align_t));
}
void* operator new[](std::size_t bytes)
{
    return ::operator new(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
    return M608Allocation::Allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
    return ::operator new(bytes, alignment);
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    try
    {
        return ::operator new(bytes);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept
{
    return ::operator new(bytes, tag);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try
    {
        return ::operator new(bytes, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
    return ::operator new(bytes, alignment, tag);
}
void operator delete(void* p) noexcept
{
    M608Allocation::Release(p);
}
void operator delete[](void* p) noexcept
{
    M608Allocation::Release(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    M608Allocation::Release(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    M608Allocation::Release(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
    M608Allocation::Release(p);
}
void operator delete[](void* p, std::align_val_t) noexcept
{
    M608Allocation::Release(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    M608Allocation::Release(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    M608Allocation::Release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    M608Allocation::Release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    M608Allocation::Release(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    M608Allocation::Release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    M608Allocation::Release(p);
}

namespace
{
using namespace MiniEngine;
using namespace MiniEngine::Rhi;
namespace RG = MiniEngine::RenderGraph;
namespace D12 = MiniEngine::Rhi::D3D12;
namespace Alloc = M608Allocation;
using Clock = std::chrono::steady_clock;
constexpr Extent2D kExtent{32, 24};
constexpr std::array<float, 4> kClear{0.25F, 0.5F, 0.75F, 1.0F};
#ifdef NDEBUG
constexpr bool kValidation = false;
constexpr std::size_t kWarmup = 120, kSamples = 600;
constexpr const char* kConfiguration = "Release";
#else
constexpr bool kValidation = true;
constexpr std::size_t kWarmup = 8, kSamples = 24;
constexpr const char* kConfiguration = "Debug";
#endif
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
double Microseconds(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - start).count();
}
template <class Function> double Measure(Alloc::Counters* counter, Function&& function)
{
    Alloc::Scope scope(counter);
    const auto start = Clock::now();
    function();
    return Microseconds(start, Clock::now());
}
struct Quantiles
{
    double median = 0, p95 = 0;
};
Quantiles Summarize(std::vector<double> values)
{
    Require(!values.empty(), "empty CPU timing series");
    std::sort(values.begin(), values.end());
    for (double value : values)
        Require(std::isfinite(value) && value >= 0, "non-finite CPU timing");
    return {(values[(values.size() - 1) / 2] + values[values.size() / 2]) * 0.5,
            values[static_cast<std::size_t>(std::ceil(static_cast<double>(values.size()) * 0.95)) - 1]};
}
void WriteString(std::ostream& output, std::string_view value)
{
    output << '"';
    for (const auto character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\')
            output << '\\' << character;
        else if (byte < 32)
            output << "\\u00" << "0123456789abcdef"[byte >> 4] << "0123456789abcdef"[byte & 15];
        else
            output << character;
    }
    output << '"';
}
std::ofstream OpenReport(const char* name)
{
    // 宏由 CMake 绑定配置目录，避免重复追加 $<CONFIG>。
    const std::filesystem::path directory(M608_COST_ARTIFACT_DIR);
    std::filesystem::create_directories(directory);
    std::ofstream output(directory / name, std::ios::trunc);
    Require(static_cast<bool>(output), "cannot open M6-08 CPU artifact");
    output << std::setprecision(12) << std::boolalpha;
    return output;
}
void WriteQuantiles(std::ostream& output, Quantiles values)
{
    output << "{\"medianUs\":" << values.median << ",\"p95Us\":" << values.p95 << '}';
}
void WriteAllocations(std::ostream& output, Alloc::Snapshot s)
{
    output << "{\"allocations\":" << s.allocations << ",\"requestedBytes\":" << s.requested
           << ",\"liveBytes\":" << s.live << ",\"peakLiveBytes\":" << s.peak << '}';
}
void WriteStatistics(std::ostream& output, const RG::GraphStatistics& s)
{
    output << "{\"declaredPasses\":" << s.declaredPasses << ",\"livePasses\":" << s.livePasses
           << ",\"culledPasses\":" << s.culledPasses << ",\"virtualResources\":" << s.virtualResources
           << ",\"resourceVersions\":" << s.resourceVersions << ",\"dependencyEdges\":" << s.dependencyEdges
           << ",\"nodes\":" << s.declaredPasses + s.resourceVersions << ",\"roots\":" << s.roots
           << ",\"liveResources\":" << s.liveResources << ",\"physicalResources\":" << s.physicalResources
           << ",\"physicalTransients\":" << s.physicalTransients << ",\"logicalTransitions\":" << s.logicalTransitions
           << ",\"planHash\":\"" << s.planHash << "\"}";
}
bool SameStatistics(const RG::GraphStatistics& a, const RG::GraphStatistics& b)
{
    return a.declaredPasses == b.declaredPasses && a.livePasses == b.livePasses && a.culledPasses == b.culledPasses &&
           a.virtualResources == b.virtualResources && a.resourceVersions == b.resourceVersions &&
           a.dependencyEdges == b.dependencyEdges && a.roots == b.roots && a.liveResources == b.liveResources &&
           a.physicalResources == b.physicalResources && a.physicalTransients == b.physicalTransients &&
           a.logicalTransitions == b.logicalTransitions && a.planHash == b.planHash;
}
bool SameStorage(const RG::GraphStorageStatistics& a, const RG::GraphStorageStatistics& b)
{
    return a.declarationBytes == b.declarationBytes && a.planBytes == b.planBytes && a.edgeCapacity == b.edgeCapacity &&
           a.versionCapacity == b.versionCapacity && a.readerCapacity == b.readerCapacity;
}
WindowDesc WindowDescription()
{
    WindowDesc desc;
    desc.title = L"M6-08 CPU benchmark hidden host";
    desc.width = kExtent.width;
    desc.height = kExtent.height;
    desc.visible = false;
    return desc;
}
std::uint64_t PixelHash(std::span<const std::byte> bytes, std::size_t pitch)
{
    Require(bytes.size() >= pitch * (kExtent.height - 1) + kExtent.width * 4, "short CPU readback");
    constexpr std::array<int, 4> expected{64, 128, 191, 255};
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint32_t y = 0; y < kExtent.height; ++y)
        for (std::uint32_t x = 0; x < kExtent.width; ++x)
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const auto actual = std::to_integer<int>(bytes[y * pitch + x * 4 + channel]);
                Require(std::abs(actual - expected[channel]) <= (channel == 3 ? 0 : 1), "clear pixel mismatch");
                hash = (hash ^ static_cast<std::uint64_t>(actual)) * 1099511628211ULL;
            }
    return hash;
}
} // namespace

namespace
{
// 真 M5 concrete；窗口和设备覆盖 swap chain、queue、readback 的整个寿命。
// 测试专用隐藏 HWND；独立 flip swap chain 不共享窗口，也不复用生产窗口类名。
class LegacyWindow final
{
  public:
    LegacyWindow()
    {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = DefWindowProcW;
        description.hInstance = m_instance;
        description.lpszClassName = kClassName;
        if (RegisterClassExW(&description) == 0)
            throw std::runtime_error("M608 legacy RegisterClassExW failed: " + std::to_string(GetLastError()));
        m_window = CreateWindowExW(0, kClassName, L"M6-08 direct M5 benchmark", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                   CW_USEDEFAULT, 320, 240, nullptr, nullptr, m_instance, nullptr);
        if (m_window == nullptr)
        {
            const auto error = GetLastError();
            UnregisterClassW(kClassName, m_instance);
            throw std::runtime_error("M608 legacy CreateWindowExW failed: " + std::to_string(error));
        }
    }
    ~LegacyWindow()
    {
        if (m_window != nullptr)
            DestroyWindow(m_window);
        UnregisterClassW(kClassName, m_instance);
    }
    LegacyWindow(const LegacyWindow&) = delete;
    LegacyWindow& operator=(const LegacyWindow&) = delete;
    [[nodiscard]] void* NativeHandle() const noexcept
    {
        return m_window;
    }

  private:
    static constexpr const wchar_t* kClassName = L"MiniEngineM608DirectM5Window";
    HINSTANCE m_instance = GetModuleHandleW(nullptr);
    HWND m_window = nullptr;
};
struct LegacyFixture
{
    LegacyWindow window;
    std::unique_ptr<D12::D3D12Device> device;
    D12::D3D12Queue queue;
    D12::D3D12ResourceStateTracker tracker;
    D12::D3D12SwapChain chain;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackBytes = 0;
    std::uint64_t pixelHash = 0, checkedFrames = 0, barriers = 0, occludedPresents = 0;
    std::array<std::byte, kExtent.width * kExtent.height * 4> packed{};

    explicit LegacyFixture(std::uint64_t luid)
    {
        D12::DeviceCreateOptions options;
        options.debugLayer = kValidation;
        options.gpuValidation = kValidation;
        options.hasAdapterLuid = true;
        options.adapterLuidLow = static_cast<std::uint32_t>(luid);
        options.adapterLuidHigh = static_cast<std::uint32_t>(luid >> 32);
        device = D12::D3D12Device::Create(options);
        auto& native = *static_cast<ID3D12Device*>(device->NativeDeviceHandle());
        auto& factory = *static_cast<IDXGIFactory7*>(device->NativeFactoryHandle());
        queue.Initialize(native);
        chain.Initialize(native, factory, queue.NativeQueue(), tracker, window.NativeHandle(), kExtent.width,
                         kExtent.height, false);
        const auto description = chain.BackBuffer(0).GetDesc();
        native.GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = readbackBytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D12::ThrowIfFailed(native.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&readback)),
                           "M608 readback");
    }
    ~LegacyFixture()
    {
        // 所有正常帧已定向等待；仅退出销毁资源集时使用 FlushGpu。
        try
        {
            queue.FlushGpu("M608-fixture-resource-set-teardown");
        }
        catch (...)
        {
        }
    }
    double Frame(bool count)
    {
        const auto index = chain.CurrentBackBufferIndex();
        auto& frame = queue.BeginFrame(index);
        tracker.BeginRecording();
        auto& commands = queue.CommandList();
        const auto elapsed = Measure(count ? &Alloc::legacy : nullptr,
                                     [&]
                                     {
                                         chain.TrackTransition(index, D3D12_RESOURCE_STATE_RENDER_TARGET);
                                         barriers += tracker.FlushBarriersTo(commands);
                                         const auto rtv = chain.RtvHandle(index);
                                         commands.OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                                         commands.ClearRenderTargetView(rtv, kClear.data(), 0, nullptr);
                                         chain.TrackTransition(index, D3D12_RESOURCE_STATE_COPY_SOURCE);
                                         barriers += tracker.FlushBarriersTo(commands);
                                         D3D12_TEXTURE_COPY_LOCATION source{};
                                         source.pResource = &chain.BackBuffer(index);
                                         source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                         D3D12_TEXTURE_COPY_LOCATION destination{};
                                         destination.pResource = readback.Get();
                                         destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                         destination.PlacedFootprint = footprint;
                                         commands.CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                                         chain.TrackTransition(index, D3D12_RESOURCE_STATE_PRESENT);
                                         barriers += tracker.FlushBarriersTo(commands);
                                         queue.CloseRecording();
                                     });
        const auto fence = queue.ExecuteClosedAndSignal(frame);
        tracker.CommitExecuted();
        if (!chain.Present())
            ++occludedPresents;
        queue.WaitForSubmittedFence(fence, "M608-bounded-readback-verification");
        void* data = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(readbackBytes)};
        D12::ThrowIfFailed(readback->Map(0, &range, &data), "M608 Map");
        try
        {
            pixelHash = PixelHash({static_cast<const std::byte*>(data), static_cast<std::size_t>(readbackBytes)},
                                  footprint.Footprint.RowPitch);
            for (std::uint32_t row = 0; row < kExtent.height; ++row)
                std::copy_n(static_cast<const std::byte*>(data) + row * footprint.Footprint.RowPitch, kExtent.width * 4,
                            packed.data() + row * kExtent.width * 4);
        }
        catch (...)
        {
            const D3D12_RANGE none{0, 0};
            readback->Unmap(0, &none);
            throw;
        }
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        ++checkedFrames;
        Require(!device->DrainInfoQueue().HasFailure(), "legacy D3D12 debug diagnostic");
        return elapsed;
    }
};
struct FrameTimes
{
    double build = 0, compile = 0, execute = 0, cleanup = 0;
};
void DeclareNativeGraph(RG::RenderGraph& graph, const FrameToken& frame, const TextureStateSnapshot& backState,
                        BufferHandle readback, const BufferStateSnapshot& bufferState)
{
    auto back = graph.ImportTexture("BackBuffer", {frame.backBuffer,
                                                   backState.descriptor,
                                                   backState.access,
                                                   ResourceAccess::Present,
                                                   RG::ContentState::Undefined,
                                                   "swapchain",
                                                   {}});
    auto cpu = graph.ImportBuffer("Readback", {readback,
                                               bufferState.descriptor,
                                               bufferState.access,
                                               ResourceAccess::CopyDestination,
                                               RG::ContentState::Undefined,
                                               "CPU owner",
                                               {}});
    graph.AddPass<RG::Tests::TexturePassData>(
        "Clear",
        [&](RG::RgBuilder& builder, RG::Tests::TexturePassData& data)
        {
            back = data.texture = builder.Write(back, ResourceAccess::ColorWrite);
            builder.SetColorAttachment(back, LoadOp::Clear, StoreOp::Store, kClear);
        },
        [](const auto&, const RG::RgResources&, IRhiCommandList&) {});
    struct CopyData
    {
        RG::RgTexture source;
        RG::RgBuffer destination;
    };
    graph.AddPass<CopyData>(
        "Readback",
        [&](RG::RgBuilder& builder, CopyData& data)
        {
            data.source = builder.Read(back, ResourceAccess::CopySource);
            cpu = data.destination = builder.Write(cpu, ResourceAccess::CopyDestination, RG::WriteCoverage::Full);
        },
        [](const CopyData& data, const RG::RgResources& resources, IRhiCommandList& commands)
        { commands.CopyTextureForReadback(resources.Get(data.source), resources.Get(data.destination), kExtent); });
    graph.Export(cpu);
    graph.Present(back);
}
struct GraphFixture
{
    InputState input;
    WindowsWindow window{WindowDescription(), input};
    std::unique_ptr<IRhiDevice> device;
    SwapChainHandle chain;
    BufferHandle readback;
    RG::RenderGraph graph;
    RG::GraphStatistics statistics;
    RG::GraphStorageStatistics storage;
    std::uint64_t pixelHash = 0, checkedFrames = 0;
    std::array<std::byte, kExtent.width * kExtent.height * 4> packed{};
    GraphFixture()
    {
        RhiDeviceCreateInfo info;
        info.nativeWindow = window.NativeHandle();
        info.enableDebugLayer = kValidation;
        info.enableGpuValidation = kValidation;
        device = CreateRhiDevice(RhiBackend::D3D12, info);
        SwapChainDesc description;
        description.extent = kExtent;
        description.vsync = false;
        chain = device->CreateSwapChain(description);
        // 公共内容账本只定义 packed 像素，native padding staging 在 warmup 首帧创建并复用。
        readback = device->CreateBuffer({kExtent.width * kExtent.height * 4, BufferUsage::CopyDestination,
                                         MemoryDomain::GpuToCpu, "M608.preallocated.readback"},
                                        {});
        graph.Reserve({2, 2});
    }
    FrameTimes Frame(bool count)
    {
        const auto frame = device->BeginFrame(chain);
        const auto texture = device->QueryTextureState(frame, frame.backBuffer);
        const auto buffer = device->QueryBufferState(frame, readback);
        std::optional<RG::CompiledRenderGraph> plan;
        FrameTimes times;
        times.build = Measure(count ? &Alloc::build : nullptr,
                              [&] { DeclareNativeGraph(graph, frame, texture, readback, buffer); });
        times.compile = Measure(count ? &Alloc::compile : nullptr, [&] { plan.emplace(graph.Compile()); });
        statistics = plan->Statistics();
        storage = plan->StorageStatistics();
        times.execute = Measure(count ? &Alloc::execute : nullptr, [&] { plan->Execute(*device, frame); });
        device->EndFrame(frame, chain);
        // 公共 API 目前仅提供 WaitIdle；计时外做逐帧像素证据，不计作 CPU 录制成本。
        device->WaitIdle();
        const auto image = device->TryReadTextureReadback(readback);
        Require(image.has_value() && !image->unavailable && image->extent == kExtent, "graph readback unavailable");
        pixelHash = PixelHash(image->bytes, image->rowPitch);
        for (std::uint32_t row = 0; row < kExtent.height; ++row)
            std::copy_n(image->bytes.data() + row * image->rowPitch, kExtent.width * 4,
                        packed.data() + row * kExtent.width * 4);
        ++checkedFrames;
        times.cleanup = Measure(count ? &Alloc::cleanup : nullptr,
                                [&]
                                {
                                    plan.reset();
                                    graph.Reset();
                                });
        return times;
    }
    NativeBackendReport Shutdown()
    {
        device->WaitIdle();
        device->Destroy(readback);
        device->Destroy(chain);
        auto& native = dynamic_cast<NativeDevice&>(*device);
        native.Shutdown();
        return native.NativeReport(kValidation);
    }
};

struct TimingSeries
{
    std::vector<double> direct, build, compile, execute, cleanup, total, clock;
    explicit TimingSeries(std::size_t count)
        : direct(count), build(count), compile(count), execute(count), cleanup(count), total(count), clock(count)
    {
    }
    void Set(std::size_t index, double legacy, const FrameTimes& graph)
    {
        direct[index] = legacy;
        build[index] = graph.build;
        compile[index] = graph.compile;
        execute[index] = graph.execute;
        cleanup[index] = graph.cleanup;
        total[index] = graph.build + graph.compile + graph.execute + graph.cleanup;
        clock[index] = Measure(nullptr, [] {});
    }
};
TEST(GraphCpuCost, NativeDirectM5VersusGraphClearReadback)
{
    const bool captureAttached =
        GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr || GetModuleHandleW(L"renderdoc.dll") != nullptr;
    TimingSeries samples(kSamples);
    RG::GraphStatistics statistics;
    RhiCapabilities capabilities;
    D12::DeviceMetadata legacyMetadata;
    NativeBackendReport nativeReport;
    std::uint64_t pixels = 0, checked = 0, barriers = 0, occluded = 0;
    constexpr std::size_t allocationFrames = 32;
    bool stable = true;
    {
        GraphFixture graph;
        capabilities = graph.device->Capabilities();
        {
            LegacyFixture legacy(capabilities.adapterLuid);
            legacyMetadata = legacy.device->Metadata();
            const auto legacyLuid =
                (static_cast<std::uint64_t>(legacyMetadata.adapter.luidHigh) << 32) | legacyMetadata.adapter.luidLow;
            Require(legacyLuid == capabilities.adapterLuid, "A/B adapter LUID mismatch");
            Require(capabilities.debugLayerEnabled == kValidation && capabilities.gpuValidationEnabled == kValidation &&
                        legacyMetadata.debugLayerActive == kValidation &&
                        legacyMetadata.gpuValidationActive == kValidation,
                    "A/B validation modes mismatch");
            for (std::size_t frame = 0; frame < kWarmup + kSamples; ++frame)
            {
                double directTime = 0;
                FrameTimes graphTime;
                // 交错 AB/BA 顺序，减少固定运行顺序与时钟漂移偏差。
                if (frame % 2 == 0)
                {
                    directTime = legacy.Frame(false);
                    graphTime = graph.Frame(false);
                }
                else
                {
                    graphTime = graph.Frame(false);
                    directTime = legacy.Frame(false);
                }
                Require(graph.packed == legacy.packed, "native A/B packed pixel bytes mismatch");
                if (frame == kWarmup)
                    statistics = graph.statistics;
                if (frame >= kWarmup)
                {
                    stable = stable && SameStatistics(statistics, graph.statistics);
                    samples.Set(frame - kWarmup, directTime, graphTime);
                }
            }
            // allocation run 单独执行；其耗时不混入主 timing 样本。
            for (std::size_t frame = 0; frame < allocationFrames; ++frame)
            {
                (void)legacy.Frame(true);
                (void)graph.Frame(true);
                Require(graph.packed == legacy.packed, "allocation-run pixel mismatch");
            }
            pixels = graph.pixelHash;
            checked = graph.checkedFrames;
            barriers = legacy.barriers;
            occluded = legacy.occludedPresents;
        }
        // 同 adapter 的 D3D12 device 可共享；先销毁对照对象，再采 graph 最终 census。
        nativeReport = graph.Shutdown();
        const auto diagnostics = graph.device->Diagnostics();
        Require(diagnostics.aliveObjects == 0 && diagnostics.retiringObjects == 0,
                "graph RHI resource retirement not empty");
    }
    const auto direct = Summarize(samples.direct);
    const auto total = Summarize(samples.total);
    const auto execution = Summarize(samples.execute);
    Require(direct.median > 0 && direct.p95 > 0, "timer too coarse for native CPU ratio");
    const bool passed = stable && nativeReport.warningErrors == 0 && nativeReport.liveResources == 0 &&
                        (kValidation || !captureAttached);
    auto output = OpenReport("native-clear-cost.json");
    output << "{\n\"schema\":\"miniengine.m6-08.cpu-cost.v1\",\n\"status\":\"" << (passed ? "PASS" : "FAIL")
           << "\",\n\"configuration\":\"" << kConfiguration
           << "\",\"performanceEvidence\":" << (!kValidation && !captureAttached)
           << ",\"captureAttached\":" << captureAttached << ",\"debug\":" << kValidation << ",\"gbv\":" << kValidation
           << ",\"warmupFrames\":" << kWarmup << ",\"sampleFrames\":" << kSamples
           << ",\"timingUsesReplacementAllocator\":true,\"timingAllocationCountersEnabled\":false"
           << ",\"allocationFrames\":" << allocationFrames
           << ",\"extent\":[32,24],\"clear\":[0.25,0.5,0.75,1],\"adapterName\":";
    WriteString(output, capabilities.adapterName);
    output << ",\"adapterLuid\":\"" << capabilities.adapterLuid << "\",\"driver\":";
    WriteString(output, capabilities.driverVersion);
    output << ",\"deviceMode\":\"hardware\",\"legacySource\":\"M5 D3D12Queue/D3D12SwapChain/concrete command list\""
           << ",\"graphStatistics\":";
    WriteStatistics(output, statistics);
    output << ",\"stableStatisticsAndHash\":" << stable << ",\"cpuUs\":{\"directM5Record\":";
    WriteQuantiles(output, direct);
    output << ",\"graphBuild\":";
    WriteQuantiles(output, Summarize(samples.build));
    output << ",\"graphCompile\":";
    WriteQuantiles(output, Summarize(samples.compile));
    output << ",\"graphExecuteSetupAndRecord\":";
    WriteQuantiles(output, execution);
    output << ",\"graphCleanup\":";
    WriteQuantiles(output, Summarize(samples.cleanup));
    output << ",\"graphTotal\":";
    WriteQuantiles(output, total);
    output << ",\"emptyTimerScope\":";
    WriteQuantiles(output, Summarize(samples.clock));
    output << "},\"totalOverDirectMedianRatio\":" << total.median / direct.median
           << ",\"totalOverDirectP95Ratio\":" << total.p95 / direct.p95
           << ",\"executeOverDirectMedianRatio\":" << execution.median / direct.median
           << ",\"totalMinusDirectMedianUs\":" << total.median - direct.median
           << ",\"allocations\":{\"directM5Record\":";
    WriteAllocations(output, Alloc::Read(Alloc::legacy));
    output << ",\"graphBuild\":";
    WriteAllocations(output, Alloc::Read(Alloc::build));
    output << ",\"graphCompile\":";
    WriteAllocations(output, Alloc::Read(Alloc::compile));
    output << ",\"graphExecute\":";
    WriteAllocations(output, Alloc::Read(Alloc::execute));
    output << ",\"graphCleanup\":";
    WriteAllocations(output, Alloc::Read(Alloc::cleanup));
    output << "},\"pixelEvidence\":{\"checkedFramesPerPath\":" << checked << ",\"pixelsPerFrame\":768"
           << ",\"allChannelsChecked\":true,\"packedFnv64\":\"" << pixels << "\",\"abMaxChannelDifference\":0}"
           << ",\"nativeEvidence\":{\"legacyBarrierCount\":" << barriers
           << ",\"graphBarrierCount\":" << nativeReport.barriers
           << ",\"graphWarningsErrors\":" << nativeReport.warningErrors
           << ",\"graphLiveResourcesAfterShutdown\":" << nativeReport.liveResources
           << ",\"legacyOccludedPresents\":" << occluded << "}"
           << ",\"allocationScope\":\"C++ new requested bytes on benchmark thread; frees follow allocation ownership; "
              "driver malloc/COM/native heaps excluded; snapshots after fixture destruction; disabled counting still "
              "retains replacement allocator headers\""
           << ",\"timingScope\":\"after BeginFrame/reset through command-list Close; graph also includes declaration, "
              "compile and cleanup; excludes submit, Present, GPU waits, readback, pixel checks, JSON and dumps\""
           << ",\"workDifferences\":[\"graph performs handle/content/import validation, "
              "dependency/culling/lifetime/access "
              "compilation and resource resolution\",\"graph adapter creates CommandRecording/events and validates "
              "render/copy operations; direct M5 invokes native APIs\",\"graph records pass labels and queries copy "
              "footprint; direct precomputes footprint\",\"both clear the same 32x24 RGBA8 backbuffer, copy to "
              "prewarmed readback and restore Present\",\"graph WaitIdle and direct targeted fence wait differ, "
              "both occur outside timing\",\"hidden HWND may return occluded; pixel evidence proves GPU work, not "
              "visibility\"]"
           << ",\"regressionInterpretation\":\"Ratios quantify the extra graph/public-RHI CPU work for a deliberately "
              "minimal two-pass workload. No universal microsecond threshold or full-scene performance claim. "
              "Debug samples include diagnostics and are correctness evidence only.\"}\n";
    output.flush();
    Require(static_cast<bool>(output), "CPU cost artifact write failed");
    EXPECT_TRUE(passed);
}
} // namespace

namespace
{
void DeclareCpuGraph(RG::RenderGraph& graph)
{
    // 一个有副作用的真实附件声明保持 live；32 条 clear->read 死链提供边与版本规模。
    auto live = graph.CreateTexture("Live", RG::Tests::MakeColorDesc());
    graph.AddPass<RG::Tests::TexturePassData>(
        "LiveClear",
        [&](RG::RgBuilder& builder, RG::Tests::TexturePassData& data)
        {
            data.texture = builder.Write(live, ResourceAccess::ColorWrite);
            builder.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store, kClear);
            builder.SideEffect("CPU retained root");
        },
        [](const auto&, const RG::RgResources&, IRhiCommandList&) {});
    for (std::uint32_t index = 0; index < 32; ++index)
    {
        const auto name = "Dead" + std::to_string(index);
        auto dead = graph.CreateTexture(name, RG::Tests::MakeColorDesc());
        dead = RG::Tests::AddAttachmentPass(graph, name + "Clear", dead);
        RG::Tests::AddSamplePass(graph, name + "Read", dead);
    }
}
void WriteStorage(std::ostream& output, const RG::GraphStorageStatistics& s)
{
    output << "{\"declarationBytes\":" << s.declarationBytes << ",\"planBytes\":" << s.planBytes
           << ",\"edgeCapacity\":" << s.edgeCapacity << ",\"versionCapacity\":" << s.versionCapacity
           << ",\"readerCapacity\":" << s.readerCapacity << '}';
}
struct CpuMemorySample
{
    std::uint64_t allocations = 0, requested = 0, compiledLive = 0, resetLive = 0;
};

TEST(GraphCpuCost, AllocationCounterTracksAlignedAndOutOfScopeRelease)
{
    // 显式调用替换函数，避免优化器消除用于验证计数器的 new/delete 表达式。
    const auto before = Alloc::Read(Alloc::selfTest);
    void* normal = nullptr;
    void* aligned = nullptr;
    void* array = nullptr;
    {
        Alloc::Scope scope(&Alloc::selfTest);
        normal = ::operator new(17);
        aligned = ::operator new(129, std::align_val_t{256}, std::nothrow);
        array = ::operator new[](31, std::nothrow);
    }
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(aligned, nullptr);
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(normal) % alignof(std::max_align_t), 0U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 256U, 0U);
    const auto allocated = Alloc::Read(Alloc::selfTest);
    EXPECT_EQ(allocated.allocations - before.allocations, 3U);
    EXPECT_EQ(allocated.requested - before.requested, 177U);
    EXPECT_EQ(allocated.live - before.live, 177U);
    ::operator delete(normal, std::size_t{17});
    ::operator delete(aligned, std::size_t{129}, std::align_val_t{256});
    ::operator delete[](array, std::nothrow);
    EXPECT_EQ(Alloc::Read(Alloc::selfTest).live, before.live);
}

TEST(GraphCpuCost, FixedGraphAllocationAndStorageRemainBounded)
{
    constexpr std::size_t warmup = 32;
    constexpr std::size_t samples = 1000;
    std::vector<CpuMemorySample> memory(samples);
    std::vector<double> buildTime(samples), compileTime(samples), cleanupTime(samples);
    RG::GraphStatistics reference;
    RG::GraphStorageStatistics storage;
    bool stable = true, storageStable = true, liveStable = true, perFrameAllocationsStable = true;
    std::uint64_t referenceCompiledLive = 0, referenceResetLive = 0;
    std::uint64_t referenceAllocations = 0, referenceRequested = 0;
    Alloc::Snapshot warmed;
    RG::GraphDumpBundle referenceDump;
    bool dumpsStable = true;
    const auto initial = Alloc::Read(Alloc::cpu);
    {
        // 从 graph 自身/Reserve 开始标记，以便 graph 销毁后验证整个归属集完全释放。
        std::unique_ptr<RG::RenderGraph> graph;
        {
            Alloc::Scope scope(&Alloc::cpu);
            graph = std::make_unique<RG::RenderGraph>();
            graph->Reserve({65, 33});
        }
        for (std::size_t index = 0; index < warmup + samples; ++index)
        {
            const auto before = Alloc::Read(Alloc::cpu);
            std::optional<RG::CompiledRenderGraph> plan;
            const auto buildUs = Measure(&Alloc::cpu, [&] { DeclareCpuGraph(*graph); });
            const auto compileUs = Measure(&Alloc::cpu, [&] { plan.emplace(graph->Compile()); });
            const auto current = plan->Statistics();
            const auto currentStorage = plan->StorageStatistics();
            const auto compiled = Alloc::Read(Alloc::cpu);
            // dump 序列化不进入 allocator/计时边界；检查前 100 个 measured generation。
            if (index == warmup)
                referenceDump = plan->Dumps();
            else if (index > warmup && index < warmup + 100)
            {
                const auto dump = plan->Dumps();
                dumpsStable = dumpsStable && dump.frameGraphJson == referenceDump.frameGraphJson &&
                              dump.dot == referenceDump.dot && dump.accessPlanJson == referenceDump.accessPlanJson &&
                              dump.transientPlanJson == referenceDump.transientPlanJson;
            }
            const auto cleanupUs = Measure(&Alloc::cpu,
                                           [&]
                                           {
                                               plan.reset();
                                               graph->Reset();
                                           });
            const auto after = Alloc::Read(Alloc::cpu);
            const auto allocations = after.allocations - before.allocations;
            const auto requested = after.requested - before.requested;
            if (index == warmup)
            {
                reference = current;
                storage = currentStorage;
                referenceCompiledLive = compiled.live;
                referenceResetLive = after.live;
                referenceAllocations = allocations;
                referenceRequested = requested;
                warmed = after;
            }
            if (index >= warmup)
            {
                const auto sample = index - warmup;
                memory[sample] = {allocations, requested, compiled.live, after.live};
                buildTime[sample] = buildUs;
                compileTime[sample] = compileUs;
                cleanupTime[sample] = cleanupUs;
                stable = stable && SameStatistics(reference, current);
                storageStable = storageStable && SameStorage(storage, currentStorage);
                liveStable = liveStable && compiled.live == referenceCompiledLive && after.live == referenceResetLive;
                perFrameAllocationsStable =
                    perFrameAllocationsStable && allocations == referenceAllocations && requested == referenceRequested;
            }
        }
        graph.reset();
    }
    const auto final = Alloc::Read(Alloc::cpu);
    const bool released = final.live == initial.live;
    const bool passed = stable && storageStable && liveStable && perFrameAllocationsStable && released && dumpsStable &&
                        reference.declaredPasses == 65 && reference.livePasses == 1 && reference.culledPasses == 64;
    auto output = OpenReport("fixed-graph-memory.json");
    output << "{\n\"schema\":\"miniengine.m6-08.cpu-allocation.v1\",\"status\":\"" << (passed ? "PASS" : "FAIL")
           << "\",\"configuration\":\"" << kConfiguration << "\",\"pureCpu\":true,\"warmupGraphs\":" << warmup
           << ",\"sampleGraphs\":" << samples << ",\"dumpComparisons\":100,\"counts\":";
    WriteStatistics(output, reference);
    output << ",\"storage\":";
    WriteStorage(output, storage);
    output << ",\"allocationsAfterWarmupAndFirstSample\":";
    WriteAllocations(output, warmed);
    output << ",\"allocationsAfterGraphDestruction\":";
    WriteAllocations(output, final);
    output << ",\"steadyFrameAllocations\":" << referenceAllocations
           << ",\"steadyFrameRequestedBytes\":" << referenceRequested
           << ",\"compiledLiveBytes\":" << referenceCompiledLive << ",\"resetLiveBytes\":" << referenceResetLive
           << ",\"statisticsHashStable\":" << stable << ",\"storageStable\":" << storageStable
           << ",\"liveBytesStable\":" << liveStable << ",\"perFrameAllocationStable\":" << perFrameAllocationsStable
           << ",\"releasedAfterDestruction\":" << released << ",\"dumpsStable\":" << dumpsStable
           << ",\"instrumentedCpuUs\":{\"build\":";
    WriteQuantiles(output, Summarize(buildTime));
    output << ",\"compile\":";
    WriteQuantiles(output, Summarize(compileTime));
    output << ",\"cleanup\":";
    WriteQuantiles(output, Summarize(cleanupTime));
    output
        << "},\"scope\":\"Only graph construction/Reserve, declaration, compilation and Reset allocations are tagged; "
           "counters follow frees after scope exit. Samples/dumps/JSON/gtest are excluded. Counts measure requested "
           "C++ new bytes, not malloc/driver heaps or allocator-header overhead. Instrumented CPU timings are separate "
           "from the native comparison with counters disabled (replacement allocation headers remain). Fixed "
           "allocations per frame are measured churn, not a leak.\""
        << ",\"samples\":[";
    for (std::size_t index = 0; index < memory.size(); ++index)
    {
        if (index != 0)
            output << ',';
        const auto& s = memory[index];
        output << "{\"index\":" << index << ",\"allocations\":" << s.allocations
               << ",\"requestedBytes\":" << s.requested << ",\"compiledLiveBytes\":" << s.compiledLive
               << ",\"resetLiveBytes\":" << s.resetLive << '}';
    }
    output << "]}\n";
    output.flush();
    Require(static_cast<bool>(output), "CPU allocation artifact write failed");
    EXPECT_TRUE(passed) << "See fixed-graph-memory.json for counts, retained bytes, storage and dump comparisons";
}
} // namespace
