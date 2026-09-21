#include "../trace/TraceFixture.h"
#include "M604ShaderFixtures.h"
#include <MiniEngine/Assets/AssetManager.h>
#include <MiniEngine/Render/M6RenderPipeline.h>
#include <MiniEngine/Render/M6SceneResources.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace MiniEngine::Render::Tests
{
namespace
{
using namespace MiniEngine::Rhi;
using namespace MiniEngine::RenderGraph;
using MiniEngine::Tests::DriveCompletion;
using MiniEngine::Tests::TraceRhiDevice;

TextureDesc MakeSampledDesc(std::string name)
{
    TextureDesc desc;
    desc.extent = {8, 8};
    desc.format = Format::Rgba8Unorm;
    desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
    desc.debugName = std::move(name);
    return desc;
}

TextureDesc MakeHdrDesc()
{
    TextureDesc desc;
    desc.extent = {8, 8};
    desc.format = Format::Rgba16Float;
    desc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
    desc.debugName = "M610.HdrColor";
    return desc;
}

TextureDesc MakeDepthDesc(std::string name)
{
    TextureDesc desc;
    desc.extent = {8, 8};
    desc.format = Format::D32Float;
    desc.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
    desc.debugName = std::move(name);
    return desc;
}

BufferDesc MakeVertexDesc()
{
    return {36, BufferUsage::Vertex | BufferUsage::CopySource, MemoryDomain::GpuOnly, "M610.vertices"};
}

BufferDesc MakeIndexDesc()
{
    return {6, BufferUsage::Index, MemoryDomain::GpuOnly, "M610.indices"};
}

BufferDesc MakeUniformDesc()
{
    return {256, BufferUsage::Uniform, MemoryDomain::CpuToGpu, "M610.uniform"};
}

ShaderDesc MakeShader(ShaderStage stage, bool positionInput, bool bindings)
{
    static const std::array<std::byte, 4> bytecode{};
    ShaderDesc desc;
    desc.stage = stage;
    desc.bytecode = bytecode;
    desc.semanticHash = std::string(64, 'a');
    desc.sourceHash = std::string(64, 'b');
    desc.entryPoint = stage == ShaderStage::Vertex ? "VSMain" : "PSMain";
    desc.debugName = stage == ShaderStage::Vertex ? "M610.VS" : "M610.PS";
    if (positionInput)
        desc.manifest.vertexInputs.push_back({VertexSemantic::Position, VertexFormat::Float3, 0});
    if (stage == ShaderStage::Pixel)
        desc.manifest.colorOutputMask = 1;
    if (bindings)
    {
        if (stage == ShaderStage::Vertex)
            desc.manifest.bindings.push_back({0, 0, BindingType::UniformBuffer, 1, 16});
        else
        {
            desc.manifest.bindings.push_back({0, 0, BindingType::UniformBuffer, 1, 16});
            desc.manifest.bindings.push_back({0, 1, BindingType::SampledTexture});
            desc.manifest.bindings.push_back({0, 2, BindingType::Sampler});
        }
    }
    return desc;
}

std::size_t CountEvent(const std::vector<std::string>& events, std::string_view operation)
{
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const auto& event)
                                                  { return event.find(operation) != std::string::npos; }));
}

std::size_t FindLabel(const std::vector<std::string>& events, std::size_t begin, std::string_view label)
{
    const auto marker = "t" + std::to_string(label.size()) + ":" + std::string(label);
    for (std::size_t i = begin; i < events.size(); ++i)
        if (events[i].find("BeginLabel") != std::string::npos && events[i].find(marker) != std::string::npos)
            return i;
    return events.size();
}

struct PipelineFixture final
{
    explicit PipelineFixture(RhiBackend backend = RhiBackend::D3D12) : device(backend)
    {
        DriveCompletion(device);
        chain = device.CreateSwapChain({{8, 8}, Format::Rgba8Unorm, 3, true, "M610.chain"});
        const std::array<float, 9> vertices{-0.5F, -0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.5F, -0.5F, 0.0F};
        const std::array<std::uint16_t, 3> indices{0, 1, 2};
        const std::array<std::byte, 256> constants{};
        vertex = device.CreateBuffer(MakeVertexDesc(), std::as_bytes(std::span(vertices)));
        index = device.CreateBuffer(MakeIndexDesc(), std::as_bytes(std::span(indices)));
        uniform = device.CreateBuffer(MakeUniformDesc(), constants);
        readback =
            device.CreateBuffer({8 * 8 * 4, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "M610.readback"}, {});
        material = device.CreateTexture(MakeSampledDesc("M610.material"));
        toneSource = device.CreateTexture(MakeSampledDesc("M610.toneSource"));
        const std::array<std::byte, 8 * 8 * 4> pixels{};
        const TextureSubresourceData uploaded{pixels, 8 * 4, pixels.size()};
        device.UploadTexture(material, std::span(&uploaded, 1));
        device.UploadTexture(toneSource, std::span(&uploaded, 1));
        sampler = device.CreateSampler({Filter::Linear,
                                        Filter::Linear,
                                        AddressMode::Clamp,
                                        AddressMode::Clamp,
                                        AddressMode::Clamp,
                                        false,
                                        CompareOp::LessEqual,
                                        1.0F,
                                        0.0F,
                                        1000.0F,
                                        {},
                                        "M610.sampler"});

        ResourceSetLayoutDesc setDesc;
        setDesc.set = 0;
        setDesc.debugName = "M610.drawSet";
        setDesc.entries = {{0, BindingType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel, false, 16},
                           {1, BindingType::SampledTexture, 1, ShaderStage::Pixel},
                           {2, BindingType::Sampler, 1, ShaderStage::Pixel}};
        drawSetLayout = device.CreateResourceSetLayout(setDesc);
        PipelineLayoutDesc drawLayoutDesc;
        drawLayoutDesc.sets[0] = drawSetLayout;
        drawLayoutDesc.setCount = 1;
        drawLayoutDesc.debugName = "M610.drawPipelineLayout";
        drawPipelineLayout = device.CreatePipelineLayout(drawLayoutDesc);
        PipelineLayoutDesc shadowLayoutDesc;
        shadowLayoutDesc.debugName = "M610.shadowPipelineLayout";
        shadowPipelineLayout = device.CreatePipelineLayout(shadowLayoutDesc);

        const auto vsPosition = device.CreateShader(MakeShader(ShaderStage::Vertex, true, true));
        const auto vsFullscreen = device.CreateShader(MakeShader(ShaderStage::Vertex, false, false));
        const auto vsShadow = device.CreateShader(MakeShader(ShaderStage::Vertex, true, false));
        const auto ps = device.CreateShader(MakeShader(ShaderStage::Pixel, false, true));
        trianglePipeline = MakeColorPipeline(vsPosition, ps, "M610.triangle", true, false, false, false);
        tonePipeline = MakeColorPipeline(vsFullscreen, ps, "M610.tone", false, false, false, false);
        forwardPipeline = MakeColorPipeline(vsPosition, ps, "M610.forward", true, true, true, true);
        skyPipeline = MakeColorPipeline(vsPosition, ps, "M610.sky", true, true, false, true);
        GraphicsPipelineDesc shadowDesc;
        shadowDesc.vertexShader = vsShadow;
        shadowDesc.layout = shadowPipelineLayout;
        shadowDesc.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
        shadowDesc.depthFormat = Format::D32Float;
        shadowDesc.depthTest = true;
        shadowDesc.depthWrite = true;
        shadowDesc.debugName = "M610.shadow";
        shadowPipeline = device.CreateGraphicsPipeline(shadowDesc);
    }

    ~PipelineFixture()
    {
        if (device.Diagnostics().activeFrameSerial == 0)
            device.Shutdown();
    }

    GraphicsPipelineHandle MakeColorPipeline(ShaderHandle vs, ShaderHandle ps, std::string name, bool hasVertexInput,
                                             bool depth, bool writesDepth, bool hdr)
    {
        GraphicsPipelineDesc desc;
        desc.vertexShader = vs;
        desc.pixelShader = ps;
        desc.layout = drawPipelineLayout;
        desc.colorAttachmentCount = 1;
        desc.colorFormats[0] = hdr ? Format::Rgba16Float : Format::Rgba8Unorm;
        if (depth)
        {
            desc.depthFormat = Format::D32Float;
            desc.depthTest = true;
            desc.depthWrite = writesDepth;
            desc.depthCompare = CompareOp::LessEqual;
        }
        else
        {
            desc.depthTest = false;
            desc.depthWrite = false;
        }
        desc.debugName = std::move(name);
        if (hasVertexInput)
            desc.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
        return device.CreateGraphicsPipeline(desc);
    }

    M6Draw Draw(std::string id, GraphicsPipelineHandle pipeline, TextureHandle texture, bool indexed,
                bool withVertex = true) const
    {
        M6Draw draw;
        draw.stableId = std::move(id);
        draw.pipeline = pipeline;
        draw.setCount = 1;
        draw.sets[0].layout = drawSetLayout;
        draw.sets[0].bindings = {{0, 0, BindingType::UniformBuffer, {uniform, 0, 16}},
                                 {1, 0, BindingType::SampledTexture, {}, texture},
                                 {2, 0, BindingType::Sampler, {}, {}, sampler}};
        if (withVertex)
            draw.vertices = {vertex, 0, 36};
        draw.indexCount = indexed ? 3 : 0;
        draw.indexType = IndexType::UInt16;
        if (indexed)
            draw.indices = {index, 0, 6};
        draw.vertexCount = 3;
        draw.stride = 12;
        return draw;
    }

    M6Draw ShadowDraw() const
    {
        M6Draw draw;
        draw.stableId = "shadow-draw";
        draw.pipeline = shadowPipeline;
        draw.vertices = {vertex, 0, 36};
        draw.indices = {index, 0, 6};
        draw.indexCount = 3;
        draw.indexType = IndexType::UInt16;
        draw.stride = 12;
        return draw;
    }

    World::RenderPacket Packet(bool duplicate) const
    {
        World::RenderPacket packet;
        packet.mainOpaque.resize(duplicate ? 2 : 1);
        packet.shadowCasters.resize(1);
        return packet;
    }

    M6PipelineResources Resources(const FrameToken& frame, std::uint32_t level, bool duplicate = false) const
    {
        M6PipelineResources resources;
        resources.extent = {8, 8};
        resources.level = level;
        const auto back = device.QueryTextureState(frame, frame.backBuffer);
        resources.backBuffer = {frame.backBuffer,
                                back.descriptor,
                                back.access,
                                ResourceAccess::Present,
                                back.fullyDefined ? ContentState::Defined : ContentState::Undefined,
                                "swapchain",
                                "acquire"};
        const auto materialState = device.QueryTextureState(frame, material);
        resources.textures.push_back(
            {"material",
             {material, materialState.descriptor, materialState.access, ResourceAccess::SampledRead,
              ContentState::Defined, "material-owner", "upload"}});
        const auto toneState = device.QueryTextureState(frame, toneSource);
        resources.toneSource = {toneSource,
                                toneState.descriptor,
                                toneState.access,
                                ResourceAccess::SampledRead,
                                ContentState::Defined,
                                "tone-owner",
                                "upload"};
        const auto appendBuffer =
            [&](BufferHandle handle, std::string name, ResourceAccess finalAccess, ShaderStage stages)
        {
            const auto state = device.QueryBufferState(frame, handle);
            resources.buffers.push_back(
                {std::move(name),
                 {handle, state.descriptor, state.access, finalAccess,
                  state.fullyDefined ? ContentState::Defined : ContentState::Undefined, "buffer-owner", "upload"},
                 finalAccess,
                 stages});
        };
        appendBuffer(vertex, "vertices", ResourceAccess::VertexRead, ShaderStage::Vertex);
        appendBuffer(index, "indices", ResourceAccess::IndexRead, ShaderStage::Vertex);
        appendBuffer(uniform, "uniform", ResourceAccess::UniformRead, ShaderStage::Vertex | ShaderStage::Pixel);
        resources.shadowDesc = MakeDepthDesc("M610.shadowMap");
        resources.depthDesc = MakeDepthDesc("M610.sceneDepth");
        resources.hdrDesc = MakeHdrDesc();
        resources.triangle = Draw("triangle", trianglePipeline, material, false);
        resources.tone = Draw("tone-draw", tonePipeline, {}, false, false);
        resources.sky = Draw("sky-draw", skyPipeline, material, true);
        resources.opaque.push_back(Draw("opaque-a", forwardPipeline, {}, true));
        if (duplicate)
            resources.opaque.push_back(Draw("opaque-b", forwardPipeline, {}, true));
        if (level >= 6)
            resources.shadow.push_back(ShadowDraw());
        if (level == 7 || level == 9)
        {
            const auto state = device.QueryBufferState(frame, readback);
            resources.capture = true;
            resources.readback = {readback,
                                  state.descriptor,
                                  state.access,
                                  ResourceAccess::CopyDestination,
                                  state.fullyDefined ? ContentState::Defined : ContentState::Undefined,
                                  "readback-owner",
                                  "allocation"};
        }
        resources.timestamps = level >= 8;
        resources.beginQuery = beginQuery;
        resources.endQuery = endQuery;
        return resources;
    }

    void RunLevel(std::uint32_t level, bool duplicate = false)
    {
        const auto before = device.Events().size();
        const auto packet = Packet(duplicate);
        const auto frame = device.BeginFrame(chain);
        auto resources = Resources(frame, level, duplicate);
        MiniEngine::RenderGraph::RenderGraph graph;
        auto plan = BuildM6RenderGraph(packet, resources, graph);
        const auto result = plan.TryExecute(device, frame);
        ASSERT_TRUE(result) << (result.error ? result.error->message : "missing graph error");
        ASSERT_TRUE(result.presentReady);
        device.EndFrame(frame, chain);
        device.CompleteThrough(frame.serial);
        const auto& events = device.Events();
        const std::array<std::string_view, 4> expected =
            level == 1   ? std::array<std::string_view, 4>{"Clear", "", "", ""}
            : level == 2 ? std::array<std::string_view, 4>{"Triangle", "", "", ""}
            : level == 3 ? std::array<std::string_view, 4>{"ToneMap", "", "", ""}
            : level == 4 ? std::array<std::string_view, 4>{"Shadow", "ForwardHDR", "ToneMap", ""}
                         : std::array<std::string_view, 4>{"Shadow", "ForwardHDR", "Skybox", "ToneMap"};
        std::size_t cursor = before;
        for (const auto label : expected)
        {
            if (label.empty())
                break;
            const auto found = FindLabel(events, cursor, label);
            ASSERT_LT(found, events.size()) << "missing pass label " << label << " at level " << level;
            cursor = found + 1;
        }
        if (level == 7 || level == 9)
        {
            EXPECT_GT(
                CountEvent(std::vector<std::string>(events.begin() + before, events.end()), "CopyTextureForReadback"),
                0U);
            const auto readbackResult = device.TryReadTextureReadback(readback);
            ASSERT_TRUE(readbackResult.has_value());
            EXPECT_TRUE(readbackResult->unavailable);
            EXPECT_EQ(readbackResult->frameSerial, frame.serial);
        }
        if (level >= 8)
        {
            EXPECT_GE(CountEvent(std::vector<std::string>(events.begin() + before, events.end()), "WriteTimestamp"),
                      2U);
            EXPECT_TRUE(device.TryReadTimestamp(beginQuery).has_value());
            EXPECT_TRUE(device.TryReadTimestamp(endQuery).has_value());
        }
    }

    TraceRhiDevice device;
    SwapChainHandle chain;
    BufferHandle vertex, index, uniform, readback;
    TextureHandle material, toneSource;
    SamplerHandle sampler;
    ResourceSetLayoutHandle drawSetLayout;
    PipelineLayoutHandle drawPipelineLayout, shadowPipelineLayout;
    GraphicsPipelineHandle trianglePipeline, tonePipeline, forwardPipeline, skyPipeline, shadowPipeline;

    TimestampQueryHandle beginQuery = device.CreateTimestampQuery("M610.begin");
    TimestampQueryHandle endQuery = device.CreateTimestampQuery("M610.end");
};

TEST(M610RenderPipeline, TraceExecutesAllNineLevelsWithStablePassOrderAndSideEffects)
{
    PipelineFixture fixture;
    for (std::uint32_t level = 1; level <= 9; ++level)
        fixture.RunLevel(level);
}

TEST(M610RenderPipeline, SharedMeshAndMaterialBindingsAreDeclaredForEveryDraw)
{
    PipelineFixture fixture;
    fixture.RunLevel(4, true);
    EXPECT_GE(CountEvent(fixture.device.Events(), "CreateFrameResourceSet"), 3U);
}

TEST(M610RenderPipeline, FixedSceneCanonicalTraceIsStableAcrossHundredRunsAndProfiles)
{
    // M6-11：同一冻结场景在 100 次独立运行中产生相同 canonical trace 与 hash，
    // 且选择 d3d11/d3d12 profile 不改变 trace；native lowering 不计入 canonical hash。
    std::string reference;
    std::uint64_t referenceHash = 0;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        PipelineFixture d11(RhiBackend::D3D11);
        PipelineFixture d12(RhiBackend::D3D12);
        d11.RunLevel(9);
        d12.RunLevel(9);
        const auto trace = d11.device.CanonicalTrace();
        EXPECT_EQ(trace, d12.device.CanonicalTrace()) << iteration;
        EXPECT_EQ(d11.device.StableHash(), d12.device.StableHash()) << iteration;
        if (iteration == 0)
        {
            reference = trace;
            referenceHash = d11.device.StableHash();
            for (const auto* label : {"Shadow", "ForwardHDR", "Skybox", "ToneMap", "Screenshot"})
                EXPECT_NE(trace.find(label), std::string::npos) << "fixed scene trace misses " << label;
            // M6-12 证据钩子（仅测试）：设置 MINIENGINE_M612_TRACE_OUT 时导出 canonical trace 与 stable hash。
            const char* traceOutput = nullptr;
#ifdef _MSC_VER
            char* ownedOutput = nullptr;
            std::size_t ownedLength = 0;
            if (_dupenv_s(&ownedOutput, &ownedLength, "MINIENGINE_M612_TRACE_OUT") == 0)
                traceOutput = ownedOutput;
#else
            traceOutput = std::getenv("MINIENGINE_M612_TRACE_OUT");
#endif
            if (traceOutput && *traceOutput)
            {
                const std::filesystem::path path(traceOutput);
                if (path.has_parent_path())
                    std::filesystem::create_directories(path.parent_path());
                std::ofstream(path) << trace << "stableHash=" << referenceHash << '\n';
            }
#ifdef _MSC_VER
            std::free(ownedOutput);
#endif
        }
        EXPECT_EQ(trace, reference) << iteration;
        EXPECT_EQ(d11.device.StableHash(), referenceHash) << iteration;
    }
}

TEST(M611Regression, SceneResourcesDestructorDoesNotThrowWhenDeviceTeardownHappensDuringUnwinding)
{
    // M611-D01 回归：失败帧清理时 M6SceneResources 的析构可能面对已不可用的 device。
    // 析构在栈展开期间抛出会触发 std::terminate（“Debug Error! abort() has been called”），
    // 因此它必须吞掉次生失败，同时保留原始异常。
    Assets::AssetManager assets;
    // 真实 shader 包：M6SceneResources 按名称匹配 PBR/Shadow/Skybox/ToneMap/GraphTriangle。
    std::vector<ShaderDesc> shaders;
    for (const auto& package : M604::LoadM604Packages())
        shaders.push_back(SelectShader(package, RhiBackend::D3D11, package.variants[0].stage));
    ASSERT_FALSE(shaders.empty());
    bool teardownDone = false;
    std::string caught;
    try
    {
        auto device = std::make_unique<TraceRhiDevice>(RhiBackend::D3D11);
        auto scene = std::make_unique<M6SceneResources>(*device, assets, shaders);
        std::cout << "M611Regression pipelines=" << scene->PipelineCount() << std::endl;
        DriveCompletion(*device);
        // 设备侧对象先被全部退休；场景资源持有的 handle 因此全部 stale，
        // 析构里的 device.Destroy 会失败——正是 D01 的第二段异常条件。
        device->Shutdown();
        teardownDone = true;
        throw std::runtime_error("original failure");
    }
    catch (const std::exception& error)
    {
        caught = error.what();
    }
    EXPECT_TRUE(teardownDone) << "构造/清理阶段抛出：" << caught;
    EXPECT_EQ(caught, "original failure") << "析构向展开路径抛出次生异常会 std::terminate（M611-D01 回归）";
}

TEST(M610RenderPipeline, InvalidLevelExtentAndPreparedDrawCountsFailDuringDeclaration)
{
    World::RenderPacket packet;
    M6PipelineResources resources;
    resources.extent = {8, 8};
    resources.backBuffer.descriptor.extent = {8, 8};
    resources.level = 0;
    MiniEngine::RenderGraph::RenderGraph graph;
    EXPECT_THROW(DeclareM6RenderGraph(packet, resources, graph), std::invalid_argument);

    resources.level = 10;
    graph.Reset();
    EXPECT_THROW(DeclareM6RenderGraph(packet, resources, graph), std::invalid_argument);

    resources.level = 1;
    resources.extent = {0, 8};
    graph.Reset();
    EXPECT_THROW(DeclareM6RenderGraph(packet, resources, graph), std::invalid_argument);

    packet.mainOpaque.resize(1);
    resources.level = 4;
    resources.extent = {8, 8};
    graph.Reset();
    EXPECT_THROW(DeclareM6RenderGraph(packet, resources, graph), std::invalid_argument);
}

} // namespace
} // namespace MiniEngine::Render::Tests