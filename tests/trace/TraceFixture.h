#pragma once
#include "TraceRhi.h"
#include <array>
#include <gtest/gtest.h>

namespace MiniEngine::Tests
{
// 正例显式注入同步完成的测试调度器；默认 TraceCompletion 不会自行前进。
inline void DriveCompletion(TraceRhiDevice& device)
{
    device.Completion().onWait = [](std::uint64_t required) { return required; };
}
struct TriangleFixture
{
    explicit TriangleFixture(RhiBackend backend = RhiBackend::D3D12) : device(backend)
    {
        DriveCompletion(device);
        chain = device.CreateSwapChain({{16, 16}, Format::Rgba8Unorm, 3, true, "main chain"});
        const std::array<float, 9> vertices{-0.5F, -0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.5F, -0.5F, 0.0F};
        const std::array<std::uint16_t, 3> indices{0, 1, 2};
        const std::array<std::byte, 512> constants{};
        vertex = device.CreateBuffer({sizeof(vertices), BufferUsage::Vertex | BufferUsage::CopySource,
                                      MemoryDomain::GpuOnly, "triangle vertices"},
                                     std::as_bytes(std::span(vertices)));
        index = device.CreateBuffer({sizeof(indices), BufferUsage::Index, MemoryDomain::GpuOnly, "triangle indices"},
                                    std::as_bytes(std::span(indices)));
        uniform = device.CreateBuffer(
            {constants.size(), BufferUsage::Uniform, MemoryDomain::CpuToGpu, "scene constants"}, constants);
        readback =
            device.CreateBuffer({16 * 16 * 4, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "screenshot"}, {});
        copy = device.CreateBuffer(
            {sizeof(vertices), BufferUsage::CopyDestination, MemoryDomain::GpuOnly, "vertex copy"}, {});
        ResourceSetLayoutDesc layout{
            0, {{0, BindingType::UniformBuffer, 1, ShaderStage::Pixel, true, 16}}, "scene layout"};
        setLayout = device.CreateResourceSetLayout(layout);
        PipelineLayoutDesc pipelineLayout;
        pipelineLayout.sets[0] = setLayout;
        pipelineLayout.setCount = 1;
        pipelineLayout.debugName = "triangle layout";
        auto pl = device.CreatePipelineLayout(pipelineLayout);
        auto vs = Shader(ShaderStage::Vertex), ps = Shader(ShaderStage::Pixel);
        GraphicsPipelineDesc desc;
        desc.vertexShader = device.CreateShader(vs);
        desc.pixelShader = device.CreateShader(ps);
        desc.layout = pl;
        desc.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0});
        desc.colorAttachmentCount = 1;
        desc.colorFormats[0] = Format::Rgba8Unorm;
        desc.depthTest = desc.depthWrite = false;
        desc.debugName = "triangle pipeline";
        pipeline = device.CreateGraphicsPipeline(desc);
        ResourceSetDesc resources;
        resources.layout = setLayout;
        resources.bindings.push_back({0, 0, BindingType::UniformBuffer, {uniform, 0, 16}});
        resources.debugName = "scene set";
        set = device.CreateResourceSet(resources);
        beginQuery = device.CreateTimestampQuery("frame begin");
        endQuery = device.CreateTimestampQuery("frame end");
    }
    static ShaderDesc Shader(ShaderStage stage)
    {
        static const std::array<std::byte, 4> bytes{};
        ShaderDesc result;
        result.stage = stage;
        result.bytecode = bytes;
        result.sourceHash = std::string(64, 'a');
        result.semanticHash = std::string(64, 'b');
        result.entryPoint = stage == ShaderStage::Vertex ? "VSMain" : "PSMain";
        if (stage == ShaderStage::Vertex)
            result.manifest.vertexInputs.push_back({VertexSemantic::Position, VertexFormat::Float3, 0});
        else
        {
            result.manifest.colorOutputMask = 1;
            result.manifest.bindings.push_back({0, 0, BindingType::UniformBuffer, 1, 16});
        }
        return result;
    }
    void Start(bool declareBuffers = true)
    {
        frame = device.BeginFrame(chain);
        commands = &device.BeginGraphics(frame);
        if (declareBuffers)
        {
            Transition(vertex, ResourceAccess::VertexRead);
            Transition(index, ResourceAccess::IndexRead);
            Transition(uniform, ResourceAccess::UniformRead);
        }
    }
    void Transition(TextureHandle texture, ResourceAccess after)
    {
        (void)device.Import(frame, texture);
        auto it = device.ResourceStates().find(texture);
        const auto before = it == device.ResourceStates().end() ? ResourceAccess::None : it->second.access;
        const AccessTransition transition{texture, {}, before, after};
        device.GraphCommandSink(frame).ApplyTransitions(std::span(&transition, 1));
    }
    void Transition(BufferHandle buffer, ResourceAccess after)
    {
        (void)device.Import(frame, buffer);
        auto it = device.ResourceStates().find(buffer);
        const auto before = it == device.ResourceStates().end() ? ResourceAccess::None : it->second.access;
        const AccessTransition transition{{}, buffer, before, after};
        device.GraphCommandSink(frame).ApplyTransitions(std::span(&transition, 1));
    }
    void Render(LoadOp load = LoadOp::Clear, StoreOp store = StoreOp::Store)
    {
        Transition(frame.backBuffer, ResourceAccess::ColorWrite);
        const ColorAttachment attachment{frame.backBuffer, load, store, {0.1F, 0.2F, 0.3F, 1.0F}};
        commands->BeginRendering({std::span(&attachment, 1), nullptr, {16, 16}});
    }
    void Bind()
    {
        commands->SetPipeline(pipeline);
        commands->SetViewport({0, 0, 16, 16, 0, 1});
        commands->SetScissor({0, 0, 16, 16});
        commands->BindVertexBuffer(0, {vertex, 0, 36}, 12);
        commands->BindIndexBuffer({index, 0, 6}, IndexType::UInt16);
        const std::array<std::uint32_t, 1> offsets{256};
        commands->BindResourceSet(0, set, offsets);
    }
    void Finish()
    {
        Transition(frame.backBuffer, ResourceAccess::Present);
        device.EndGraphics(frame, *commands);
        device.EndFrame(frame, chain);
    }
    void FixedTriangle()
    {
        Start();
        commands->BeginLabel("fixed triangle");
        commands->WriteTimestamp(beginQuery);
        Render();
        Bind();
        commands->Draw(3, 1, 0, 0);
        commands->DrawIndexed(3, 1, 0, 0, 0);
        commands->EndRendering();
        Transition(vertex, ResourceAccess::CopySource);
        Transition(copy, ResourceAccess::CopyDestination);
        commands->CopyBuffer({vertex, 0, 36}, {copy, 0, 36}, 36);
        // 模拟 graph 每帧 import：截图 source 和 destination 都必须声明。
        auto source = device.Import(frame, frame.backBuffer);
        auto destination = device.Import(frame, readback);
        Transition(std::get<TextureHandle>(device.Resolve(source)), ResourceAccess::CopySource);
        Transition(std::get<BufferHandle>(device.Resolve(destination)), ResourceAccess::CopyDestination);
        commands->CopyTextureForReadback(frame.backBuffer, readback, {16, 16});
        commands->WriteTimestamp(endQuery);
        commands->EndLabel();
        Finish();
    }
    TraceRhiDevice device;
    SwapChainHandle chain;
    FrameToken frame;
    IRhiCommandList* commands = nullptr;
    BufferHandle vertex, index, uniform, readback, copy;
    ResourceSetLayoutHandle setLayout;
    ResourceSetHandle set;
    GraphicsPipelineHandle pipeline;
    TimestampQueryHandle beginQuery, endQuery;
};
} // namespace MiniEngine::Tests
