#include "ShaderRevisionTransaction.h"
#include <array>
#include <gtest/gtest.h>
#include <stdexcept>

using namespace MiniEngine::Rhi;
namespace
{
ShaderPackage Package()
{
    ShaderPackage package;
    package.assetId = "test.shader";
    for (std::size_t i = 0; i < 2; ++i)
    {
        auto& v = package.variants[i];
        v.backend = static_cast<RhiBackend>(i);
        v.stage = ShaderStage::Vertex;
        v.bytecode = {std::byte{static_cast<unsigned char>(i + 1)}, std::byte{2}};
        v.bytecodeHash = ShaderBytecodeHash(v.bytecode);
        v.sourceHash = std::string(64, i ? 'c' : 'a');
        v.semanticHash = std::string(64, 'b');
        v.entryPoint = "VSMain";
    }
    return package;
}
TEST(ShaderPackageContract, BackendSelectionSharesSemanticsWhileOwningDifferentBytes)
{
    auto package = Package();
    EXPECT_EQ(AssessShaderPackage(package).status, RhiCapabilityStatus::Ready);
    auto a = SelectShader(package, RhiBackend::D3D11, ShaderStage::Vertex);
    auto b = SelectShader(package, RhiBackend::D3D12, ShaderStage::Vertex);
    EXPECT_EQ(a.semanticHash, b.semanticHash);
    EXPECT_NE(a.sourceHash, b.sourceHash);
    EXPECT_NE(a.bytecode[0], b.bytecode[0]);
    EXPECT_EQ(a.bytecode.data(), package.variants[0].bytecode.data());
    EXPECT_THROW(SelectShader(package, RhiBackend::D3D11, ShaderStage::Pixel), RhiValidationError);
}
TEST(ShaderPackageContract, SemanticOrReflectionMismatchBlocksBothBackendSelections)
{
    auto package = Package();
    package.variants[1].semanticHash[0] = 'd';
    EXPECT_EQ(AssessShaderPackage(package).status, RhiCapabilityStatus::Blocked);
    EXPECT_THROW(SelectShader(package, RhiBackend::D3D11, ShaderStage::Vertex), RhiValidationError);
    package = Package();
    package.variants[1].manifest.vertexInputs.push_back({VertexSemantic::Position, VertexFormat::Float3, 0});
    EXPECT_EQ(AssessShaderPackage(package).status, RhiCapabilityStatus::Blocked);
}
TEST(ShaderPackageContract, CorruptBytesAndDuplicateBackendCannotProduceReadyPackage)
{
    auto package = Package();
    package.variants[1].bytecode[0] = std::byte{9};
    EXPECT_EQ(AssessShaderPackage(package).status, RhiCapabilityStatus::Blocked);
    package = Package();
    package.variants[1].backend = RhiBackend::D3D11;
    EXPECT_EQ(AssessShaderPackage(package).status, RhiCapabilityStatus::Blocked);
    package = Package();
    package.variants[1].sourceHash = "missing";
    EXPECT_EQ(AssessShaderPackage(package).status, RhiCapabilityStatus::Blocked);
}
struct OwnedRevision : ResourcePayload
{
    explicit OwnedRevision(int& count) : live(count)
    {
        ++live;
    }
    ~OwnedRevision() override
    {
        --live;
    }
    int& live;
};
RhiCapabilities Caps(RhiBackend backend)
{
    RhiCapabilities c;
    c.backend = backend;
    c.maxTextureDimension2D = 4096;
    c.maxColorAttachments = 4;
    c.formatSupport.fill(31);
    return c;
}
TEST(ShaderPackageContract, TransactionRejectsSwappedOrDuplicateBackendOwners)
{
    DeviceLifetime a(Caps(RhiBackend::D3D11)), b(Caps(RhiBackend::D3D12)), c(Caps(RhiBackend::D3D11));
    EXPECT_THROW((ShaderRevisionTransaction(b, a)), RhiValidationError);
    EXPECT_THROW((ShaderRevisionTransaction(a, c)), RhiValidationError);
    EXPECT_THROW((ShaderRevisionTransaction(a, a)), RhiValidationError);
}
class ShaderRevisionTransactionTest : public testing::Test
{
  protected:
    DeviceLifetime a{Caps(RhiBackend::D3D11)}, b{Caps(RhiBackend::D3D12)};
    ShaderRevisionTransaction transaction{a, b};
    int live = 0;
    int built = 0;
    std::array<ShaderPackage, 1> packages{Package()};
    std::array<SwapChainHandle, 2> chains{};
    std::array<FrameToken, 2> frames{};
    std::array<ShaderRevisionTransaction::Builder, 2> Builders()
    {
        ShaderRevisionTransaction::Builder make = [&](std::span<const ShaderPackage> values)
        {
            EXPECT_EQ(values.size(), 1U);
            ++built;
            return ShaderRevisionBackend{std::make_unique<OwnedRevision>(live),
                                         "complete shader/pipeline/set revision key"};
        };
        return {make, make};
    }
    ShaderRevisionTransaction::Smoke Smoke()
    {
        return [](RhiBackend, ResourcePayload&) { return true; };
    }
    void BeginFrames()
    {
        for (std::size_t i = 0; i < 2; ++i)
        {
            auto& device = i ? b : a;
            if (!chains[i])
            {
                chains[i] = device.CreateSwapChain({{16, 16}}, [] { return std::make_unique<ResourcePayload>(); });
                device.ResizeBackBuffers(chains[i], {16, 16},
                                         []
                                         {
                                             std::vector<DeviceLifetime::BackBufferCandidate> result;
                                             for (int j = 0; j < 3; ++j)
                                                 result.push_back({{TextureDimension::Texture2D,
                                                                    {16, 16},
                                                                    1,
                                                                    1,
                                                                    1,
                                                                    Format::Rgba8Unorm,
                                                                    TextureUsage::ColorAttachment,
                                                                    ""},
                                                                   std::make_unique<ResourcePayload>()});
                                             return result;
                                         });
            }
            frames[i] = device.BeginFrame(chains[i]);
            transaction.MarkUsed(static_cast<RhiBackend>(i), frames[i]);
        }
    }
    void EndFrames()
    {
        a.EndFrame(frames[0], chains[0]);
        b.EndFrame(frames[1], chains[1]);
    }
    void TearDown() override
    {
        for (std::size_t i = 0; i < 2; ++i)
        {
            auto& device = i ? b : a;
            if (device.Diagnostics().activeFrameSerial)
                device.EndFrame(frames[i], chains[i]);
            device.Collect(device.LastSubmitted());
        }
        transaction.Collect();
        transaction.Shutdown();
        EXPECT_EQ(live, 0);
        for (std::size_t i = 0; i < 2; ++i)
        {
            auto& device = i ? b : a;
            if (chains[i])
            {
                device.Destroy(chains[i]);
                device.Collect(device.LastSubmitted());
            }
            EXPECT_NO_THROW(device.CheckShutdown());
        }
    }
};
TEST_F(ShaderRevisionTransactionTest, CompileManifestAndSecondBackendCreationFailurePreserveEntireOldRevision)
{
    transaction.Reload("one", packages, Builders(), Smoke());
    EXPECT_EQ(live, 2);
    auto invalid = packages;
    invalid[0].variants[1].semanticHash[0] = 'd';
    const auto calls = built;
    EXPECT_THROW(transaction.Reload("bad-manifest", invalid, Builders(), Smoke()), RhiValidationError);
    EXPECT_EQ(calls, built);
    auto builders = Builders();
    builders[1] = [](auto) -> ShaderRevisionBackend
    { throw std::runtime_error("PSO or resource set creation failed"); };
    EXPECT_THROW(transaction.Reload("bad-native", packages, builders, Smoke()), std::runtime_error);
    EXPECT_EQ(transaction.CurrentRevision(), "one");
    EXPECT_EQ(transaction.RetiringCount(), 0U);
    EXPECT_EQ(live, 2);
}
TEST_F(ShaderRevisionTransactionTest, SecondBackendSmokeFailureAndDifferentPipelineKeysRollbackBothCandidates)
{
    transaction.Reload("one", packages, Builders(), Smoke());
    int smokes = 0;
    EXPECT_THROW(transaction.Reload("bad-smoke", packages, Builders(),
                                    [&](RhiBackend backend, ResourcePayload&)
                                    {
                                        ++smokes;
                                        return backend == RhiBackend::D3D11;
                                    }),
                 RhiValidationError);
    EXPECT_EQ(smokes, 2);
    EXPECT_EQ(live, 2);
    EXPECT_EQ(transaction.CurrentRevision(), "one");
    auto builders = Builders();
    builders[1] = [&](auto)
    { return ShaderRevisionBackend{std::make_unique<OwnedRevision>(live), "different pipeline"}; };
    EXPECT_THROW(transaction.Reload("bad-key", packages, builders, Smoke()), RhiValidationError);
    EXPECT_EQ(live, 2);
    EXPECT_EQ(transaction.CurrentRevision(), "one");
}
TEST_F(ShaderRevisionTransactionTest, CommitRetainsOldObjectsUntilBothBackendCompletions)
{
    transaction.Reload("one", packages, Builders(), Smoke());
    BeginFrames();
    EndFrames();
    transaction.Reload("two", packages, Builders(), Smoke());
    EXPECT_EQ(transaction.CurrentRevision(), "two");
    EXPECT_EQ(live, 4);
    EXPECT_EQ(transaction.RetiringCount(), 1U);
    transaction.Collect();
    EXPECT_EQ(live, 4);
    a.Collect(frames[0].serial);
    transaction.Collect();
    EXPECT_EQ(live, 4);
    EXPECT_THROW(transaction.Shutdown(), RhiValidationError);
    b.Collect(frames[1].serial);
    transaction.Collect();
    EXPECT_EQ(live, 2);
    EXPECT_EQ(transaction.RetiringCount(), 0U);
}
TEST_F(ShaderRevisionTransactionTest, FrameBoundaryAndReentrantMutationAreRejectedBeforeBuilder)
{
    transaction.Reload("one", packages, Builders(), Smoke());
    BeginFrames();
    const auto before = built;
    EXPECT_THROW(transaction.Reload("two", packages, Builders(), Smoke()), RhiValidationError);
    EXPECT_EQ(built, before);
    EndFrames();
    auto builders = Builders();
    builders[0] = [&](auto) -> ShaderRevisionBackend
    {
        transaction.Shutdown();
        return {};
    };
    EXPECT_THROW(transaction.Reload("reentry", packages, builders, Smoke()), RhiValidationError);
    EXPECT_EQ(transaction.CurrentRevision(), "one");
    EXPECT_EQ(live, 2);
}
TEST_F(ShaderRevisionTransactionTest, RepeatedRevisionCommitsReachBoundedDeferredPlatform)
{
    transaction.Reload("initial", packages, Builders(), Smoke());
    for (std::uint64_t i = 1; i <= 1000; ++i)
    {
        if (i > 3)
        {
            a.Collect(i - 3);
            b.Collect(i - 3);
            transaction.Collect();
        }
        BeginFrames();
        EndFrames();
        transaction.Reload(std::to_string(i), packages, Builders(), Smoke());
        EXPECT_LE(transaction.RetiringCount(), 3U);
        EXPECT_LE(live, 8);
    }
}
} // namespace
