#include <MiniEngine/Rhi/RhiShaderPackage.h>
#include <algorithm>
#include <tuple>
namespace MiniEngine::Rhi
{
namespace
{
ShaderDesc Describe(const ShaderPackage& package, const ShaderVariant& v)
{
    return {v.stage, v.bytecode, v.semanticHash, v.entryPoint, package.assetId, v.sourceHash, v.manifest};
}
ShaderInterface Canonical(ShaderInterface value)
{
    std::sort(value.bindings.begin(), value.bindings.end(),
              [](const auto& a, const auto& b) { return std::tie(a.set, a.binding) < std::tie(b.set, b.binding); });
    std::sort(value.vertexInputs.begin(), value.vertexInputs.end(), [](const auto& a, const auto& b)
              { return std::tie(a.semantic, a.semanticIndex) < std::tie(b.semantic, b.semanticIndex); });
    return value;
}
[[noreturn]] void Block(const ShaderPackage& package, const char* message)
{
    throw RhiValidationError(
        {RhiErrorCode::Unsupported, "ShaderPackageParity", "ShaderAsset", package.assetId, "", message});
}
} // namespace
std::uint64_t ShaderBytecodeHash(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto b : bytes)
    {
        hash ^= std::to_integer<unsigned char>(b);
        hash *= 1099511628211ULL;
    }
    return hash;
}
void ValidateShaderPackage(const ShaderPackage& package)
{
    if (package.assetId.empty())
        Block(package, "asset identity is missing");
    bool seen[2]{};
    for (const auto& v : package.variants)
    {
        if (v.backend != RhiBackend::D3D11 && v.backend != RhiBackend::D3D12)
            Block(package, "unknown backend variant");
        const auto index = static_cast<unsigned>(v.backend);
        if (seen[index])
            Block(package, "duplicate backend variant");
        seen[index] = true;
        ValidateShaderDesc(Describe(package, v));
        if (ShaderBytecodeHash(v.bytecode) != v.bytecodeHash)
            Block(package, "bytecode integrity mismatch");
    }
    const auto& a = package.variants[0];
    const auto& b = package.variants[1];
    if (a.stage != b.stage || a.entryPoint != b.entryPoint || a.semanticHash != b.semanticHash ||
        Canonical(a.manifest) != Canonical(b.manifest))
        Block(package, "backend semantic revision or reflected interface differs");
}
RhiCapabilityAssessment AssessShaderPackage(const ShaderPackage& package)
{
    try
    {
        ValidateShaderPackage(package);
        return {RhiCapabilityStatus::Ready, std::nullopt};
    }
    catch (const RhiValidationError& error)
    {
        return {RhiCapabilityStatus::Blocked, error.Error()};
    }
}
ShaderDesc SelectShader(const ShaderPackage& package, RhiBackend backend, ShaderStage stage)
{
    ValidateShaderPackage(package);
    auto found = std::find_if(package.variants.begin(), package.variants.end(),
                              [&](const auto& v) { return v.backend == backend && v.stage == stage; });
    if (found == package.variants.end())
        Block(package, "requested backend/stage does not exist");
    return Describe(package, *found);
}
} // namespace MiniEngine::Rhi
