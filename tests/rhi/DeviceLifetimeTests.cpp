#include "DeviceLifetime.h"
#include <MiniEngine/Rhi/RhiValidation.h>
#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

using namespace MiniEngine::Rhi;
namespace
{
RhiCapabilities Caps()
{
    RhiCapabilities c;
    c.maxTextureDimension2D = 16384;
    c.maxColorAttachments = 4;
    c.maxAnisotropy = 16;
    c.uniformBufferOffsetAlignment = 256;
    c.formatSupport.fill(31);
    c.cubeFormatSupport.fill(31);
    return c;
}
struct Payload final : ResourcePayload
{
    explicit Payload(int& count) : live(count)
    {
        ++live;
    }
    ~Payload() override
    {
        --live;
    }
    int& live;
};
class DeviceLifetimeTest : public testing::Test
{
  protected:
    int live = 0;
    DeviceLifetime device{Caps()};
    PayloadFactory Make()
    {
        return [&] { return std::make_unique<Payload>(live); };
    }
    DeviceLifetime::DynamicPayloadFactory MakeDynamic()
    {
        return [&](const BufferDesc&, std::span<const std::byte>) { return Make()(); };
    }
    TextureDesc Texture(Extent2D size = {64, 64})
    {
        return {TextureDimension::Texture2D,
                size,
                1,
                1,
                1,
                Format::Rgba8Unorm,
                TextureUsage::Sampled | TextureUsage::ColorAttachment | TextureUsage::CopyDestination,
                "texture"};
    }
    std::vector<DeviceLifetime::BackBufferCandidate> BackBuffers(Extent2D size)
    {
        std::vector<DeviceLifetime::BackBufferCandidate> candidates;
        for (int i = 0; i < 3; ++i)
        {
            candidates.push_back({Texture(size), Make()()});
        }
        return candidates;
    }
    SwapChainHandle Chain()
    {
        SwapChainDesc d;
        d.extent = {64, 64};
        auto h = device.CreateSwapChain(d, Make());
        device.ResizeBackBuffers(h, d.extent, [&] { return BackBuffers({64, 64}); });
        return h;
    }
    ResourceSetLayoutHandle Layout()
    {
        ResourceSetLayoutDesc d;
        d.entries.push_back({0, BindingType::SampledTexture, 1, ShaderStage::Pixel, false});
        return device.CreateResourceSetLayout(d, Make());
    }
    ResourceSetHandle Set(ResourceSetLayoutHandle layout, TextureHandle texture, PayloadFactory make)
    {
        ResourceSetDesc d;
        d.layout = layout;
        ResourceBinding b;
        b.type = BindingType::SampledTexture;
        b.texture = texture;
        d.bindings.push_back(b);
        return device.CreateResourceSet(d, make);
    }
    void Finish(SwapChainHandle chain)
    {
        device.Collect(device.LastSubmitted());
        device.Destroy(chain);
        device.Collect(device.LastSubmitted());
        EXPECT_NO_THROW(device.CheckShutdown());
        EXPECT_EQ(live, 0);
    }
};
TEST_F(DeviceLifetimeTest, ValidationPrecedesBackendCreationAndFailureDoesNotAllocateSlot)
{
    bool invoked = false;
    EXPECT_THROW(device.CreateBuffer({},
                                     [&]
                                     {
                                         invoked = true;
                                         return Make()();
                                     }),
                 RhiValidationError);
    EXPECT_FALSE(invoked);
    EXPECT_EQ(device.Stats().slots, 0U);
    BufferDesc d{64, BufferUsage::Vertex, MemoryDomain::GpuOnly, "mesh"};
    EXPECT_THROW(device.CreateBuffer(d, []() -> std::unique_ptr<ResourcePayload>
                                     { throw std::runtime_error("allocation failed"); }),
                 std::runtime_error);
    EXPECT_EQ(device.Stats().alive, 0U);
    EXPECT_EQ(live, 0);
    auto b = device.CreateBuffer(d, Make());
    device.Destroy(b);
    EXPECT_THROW(device.ValidateAlive(b), RhiValidationError);
    EXPECT_THROW(device.Destroy(b), RhiValidationError);
    EXPECT_EQ(live, 1);
    device.Collect(0);
    EXPECT_EQ(live, 0);
    EXPECT_NO_THROW(device.CheckShutdown());
}
TEST_F(DeviceLifetimeTest, TypedRegistryOwnerRejectsForgedCrossKindAndForeignDevice)
{
    auto t = device.CreateTexture(Texture(), Make());
    BufferHandle wrong{t.Index(), t.Generation(), t.Owner()};
    EXPECT_THROW(device.ValidateAlive(wrong), RhiValidationError);
    DeviceLifetime other(Caps());
    EXPECT_THROW(other.ValidateAlive(t), RhiValidationError);
    device.Destroy(t);
    device.Collect(0);
}
TEST_F(DeviceLifetimeTest, ImmutableSetPinsResourcesAndUsePropagatesToRetiringDependencies)
{
    auto chain = Chain();
    auto layout = Layout();
    auto texture = device.CreateTexture(Texture(), Make());
    auto set = Set(layout, texture, Make());
    EXPECT_THROW(device.Destroy(texture), RhiValidationError);
    EXPECT_THROW(device.Destroy(layout), RhiValidationError);
    auto frame = device.BeginFrame(chain);
    device.Use(frame, set);
    device.Destroy(set);
    device.Destroy(texture);
    device.Destroy(layout);
    EXPECT_THROW(device.Use(frame, set), RhiValidationError);
    const int before = live;
    device.Collect(0);
    EXPECT_EQ(live, before);
    EXPECT_EQ(device.Stats().retiring, 3U);
    EXPECT_THROW(device.Collect(frame.serial), RhiValidationError); // 尚未提交，CPU 录制结束不是 completion。
    device.EndFrame(frame, chain);
    device.Collect(frame.serial);
    EXPECT_EQ(live, before - 3);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, ReloadFailurePreservesOldRevisionAndSuccessRetiresInFlightRevision)
{
    auto chain = Chain();
    auto layout = Layout();
    auto oldTexture = device.CreateTexture(Texture(), Make());
    auto current = Set(layout, oldTexture, Make());
    const auto original = current;
    auto frame = device.BeginFrame(chain);
    device.Use(frame, current);
    device.EndFrame(frame, chain);
    auto replacementTexture = device.CreateTexture(Texture(), Make());
    EXPECT_THROW(Set(layout, replacementTexture, []() -> std::unique_ptr<ResourcePayload>
                     { throw std::runtime_error("descriptor allocation failed"); }),
                 std::runtime_error);
    EXPECT_EQ(current, original);
    EXPECT_NO_THROW(device.ValidateAlive(current));
    EXPECT_THROW(device.Destroy(oldTexture), RhiValidationError);
    auto replacement = Set(layout, replacementTexture, Make());
    device.CommitReplacement(current, replacement);
    EXPECT_EQ(current, replacement);
    device.Destroy(oldTexture);
    device.Collect(0);
    EXPECT_EQ(device.Stats().retiring, 2U);
    device.Collect(frame.serial);
    EXPECT_EQ(device.Stats().retiring, 0U);
    device.Destroy(current);
    device.Destroy(replacementTexture);
    device.Destroy(layout);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, PipelineRetainsShaderAndLayoutUntilLogicalDestroy)
{
    const std::array<std::byte, 4> bytecode{};
    ShaderDesc d;
    d.bytecode = bytecode;
    d.entryPoint = "VSMain";
    d.sourceHash = std::string(64, 'a');
    d.semanticHash = std::string(64, 'b');
    auto vertex = device.CreateShader(d, Make());
    d.stage = ShaderStage::Pixel;
    auto pixel = device.CreateShader(d, Make());
    auto layout = device.CreatePipelineLayout({}, Make());
    GraphicsPipelineDesc p;
    p.vertexShader = vertex;
    p.pixelShader = pixel;
    p.layout = layout;
    p.depthFormat = Format::D32Float;
    auto pipeline = device.CreateGraphicsPipeline(p, Make());
    EXPECT_THROW(device.Destroy(vertex), RhiValidationError);
    EXPECT_THROW(device.Destroy(layout), RhiValidationError);
    device.Destroy(pipeline);
    device.Destroy(vertex);
    device.Destroy(pixel);
    device.Destroy(layout);
    device.Collect(0);
    EXPECT_EQ(live, 0);
    EXPECT_NO_THROW(device.CheckShutdown());
}
TEST_F(DeviceLifetimeTest, DynamicSliceRequiresExactCurrentFrameAndCannotBecomePersistent)
{
    auto chain = Chain();
    ResourceSetLayoutDesc ld;
    ld.entries.push_back({0, BindingType::UniformBuffer, 1, ShaderStage::Vertex, true});
    auto layout = device.CreateResourceSetLayout(ld, Make());
    auto frame = device.BeginFrame(chain);
    const std::array<std::byte, 13> data{};
    auto slice = device.WriteDynamicBuffer(frame, data, 256, MakeDynamic());
    EXPECT_NO_THROW(device.ValidateDynamicSlice(frame, slice));
    auto forged = frame;
    ++forged.owner;
    EXPECT_THROW(device.ValidateDynamicSlice(forged, slice), RhiValidationError);
    auto expanded = slice;
    expanded.view.size = 16;
    EXPECT_THROW(device.ValidateDynamicSlice(frame, expanded), RhiValidationError);
    EXPECT_THROW(device.Destroy(slice.view.buffer), RhiValidationError);
    ResourceSetDesc sd;
    sd.layout = layout;
    ResourceBinding binding;
    binding.buffer = slice.view;
    sd.bindings.push_back(binding);
    EXPECT_THROW(device.CreateResourceSet(sd, Make()), RhiValidationError);
    device.Destroy(layout);
    device.EndFrame(frame, chain);
    EXPECT_THROW(device.ValidateAlive(slice.view.buffer), RhiValidationError);
    auto next = device.BeginFrame(chain);
    EXPECT_THROW(device.ValidateDynamicSlice(next, slice), RhiValidationError);
    EXPECT_THROW(device.EndFrame(frame, chain), RhiValidationError);
    device.EndFrame(next, chain);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, RecycleLaneCannotBeReusedUntilBackendCompletion)
{
    auto chain = Chain();
    for (int i = 0; i < 3; ++i)
    {
        auto f = device.BeginFrame(chain);
        device.EndFrame(f, chain);
    }
    EXPECT_THROW(device.BeginFrame(chain), RhiValidationError);
    device.Collect(1);
    auto f = device.BeginFrame(chain);
    EXPECT_EQ(f.serial, 4U);
    device.EndFrame(f, chain);
    EXPECT_THROW(device.Collect(0), RhiValidationError);
    EXPECT_THROW(device.Collect(5), RhiValidationError);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, ResizeInvalidatesOldBackBuffersAndZeroSizeSuspendsWithoutWait)
{
    auto chain = Chain();
    auto frame = device.BeginFrame(chain);
    auto old = frame.backBuffer;
    EXPECT_THROW(device.Destroy(old), RhiValidationError);
    EXPECT_THROW(
        device.ResizeBackBuffers(chain, {32, 32}, [] { return std::vector<DeviceLifetime::BackBufferCandidate>{}; }),
        RhiValidationError);
    device.EndFrame(frame, chain);
    device.ResizeBackBuffers(chain, {0, 0}, {});
    EXPECT_EQ(device.BeginFrame(chain).serial, 0U);
    EXPECT_THROW(
        device.ResizeBackBuffers(chain, {32, 32}, [] { return std::vector<DeviceLifetime::BackBufferCandidate>{}; }),
        RhiValidationError);
    device.Collect(frame.serial);
    device.ResizeBackBuffers(chain, {32, 32}, [&] { return BackBuffers({32, 32}); });
    EXPECT_THROW(device.ValidateAlive(old), RhiValidationError);
    auto next = device.BeginFrame(chain);
    EXPECT_NE(next.backBuffer, old);
    device.EndFrame(next, chain);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, FailedResizeLeavesSuspendedAndOldHandlesInvalid)
{
    auto chain = Chain();
    auto f = device.BeginFrame(chain);
    auto old = f.backBuffer;
    device.EndFrame(f, chain);
    device.Collect(f.serial);
    EXPECT_THROW(device.ResizeBackBuffers(chain, {32, 32}, []() -> std::vector<DeviceLifetime::BackBufferCandidate>
                                          { throw std::runtime_error("resize failure"); }),
                 std::runtime_error);
    EXPECT_THROW(device.ValidateAlive(old), RhiValidationError);
    EXPECT_EQ(device.BeginFrame(chain).serial, 0U);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, TenThousandFramesReachBoundedRegistryAndRetirementPlateau)
{
    auto chain = Chain();
    const std::array<std::byte, 64> data{};
    std::size_t plateau = 0;
    for (std::uint64_t i = 1; i <= 10000; ++i)
    {
        if (i > 3)
        {
            device.Collect(i - 3);
        }
        auto frame = device.BeginFrame(chain);
        auto slice = device.WriteDynamicBuffer(frame, data, 256, MakeDynamic());
        EXPECT_NO_THROW(device.ValidateDynamicSlice(frame, slice));
        device.EndFrame(frame, chain);
        auto stats = device.Stats();
        EXPECT_LE(stats.retiring, 3U);
        EXPECT_EQ(stats.alive, 4U);
        if (i == 100)
        {
            plateau = stats.slots;
        }
        if (i > 100)
        {
            EXPECT_EQ(stats.slots, plateau);
        }
    }
    EXPECT_EQ(plateau, 7U);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, DiagnosticsDistinguishRecordingSubmittedAndCompleted)
{
    auto chain = Chain();
    auto frame = device.BeginFrame(chain);
    EXPECT_EQ(device.Diagnostics().activeFrameSerial, frame.serial);
    EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, 0U);
    device.EndFrame(frame, chain);
    EXPECT_EQ(device.Diagnostics().activeFrameSerial, 0U);
    EXPECT_EQ(device.Diagnostics().lastSubmittedSerial, frame.serial);
    EXPECT_EQ(device.Diagnostics().completedSerial, 0U);
    Finish(chain);
    EXPECT_EQ(device.Diagnostics().aliveObjects, 0U);
    EXPECT_EQ(device.Diagnostics().retiringObjects, 0U);
}
TEST_F(DeviceLifetimeTest, ShutdownRequiresOwnersAndSubmittedWorkToBeGone)
{
    auto chain = Chain();
    EXPECT_THROW(device.CheckShutdown(), RhiValidationError);
    auto frame = device.BeginFrame(chain);
    EXPECT_THROW(device.CheckShutdown(), RhiValidationError);
    device.EndFrame(frame, chain);
    EXPECT_THROW(device.Destroy(chain), RhiValidationError);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, ResourceBindingRejectsWrongUsageAndDuplicatesBeforeBackendCreation)
{
    auto layout = Layout();
    auto d = Texture();
    d.usage = TextureUsage::ColorAttachment;
    auto texture = device.CreateTexture(d, Make());
    bool called = false;
    EXPECT_THROW(Set(layout, texture,
                     [&]
                     {
                         called = true;
                         return Make()();
                     }),
                 RhiValidationError);
    EXPECT_FALSE(called);
    ResourceSetLayoutDesc duplicate;
    duplicate.entries = {{0}, {0}};
    EXPECT_THROW(device.CreateResourceSetLayout(duplicate, Make()), RhiValidationError);
    device.Destroy(texture);
    device.Destroy(layout);
    device.Collect(0);
    EXPECT_EQ(live, 0);
}
TEST(DeferredRegistry, LastUseSurvivesNonAssignablePayloadAndOutOfOrderRetirements)
{
    struct Value final
    {
        explicit Value(int value) : storage(std::make_unique<int>(value))
        {
        }
        Value(Value&&) noexcept = default;
        Value& operator=(Value&&) = delete;
        std::unique_ptr<int> storage;
    };
    DeferredRegistry<BufferHandle, Value> registry("Buffer", "test");
    auto a = registry.Create(Value{42});
    auto b = registry.Create(Value{7});
    registry.MarkUsed(a, 3);
    registry.MarkUsed(a, 2);
    registry.MarkUsed(b, 1);
    EXPECT_EQ(registry.LastUse(a), 3U);
    registry.Destroy(a);
    registry.Destroy(b);
    registry.Collect(1);
    EXPECT_EQ(registry.Stats().retiring, 1U);
    registry.Collect(2);
    EXPECT_EQ(registry.Stats().retiring, 1U);
    registry.Collect(3);
    EXPECT_EQ(registry.Stats().retiring, 0U);
    auto reused = registry.Create(Value{9});
    EXPECT_EQ(*registry.Get(reused).storage, 9);
    EXPECT_THROW(registry.Get(a), RhiValidationError);
    registry.Destroy(reused);
    registry.Collect(3);
}
TEST_F(DeviceLifetimeTest, UploadBeforeFirstFrameIsProtectedAndFailureDoesNotPublishSubmission)
{
    BufferDesc desc{64, BufferUsage::Vertex | BufferUsage::CopyDestination, MemoryDomain::GpuOnly, "upload"};
    auto buffer = device.CreateBuffer(desc, Make());
    const std::array<std::byte, 8> bytes{};
    EXPECT_THROW(device.UploadBuffer(buffer, 0, bytes,
                                     [](ResourcePayload&, std::uint64_t, std::span<const std::byte>, std::uint64_t)
                                     { throw std::runtime_error("failed before submission"); }),
                 std::runtime_error);
    EXPECT_EQ(device.LastSubmitted(), 0U);
    EXPECT_NO_THROW(device.ValidateAlive(buffer));
    std::vector<std::byte> copied;
    auto serial = device.UploadBuffer(
        buffer, 4, bytes,
        [&](ResourcePayload&, std::uint64_t offset, std::span<const std::byte> data, std::uint64_t ticket)
        {
            EXPECT_EQ(offset, 4U);
            EXPECT_EQ(ticket, 1U);
            copied.assign(data.begin(), data.end());
            EXPECT_THROW(device.Destroy(buffer), RhiValidationError);
            EXPECT_THROW(device.Collect(0), RhiValidationError);
        });
    EXPECT_EQ(serial, 1U);
    EXPECT_EQ(copied.size(), bytes.size());
    device.Destroy(buffer);
    device.Collect(0);
    EXPECT_EQ(live, 1);
    device.Collect(serial);
    EXPECT_EQ(live, 0);
    EXPECT_NO_THROW(device.CheckShutdown());
}
TEST_F(DeviceLifetimeTest, TextureUploadIsTrackedAndLiveRevisionCannotBeOverwritten)
{
    auto texture = device.CreateTexture(Texture({2, 2}), Make());
    const std::array<std::byte, 16> bytes{};
    const TextureSubresourceData sub{bytes, 8, 16};
    auto serial =
        device.UploadTexture(texture, std::span(&sub, 1),
                             [&](ResourcePayload&, std::span<const TextureSubresourceData> data, std::uint64_t ticket)
                             {
                                 EXPECT_EQ(data[0].bytes.size(), 16U);
                                 EXPECT_EQ(ticket, 1U);
                             });
    EXPECT_EQ(serial, 1U);
    EXPECT_THROW(device.UploadTexture(texture, std::span(&sub, 1), [](auto&, auto, auto) {}), RhiValidationError);
    device.Collect(serial);
    auto layout = Layout();
    auto set = Set(layout, texture, Make());
    EXPECT_THROW(device.UploadTexture(texture, std::span(&sub, 1), [](auto&, auto, auto) {}), RhiValidationError);
    device.Destroy(set);
    device.Destroy(texture);
    device.Destroy(layout);
    device.Collect(serial);
    EXPECT_EQ(live, 0);
}
TEST_F(DeviceLifetimeTest, EndFrameRejectsAnotherSwapChainWithoutLosingActiveFrame)
{
    auto chain = Chain();
    auto other = Chain();
    auto frame = device.BeginFrame(chain);
    EXPECT_THROW(device.EndFrame(frame, other), RhiValidationError);
    EXPECT_EQ(device.Diagnostics().activeFrameSerial, frame.serial);
    device.EndFrame(frame, chain);
    device.Collect(frame.serial);
    device.Destroy(other);
    Finish(chain);
}
TEST_F(DeviceLifetimeTest, InvalidBackBufferCandidatesAreReleasedWithoutLeakingHandles)
{
    auto chain = Chain();
    EXPECT_THROW(device.ResizeBackBuffers(chain, {32, 32},
                                          [&]
                                          {
                                              auto candidates = BackBuffers({32, 32});
                                              candidates[1].desc.extent = {};
                                              return candidates;
                                          }),
                 RhiValidationError);
    EXPECT_EQ(device.Stats().alive, 1U);
    EXPECT_EQ(device.Stats().retiring, 0U);
    EXPECT_EQ(live, 1);
    EXPECT_EQ(device.BeginFrame(chain).serial, 0U);
    Finish(chain);
}
TEST(DeferredRegistry, ImmediateReleaseRejectsIncompleteResources)
{
    DeferredRegistry<BufferHandle, std::unique_ptr<int>> registry("Buffer", "d3d11");
    auto handle = registry.Create(std::make_unique<int>(42));
    registry.MarkUsed(handle, 1);
    try
    {
        registry.DestroyCompleted(handle);
        FAIL();
    }
    catch (const RhiValidationError& error)
    {
        EXPECT_EQ(error.Error().code, RhiErrorCode::InvalidState);
        EXPECT_EQ(error.Error().backend, "d3d11");
    }
    EXPECT_EQ(*registry.Get(handle), 42);
    registry.Collect(1);
    registry.DestroyCompleted(handle);
    EXPECT_EQ(registry.Stats().alive, 0U);
    EXPECT_EQ(registry.Stats().retiring, 0U);
}

TEST_F(DeviceLifetimeTest, NativeAcquireIndexIsIndependentOfUploadSerialAndRecycleLane)
{
    const auto chain = Chain();
    const auto buffers = device.BackBuffers(chain);
    const auto buffer = device.CreateBuffer({16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "initial vertex"}, Make());
    const std::array<std::byte, 16> initial{};
    device.InitializeBuffer(buffer, initial, [](ResourcePayload&, auto, auto, auto) {});
    device.Collect(1);
    const auto frame = device.BeginFrame(chain, 0);
    EXPECT_EQ(frame.serial, 2);
    EXPECT_EQ(frame.recycleLane, 1);
    EXPECT_EQ(frame.backBuffer, buffers[0]);
    device.EndFrame(frame, chain);
    EXPECT_EQ(device.RequiredFrameCompletion(chain, 0), 2);
    EXPECT_EQ(device.RequiredFrameCompletion(chain, 2), 0);
    EXPECT_THROW(device.BeginFrame(chain, 0), RhiValidationError);
    EXPECT_EQ(device.Diagnostics().activeFrameSerial, 0);
    const auto second = device.BeginFrame(chain, 2);
    EXPECT_EQ(second.backBuffer, buffers[2]);
    device.EndFrame(second, chain);
    device.Collect(second.serial);
    device.Destroy(buffer);
    device.Destroy(chain);
    device.Collect(second.serial);
    device.CheckShutdown();
}
TEST_F(DeviceLifetimeTest, InvalidNativeAcquireDoesNotIssueAFrame)
{
    const auto chain = Chain();
    EXPECT_THROW(device.RequiredFrameCompletion(chain, 3), RhiValidationError);
    EXPECT_THROW(device.BeginFrame(chain, 100), RhiValidationError);
    EXPECT_EQ(device.Diagnostics().activeFrameSerial, 0);
    const auto frame = device.BeginFrame(chain, 2);
    EXPECT_EQ(frame.serial, 1);
    device.EndFrame(frame, chain);
    device.Collect(frame.serial);
    device.Destroy(chain);
    device.CheckShutdown();
}
TEST_F(DeviceLifetimeTest, InitialDataSubmissionIsTrackedWithoutPublicCopyDestinationUsage)
{
    const auto buffer = device.CreateBuffer({16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "initial vertex"}, Make());
    const std::array<std::byte, 16> bytes{};
    EXPECT_THROW(device.InitializeBuffer(buffer, bytes, [](ResourcePayload&, auto, auto, auto)
                                         { throw std::runtime_error("before submit"); }),
                 std::runtime_error);
    EXPECT_EQ(device.LastSubmitted(), 0);
    EXPECT_EQ(device.LastUse(buffer), 0);
    EXPECT_THROW(device.UploadBuffer(buffer, 0, bytes, [](ResourcePayload&, auto, auto, auto) {}), RhiValidationError);
    ResourcePayload* observed = nullptr;
    const auto serial = device.InitializeBuffer(buffer, bytes,
                                                [&](ResourcePayload& payload, auto offset, auto input, auto value)
                                                {
                                                    observed = &payload;
                                                    EXPECT_EQ(offset, 0);
                                                    EXPECT_EQ(input.size(), bytes.size());
                                                    EXPECT_EQ(value, 1);
                                                });
    EXPECT_EQ(observed, &device.Payload(buffer));
    EXPECT_EQ(serial, 1);
    EXPECT_THROW(device.InitializeBuffer(buffer, bytes, [](ResourcePayload&, auto, auto, auto) {}), RhiValidationError);
    device.Destroy(buffer);
    EXPECT_EQ(device.Stats().retiring, 1);
    EXPECT_EQ(live, 1);
    device.Collect(serial);
    EXPECT_EQ(live, 0);
    device.CheckShutdown();
}

TEST_F(DeviceLifetimeTest, OtherBackBufferCannotAliasTheAcquiredNativeSurface)
{
    const auto chain = Chain();
    const auto buffers = device.BackBuffers(chain);
    const auto frame = device.BeginFrame(chain, 1);
    EXPECT_THROW(device.Use(frame, buffers[0]), RhiValidationError);
    EXPECT_THROW(device.Use(frame, buffers[2]), RhiValidationError);
    EXPECT_EQ(device.LastUse(buffers[0]), 0U);
    EXPECT_EQ(device.LastUse(buffers[2]), 0U);
    EXPECT_NO_THROW(device.Use(frame, buffers[1]));
    device.EndFrame(frame, chain);
    device.Collect(frame.serial);
    Finish(chain);
}
// M6 审计 C5 回归：Retire 的 swap chain 分支此前用魔数 9；变体顺序一旦改变，
// 该分支会静默失效（swap chain 不再等待空闲边界）。命名常量与变体顺序必须一致。
TEST(ResourceIdentityOrder, SwapChainIsTheTenthAlternativeMatchingNamedConstant)
{
    const ResourceIdentity identity = SwapChainHandle{};
    EXPECT_EQ(identity.index(), kSwapChainIdentityIndex);
    EXPECT_EQ(kSwapChainIdentityIndex, 9U);
    EXPECT_EQ(std::variant_size_v<ResourceIdentity>, 10U);
}
} // namespace
