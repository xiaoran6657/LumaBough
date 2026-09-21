#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
struct ParityResolution final
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    auto operator<=>(const ParityResolution&) const = default;
};

struct ParityMetadata final
{
    std::string assetManifestHash;
    std::string shaderSemanticHash;
    std::string environmentHash;
    std::string camera;
    std::string light;
    std::string exposure;
    std::string iblProfile;
    std::string shadow;
    std::uint64_t fixedTick = 0U;
    std::string visibleSequenceHash;
    ParityResolution resolution;
    std::string debugView;
    // 诊断字段用于解释环境差异，不参与同机跨后端可比性判定。
    std::string backend;
    std::string driver;
    std::string compiler;
};

struct ParityComparison final
{
    bool comparable = false;
    std::vector<std::string> mismatchedFieldNames;
};

[[nodiscard]] inline ParityComparison CompareMetadata(const ParityMetadata& left, const ParityMetadata& right)
{
    ParityComparison result;
    const auto mismatch = [&result](const char* name) { result.mismatchedFieldNames.emplace_back(name); };
    if (left.assetManifestHash.empty() || right.assetManifestHash.empty() ||
        left.assetManifestHash != right.assetManifestHash)
        mismatch("assetManifestHash");
    if (left.shaderSemanticHash.empty() || right.shaderSemanticHash.empty() ||
        left.shaderSemanticHash != right.shaderSemanticHash)
        mismatch("shaderSemanticHash");
    if (left.environmentHash.empty() || right.environmentHash.empty() || left.environmentHash != right.environmentHash)
        mismatch("environmentHash");
    if (left.camera != right.camera)
        mismatch("camera");
    if (left.light != right.light)
        mismatch("light");
    if (left.exposure != right.exposure)
        mismatch("exposure");
    if (left.iblProfile != right.iblProfile)
        mismatch("iblProfile");
    if (left.shadow != right.shadow)
        mismatch("shadow");
    if (left.fixedTick != right.fixedTick)
        mismatch("fixedTick");
    if (left.visibleSequenceHash != right.visibleSequenceHash)
        mismatch("visibleSequenceHash");
    if (left.resolution != right.resolution)
        mismatch("resolution");
    if (left.debugView != right.debugView)
        mismatch("debugView");
    result.comparable = result.mismatchedFieldNames.empty();
    return result;
}
} // namespace MiniEngine::Rhi::D3D12