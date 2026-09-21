#include "M604D3D11Probe.h"
#include "M604D3D12Probe.h"
#include "M604ShaderFixtures.h"
#include "ShaderRevisionTransaction.h"
#include <MiniEngine/Assets/PbrVertex.h>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <iostream>

using namespace MiniEngine::Rhi;
using namespace MiniEngine::Rhi::M604;
namespace
{
ShaderDesc Shader(std::span<const ShaderPackage> packages, const std::string& stem, ShaderStage stage,
                  RhiBackend backend)
{
    const auto id = stem + (stage == ShaderStage::Vertex ? "VSMain" : "PSMain");
    const auto found = std::find_if(packages.begin(), packages.end(), [&](const auto& p) { return p.assetId == id; });
    if (found == packages.end())
        throw std::runtime_error("missing asset " + id);
    return SelectShader(*found, backend, stage);
}
std::vector<ResourceSetLayoutDesc> Layouts(const ShaderDesc& vs, const ShaderDesc* ps)
{
    std::vector<ResourceSetLayoutDesc> result;
    const auto add = [&](const ShaderDesc& shader)
    {
        for (const auto& b : shader.manifest.bindings)
        {
            while (result.size() <= b.set)
            {
                ResourceSetLayoutDesc layout;
                layout.set = static_cast<std::uint8_t>(result.size());
                result.push_back(layout);
            }
            auto& entries = result[b.set].entries;
            const auto found =
                std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.binding == b.binding; });
            if (found != entries.end())
            {
                found->visibility = found->visibility | shader.stage;
                continue;
            }
            const bool dynamic = b.type == BindingType::UniformBuffer &&
                                 ((b.set == 0 && b.binding == 0) || (b.set == 2 && b.binding == 0));
            entries.push_back({b.binding, b.type, b.count, shader.stage, dynamic, b.uniformBytes, b.textureDimension,
                               b.comparisonSampler});
        }
    };
    add(vs);
    if (ps)
        add(*ps);
    return result;
}
GraphicsPipelineDesc Descriptor(const std::string& stem, const ShaderDesc& vs)
{
    GraphicsPipelineDesc p;
    p.debugName = stem;
    // Reflection 定义输入集合；M5 固定 interleaved 顶点存储定义实际 byte offset。
    for (const auto& input : vs.manifest.vertexInputs)
    {
        std::uint16_t offset = 0;
        switch (input.semantic)
        {
        case VertexSemantic::Position:
            offset = static_cast<std::uint16_t>(offsetof(MiniEngine::Assets::PbrVertex, position));
            break;
        case VertexSemantic::Normal:
            offset = static_cast<std::uint16_t>(offsetof(MiniEngine::Assets::PbrVertex, normal));
            break;
        case VertexSemantic::TexCoord:
            offset = static_cast<std::uint16_t>(offsetof(MiniEngine::Assets::PbrVertex, uv0));
            break;
        case VertexSemantic::Tangent:
            offset = static_cast<std::uint16_t>(offsetof(MiniEngine::Assets::PbrVertex, tangent));
            break;
        case VertexSemantic::Color:
            offset = 48;
            break;
        }
        p.vertexAttributes.push_back({input.semantic, input.format, offset, 0, 0, input.semanticIndex});
    }
    if (stem == "ShadowDepth")
    {
        p.depthFormat = Format::D32Float;
        p.depthBias = 1000;
        p.slopeScaledDepthBias = 1.5F;
    }
    else
    {
        p.colorAttachmentCount = 1;
        p.colorFormats[0] = stem == "ToneMap" ? Format::Rgba8Unorm : Format::Rgba16Float;
        p.depthFormat = stem == "ToneMap" ? Format::Unknown : Format::D32Float;
        if (stem == "ToneMap")
        {
            p.depthTest = false;
            p.depthWrite = false;
            p.cullMode = CullMode::None;
        }
        if (stem == "Skybox")
        {
            p.depthWrite = false;
            p.depthCompare = CompareOp::LessEqual;
            p.cullMode = CullMode::None;
        }
    }
    return p;
}
struct Batch : ResourcePayload
{
    explicit Batch(int& counter) : live(counter)
    {
        ++live;
    }
    ~Batch() override
    {
        --live;
    }
    int& live;
    std::vector<std::unique_ptr<ResourcePayload>> pipelines;
    ResourcePayload* tone = nullptr;
};
template <class Probe>
ShaderRevisionBackend Build(Probe& probe, RhiBackend backend, std::span<const ShaderPackage> packages, int& live)
{
    auto batch = std::make_unique<Batch>(live);
    std::string key;
    for (const std::string stem : {"PbrForward", "ShadowDepth", "Skybox", "ToneMap"})
    {
        const auto vs = Shader(packages, stem, ShaderStage::Vertex, backend);
        std::optional<ShaderDesc> ps;
        if (stem != "ShadowDepth")
            ps = Shader(packages, stem, ShaderStage::Pixel, backend);
        const auto layouts = Layouts(vs, ps ? &*ps : nullptr);
        auto p = Descriptor(stem, vs);
        const auto create = [&]
        {
            ValidateGraphicsPipeline(p, vs, ps ? &*ps : nullptr, layouts, probe.Capabilities());
            key += PipelineSemanticKey(p, vs, ps ? &*ps : nullptr, layouts);
            batch->pipelines.push_back(probe.CreatePipeline(vs, ps ? &*ps : nullptr, p));
        };
        create();
        if (stem == "ToneMap")
            batch->tone = batch->pipelines.back().get();
        if (stem == "PbrForward")
        {
            p.frontFace = FrontFace::CounterClockwise;
            create();
            p.depthFormat = Format::D24UnormS8Uint;
            p.depthCompare = CompareOp::LessEqual;
            create();
        }
        if (stem == "ShadowDepth")
        {
            p.depthBias = 2000;
            p.depthBiasClamp = 0.01F;
            create();
        }
    }
    if (!batch->tone)
        throw std::runtime_error("missing complete ToneMap set");
    return {std::move(batch), std::move(key)};
}
bool Pixels(const std::vector<std::byte>& pixels)
{
    if (pixels.size() != 64)
        return false;
    const std::array<double, 3> hdr{4, 2, 1};
    std::array<int, 4> expected{0, 0, 0, 255};
    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto x = hdr[i] / (1 + hdr[i]);
        expected[i] =
            static_cast<int>(std::lround((x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055) * 255));
    }
    for (std::size_t i = 0; i < pixels.size(); ++i)
        if (std::abs(std::to_integer<int>(pixels[i]) - expected[i % 4]) > 1)
            return false;
    return true;
}
SwapChainHandle Chain(DeviceLifetime& device)
{
    const auto chain = device.CreateSwapChain({{4, 4}}, [] { return std::make_unique<ResourcePayload>(); });
    device.ResizeBackBuffers(chain, {4, 4},
                             []
                             {
                                 std::vector<DeviceLifetime::BackBufferCandidate> result;
                                 for (int i = 0; i < 3; ++i)
                                     result.push_back({{TextureDimension::Texture2D,
                                                        {4, 4},
                                                        1,
                                                        1,
                                                        1,
                                                        Format::Rgba8Unorm,
                                                        TextureUsage::ColorAttachment,
                                                        "probe token"},
                                                       std::make_unique<ResourcePayload>()});
                                 return result;
                             });
    return chain;
}
void RunPipelineProbe(bool warp)
{
    auto packages = LoadM604Packages();
    ASSERT_EQ(packages.size(), 9U);
    D3D11Probe d11(warp);
    D3D12Probe d12(warp);
    DeviceLifetime a(d11.Capabilities()), b(d12.Capabilities());
    const auto ca = Chain(a), cb = Chain(b);
    int live = 0;
    ShaderRevisionTransaction transaction(a, b);
    std::array<ShaderRevisionTransaction::Builder, 2> builders{
        [&](auto values) { return Build(d11, RhiBackend::D3D11, values, live); },
        [&](auto values) { return Build(d12, RhiBackend::D3D12, values, live); }};
    std::array<std::vector<std::byte>, 2> output;
    const auto smoke = [&](RhiBackend backend, ResourcePayload& payload)
    {
        auto& batch = dynamic_cast<Batch&>(payload);
        auto pixels = backend == RhiBackend::D3D11 ? d11.DrawToneMap(*batch.tone) : d12.DrawToneMap(*batch.tone);
        const bool correct = Pixels(pixels);
        output[static_cast<std::size_t>(backend)] = std::move(pixels);
        return correct;
    };
    transaction.Reload("initial", packages, builders, smoke);
    EXPECT_EQ(live, 2);
    EXPECT_EQ(output[0], output[1]);
    const auto before11 = d11.NativeCreationCount(), before12 = d12.NativeCreationCount();
    EXPECT_GT(before11, 0U);
    EXPECT_GT(before12, 0U);
    std::array<FrameToken, 2> last{};
    for (int frame = 0; frame < 8; ++frame)
    {
        a.Collect(a.LastSubmitted());
        b.Collect(b.LastSubmitted());
        last = {a.BeginFrame(ca), b.BeginFrame(cb)};
        transaction.MarkUsed(RhiBackend::D3D11, last[0]);
        transaction.MarkUsed(RhiBackend::D3D12, last[1]);
        EXPECT_TRUE(smoke(RhiBackend::D3D11, transaction.BackendPayload(RhiBackend::D3D11)));
        EXPECT_TRUE(smoke(RhiBackend::D3D12, transaction.BackendPayload(RhiBackend::D3D12)));
        a.EndFrame(last[0], ca);
        b.EndFrame(last[1], cb);
    }
    EXPECT_EQ(d11.NativeCreationCount(), before11);
    EXPECT_EQ(d12.NativeCreationCount(), before12);
    auto broken = builders;
    broken[1] = [](auto) -> ShaderRevisionBackend
    { throw std::runtime_error("injected second backend create failure"); };
    EXPECT_THROW(transaction.Reload("bad-create", packages, broken, smoke), std::runtime_error);
    EXPECT_EQ(transaction.CurrentRevision(), "initial");
    EXPECT_EQ(live, 2);
    EXPECT_THROW(transaction.Reload("bad-smoke", packages, builders, [&](RhiBackend backend, ResourcePayload& payload)
                                    { return smoke(backend, payload) && backend == RhiBackend::D3D11; }),
                 RhiValidationError);
    EXPECT_EQ(transaction.CurrentRevision(), "initial");
    EXPECT_EQ(live, 2);
    EXPECT_TRUE(smoke(RhiBackend::D3D11, transaction.BackendPayload(RhiBackend::D3D11)));
    EXPECT_TRUE(smoke(RhiBackend::D3D12, transaction.BackendPayload(RhiBackend::D3D12)));
    transaction.Reload("committed", packages, builders, smoke);
    EXPECT_EQ(live, 4);
    EXPECT_EQ(transaction.RetiringCount(), 1U);
    // Draw 已等待实际 EVENT/fence；有意分步交回证明，检验双 owner 退休屏障。
    transaction.Collect();
    EXPECT_EQ(live, 4);
    a.Collect(last[0].serial);
    transaction.Collect();
    EXPECT_EQ(live, 4);
    b.Collect(last[1].serial);
    transaction.Collect();
    EXPECT_EQ(live, 2);
    EXPECT_EQ(output[0], output[1]);
    transaction.Shutdown();
    EXPECT_EQ(live, 0);
    a.Destroy(ca);
    b.Destroy(cb);
    a.Collect(a.LastSubmitted());
    b.Collect(b.LastSubmitted());
    a.CheckShutdown();
    b.CheckShutdown();
    d11.CheckClean();
    d12.CheckClean();
    d11.CheckNoPipelineResources();
    d12.CheckNoPipelineResources();
    std::cout << "M604_CENSUS mode=" << (warp ? "warp" : "hardware")
              << " sawDevice=both d3d11PayloadObjects=0 d3d12PayloadObjects=0\n";
    const auto evidence = std::filesystem::path(M604_ARTIFACT_DIR) / (warp ? "warp" : "hardware");
    std::filesystem::create_directories(evidence);
    for (std::size_t backend = 0; backend < 2; ++backend)
    {
        const auto stem = backend ? "d3d12" : "d3d11";
        std::ofstream raw(evidence / (std::string(stem) + ".rgba"), std::ios::binary);
        raw.write(reinterpret_cast<const char*>(output[backend].data()),
                  static_cast<std::streamsize>(output[backend].size()));
        std::ofstream ppm(evidence / (std::string(stem) + ".ppm"), std::ios::binary);
        ppm << "P6\n4 4\n255\n";
        for (std::size_t pixel = 0; pixel < 16; ++pixel)
            ppm.write(reinterpret_cast<const char*>(output[backend].data() + pixel * 4), 3);
        if (!raw || !ppm)
            throw std::runtime_error("native image evidence write failed");
    }
    std::cout << "M604 mode=" << (warp ? "warp" : "hardware")
              << " packages=9 variants=18 pipelinesPerBackend=7 frames=8 frameNativeCreates=0 debugFailures=0 "
                 "rollback=create+smoke retire=both-completions rgba=";
    for (std::size_t i = 0; i < 4; ++i)
        std::cout << std::to_integer<int>(output[0][i]) << ',';
    std::cout << " nativeCreatesBeforeFrames=" << before11 << ',' << before12
              << " adapters=" << d11.Capabilities().adapterName << " / " << d12.Capabilities().adapterName << '\n';
}
TEST(M604NativePipeline, HardwareDualRevisionFixedDraw)
{
    RunPipelineProbe(false);
}
TEST(M604NativePipeline, WarpDualRevisionFixedDraw)
{
    RunPipelineProbe(true);
}
} // namespace
