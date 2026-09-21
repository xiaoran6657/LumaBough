#include <MiniEngine/Rhi/RhiValidation.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

using namespace MiniEngine::Rhi;
namespace
{
RhiCapabilities FullCapabilities()
{
    RhiCapabilities c;
    c.maxTextureDimension2D = 16384;
    c.maxColorAttachments = 8;
    c.maxAnisotropy = 16;
    c.gpuTimestamps = true;
    c.timestampFrequency = 1000000;
    for (std::size_t i = 1; i < c.kFormatCount; ++i)
    {
        c.formatSupport[i] = kFormatSupportSampled | kFormatSupportCopySource | kFormatSupportCopyDestination;
        c.formatSupport[i] |=
            (i == static_cast<std::size_t>(Format::D32Float) || i == static_cast<std::size_t>(Format::D24UnormS8Uint))
                ? kFormatSupportDepth
                : kFormatSupportColor;
    }
    c.cubeFormatSupport = c.formatSupport;
    c.depthComparisonSampling = true;
    return c;
}
TextureDesc Hdr()
{
    TextureDesc d;
    d.extent = {64, 32};
    d.format = Format::Rgba16Float;
    d.usage = TextureUsage::Sampled | TextureUsage::ColorAttachment;
    d.debugName = "HDR";
    return d;
}
template <class F> void ExpectError(F&& action, RhiErrorCode code)
{
    try
    {
        action();
        FAIL() << "expected structured descriptor/capability error";
    }
    catch (const RhiValidationError& e)
    {
        EXPECT_EQ(e.Error().code, code) << e.what();
        EXPECT_FALSE(e.Error().operation.empty());
        EXPECT_FALSE(e.Error().objectType.empty());
        EXPECT_FALSE(e.Error().message.empty());
    }
}
struct TextureCase
{
    const char* name;
    void (*mutate)(TextureDesc&);
    RhiErrorCode code = RhiErrorCode::InvalidArgument;
};
void PrintTo(const TextureCase& value, std::ostream* stream)
{
    *stream << value.name;
}
class InvalidTexture : public testing::TestWithParam<TextureCase>
{
};
TEST_P(InvalidTexture, RejectsWithoutMutatingInput)
{
    auto d = Hdr();
    GetParam().mutate(d);
    const auto before = d;
    ExpectError([&] { ValidateTextureDesc(d, FullCapabilities()); }, GetParam().code);
    EXPECT_EQ(d, before);
}
const std::vector<TextureCase> kTextureCases{
    {"WidthZero", [](auto& d) { d.extent.width = 0; }},
    {"HeightZero", [](auto& d) { d.extent.height = 0; }},
    {"MipZero", [](auto& d) { d.mipLevels = 0; }},
    {"LayerZero", [](auto& d) { d.arrayLayers = 0; }},
    {"TooManyMips", [](auto& d) { d.mipLevels = 8; }},
    {"UnknownDimension", [](auto& d) { d.dimension = static_cast<TextureDimension>(255); }},
    {"CubeNotSquare",
     [](auto& d)
     {
         d.dimension = TextureDimension::TextureCube;
         d.arrayLayers = 6;
     }},
    {"CubeWrongLayers",
     [](auto& d)
     {
         d.dimension = TextureDimension::TextureCube;
         d.extent.height = 64;
     }},
    {"ArrayUnsupported", [](auto& d) { d.arrayLayers = 2; }, RhiErrorCode::Unsupported},
    {"CubeArray",
     [](auto& d)
     {
         d.dimension = TextureDimension::TextureCube;
         d.extent.height = 64;
         d.arrayLayers = 12;
     }},
    {"MSAAUnsupported", [](auto& d) { d.sampleCount = 4; }, RhiErrorCode::Unsupported},
    {"SampleZero", [](auto& d) { d.sampleCount = 0; }, RhiErrorCode::Unsupported},
    {"UnknownFormat", [](auto& d) { d.format = Format::Unknown; }},
    {"CountFormat", [](auto& d) { d.format = Format::Count; }},
    {"InvalidFormatEnum", [](auto& d) { d.format = static_cast<Format>(255); }},
    {"NoUsage", [](auto& d) { d.usage = TextureUsage::None; }},
    {"UnknownUsage", [](auto& d) { d.usage = static_cast<TextureUsage>(128); }},
    {"ColorAndDepth", [](auto& d) { d.usage = TextureUsage::ColorAttachment | TextureUsage::DepthStencil; }},
    {"ColorOnDepth", [](auto& d) { d.format = Format::D32Float; }},
    {"DepthOnColor", [](auto& d) { d.usage = TextureUsage::DepthStencil; }},
    {"BeyondDevice", [](auto& d) { d.extent.width = 16385; }, RhiErrorCode::Unsupported}};
INSTANTIATE_TEST_SUITE_P(M6, InvalidTexture, testing::ValuesIn(kTextureCases),
                         [](const testing::TestParamInfo<TextureCase>& info) { return info.param.name; });
} // namespace
TEST(RhiDescriptorTests, AcceptsHdrDepthCubeAndMaximumMipShapes)
{
    auto caps = FullCapabilities();
    auto d = Hdr();
    EXPECT_NO_THROW(ValidateTextureDesc(d, caps));
    d.mipLevels = 7;
    EXPECT_NO_THROW(ValidateTextureDesc(d, caps));
    d.dimension = TextureDimension::TextureCube;
    d.extent.height = 64;
    d.arrayLayers = 6;
    EXPECT_NO_THROW(ValidateTextureDesc(d, caps));
    d = Hdr();
    d.format = Format::D32Float;
    d.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
    EXPECT_NO_THROW(ValidateTextureDesc(d, caps));
}
TEST(RhiDescriptorTests, EachMissingFormatUsageIsUnsupported)
{
    auto c = FullCapabilities();
    for (auto flag : {TextureUsage::Sampled, TextureUsage::ColorAttachment, TextureUsage::DepthStencil,
                      TextureUsage::CopySource, TextureUsage::CopyDestination})
    {
        auto d = Hdr();
        d.usage = flag;
        if (flag == TextureUsage::DepthStencil)
        {
            d.format = Format::D32Float;
        }
        auto removed = c;
        removed.formatSupport[static_cast<std::size_t>(d.format)] = 0;
        ExpectError([&] { ValidateTextureDesc(d, removed); }, RhiErrorCode::Unsupported);
        EXPECT_NO_THROW(ValidateTextureDesc(d, c));
    }
}
TEST(RhiDescriptorTests, BufferMemoryUsageAndSharedRangeValidation)
{
    BufferDesc d;
    d.size = 64;
    d.usage = BufferUsage::Vertex;
    EXPECT_NO_THROW(ValidateBufferDesc(d));
    for (BufferDesc invalid :
         std::array{BufferDesc{0, BufferUsage::Vertex, MemoryDomain::GpuOnly, "zero"},
                    BufferDesc{64, BufferUsage::None, MemoryDomain::GpuOnly, "usage"},
                    BufferDesc{64, static_cast<BufferUsage>(128), MemoryDomain::GpuOnly, "unknown"},
                    BufferDesc{64, BufferUsage::Vertex, static_cast<MemoryDomain>(255), "memory"},
                    BufferDesc{64, BufferUsage::Vertex, MemoryDomain::GpuToCpu, "readback"},
                    BufferDesc{64, BufferUsage::CopyDestination, MemoryDomain::CpuToGpu, "upload"},
                    BufferDesc{17, BufferUsage::Uniform, MemoryDomain::GpuOnly, "unaligned"},
                    BufferDesc{65552, BufferUsage::Uniform, MemoryDomain::GpuOnly, "uniform large"},
                    BufferDesc{64, BufferUsage::Uniform | BufferUsage::Vertex, MemoryDomain::GpuOnly, "combined"}})
    {
        ExpectError([&] { ValidateBufferDesc(invalid); }, RhiErrorCode::InvalidArgument);
    }
    d.size = std::uint64_t{1} << 32;
    ExpectError([&] { ValidateBufferDesc(d); }, RhiErrorCode::Unsupported);
    d = {65536, BufferUsage::Uniform, MemoryDomain::GpuOnly, "max uniform"};
    EXPECT_NO_THROW(ValidateBufferDesc(d));
    d = {64, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "readback"};
    EXPECT_NO_THROW(ValidateBufferDesc(d));
    d = {64, BufferUsage::CopySource, MemoryDomain::CpuToGpu, "upload"};
    EXPECT_NO_THROW(ValidateBufferDesc(d));
}
TEST(RhiDescriptorTests, SamplerEnumsNumericRangesAndOptionalAnisotropy)
{
    auto c = FullCapabilities();
    SamplerDesc d;
    EXPECT_NO_THROW(ValidateSamplerDesc(d, c));
    using Mutation = void (*)(SamplerDesc&);
    const std::vector<Mutation> invalid{[](auto& v) { v.minMagFilter = static_cast<Filter>(255); },
                                        [](auto& v) { v.mipFilter = Filter::Anisotropic; },
                                        [](auto& v) { v.addressU = static_cast<AddressMode>(255); },
                                        [](auto& v) { v.addressV = static_cast<AddressMode>(255); },
                                        [](auto& v) { v.addressW = static_cast<AddressMode>(255); },
                                        [](auto& v) { v.comparison = static_cast<CompareOp>(255); },
                                        [](auto& v) { v.minLod = 1001; },
                                        [](auto& v) { v.minLod = std::numeric_limits<float>::quiet_NaN(); },
                                        [](auto& v) { v.maxLod = std::numeric_limits<float>::infinity(); },
                                        [](auto& v) { v.maxAnisotropy = 1.5F; },
                                        [](auto& v) { v.maxAnisotropy = 0; },
                                        [](auto& v) { v.maxAnisotropy = 2; },
                                        [](auto& v) { v.borderColor[3] = std::numeric_limits<float>::infinity(); },
                                        [](auto& v)
                                        {
                                            v.minMagFilter = Filter::Anisotropic;
                                            v.mipFilter = Filter::Nearest;
                                        }};
    for (auto change : invalid)
    {
        auto v = d;
        change(v);
        ExpectError([&] { ValidateSamplerDesc(v, c); }, RhiErrorCode::InvalidArgument);
    }
    d.minMagFilter = Filter::Anisotropic;
    d.maxAnisotropy = 16;
    EXPECT_NO_THROW(ValidateSamplerDesc(d, c));
    c.maxAnisotropy = 8;
    ExpectError([&] { ValidateSamplerDesc(d, c); }, RhiErrorCode::Unsupported);
    d = {};
    d.comparisonEnabled = true;
    d.addressU = AddressMode::Border;
    EXPECT_NO_THROW(ValidateSamplerDesc(d, c));
}
TEST(RhiDescriptorTests, DefaultCapabilitiesAndEveryRequiredCapabilityFailFast)
{
    ExpectError([] { RequireM6Capabilities(RhiCapabilities{}); }, RhiErrorCode::Unsupported);
    auto full = FullCapabilities();
    EXPECT_NO_THROW(RequireM6Capabilities(full));
    using Change = void (*)(RhiCapabilities&);
    const std::vector<Change> changes{[](auto& c) { c.backend = static_cast<RhiBackend>(255); },
                                      [](auto& c) { c.gpuTimestamps = false; },
                                      [](auto& c) { c.timestampFrequency = 0; },
                                      [](auto& c) { c.maxTextureDimension2D = 1024; },
                                      [](auto& c) { c.maxColorAttachments = 0; },
                                      [](auto& c) { c.uniformBufferOffsetAlignment = 0; },
                                      [](auto& c) { c.uniformBufferOffsetAlignment = 3; },
                                      [](auto& c) { c.maxAnisotropy = 0; },
                                      [](auto& c) { c.depthComparisonSampling = false; },
                                      [](auto& c) { c.cubeFormatSupport.fill(0); }};
    for (auto change : changes)
    {
        auto c = full;
        change(c);
        ExpectError([&] { RequireM6Capabilities(c); }, RhiErrorCode::Unsupported);
    }
    for (auto format : {Format::Rgba8Unorm, Format::Rgba8UnormSrgb, Format::Rg16Float, Format::Rgba16Float,
                        Format::R32Float, Format::D32Float})
    {
        auto c = full;
        c.formatSupport[static_cast<std::size_t>(format)] = 0;
        ExpectError([&] { RequireM6Capabilities(c); }, RhiErrorCode::Unsupported);
    }
    // D24 仅为旧 D3D11 主深度 profile 提供表达；默认 M6 profile 仍使用 D32。
    full.formatSupport[static_cast<std::size_t>(Format::D24UnormS8Uint)] = 0;
    EXPECT_NO_THROW(RequireM6Capabilities(full));
    full.maxAnisotropy = 1;
    EXPECT_NO_THROW(RequireM6Capabilities(full));
}
TEST(RhiDescriptorTests, StableHashIgnoresNamesAndUsesAllBufferFields)
{
    BufferDesc d{64, BufferUsage::Vertex, MemoryDomain::GpuOnly, "one"};
    EXPECT_EQ(SemanticHash(d), 0xcebffd77c1e2b4d8ULL);
    const auto baseline = SemanticHash(d);
    auto b = d;
    b.debugName = "another";
    EXPECT_FALSE(d == b);
    EXPECT_TRUE(SemanticallyEqual(d, b));
    EXPECT_EQ(baseline, SemanticHash(b));
    b = d;
    b.size += 1;
    EXPECT_NE(baseline, SemanticHash(b));
    b = d;
    b.usage = BufferUsage::Index;
    EXPECT_NE(baseline, SemanticHash(b));
    b = d;
    b.memory = MemoryDomain::CpuToGpu;
    EXPECT_NE(baseline, SemanticHash(b));
}
TEST(RhiDescriptorTests, TextureSemanticHashIncludesEveryCreationField)
{
    const auto d = Hdr();
    const auto expected = SemanticHash(d);
    using Change = void (*)(TextureDesc&);
    const std::vector<Change> changes{[](auto& v) { v.dimension = TextureDimension::TextureCube; },
                                      [](auto& v) { ++v.extent.width; },
                                      [](auto& v) { ++v.extent.height; },
                                      [](auto& v) { ++v.mipLevels; },
                                      [](auto& v) { ++v.arrayLayers; },
                                      [](auto& v) { ++v.sampleCount; },
                                      [](auto& v) { v.format = Format::Rgba8Unorm; },
                                      [](auto& v) { v.usage = TextureUsage::Sampled; },
                                      [](auto& v) { v.clearColorHint[0] = 2; },
                                      [](auto& v) { v.clearColorHint[1] = 2; },
                                      [](auto& v) { v.clearColorHint[2] = 2; },
                                      [](auto& v) { v.clearColorHint[3] = 2; },
                                      [](auto& v) { v.clearDepthHint = .5F; },
                                      [](auto& v) { v.clearStencilHint = 1; }};
    for (auto change : changes)
    {
        auto v = d;
        change(v);
        EXPECT_NE(expected, SemanticHash(v));
        EXPECT_FALSE(SemanticallyEqual(d, v));
    }
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(SemanticHash(d), expected);
    }
}
TEST(RhiDescriptorTests, TextureClearHintsUseSchemaTwoAndCanonicalizeZero)
{
    auto original = Hdr();
    auto changed = original;
    changed.clearColorHint[0] = -0.0F;
    EXPECT_TRUE(SemanticallyEqual(original, changed));
    const auto json = ToDiagnosticJson(original);
    EXPECT_NE(json.find("\"schemaVersion\":2"), std::string::npos);
    EXPECT_NE(json.find("\"clearDepth\":1"), std::string::npos);
}
TEST(RhiDescriptorTests, SamplerSemanticHashCoversFieldsAndCanonicalizesSignedZero)
{
    SamplerDesc d;
    const auto baseline = SemanticHash(d);
    using Change = void (*)(SamplerDesc&);
    const std::vector<Change> changes{[](auto& v) { v.minMagFilter = Filter::Nearest; },
                                      [](auto& v) { v.mipFilter = Filter::Nearest; },
                                      [](auto& v) { v.addressU = AddressMode::Clamp; },
                                      [](auto& v) { v.addressV = AddressMode::Clamp; },
                                      [](auto& v) { v.addressW = AddressMode::Clamp; },
                                      [](auto& v) { v.comparisonEnabled = true; },
                                      [](auto& v) { v.comparison = CompareOp::Always; },
                                      [](auto& v) { v.maxAnisotropy = 2; },
                                      [](auto& v) { v.minLod = 1; },
                                      [](auto& v) { v.maxLod = 10; },
                                      [](auto& v) { v.borderColor[0] = 1; },
                                      [](auto& v) { v.borderColor[1] = 1; },
                                      [](auto& v) { v.borderColor[2] = 1; },
                                      [](auto& v) { v.borderColor[3] = 1; }};
    for (auto change : changes)
    {
        auto v = d;
        change(v);
        EXPECT_NE(baseline, SemanticHash(v));
        EXPECT_FALSE(SemanticallyEqual(d, v));
    }
    auto zero = d;
    zero.minLod = -0.0F;
    zero.borderColor[0] = -0.0F;
    EXPECT_EQ(zero, d);
    EXPECT_TRUE(SemanticallyEqual(zero, d));
    EXPECT_EQ(SemanticHash(zero), baseline);
    zero.minLod = std::numeric_limits<float>::quiet_NaN();
    ExpectError([&] { static_cast<void>(SemanticHash(zero)); }, RhiErrorCode::InvalidArgument);
    EXPECT_NE(ToDiagnosticJson(zero).find("\"NaN\""), std::string::npos);
}
TEST(RhiDescriptorTests, DiagnosticJsonIncludesEscapedNamesAndStableNumericFields)
{
    BufferDesc d{64, BufferUsage::Vertex, MemoryDomain::GpuOnly, "name\"\\\n"};
    EXPECT_EQ(ToDiagnosticJson(d),
              "{\"schemaVersion\":1,\"size\":64,\"usage\":1,\"memory\":0,\"debugName\":\"name\\\"\\\\\\u000a\"}");
    auto texture = Hdr();
    EXPECT_NE(ToDiagnosticJson(texture).find("\"height\":32"), std::string::npos);
    SamplerDesc sampler;
    EXPECT_NE(ToDiagnosticJson(sampler).find("\"comparisonEnabled\":false"), std::string::npos);
}

TEST(RhiDescriptorTests, Texture2DSupportDoesNotImplyCubeSupport)
{
    auto c = FullCapabilities();
    c.cubeFormatSupport.fill(0);
    auto d = Hdr();
    EXPECT_NO_THROW(ValidateTextureDesc(d, c));
    d.dimension = TextureDimension::TextureCube;
    d.extent.height = 64;
    d.arrayLayers = 6;
    ExpectError([&] { ValidateTextureDesc(d, c); }, RhiErrorCode::Unsupported);
    EXPECT_FALSE(c.SupportsSampled(Format::Rgba16Float, static_cast<TextureDimension>(255)));
}

TEST(RhiDescriptorTests, MissingRequiredCapabilityHasExplicitBlockedStatus)
{
    auto missing = AssessM6Capabilities(RhiCapabilities{});
    EXPECT_EQ(missing.status, RhiCapabilityStatus::Blocked);
    ASSERT_TRUE(missing.error);
    EXPECT_EQ(missing.error->code, RhiErrorCode::Unsupported);
    auto ready = AssessM6Capabilities(FullCapabilities());
    EXPECT_EQ(ready.status, RhiCapabilityStatus::Ready);
    EXPECT_FALSE(ready.error);
}
