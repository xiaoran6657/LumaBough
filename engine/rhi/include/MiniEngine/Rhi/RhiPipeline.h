#pragma once
#include <MiniEngine/Rhi/RhiValidation.h>

namespace MiniEngine::Rhi
{
// 单个 shader 的合法性与 reflection 逻辑接口检查；不在运行时编译/反射 bytecode。
void ValidateShaderDesc(const ShaderDesc& shader);
void ValidateResourceSetLayout(const ResourceSetLayoutDesc& layout);
// layouts 按 set 顺序提供 resolved 值，key 不包含任何 registry handle 或 debug name。
void ValidateGraphicsPipeline(const GraphicsPipelineDesc& pipeline, const ShaderDesc& vertex, const ShaderDesc* pixel,
                              std::span<const ResourceSetLayoutDesc> layouts, const RhiCapabilities& capabilities);
std::string SemanticKey(const ResourceSetLayoutDesc& layout);
std::string PipelineLayoutKey(std::span<const ResourceSetLayoutDesc> layouts);
std::string PipelineSemanticKey(const GraphicsPipelineDesc& pipeline, const ShaderDesc& vertex, const ShaderDesc* pixel,
                                std::span<const ResourceSetLayoutDesc> layouts);
std::uint64_t SemanticHash(const ResourceSetLayoutDesc& layout);
std::uint64_t HashSemanticKey(std::string_view key);
std::uint32_t VertexFormatBytes(VertexFormat format);
// 相同 hash 仅作快速筛选；兼容性比较完整 canonical key，避免 hash 碰撞误留旧 set。
bool LayoutCompatible(const ResourceSetLayoutDesc& left, const ResourceSetLayoutDesc& right);
} // namespace MiniEngine::Rhi
