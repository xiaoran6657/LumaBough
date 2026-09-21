#include "M604ShaderFixtures.h"
#include "NativeDevice.h"
#include "SmokeScene.h"
#include <MiniEngine/Core/Input.h>
#include <MiniEngine/Platform/Windows/WindowsWindow.h>
#include <MiniEngine/Rhi/RhiResults.h>
#include <gtest/gtest.h>

using namespace MiniEngine;
using namespace MiniEngine::Rhi;
using namespace MiniEngine::Sandbox;
namespace
{
SmokeShaders Shaders(std::span<const ShaderPackage> packages, RhiBackend backend)
{
    SmokeShaders result;
    for (const auto& package : packages)
    {
        if (package.assetId == "ToneMapVSMain")
            result.toneVertex = SelectShader(package, backend, ShaderStage::Vertex);
        if (package.assetId == "ToneMapPSMain")
            result.tonePixel = SelectShader(package, backend, ShaderStage::Pixel);
        if (package.assetId == "ShadowDepthVSMain")
            result.depthVertex = SelectShader(package, backend, ShaderStage::Vertex);
    }
    return result;
}
void RunReload(RhiBackend backend, bool warp)
{
    InputState input;
    WindowDesc desc;
    desc.title = L"M6-06 adapter negative control";
    desc.width = 64;
    desc.height = 48;
    desc.visible = false;
    WindowsWindow window(desc, input);
    RhiDeviceCreateInfo info;
    info.nativeWindow = window.NativeHandle();
    info.useWarp = warp;
    info.enableDebugLayer = true;
    info.enableGpuValidation = backend == RhiBackend::D3D12;
    auto device = CreateRhiDevice(backend, info);
    auto& native = dynamic_cast<NativeDevice&>(*device);
    SwapChainDesc chainDesc;
    chainDesc.extent = {64, 48};
    chainDesc.vsync = false;
    auto chain = device->CreateSwapChain(chainDesc);
    {
        auto packages = M604::LoadM604Packages();
        auto shaders = Shaders(packages, backend);
        SmokeScene scene(*device, shaders, {64, 48}, 3);
        // 调用者的离线包可以在 CreateShader 返回后释放；原生 PSO 保留自己的副本。
        packages.clear();
        const auto before = scene.Render(chain, true);
        device->WaitIdle();
        const auto imageBefore = device->TryReadTextureReadback(before.readback);
        ASSERT_TRUE(imageBefore);
        const auto originalPipeline = scene.TonePipeline();
        packages = M604::LoadM604Packages();
        shaders = Shaders(packages, backend);
        auto invalidPixel = shaders.tonePixel;
        invalidPixel.bytecode = {};
        // 第一个 candidate shader 已创建、第二个在 public 校验被拒绝，旧 PSO 必须继续可画。
        EXPECT_THROW(scene.ReloadToneShaders(shaders.toneVertex, invalidPixel), RhiException);
        EXPECT_EQ(scene.TonePipeline(), originalPipeline);
        const auto afterFailure = scene.Render(chain, true);
        device->WaitIdle();
        const auto imageAfterFailure = device->TryReadTextureReadback(afterFailure.readback);
        ASSERT_TRUE(imageAfterFailure);
        EXPECT_EQ(imageBefore->bytes, imageAfterFailure->bytes);
        // 再提交一帧后立即发布完整 shader/PSO revision，不调用 WaitIdle。
        scene.Render(chain, false);
        scene.ReloadToneShaders(shaders.toneVertex, shaders.tonePixel);
        EXPECT_NE(scene.TonePipeline(), originalPipeline);
        scene.ReloadExposure(1.0F);
        const auto afterSuccess = scene.Render(chain, true);
        device->WaitIdle();
        const auto imageAfterSuccess = device->TryReadTextureReadback(afterSuccess.readback);
        ASSERT_TRUE(imageAfterSuccess);
        EXPECT_NE(imageBefore->bytes, imageAfterSuccess->bytes);
        EXPECT_EQ(imageAfterSuccess->frameSerial, afterSuccess.frame.serial);
        EXPECT_EQ(imageAfterSuccess->source, afterSuccess.frame.backBuffer);
        auto begin = device->TryReadTimestamp(afterSuccess.beginQuery);
        auto end = device->TryReadTimestamp(afterSuccess.endQuery);
        ASSERT_TRUE(begin);
        ASSERT_TRUE(end);
        EXPECT_TRUE(TimestampDeltaSeconds(*begin, *end).has_value());
        const auto report = native.NativeReport();
        EXPECT_EQ(report.warningErrors, 0U);
        EXPECT_GT(report.submittedBatches, 4U);
        if (backend == RhiBackend::D3D11)
            EXPECT_GT(report.explicitUnbinds, 0U);
        else
            EXPECT_GT(report.barriers, 0U);
    }
    device->Destroy(chain);
    native.Shutdown();
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U);
    EXPECT_EQ(report.liveResources, 0U);
    EXPECT_EQ(device->Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device->Diagnostics().retiringObjects, 0U);
}
TEST(M606NativeAdapter, D3D11HardwareReloadRollbackAndOwnedShaderBytes)
{
    RunReload(RhiBackend::D3D11, false);
}
TEST(M606NativeAdapter, D3D12HardwareReloadRollbackAndOwnedShaderBytes)
{
    RunReload(RhiBackend::D3D12, false);
}
TEST(M606NativeAdapter, D3D11WarpReloadRollbackAndOwnedShaderBytes)
{
    RunReload(RhiBackend::D3D11, true);
}
TEST(M606NativeAdapter, D3D12WarpReloadRollbackAndOwnedShaderBytes)
{
    RunReload(RhiBackend::D3D12, true);
}
TEST(M606Factory, UnsupportedRequestsNeverFallback)
{
    RhiDeviceCreateInfo info;
    info.enableGpuValidation = true;
    try
    {
        auto device = CreateRhiDevice(RhiBackend::D3D11, info);
        FAIL() << "D3D11 accepted unsupported GPU validation";
    }
    catch (const RhiException& error)
    {
        EXPECT_NE(std::string(error.what()).find("d3d11"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("no backend fallback"), std::string::npos);
    }
    EXPECT_THROW(CreateRhiDevice(static_cast<RhiBackend>(99), {}), RhiException);
}
} // namespace

#include <charconv>
#include <cstddef>
#include <functional>
#include <limits>
#include <string_view>
#include <utility>

namespace
{
WindowDesc HiddenAdapterWindow(Extent2D extent)
{
    WindowDesc desc;
    desc.title = L"M6-06 adapter contract";
    desc.width = extent.width;
    desc.height = extent.height;
    desc.visible = false;
    return desc;
}

struct AdapterFixture final
{
    InputState input;
    WindowDesc windowDesc;
    WindowsWindow window;
    RhiDeviceCreateInfo createInfo;
    std::unique_ptr<IRhiDevice> device;
    NativeDevice* native = nullptr;

    AdapterFixture(const RhiBackend backend, const Extent2D extent)
        : windowDesc(HiddenAdapterWindow(extent)), window(windowDesc, input)
    {
        createInfo.nativeWindow = window.NativeHandle();
        createInfo.useWarp = false;
        createInfo.enableDebugLayer = true;
        createInfo.enableGpuValidation = backend == RhiBackend::D3D12;
        device = CreateRhiDevice(backend, createInfo);
        native = &dynamic_cast<NativeDevice&>(*device);
    }
};

struct ResourceGuard final
{
    IRhiDevice& device;
    std::vector<std::function<void()>> cleanup;

    template <class Handle> Handle Own(const Handle handle)
    {
        if (handle)
        {
            cleanup.emplace_back(
                [this, handle]
                {
                    try
                    {
                        device.Destroy(handle);
                    }
                    catch (...)
                    {
                    }
                });
        }
        return handle;
    }

    void Release() noexcept
    {
        for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it)
            (*it)();
        cleanup.clear();
    }

    ~ResourceGuard()
    {
        Release();
    }
};

std::optional<std::uint64_t> TraceCounter(std::string_view trace, std::string_view name)
{
    const std::string key = std::string(name) + '=';
    const auto begin = trace.find(key);
    if (begin == std::string_view::npos)
        return std::nullopt;
    const auto valueBegin = begin + key.size();
    const auto valueEnd = trace.find_first_not_of("0123456789", valueBegin);
    const auto value = trace.substr(valueBegin, valueEnd == std::string_view::npos ? trace.size() - valueBegin
                                                                                   : valueEnd - valueBegin);
    if (value.empty())
        return std::nullopt;
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

void ExpectCleanShutdown(NativeDevice& native, IRhiDevice& device)
{
    const auto report = native.NativeReport(true);
    EXPECT_EQ(report.warningErrors, 0U);
    EXPECT_EQ(report.liveResources, 0U);
    const auto diagnostics = device.Diagnostics();
    EXPECT_EQ(diagnostics.aliveObjects, 0U);
    EXPECT_EQ(diagnostics.retiringObjects, 0U);
}

void RunDepthReadOnlyLoadStore(const RhiBackend backend)
{
    constexpr Extent2D extent{32, 24};
    AdapterFixture fixture(backend, extent);
    auto& device = *fixture.device;
    ResourceGuard resources(device);

    const SwapChainDesc swapChainDescription{extent, Format::Rgba8Unorm,          3,
                                             false,  "M606.depth-readonly.chain", fixture.window.NativeHandle()};
    const auto chain = resources.Own(device.CreateSwapChain(swapChainDescription));

    TextureDesc depthDescription{TextureDimension::Texture2D,
                                 extent,
                                 1,
                                 1,
                                 1,
                                 Format::D32Float,
                                 TextureUsage::DepthStencil | TextureUsage::Sampled,
                                 "M606.depth-readonly.texture"};
    depthDescription.clearDepthHint = 1.0F;
    const auto depth = resources.Own(device.CreateTexture(depthDescription));

    const auto packages = M604::LoadM604Packages();
    const auto shader = resources.Own(device.CreateShader(Shaders(packages, backend).depthVertex));

    ResourceSetLayoutDesc emptySet0;
    emptySet0.set = 0;
    emptySet0.debugName = "M606.depth-readonly.set0";
    ResourceSetLayoutDesc emptySet1;
    emptySet1.set = 1;
    emptySet1.debugName = "M606.depth-readonly.set1";
    ResourceSetLayoutDesc objectSet;
    objectSet.set = 2;
    objectSet.entries.push_back({0, BindingType::UniformBuffer, 1, ShaderStage::Vertex, true, 208});
    objectSet.debugName = "M606.depth-readonly.object-set";
    const auto set0 = resources.Own(device.CreateResourceSetLayout(emptySet0));
    const auto set1 = resources.Own(device.CreateResourceSetLayout(emptySet1));
    const auto set2 = resources.Own(device.CreateResourceSetLayout(objectSet));

    PipelineLayoutDesc pipelineLayoutDescription;
    pipelineLayoutDescription.sets = {set0, set1, set2};
    pipelineLayoutDescription.setCount = 3;
    pipelineLayoutDescription.debugName = "M606.depth-readonly.pipeline-layout";
    const auto pipelineLayout = resources.Own(device.CreatePipelineLayout(pipelineLayoutDescription));

    const std::array<float, 9> vertices{
        -0.75F, -0.75F, 0.25F, 0.0F, 0.75F, 0.25F, 0.75F, -0.75F, 0.25F,
    };
    const auto vertexBuffer =
        resources.Own(device.CreateBuffer({sizeof(vertices), BufferUsage::Vertex | BufferUsage::CopySource,
                                           MemoryDomain::GpuOnly, "M606.depth-readonly.vertices"},
                                          std::as_bytes(std::span(vertices))));

    GraphicsPipelineDesc writeDescription;
    writeDescription.vertexShader = shader;
    writeDescription.layout = pipelineLayout;
    writeDescription.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
    writeDescription.depthFormat = Format::D32Float;
    writeDescription.cullMode = CullMode::None;
    writeDescription.depthTest = true;
    writeDescription.depthWrite = true;
    writeDescription.depthCompare = CompareOp::LessEqual;
    writeDescription.debugName = "M606.depth-readonly.write-pipeline";
    const auto writePipeline = resources.Own(device.CreateGraphicsPipeline(writeDescription));

    auto readDescription = writeDescription;
    readDescription.depthWrite = false;
    readDescription.debugName = "M606.depth-readonly.read-pipeline";
    const auto readPipeline = resources.Own(device.CreateGraphicsPipeline(readDescription));

    const auto frame = device.BeginFrame(chain);
    ASSERT_NE(frame.serial, 0U);
    auto& commands = device.BeginGraphics(frame);
    auto& graph = device.GraphCommandSink(frame);

    const std::array imports{
        GraphResourceImport{frame.backBuffer, {}},
        GraphResourceImport{depth, {}},
        GraphResourceImport{{}, vertexBuffer},
    };
    graph.ImportResources(imports);

    const std::array initialTransitions{
        AccessTransition{depth, {}, ResourceAccess::None, ResourceAccess::DepthWrite},
        AccessTransition{{}, vertexBuffer, ResourceAccess::None, ResourceAccess::VertexRead},
    };
    graph.ApplyTransitions(initialTransitions);

    const std::array<float, 52> objectConstants = []
    {
        std::array<float, 52> value{};
        for (std::size_t matrix = 0; matrix < 3; ++matrix)
            for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
                value[matrix * 16 + diagonal * 5] = 1.0F;
        value[48] = 1.0F;
        return value;
    }();
    const auto dynamic = device.WriteDynamicBuffer(frame, std::as_bytes(std::span(objectConstants)), 256);
    const std::array dynamicImport{GraphResourceImport{{}, dynamic.view.buffer}};
    graph.ImportResources(dynamicImport);
    const AccessTransition dynamicTransition{
        {}, dynamic.view.buffer, ResourceAccess::None, ResourceAccess::UniformRead};
    graph.ApplyTransitions(std::span(&dynamicTransition, 1));

    ResourceSetDesc emptySet0Description{set0, {}, "M606.depth-readonly.empty0"};
    ResourceSetDesc emptySet1Description{set1, {}, "M606.depth-readonly.empty1"};
    const auto boundSet0 = resources.Own(device.CreateResourceSet(emptySet0Description));
    const auto boundSet1 = resources.Own(device.CreateResourceSet(emptySet1Description));
    ResourceSetDesc objectSetDescription;
    objectSetDescription.layout = set2;
    objectSetDescription.bindings.push_back({0, 0, BindingType::UniformBuffer, dynamic.view, {}, {}});
    objectSetDescription.debugName = "M606.depth-readonly.object";
    const auto boundObjectSet = device.CreateFrameResourceSet(frame, objectSetDescription);

    const std::array<std::uint32_t, 0> noOffsets{};
    const std::array dynamicOffsets{std::uint32_t{0}};

    auto drawDepth =
        [&](const GraphicsPipelineHandle pipeline, const DepthAttachment& attachment, std::string_view label)
    {
        commands.BeginLabel(label);
        commands.BeginRendering({{}, &attachment, extent});
        commands.SetPipeline(pipeline);
        commands.SetViewport({0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1});
        commands.SetScissor({0, 0, extent.width, extent.height});
        commands.BindVertexBuffer(0, {vertexBuffer, 0, sizeof(vertices)}, 12);
        commands.BindResourceSet(0, boundSet0, std::span<const std::uint32_t>{noOffsets});
        commands.BindResourceSet(1, boundSet1, std::span<const std::uint32_t>{noOffsets});
        commands.BindResourceSet(2, boundObjectSet, std::span<const std::uint32_t>{dynamicOffsets});
        commands.Draw(3, 1, 0, 0);
        commands.EndRendering();
        commands.EndLabel();
    };

    const DepthAttachment writeAttachment{depth, LoadOp::Clear, StoreOp::Store, 1.0F, 0};
    drawDepth(writePipeline, writeAttachment, "M606.DepthWrite");
    const AccessTransition depthReadTransition{depth, {}, ResourceAccess::DepthWrite, ResourceAccess::DepthRead};
    graph.ApplyTransitions(std::span(&depthReadTransition, 1));
    const DepthAttachment readAttachment{depth, LoadOp::Load, StoreOp::Store, 1.0F, 0};
    drawDepth(readPipeline, readAttachment, "M606.DepthReadOnly");

    const AccessTransition backBufferWrite{frame.backBuffer, {}, ResourceAccess::Present, ResourceAccess::ColorWrite};
    graph.ApplyTransitions(std::span(&backBufferWrite, 1));
    Render::ClearPass(commands, frame.backBuffer, extent, {0, 0, 0, 1});
    const AccessTransition backBufferPresent{frame.backBuffer, {}, ResourceAccess::ColorWrite, ResourceAccess::Present};
    graph.ApplyTransitions(std::span(&backBufferPresent, 1));

    device.EndGraphics(frame, commands);
    device.EndFrame(frame, chain);
    device.WaitIdle();

    const auto report = fixture.native->NativeReport();
    EXPECT_EQ(report.warningErrors, 0U);
    EXPECT_GT(report.barriers, 0U);

    resources.Release();
    fixture.native->Shutdown();
    ExpectCleanShutdown(*fixture.native, device);
}

void RunPackedReadback(const RhiBackend backend)
{
    constexpr Extent2D extent{67, 3};
    constexpr std::uint64_t packedRowBytes = static_cast<std::uint64_t>(extent.width) * 4U;
    constexpr std::uint64_t packedBytes = packedRowBytes * extent.height;
    AdapterFixture fixture(backend, extent);
    auto& device = *fixture.device;
    ResourceGuard resources(device);

    const SwapChainDesc swapChainDescription{extent, Format::Rgba8Unorm,           3,
                                             false,  "M606.packed-readback.chain", fixture.window.NativeHandle()};
    const auto chain = resources.Own(device.CreateSwapChain(swapChainDescription));
    const auto readback = resources.Own(device.CreateBuffer(
        {packedBytes, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "M606.packed-readback.buffer"}, {}));

    const auto frame = device.BeginFrame(chain);
    ASSERT_NE(frame.serial, 0U);
    auto& commands = device.BeginGraphics(frame);
    auto& graph = device.GraphCommandSink(frame);
    const std::array imports{
        GraphResourceImport{frame.backBuffer, {}},
        GraphResourceImport{{}, readback},
    };
    graph.ImportResources(imports);

    const AccessTransition toColor{frame.backBuffer, {}, ResourceAccess::Present, ResourceAccess::ColorWrite};
    const AccessTransition toCopy{{}, readback, ResourceAccess::None, ResourceAccess::CopyDestination};
    const std::array initialTransitions{toColor, toCopy};
    graph.ApplyTransitions(initialTransitions);
    Render::ClearPass(commands, frame.backBuffer, extent, {0, 0, 0, 1});

    const AccessTransition source{frame.backBuffer, {}, ResourceAccess::ColorWrite, ResourceAccess::CopySource};
    graph.ApplyTransitions(std::span(&source, 1));
    commands.CopyTextureForReadback(frame.backBuffer, readback, extent);

    const AccessTransition present{frame.backBuffer, {}, ResourceAccess::CopySource, ResourceAccess::Present};
    graph.ApplyTransitions(std::span(&present, 1));
    device.EndGraphics(frame, commands);
    device.EndFrame(frame, chain);
    device.WaitIdle();

    const auto result = device.TryReadTextureReadback(readback);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->extent, extent);
    EXPECT_EQ(result->format, Format::Rgba8Unorm);
    EXPECT_EQ(result->rowPitch, packedRowBytes);
    EXPECT_EQ(result->bytes.size(), static_cast<std::size_t>(packedBytes));
    EXPECT_EQ(result->frameSerial, frame.serial);
    EXPECT_EQ(result->source, frame.backBuffer);
    const std::array<unsigned char, 4> expectedPixel{0, 0, 0, 255};
    for (std::size_t offset = 0; offset < result->bytes.size(); offset += 4)
    {
        EXPECT_EQ(std::to_integer<unsigned char>(result->bytes[offset]), expectedPixel[0]);
        EXPECT_EQ(std::to_integer<unsigned char>(result->bytes[offset + 1]), expectedPixel[1]);
        EXPECT_EQ(std::to_integer<unsigned char>(result->bytes[offset + 2]), expectedPixel[2]);
        EXPECT_EQ(std::to_integer<unsigned char>(result->bytes[offset + 3]), expectedPixel[3]);
    }

    const auto report = fixture.native->NativeReport();
    EXPECT_EQ(report.warningErrors, 0U);
    EXPECT_GT(report.barriers, 0U);
    resources.Release();
    fixture.native->Shutdown();
    ExpectCleanShutdown(*fixture.native, device);
}

void RunPipelineCacheReuse(const RhiBackend backend)
{
    constexpr Extent2D extent{8, 8};
    AdapterFixture fixture(backend, extent);
    auto& device = *fixture.device;
    ResourceGuard resources(device);

    auto packages = M604::LoadM604Packages();
    const auto shader = resources.Own(device.CreateShader(Shaders(packages, backend).depthVertex));

    ResourceSetLayoutDesc emptySet0;
    emptySet0.set = 0;
    emptySet0.debugName = "M606.pipeline-cache.set0";
    ResourceSetLayoutDesc emptySet1;
    emptySet1.set = 1;
    emptySet1.debugName = "M606.pipeline-cache.set1";
    ResourceSetLayoutDesc objectSet;
    objectSet.set = 2;
    objectSet.entries.push_back({0, BindingType::UniformBuffer, 1, ShaderStage::Vertex, true, 208});
    objectSet.debugName = "M606.pipeline-cache.set2";
    const auto set0 = resources.Own(device.CreateResourceSetLayout(emptySet0));
    const auto set1 = resources.Own(device.CreateResourceSetLayout(emptySet1));
    const auto set2 = resources.Own(device.CreateResourceSetLayout(objectSet));

    PipelineLayoutDesc pipelineLayoutDescription;
    pipelineLayoutDescription.sets = {set0, set1, set2};
    pipelineLayoutDescription.setCount = 3;
    pipelineLayoutDescription.debugName = "M606.pipeline-cache.layout";
    const auto pipelineLayout = resources.Own(device.CreatePipelineLayout(pipelineLayoutDescription));

    GraphicsPipelineDesc description;
    description.vertexShader = shader;
    description.layout = pipelineLayout;
    description.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
    description.depthFormat = Format::D32Float;
    description.cullMode = CullMode::None;
    description.depthTest = true;
    description.depthWrite = false;
    description.depthCompare = CompareOp::LessEqual;
    description.debugName = "M606.pipeline-cache.pipeline";

    const auto first = resources.Own(device.CreateGraphicsPipeline(description));
    const auto afterFirst = fixture.native->NativeReport();
    const auto second = resources.Own(device.CreateGraphicsPipeline(description));
    const auto afterSecond = fixture.native->NativeReport();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first, second);
    EXPECT_EQ(afterSecond.warningErrors, 0U);

    if (backend == RhiBackend::D3D12)
    {
        const auto firstCreated = TraceCounter(afterFirst.trace, "psoCreated");
        const auto secondCreated = TraceCounter(afterSecond.trace, "psoCreated");
        const auto firstHits = TraceCounter(afterFirst.trace, "psoCacheHits");
        const auto secondHits = TraceCounter(afterSecond.trace, "psoCacheHits");
        ASSERT_TRUE(firstCreated.has_value());
        ASSERT_TRUE(secondCreated.has_value());
        ASSERT_TRUE(firstHits.has_value());
        ASSERT_TRUE(secondHits.has_value());
        EXPECT_EQ(*secondCreated, *firstCreated);
        EXPECT_GT(*secondHits, *firstHits);
    }

    fixture.native->WaitIdle();
    resources.Release();
    fixture.native->Shutdown();
    ExpectCleanShutdown(*fixture.native, device);
}

class M606AdapterBackendTest : public ::testing::TestWithParam<RhiBackend>
{
};

std::string BackendParameterName(const ::testing::TestParamInfo<RhiBackend>& info)
{
    return std::string(ToString(info.param));
}

TEST_P(M606AdapterBackendTest, DepthReadOnlyAttachmentUsesLoadStore)
{
    RunDepthReadOnlyLoadStore(GetParam());
}

TEST_P(M606AdapterBackendTest, PackedReadbackNormalizesD3D12RowPadding)
{
    RunPackedReadback(GetParam());
}

TEST_P(M606AdapterBackendTest, IdenticalGraphicsPipelineUsesNativeCache)
{
    RunPipelineCacheReuse(GetParam());
}

INSTANTIATE_TEST_SUITE_P(M606AdapterBackends, M606AdapterBackendTest,
                         ::testing::Values(RhiBackend::D3D11, RhiBackend::D3D12), BackendParameterName);
} // namespace
