#include "D3D12Parity.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace D12 = MiniEngine::Rhi::D3D12;

namespace
{
D12::ParityMetadata Fixture()
{
    D12::ParityMetadata metadata;
    metadata.assetManifestHash = "asset";
    metadata.shaderSemanticHash = "shader";
    metadata.environmentHash = "environment";
    metadata.camera = "camera-v1";
    metadata.light = "light-v1";
    metadata.exposure = "exposure-1";
    metadata.iblProfile = "ibl-v1";
    metadata.shadow = "shadow-v1";
    metadata.fixedTick = 120U;
    metadata.visibleSequenceHash = "visible";
    metadata.resolution = {1280U, 720U};
    metadata.debugView = "Lit";
    metadata.backend = "d3d11";
    metadata.driver = "driver-a";
    metadata.compiler = "fxc";
    return metadata;
}

void ExpectBlockedForField(const std::string& fieldName)
{
    D12::ParityMetadata left = Fixture();
    D12::ParityMetadata right = left;
    if (fieldName == "assetManifestHash")
        right.assetManifestHash = "changed";
    else if (fieldName == "shaderSemanticHash")
        right.shaderSemanticHash = "changed";
    else if (fieldName == "environmentHash")
        right.environmentHash = "changed";
    else if (fieldName == "camera")
        right.camera = "camera-v2";
    else if (fieldName == "light")
        right.light = "light-v2";
    else if (fieldName == "exposure")
        right.exposure = "exposure-2";
    else if (fieldName == "iblProfile")
        right.iblProfile = "ibl-v2";
    else if (fieldName == "shadow")
        right.shadow = "shadow-v2";
    else if (fieldName == "fixedTick")
        ++right.fixedTick;
    else if (fieldName == "visibleSequenceHash")
        right.visibleSequenceHash = "changed";
    else if (fieldName == "resolution")
        ++right.resolution.width;
    else if (fieldName == "debugView")
        right.debugView = "Normal";

    const D12::ParityComparison result = D12::CompareMetadata(left, right);
    EXPECT_FALSE(result.comparable);
    EXPECT_EQ(result.mismatchedFieldNames, std::vector<std::string>{fieldName});
}
} // namespace

TEST(ParityMetadataTests, IdenticalSemanticMetadataIsComparable)
{
    const D12::ParityComparison result = D12::CompareMetadata(Fixture(), Fixture());
    EXPECT_TRUE(result.comparable);
    EXPECT_TRUE(result.mismatchedFieldNames.empty());
}

TEST(ParityMetadataTests, EachSemanticFieldChangeBlocksComparison)
{
    for (const char* field :
         {"assetManifestHash", "shaderSemanticHash", "environmentHash", "camera", "light", "exposure", "iblProfile",
          "shadow", "fixedTick", "visibleSequenceHash", "resolution", "debugView"})
    {
        ExpectBlockedForField(field);
    }
}

TEST(ParityMetadataTests, BackendDiagnosticsDoNotBlockComparison)
{
    D12::ParityMetadata right = Fixture();
    right.backend = "d3d12";
    right.driver = "driver-b";
    right.compiler = "dxc";
    const D12::ParityComparison result = D12::CompareMetadata(Fixture(), right);
    EXPECT_TRUE(result.comparable);
}

TEST(ParityMetadataTests, MissingRequiredHashBlocksEvenWhenBothAreEmpty)
{
    D12::ParityMetadata left = Fixture();
    D12::ParityMetadata right = Fixture();
    left.environmentHash.clear();
    right.environmentHash.clear();
    const D12::ParityComparison result = D12::CompareMetadata(left, right);
    EXPECT_FALSE(result.comparable);
    EXPECT_EQ(result.mismatchedFieldNames, std::vector<std::string>{"environmentHash"});
}