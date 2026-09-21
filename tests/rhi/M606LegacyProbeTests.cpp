#include "M604D3D11Probe.h"
#include "M604D3D12Probe.h"
#include "M604ShaderFixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace MiniEngine::Rhi;
using namespace MiniEngine::Rhi::M604;

#ifndef M606_LEGACY_DIR
#define M606_LEGACY_DIR "m606-legacy"
#endif

namespace
{
ShaderDesc Shader(std::span<const ShaderPackage> packages, std::string_view assetId, RhiBackend backend,
                  ShaderStage stage)
{
    const auto found = std::find_if(packages.begin(), packages.end(),
                                    [&](const ShaderPackage& package) { return package.assetId == assetId; });
    if (found == packages.end())
        throw std::runtime_error("M606 fixture shader missing: " + std::string(assetId));
    return SelectShader(*found, backend, stage);
}

GraphicsPipelineDesc ToneMapPipeline()
{
    GraphicsPipelineDesc pipeline;
    pipeline.colorAttachmentCount = 1;
    pipeline.colorFormats[0] = Format::Rgba8Unorm;
    pipeline.depthFormat = Format::Unknown;
    pipeline.cullMode = CullMode::None;
    pipeline.depthTest = false;
    pipeline.depthWrite = false;
    pipeline.debugName = "M606.LegacyReference.ToneMap";
    return pipeline;
}

GraphicsPipelineDesc DepthPipeline()
{
    GraphicsPipelineDesc pipeline;
    pipeline.vertexAttributes.push_back({VertexSemantic::Position, VertexFormat::Float3, 0, 0, 0, 0});
    pipeline.depthFormat = Format::D32Float;
    pipeline.cullMode = CullMode::None;
    pipeline.depthTest = true;
    pipeline.depthWrite = true;
    pipeline.depthCompare = CompareOp::Less;
    pipeline.colorAttachmentCount = 0;
    pipeline.debugName = "M606.LegacyReference.ShadowDepth";
    return pipeline;
}

std::string BackendName(RhiBackend backend)
{
    return backend == RhiBackend::D3D11 ? "d3d11" : "d3d12";
}

std::string ModeName(bool warp)
{
    return warp ? "warp" : "hardware";
}

void WriteRaw(const std::filesystem::path& path, const std::vector<std::byte>& pixels)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("M606 legacy raw output cannot open: " + path.string());
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!output)
        throw std::runtime_error("M606 legacy raw output failed: " + path.string());
}

void WriteJson(const std::filesystem::path& path, RhiBackend backend, bool warp, const M606Readback& clear,
               const std::vector<std::byte>& tone4x4, const M606Readback& exposure0, const M606Readback& exposure1,
               const M606Readback& resized0, const M606Readback& resized1)
{
    std::ofstream output(path);
    if (!output)
        throw std::runtime_error("M606 legacy JSON output cannot open: " + path.string());
    output << std::boolalpha << "{\n";
    output << "  \"source\": \"m604-concrete-probe-m606-fixed\",\n";
    output << "  \"backend\": \"" << BackendName(backend) << "\",\n";
    output << "  \"deviceMode\": \"" << ModeName(warp) << "\",\n";
    output << "  \"debug\": true,\n";
    output << "  \"sameInput\": true,\n";
    output << "  \"inputKey\": {\n";
    output << "    \"initialExtent\": [96, 64],\n";
    output << "    \"resizedExtent\": [113, 75],\n";
    output << "    \"clearRGBA\": [0.04, 0.08, 0.14, 1.0],\n";
    output << "    \"hdrRGBA\": [4.0, 2.0, 1.0, 1.0],\n";
    output << "    \"depth\": 0.25,\n";
    output << "    \"vertexData\": [[-0.75, -0.75, 0.25], [0.0, 0.75, 0.25], [0.75, -0.75, 0.25]],\n";
    output << "    \"indices\": [0, 1, 2],\n";
    output << "    \"depthResource\": \"R32_TYPELESS\",\n";
    output << "    \"depthDSV\": \"D32_FLOAT\",\n";
    output << "    \"depthSRV\": \"R32_FLOAT\",\n";
    output << "    \"target\": \"R8G8B8A8_UNORM\",\n";
    output << "    \"toneConstants\": \"ExposureEv,DebugHdr,1/width,1/height\",\n";
    output << "    \"shaderPackage\": \"M604-generated\",\n";
    output << "    \"depthRepeats\": 2\n";
    output << "  },\n";
    output << "  \"level2\": {\"extent\": [4, 4], \"sourceFormat\": \"R32G32B32A32_FLOAT\", "
              "\"targetFormat\": \"R8G8B8A8_UNORM\", \"sameInput\": false},\n";
    output << "  \"frames\": [\n";
    auto frame = [&](std::string_view name, const M606Readback& value, bool comma)
    {
        output << "    {\"name\": \"" << name << "\", \"width\": " << value.width << ", \"height\": " << value.height
               << ", \"completion\": " << value.completion << ", \"timestampBegin\": " << value.timestampBegin
               << ", \"timestampEnd\": " << value.timestampEnd
               << ", \"timestampFrequency\": " << value.timestampFrequency
               << ", \"timestampDisjoint\": " << value.timestampDisjoint << "}" << (comma ? "," : "") << "\n";
    };
    frame("level1-clear-96x64", clear, true);
    frame("level3-depth-ev0-96x64", exposure0, true);
    frame("level3-depth-ev1-96x64", exposure1, true);
    frame("level4-resize-ev0-113x75", resized0, true);
    frame("level5-readback-timestamp-113x75", resized0, true);
    frame("level6-revision-ev1-113x75", resized1, false);
    output << "  ],\n";
    output << "  \"nativeReadback\": {\"level2Bytes\": " << tone4x4.size()
           << ", \"tightRGBA8\": true, \"completionProof\": \"EVENT-or-fence\", "
              "\"boundedTimeoutMs\": 5000},\n";
    output << "  \"diagnostics\": {\"normalDebugWarningsErrors\": 0, \"nativeCensus\": \"checked by probe\"}\n";
    output << "}\n";
    if (!output)
        throw std::runtime_error("M606 legacy JSON output failed: " + path.string());
}

template <class Probe> void RunLegacyReference(RhiBackend backend, bool warp)
{
    const auto packages = LoadM604Packages();
    const auto toneVertex = Shader(packages, "ToneMapVSMain", backend, ShaderStage::Vertex);
    const auto tonePixel = Shader(packages, "ToneMapPSMain", backend, ShaderStage::Pixel);
    const auto depthVertex = Shader(packages, "ShadowDepthVSMain", backend, ShaderStage::Vertex);
    Probe probe(warp);
    auto tonePipeline = probe.CreatePipeline(toneVertex, &tonePixel, ToneMapPipeline());
    auto depthPipeline = probe.CreatePipeline(depthVertex, nullptr, DepthPipeline());
    const auto clear = probe.DrawM606Clear(96, 64);
    ASSERT_EQ(clear.width, 96U);
    ASSERT_EQ(clear.height, 64U);
    ASSERT_EQ(clear.rgba.size(), static_cast<std::size_t>(96 * 64 * 4));
    ASSERT_EQ(std::to_integer<unsigned char>(clear.rgba[0]), 10U);
    ASSERT_EQ(std::to_integer<unsigned char>(clear.rgba[1]), 20U);
    ASSERT_EQ(std::to_integer<unsigned char>(clear.rgba[2]), 36U);
    ASSERT_EQ(std::to_integer<unsigned char>(clear.rgba[3]), 255U);
    ASSERT_GT(clear.timestampFrequency, 0ULL);
    ASSERT_LE(clear.timestampBegin, clear.timestampEnd);
    const auto tone4x4 = probe.DrawToneMap(*tonePipeline);
    ASSERT_EQ(tone4x4.size(), static_cast<std::size_t>(4 * 4 * 4));
    const auto depth0 = probe.DrawM606DepthToneMap(*depthPipeline, *tonePipeline, 96, 64, 0.0F);
    const auto depth1 = probe.DrawM606DepthToneMap(*depthPipeline, *tonePipeline, 96, 64, 1.0F);
    ASSERT_EQ(depth0.rgba.size(), clear.rgba.size());
    ASSERT_EQ(depth1.rgba.size(), clear.rgba.size());
    ASSERT_EQ(depth0.width, 96U);
    ASSERT_EQ(depth0.height, 64U);
    ASSERT_FALSE(depth0.timestampDisjoint);
    ASSERT_GT(depth0.timestampFrequency, 0ULL);
    ASSERT_LE(depth0.timestampBegin, depth0.timestampEnd);
    ASSERT_GT(depth0.completion, clear.completion);
    EXPECT_NE(depth0.rgba, depth1.rgba);

    const auto center = (static_cast<std::size_t>(depth0.height / 2) * depth0.width + depth0.width / 2) * 4;
    const auto corner = 0U;
    EXPECT_LT(std::to_integer<unsigned char>(depth0.rgba[center]), std::to_integer<unsigned char>(depth0.rgba[corner]))
        << "fixed .25 depth should tone-map darker than clear depth 1";
    EXPECT_GT(std::to_integer<unsigned char>(depth1.rgba[center]), std::to_integer<unsigned char>(depth0.rgba[center]))
        << "EV1 should increase the fixed depth sample";
    const auto resized0 = probe.DrawM606DepthToneMap(*depthPipeline, *tonePipeline, 113, 75, 0.0F);
    const auto resized1 = probe.DrawM606DepthToneMap(*depthPipeline, *tonePipeline, 113, 75, 1.0F);
    ASSERT_EQ(resized0.width, 113U);
    ASSERT_EQ(resized0.height, 75U);
    ASSERT_EQ(resized0.rgba.size(), static_cast<std::size_t>(113 * 75 * 4));
    ASSERT_EQ(resized1.rgba.size(), resized0.rgba.size());
    ASSERT_GT(resized0.completion, depth1.completion);
    EXPECT_NE(resized0.rgba, resized1.rgba);
    ASSERT_LE(resized0.timestampBegin, resized0.timestampEnd);
    ASSERT_GT(resized0.timestampFrequency, 0ULL);

    const auto outputDir = std::filesystem::path(M606_LEGACY_DIR) / ModeName(warp);
    std::filesystem::create_directories(outputDir);
    const auto backendName = BackendName(backend);
    WriteRaw(outputDir / (backendName + "-level1-clear-96x64.rgba"), clear.rgba);
    WriteRaw(outputDir / (backendName + "-level2-tone-4x4.rgba"), tone4x4);
    WriteRaw(outputDir / (backendName + "-level3-depth-ev0-96x64.rgba"), depth0.rgba);
    WriteRaw(outputDir / (backendName + "-level3-depth-ev1-96x64.rgba"), depth1.rgba);
    WriteRaw(outputDir / (backendName + "-level4-resize-ev0-113x75.rgba"), resized0.rgba);
    WriteRaw(outputDir / (backendName + "-level5-readback-timestamp-113x75.rgba"), resized0.rgba);
    WriteRaw(outputDir / (backendName + "-level6-revision-ev1-113x75.rgba"), resized1.rgba);
    WriteJson(outputDir / (backendName + "-legacy.json"), backend, warp, clear, tone4x4, depth0, depth1, resized0,
              resized1);

    const auto createdBeforeReset = probe.NativeCreationCount();
    probe.ResetM606();
    EXPECT_GT(probe.NativeCreationCount(), 0ULL);
    EXPECT_GE(probe.NativeCreationCount(), createdBeforeReset);
    depthPipeline.reset();
    tonePipeline.reset();
    probe.CheckClean();
    probe.CheckNoPipelineResources();
    std::cout << "M606_LEGACY backend=" << backendName << " mode=" << ModeName(warp)
              << " level1=96x64 level2=4x4 level3=depth-ev0+ev1 level4=113x75 level5=event-or-fence"
              << " level6=revision-ev1 diagnostics=0\n";
}

TEST(M606LegacyReference, D3D11HardwareFixedInputs)
{
    RunLegacyReference<D3D11Probe>(RhiBackend::D3D11, false);
}
TEST(M606LegacyReference, D3D11WarpFixedInputs)
{
    RunLegacyReference<D3D11Probe>(RhiBackend::D3D11, true);
}
TEST(M606LegacyReference, D3D12HardwareFixedInputs)
{
    RunLegacyReference<D3D12Probe>(RhiBackend::D3D12, false);
}
TEST(M606LegacyReference, D3D12WarpFixedInputs)
{
    RunLegacyReference<D3D12Probe>(RhiBackend::D3D12, true);
}
} // namespace
