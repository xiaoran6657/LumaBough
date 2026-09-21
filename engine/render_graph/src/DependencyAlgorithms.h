#pragma once
#include <MiniEngine/RenderGraph/RenderGraphTypes.h>
#include <span>
#include <string_view>
#include <vector>

namespace MiniEngine::RenderGraph::Detail
{
// 扁平邻接表一次 reserve/resize，不为每条 edge 分配链表节点。
struct DependencyIndex final
{
    std::vector<std::uint32_t> outOffsets, inOffsets, outEdges, inEdges;
    void Build(std::uint32_t passCount, std::span<const DependencyEdge> edges);
    void Clear();
};
std::vector<std::uint32_t> StableTopologicalSort(const DependencyIndex& index, std::span<const DependencyEdge> edges,
                                                 std::span<const std::uint8_t> live,
                                                 std::span<const std::string_view> passNames,
                                                 std::span<const std::string_view> resourceNames);
} // namespace MiniEngine::RenderGraph::Detail
