#include "NativeDevice.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace MiniEngine::Rhi;

namespace
{
RhiCapabilities TestCapabilities()
{
    RhiCapabilities capabilities;
    capabilities.maxTextureDimension2D = 16384;
    capabilities.maxColorAttachments = 4;
    capabilities.maxAnisotropy = 16;
    capabilities.uniformBufferOffsetAlignment = 256;
    capabilities.formatSupport.fill(31);
    capabilities.cubeFormatSupport.fill(31);
    return capabilities;
}

struct CountingPayload final : ResourcePayload
{
    explicit CountingPayload(std::size_t& live) : m_live(live)
    {
        ++m_live;
    }

    ~CountingPayload() override
    {
        --m_live;
    }

    std::size_t& m_live;
};

class FailingInitialUploadBackend final : public NativeRhiBackend
{
  public:
    explicit FailingInitialUploadBackend(std::size_t& live) : m_live(live), m_capabilities(TestCapabilities())
    {
    }

    const RhiCapabilities& Capabilities() const override
    {
        return m_capabilities;
    }

    void Attach(DeviceLifetime&) override
    {
    }

    std::unique_ptr<ResourcePayload> CreateBuffer(const BufferDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateTexture(const TextureDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateSampler(const SamplerDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateShader(const ShaderDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateResourceSetLayout(const ResourceSetLayoutDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateResourceSet(const ResourceSetDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreatePipelineLayout(const PipelineLayoutDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateGraphicsPipeline(const GraphicsPipelineDesc&) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateTimestampQuery(std::string_view) override
    {
        return MakePayload();
    }

    std::unique_ptr<ResourcePayload> CreateSwapChain(const SwapChainDesc&) override
    {
        return MakePayload();
    }

    std::vector<DeviceLifetime::BackBufferCandidate> ResizeBackBuffers(ResourcePayload&, const SwapChainDesc&) override
    {
        return {};
    }

    std::uint32_t CurrentBackBufferIndex(ResourcePayload&) override
    {
        return 0;
    }

    void BeginFrame(const FrameToken&, ResourcePayload&) override
    {
    }

    void Consume(const CommandEvent&) override
    {
    }

    void Submit(const FrameToken&) override
    {
    }

    void Present(ResourcePayload&) override
    {
    }

    std::unique_ptr<ResourcePayload> CreateDynamicBuffer(const BufferDesc&, std::span<const std::byte>) override
    {
        return MakePayload();
    }

    void UploadBuffer(ResourcePayload&, std::uint64_t, std::span<const std::byte>, std::uint64_t) override
    {
        ++uploadCalls;
        throw std::runtime_error("injected pre-submit upload failure");
    }

    void UploadTexture(ResourcePayload&, std::span<const TextureSubresourceData>, std::uint64_t serial) override
    {
        completedSerial = serial;
    }

    std::uint64_t PollCompleted() override
    {
        return completedSerial;
    }

    std::uint64_t WaitFor(std::uint64_t) override
    {
        return 0;
    }

    void WaitIdle() override
    {
    }

    std::optional<TimestampResult> TryReadTimestamp(ResourcePayload&) override
    {
        return std::nullopt;
    }

    std::optional<TextureReadbackResult> TryReadTextureReadback(ResourcePayload&) override
    {
        return std::nullopt;
    }

    NativeBackendReport Report(bool) override
    {
        ++reportCalls;
        if (reportThrows)
            throw std::runtime_error("diagnostic collection failure");
        return {};
    }

    std::array<std::unique_ptr<ResourcePayload>, 4> PrepareEnvironment(
        ResourcePayload&, std::string_view, std::uint64_t, const std::array<TextureDesc, 4>& descriptors) override
    {
        ++preparationCalls;
        preparedDescriptors = descriptors;
        if (failPreparation)
            throw std::runtime_error("primary environment preparation failure");
        std::array<std::unique_ptr<ResourcePayload>, 4> result;
        for (std::size_t i = 0; i < 4; ++i)
            if (i != missingPreparedTexture)
                result[i] = MakePayload();
        return result;
    }
    std::uint64_t completedSerial = 0;
    std::size_t preparationCalls = 0;
    std::size_t missingPreparedTexture = 4;
    std::array<TextureDesc, 4> preparedDescriptors{};
    std::size_t uploadCalls = 0;
    bool failPreparation = false;
    bool reportThrows = false;
    std::size_t reportCalls = 0;

  private:
    std::unique_ptr<ResourcePayload> MakePayload()
    {
        return std::make_unique<CountingPayload>(m_live);
    }

    std::size_t& m_live;
    RhiCapabilities m_capabilities;
};

TEST(NativeDeviceTest, InitialUploadFailureCollectsCreatedPayloadBeforeRethrow)
{
    std::size_t livePayloads = 0;
    auto backend = std::make_unique<FailingInitialUploadBackend>(livePayloads);
    auto* backendPointer = backend.get();
    NativeDevice device(std::move(backend), {});

    const auto baseline = device.Diagnostics();
    EXPECT_EQ(baseline.aliveObjects, 0U);
    EXPECT_EQ(baseline.retiringObjects, 0U);
    EXPECT_EQ(baseline.lastSubmittedSerial, 0U);
    EXPECT_EQ(baseline.completedSerial, 0U);
    EXPECT_EQ(livePayloads, 0U);

    const BufferDesc desc{64, BufferUsage::Vertex, MemoryDomain::GpuOnly, "initial upload failure"};
    const std::array<std::byte, 16> initial{};
    EXPECT_THROW(device.CreateBuffer(desc, std::span<const std::byte>(initial)), RhiException);
    EXPECT_EQ(backendPointer->uploadCalls, 1U);

    const auto afterFailure = device.Diagnostics();
    EXPECT_EQ(afterFailure.aliveObjects, baseline.aliveObjects);
    EXPECT_EQ(afterFailure.retiringObjects, baseline.retiringObjects);
    EXPECT_EQ(afterFailure.lastSubmittedSerial, baseline.lastSubmittedSerial);
    EXPECT_EQ(afterFailure.completedSerial, baseline.completedSerial);
    EXPECT_EQ(livePayloads, 0U);

    const auto recovered = device.CreateBuffer(desc, std::span<const std::byte>{});
    EXPECT_EQ(device.Diagnostics().aliveObjects, 1U);
    EXPECT_EQ(livePayloads, 1U);
    device.Destroy(recovered);
    device.WaitIdle();
    EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
    EXPECT_EQ(livePayloads, 0U);
    EXPECT_NO_THROW(device.Shutdown());
}

TEST(NativeDeviceTest, PreparedEnvironmentValidatesInputBeforeCallingBackend)
{
    std::size_t live = 0;
    auto backend = std::make_unique<FailingInitialUploadBackend>(live);
    auto* observed = backend.get();
    NativeDevice device(std::move(backend), {});
    TextureDesc desc;
    desc.extent = {1, 1};
    desc.format = Format::Rgba16Float;
    desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
    const auto panorama = device.CreateTexture(desc);
    EXPECT_THROW((void)device.PrepareEnvironment(panorama, "shaders", 1), RhiException);
    const std::array<std::byte, 8> texel{};
    const TextureSubresourceData upload{texel, 8, 8};
    device.UploadTexture(panorama, std::span(&upload, 1));
    EXPECT_THROW((void)device.PrepareEnvironment(panorama, "", 1), RhiException);
    EXPECT_THROW((void)device.PrepareEnvironment(panorama, "shaders", 0), RhiException);
    EXPECT_EQ(observed->preparationCalls, 0);
    const auto prepared = device.PrepareEnvironment(panorama, "shaders", 7);
    EXPECT_EQ(prepared.sourceRevision, 7);
    EXPECT_EQ(observed->preparationCalls, 1);
    EXPECT_EQ(prepared.descriptors[0].mipLevels, 10);
    EXPECT_EQ(prepared.descriptors[1].extent.width, 32);
    EXPECT_EQ(prepared.descriptors[2].mipLevels, 8);
    EXPECT_EQ(prepared.descriptors[3].format, Format::Rg16Float);
    for (auto handle : prepared.textures)
        device.Destroy(handle);
    device.Destroy(panorama);
    device.Shutdown();
    EXPECT_EQ(live, 0);
}
TEST(NativeDeviceTest, PartialPreparedEnvironmentNeverLeaksPublishedOwners)
{
    std::size_t live = 0;
    auto backend = std::make_unique<FailingInitialUploadBackend>(live);
    auto* observed = backend.get();
    NativeDevice device(std::move(backend), {});
    TextureDesc desc;
    desc.extent = {1, 1};
    desc.format = Format::Rgba16Float;
    desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
    const auto panorama = device.CreateTexture(desc);
    const std::array<std::byte, 8> texel{};
    const TextureSubresourceData upload{texel, 8, 8};
    device.UploadTexture(panorama, std::span(&upload, 1));
    const auto previous = device.PrepareEnvironment(panorama, "shaders", 1);
    observed->missingPreparedTexture = 2;
    EXPECT_THROW((void)device.PrepareEnvironment(panorama, "shaders", 2), std::runtime_error);
    EXPECT_EQ(device.Diagnostics().aliveObjects, 5);
    for (auto texture : previous.textures)
        device.Destroy(texture);
    device.Destroy(panorama);
    device.Shutdown();
    EXPECT_EQ(live, 0);
}

TEST(NativeDeviceTest, EnvironmentFailureCollectsDiagnosticsWithoutReplacingPrimaryError)
{
    std::size_t live = 0;
    auto backend = std::make_unique<FailingInitialUploadBackend>(live);
    auto* observed = backend.get();
    NativeDevice device(std::move(backend), {});
    TextureDesc desc;
    desc.extent = {1, 1};
    desc.format = Format::Rgba16Float;
    desc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
    const auto panorama = device.CreateTexture(desc);
    const std::array<std::byte, 8> texel{};
    const TextureSubresourceData upload{texel, 8, 8};
    device.UploadTexture(panorama, std::span(&upload, 1));

    observed->failPreparation = true;
    observed->reportThrows = true;
    try
    {
        static_cast<void>(device.PrepareEnvironment(panorama, "shaders", 1));
        FAIL() << "environment preparation unexpectedly succeeded";
    }
    catch (const RhiException& error)
    {
        EXPECT_NE(std::string_view(error.what()).find("primary environment preparation failure"),
                  std::string_view::npos);
        EXPECT_EQ(std::string_view(error.what()).find("diagnostic collection failure"), std::string_view::npos);
    }
    EXPECT_EQ(observed->preparationCalls, 1U);
    EXPECT_EQ(observed->reportCalls, 1U);
    EXPECT_EQ(device.Diagnostics().aliveObjects, 1U);
    device.Destroy(panorama);
    device.Shutdown();
    EXPECT_EQ(live, 0U);
}

TEST(NativeDeviceTest, FrameTraceIsExplicitAndCommandCountSurvivesDisabledCapture)
{
    std::size_t live = 0;
    auto backend = std::make_unique<FailingInitialUploadBackend>(live);
    NativeDevice device(std::move(backend), {});
    device.ResetFrameDiagnostics(true);
    const auto first = device.CreateBuffer({16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "trace buffer"}, {});
    EXPECT_NE(device.SemanticTrace().find("CreateBuffer"), std::string::npos);
    EXPECT_EQ(device.FrameCommandCount(), 1);
    device.Destroy(first);
    device.ResetFrameDiagnostics(false);
    const auto second = device.CreateBuffer({16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "count only"}, {});
    EXPECT_EQ(device.SemanticTrace(), "miniengine.native-rhi-semantic.v1\n");
    EXPECT_EQ(device.FrameCommandCount(), 1);
    device.Destroy(second);
    device.Shutdown();
    EXPECT_EQ(live, 0);
}
} // namespace
