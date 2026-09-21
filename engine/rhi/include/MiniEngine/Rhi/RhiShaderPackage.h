#pragma once
#include <MiniEngine/Rhi/RhiPipeline.h>

namespace MiniEngine::Rhi
{
// 解码后的离线资产：路径/编译器/native reflection 留在 asset loader，运行时只持有值。
// bytecodeHash 为传输完整性用的 FNV-1a；source/semanticHash 是离线 SHA-256，不以 FNV 充当密码学验证。
struct ShaderVariant final
{
    RhiBackend backend = RhiBackend::D3D11;
    ShaderStage stage = ShaderStage::Vertex;
    std::vector<std::byte> bytecode;
    std::uint64_t bytecodeHash = 0;
    std::string sourceHash;
    std::string semanticHash;
    std::string entryPoint;
    ShaderInterface manifest;
};
struct ShaderPackage final
{
    std::string assetId;
    std::array<ShaderVariant, 2> variants;
};
std::uint64_t ShaderBytecodeHash(std::span<const std::byte> bytes);
// 必须同时验证两个变体；语义或接口不同返回 Blocked，不允许只选一边继续渲染。
RhiCapabilityAssessment AssessShaderPackage(const ShaderPackage& package);
void ValidateShaderPackage(const ShaderPackage& package);
// 返回的 bytecode 借用 package；调用方须让 package 活过 CreateShader 的消费调用。
ShaderDesc SelectShader(const ShaderPackage& package, RhiBackend backend, ShaderStage stage);
} // namespace MiniEngine::Rhi
