#include "M604ShaderFixtures.h"
#include "NativeDevice.h"
#include "RenderGraphTestFixtures.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/RenderGraph/TransientResourcePool.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <set>

using namespace MiniEngine;
using namespace MiniEngine::Rhi;
namespace RG = MiniEngine::RenderGraph;
namespace
{
constexpr Extent2D kExtent{32, 24};
constexpr std::array<std::array<int, 4>, 2> kExpected{{{231, 213, 188, 255}, {242, 231, 213, 255}}};
struct ToneFixture
{
    IRhiDevice& device;
    std::vector<std::function<void()>> cleanup;
    TextureHandle source;
    TextureDesc sourceDesc;
    BufferDesc constantsDesc{16, BufferUsage::Uniform, MemoryDomain::GpuOnly, "m609.exposure"};
    std::array<BufferHandle, 2> constants{};
    std::array<ResourceSetHandle, 2> sets{};
    ResourceSetLayoutHandle setLayout;
    GraphicsPipelineHandle pipeline;
    SamplerHandle sampler;
    template <class T> T Own(T handle)
    {
        cleanup.emplace_back([this, handle] { device.Destroy(handle); });
        return handle;
    }
    void Clear()
    {
        for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it)
            (*it)();
        cleanup.clear();
    }
    ~ToneFixture()
    {
        for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it)
            try
            {
                (*it)();
            }
            catch (...)
            {
            }
    }
    ResourceSetDesc SetDesc(unsigned owner, TextureHandle texture) const
    {
        return {setLayout,
                {{7, 0, BindingType::Sampler, {}, {}, sampler},
                 {9, 0, BindingType::UniformBuffer, {constants[owner], 0, 16}},
                 {10, 0, BindingType::SampledTexture, {}, texture}},
                "m609.tone-set"};
    }
    explicit ToneFixture(IRhiDevice& value, RhiBackend backend) : device(value)
    {
        auto packages = M604::LoadM604Packages();
        ShaderHandle vs, ps;
        for (const auto& package : packages)
            if (package.assetId == "ToneMapVSMain")
                vs = Own(device.CreateShader(SelectShader(package, backend, ShaderStage::Vertex)));
            else if (package.assetId == "ToneMapPSMain")
                ps = Own(device.CreateShader(SelectShader(package, backend, ShaderStage::Pixel)));
        if (!vs || !ps)
            throw std::runtime_error("ToneMap shader fixture missing");
        ResourceSetLayoutDesc setDesc;
        setDesc.entries = {{7, BindingType::Sampler, 1, ShaderStage::Pixel, false},
                           {9, BindingType::UniformBuffer, 1, ShaderStage::Pixel, false, 16},
                           {10, BindingType::SampledTexture, 1, ShaderStage::Pixel, false}};
        setLayout = Own(device.CreateResourceSetLayout(setDesc));
        PipelineLayoutDesc layoutDesc;
        layoutDesc.setCount = 1;
        layoutDesc.sets[0] = setLayout;
        const auto layout = Own(device.CreatePipelineLayout(layoutDesc));
        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vs;
        pipelineDesc.pixelShader = ps;
        pipelineDesc.layout = layout;
        pipelineDesc.colorAttachmentCount = 1;
        pipelineDesc.colorFormats[0] = Format::Rgba8Unorm;
        pipelineDesc.cullMode = CullMode::None;
        pipelineDesc.depthTest = pipelineDesc.depthWrite = false;
        pipeline = Own(device.CreateGraphicsPipeline(pipelineDesc));
        SamplerDesc samplerDesc;
        samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = AddressMode::Clamp;
        sampler = Own(device.CreateSampler(samplerDesc));
        sourceDesc.extent = {1, 1};
        sourceDesc.format = Format::Rgba16Float;
        sourceDesc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
        source = Own(device.CreateTexture(sourceDesc));
        const std::array<std::uint16_t, 4> pixel{0x4400, 0x4000, 0x3c00, 0x3c00};
        const TextureSubresourceData upload{std::as_bytes(std::span(pixel)), 8, 8};
        device.UploadTexture(source, std::span(&upload, 1));
        struct ToneConstants
        {
            float exposure;
            std::uint32_t debug = 0;
            float x = 1.0F / 32, y = 1.0F / 24;
        };
        for (unsigned owner = 0; owner < 2; ++owner)
        {
            const ToneConstants data{static_cast<float>(owner)};
            constants[owner] = Own(device.CreateBuffer(constantsDesc, std::as_bytes(std::span(&data, 1))));
            sets[owner] = Own(device.CreateResourceSet(SetDesc(owner, source)));
        }
    }
    void Draw(unsigned owner, IRhiCommandList& commands) const
    {
        commands.SetPipeline(pipeline);
        commands.SetViewport({0, 0, 32, 24, 0, 1});
        commands.SetScissor({0, 0, 32, 24});
        commands.BindResourceSet(0, sets[owner], {});
        commands.Draw(3, 1, 0, 0);
    }
};
struct CopyData
{
    RG::RgTexture source;
    RG::RgBuffer destination;
};
void Save(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary);
    ASSERT_TRUE(out);
    out << text;
    ASSERT_TRUE(out);
}
bool CheckPixels(IRhiDevice& device, BufferHandle buffer, unsigned owner, const std::filesystem::path& directory,
                 bool save)
{
    const auto image = device.TryReadTextureReadback(buffer);
    if (!image || image->unavailable || image->extent != kExtent ||
        image->bytes.size() < image->rowPitch * kExtent.height)
        return false;
    bool correct = true;
    std::ofstream ppm;
    if (save)
    {
        ppm.open(directory / (std::string("owner-") + char('A' + owner) + ".ppm"), std::ios::binary);
        ppm << "P6\n" << kExtent.width << " " << kExtent.height << "\n255\n";
    }
    for (std::uint32_t y = 0; y < kExtent.height; ++y)
        for (std::uint32_t x = 0; x < kExtent.width; ++x)
            for (unsigned c = 0; c < 4; ++c)
            {
                const auto value = std::to_integer<int>(image->bytes[y * image->rowPitch + x * 4 + c]);
                if (std::abs(value - kExpected[owner][c]) > (c == 3 ? 0 : 1))
                    correct = false;
                if (save && c < 3)
                    ppm.put(static_cast<char>(value));
            }
    return correct && (!save || ppm.good());
}
void RunNativeReuse(RhiBackend backend, bool warp)
{
    const auto directory = std::filesystem::path(M609_NATIVE_ARTIFACT_DIR) /
                           (std::string(ToString(backend)) + (warp ? "-warp" : "-hardware"));
    std::filesystem::create_directories(directory);
    InputState input;
    WindowDesc desc;
    desc.title = L"m6-transient-reuse";
    desc.width = kExtent.width;
    desc.height = kExtent.height;
    desc.visible = false;
    WindowsWindow window(desc, input);
    RhiDeviceCreateInfo info;
    info.nativeWindow = window.NativeHandle();
    info.enableDebugLayer = true;
    info.enableGpuValidation = backend == RhiBackend::D3D12;
    info.useWarp = warp;
    auto device = CreateRhiDevice(backend, info);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    SwapChainDesc chainDesc;
    chainDesc.extent = kExtent;
    chainDesc.vsync = false;
    const auto chain = device->CreateSwapChain(chainDesc);
    RG::TransientResourcePool pool(*device);
    const BufferDesc readbackDesc{kExtent.width * kExtent.height * 4, BufferUsage::CopyDestination,
                                  MemoryDomain::GpuToCpu, "m609.readback"};
    std::array<std::array<BufferHandle, 2>, 3> readbacks{};
    for (auto& lane : readbacks)
        for (auto& buffer : lane)
            buffer = device->CreateBuffer(readbackDesc, {});
    ToneFixture tone(*device, backend);
    std::map<std::uint32_t, TextureHandle> physicalByLane;
    std::set<std::uint32_t> usedLanes;
    std::uint64_t pixelFrames = 0, stableHash = 0;
    std::ofstream metrics(directory / "frames.csv");
    metrics << "frame,lane,created,reused,retired,resources,bytes,highWaterBytes,completedSerial\n";
    for (unsigned i = 0; i < 180; ++i)
    {
        const auto frame = device->BeginFrame(chain);
        ASSERT_LT(frame.recycleLane, readbacks.size());
        const auto lane = frame.recycleLane;
        if (usedLanes.contains(lane))
        {
            for (unsigned owner = 0; owner < 2; ++owner)
                ASSERT_TRUE(CheckPixels(*device, readbacks[lane][owner], owner, directory, false));
            ++pixelFrames;
        }
        usedLanes.insert(lane);
        RG::RenderGraph graph;
        const auto backState = device->QueryTextureState(frame, frame.backBuffer);
        auto back = graph.ImportTexture("BackBuffer", {frame.backBuffer,
                                                       backState.descriptor,
                                                       backState.access,
                                                       ResourceAccess::Present,
                                                       RG::ContentState::Undefined,
                                                       "swapchain",
                                                       {}});
        const auto sourceState = device->QueryTextureState(frame, tone.source);
        const auto source = graph.ImportTexture("ToneSource", {tone.source, tone.sourceDesc, sourceState.access,
                                                               ResourceAccess::SampledRead, RG::ContentState::Defined,
                                                               "tone fixture", "FP16 upload"});
        std::array<RG::RgBuffer, 2> constants;
        for (unsigned owner = 0; owner < 2; ++owner)
        {
            const auto state = device->QueryBufferState(frame, tone.constants[owner]);
            constants[owner] = graph.ImportBuffer(owner == 0 ? "ExposureA" : "ExposureB",
                                                  {tone.constants[owner], tone.constantsDesc, state.access,
                                                   ResourceAccess::UniformRead, RG::ContentState::Defined,
                                                   "tone fixture", "initialData"});
        }
        std::array<TextureHandle, 2> physical{};
        for (unsigned owner = 0; owner < 2; ++owner)
        {
            auto transientDesc = RG::Tests::MakeColorDesc();
            transientDesc.debugName = "m609.shared";
            auto texture = graph.CreateTexture(owner == 0 ? "A" : "B", transientDesc);
            graph.AddPass<RG::Tests::TexturePassData>(
                owner == 0 ? "DrawA" : "DrawB",
                [&](RG::RgBuilder& builder, RG::Tests::TexturePassData& data)
                {
                    texture = data.texture = builder.Write(texture, ResourceAccess::ColorWrite);
                    builder.SetColorAttachment(data.texture, LoadOp::Clear, StoreOp::Store);
                    (void)builder.Read(source, ResourceAccess::SampledRead);
                    (void)builder.Read(constants[owner], ResourceAccess::UniformRead);
                },
                [&, owner](const RG::Tests::TexturePassData& data, const RG::RgResources& resources,
                           IRhiCommandList& commands)
                {
                    physical[owner] = resources.Get(data.texture);
                    tone.Draw(owner, commands);
                });
            const auto snapshot = device->QueryBufferState(frame, readbacks[lane][owner]);
            auto cpu = graph.ImportBuffer(owner == 0 ? "ReadbackA" : "ReadbackB", {readbacks[lane][owner],
                                                                                   readbackDesc,
                                                                                   snapshot.access,
                                                                                   ResourceAccess::CopyDestination,
                                                                                   RG::ContentState::Undefined,
                                                                                   "test owner",
                                                                                   {}});
            graph.AddPass<CopyData>(
                owner == 0 ? "ReadA" : "ReadB",
                [&](RG::RgBuilder& builder, CopyData& data)
                {
                    data.source = builder.Read(texture, ResourceAccess::CopySource);
                    cpu = data.destination =
                        builder.Write(cpu, ResourceAccess::CopyDestination, RG::WriteCoverage::Full);
                },
                [](const CopyData& data, const RG::RgResources& resources, IRhiCommandList& commands)
                {
                    commands.CopyTextureForReadback(resources.Get(data.source), resources.Get(data.destination),
                                                    kExtent);
                });
            graph.Export(cpu);
            if (owner == 0)
                graph.AddPass<RG::Tests::TexturePassData>(
                    "BindAForRead",
                    [&](RG::RgBuilder& builder, RG::Tests::TexturePassData& data)
                    {
                        data.texture = builder.Read(texture, ResourceAccess::SampledRead);
                        (void)builder.Read(constants[0], ResourceAccess::UniformRead);
                        builder.SideEffect("bind A SRV before B reuses its physical object");
                    },
                    [&](const RG::Tests::TexturePassData& data, const RG::RgResources& resources,
                        IRhiCommandList& commands)
                    {
                        const auto set =
                            device->CreateFrameResourceSet(frame, tone.SetDesc(0, resources.Get(data.texture)));
                        commands.SetPipeline(tone.pipeline);
                        commands.BindResourceSet(0, set, {});
                    });
        }
        graph.Present(RG::Tests::AddAttachmentPass(graph, "BackClear", back));
        auto plan = graph.Compile();
        ASSERT_EQ(plan.Statistics().virtualResources, 8U);
        ASSERT_EQ(plan.Statistics().physicalResources, 7U);
        ASSERT_EQ(plan.Statistics().physicalTransients, 1U);
        if (i == 3)
            stableHash = plan.Statistics().planHash;
        if (i >= 3)
            EXPECT_EQ(plan.Statistics().planHash, stableHash);
        if (i == 179)
        {
            const auto dumps = plan.Dumps({frame.serial, M609_BUILD_CONFIG, M609_SOURCE_COMMIT});
            Save(directory / "m6-framegraph.json", dumps.frameGraphJson);
            Save(directory / "m6-access-plan.json", dumps.accessPlanJson);
            Save(directory / "m6-transient-plan.json", dumps.transientPlanJson);
            Save(directory / "m6-framegraph.dot", dumps.dot);
        }
        plan.Execute(*device, frame, pool);
        ASSERT_EQ(physical[0], physical[1]);
        if (physicalByLane.contains(lane))
            EXPECT_EQ(physicalByLane.at(lane), physical[0]);
        else
        {
            for (const auto& [otherLane, handle] : physicalByLane)
            {
                (void)otherLane;
                EXPECT_NE(handle, physical[0]);
            }
            physicalByLane.emplace(lane, physical[0]);
        }
        device->EndFrame(frame, chain);
        const auto stats = pool.Statistics();
        if (i >= 3)
        {
            EXPECT_EQ(stats.created, 3U);
            EXPECT_EQ(stats.resources, 3U);
            EXPECT_EQ(stats.highWaterResources, 3U);
            EXPECT_EQ(stats.highWaterBytes, 3U * kExtent.width * kExtent.height * 4);
            EXPECT_EQ(stats.retired, 0U);
        }
        metrics << i << ',' << lane << ',' << stats.created << ',' << stats.reused << ',' << stats.retired << ','
                << stats.resources << ',' << stats.bytes << ',' << stats.highWaterBytes << ','
                << device->Diagnostics().completedSerial << '\n';
    }
    device->WaitIdle();
    for (const auto lane : usedLanes)
    {
        for (unsigned owner = 0; owner < 2; ++owner)
            ASSERT_TRUE(CheckPixels(*device, readbacks[lane][owner], owner, directory, lane == 0));
        ++pixelFrames;
    }
    EXPECT_EQ(pixelFrames, 180U);
    EXPECT_EQ(usedLanes.size(), 3U);
    const auto stats = pool.Statistics();
    tone.Clear();
    pool.Clear();
    for (const auto& lane : readbacks)
        for (const auto buffer : lane)
            device->Destroy(buffer);
    device->Destroy(chain);
    native.Shutdown();
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U);
    EXPECT_EQ(report.liveResources, 0U);
    EXPECT_EQ(device->Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device->Diagnostics().retiringObjects, 0U);
    if (backend == RhiBackend::D3D12)
        EXPECT_GT(report.barriers, 0U);
    else
        EXPECT_GT(report.explicitUnbinds, 0U);
    EXPECT_NE(report.trace.find("resource=m609.shared"), std::string::npos);
    if (backend == RhiBackend::D3D12)
    {
        EXPECT_NE(report.trace.find("nativeBefore=128 nativeAfter=4"), std::string::npos);
        EXPECT_NE(report.trace.find("graph-barrier-batch"), std::string::npos);
    }
    else
        EXPECT_NE(report.trace.find("unbinds=1"), std::string::npos);
    Save(directory / "native-trace.txt", report.trace);
    Save(directory / "semantic-trace.txt", native.SemanticTrace());
    std::ofstream output(directory / "summary.json");
    output << "{\"scenario\":\"m6-transient-reuse\",\"frames\":180,\"pixelFrames\":" << pixelFrames
           << ",\"created\":" << stats.created << ",\"reused\":" << stats.reused
           << ",\"retiredBeforeClear\":" << stats.retired << ",\"bytes\":" << stats.bytes
           << ",\"highWaterBytes\":" << stats.highWaterBytes
           << ",\"virtualResources\":8,\"physicalResources\":7,\"physicalTransients\":1"
           << ",\"warningsErrors\":" << report.warningErrors
           << ",\"liveResourcesAfterShutdown\":" << report.liveResources << ",\"barriers\":" << report.barriers
           << ",\"explicitUnbinds\":" << report.explicitUnbinds << "}\n";
}
TEST(TransientNative, D3D11HardwareReusePixelsThreeLanes)
{
    RunNativeReuse(RhiBackend::D3D11, false);
}
TEST(TransientNative, D3D12HardwareReusePixelsThreeLanes)
{
    RunNativeReuse(RhiBackend::D3D12, false);
}
TEST(TransientNative, D3D11WarpReusePixelsThreeLanes)
{
    RunNativeReuse(RhiBackend::D3D11, true);
}
TEST(TransientNative, D3D12WarpReusePixelsThreeLanes)
{
    RunNativeReuse(RhiBackend::D3D12, true);
}
} // namespace
