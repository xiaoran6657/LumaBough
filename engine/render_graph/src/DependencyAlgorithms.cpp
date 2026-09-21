#include "DependencyAlgorithms.h"
#include <algorithm>
#include <functional>
#include <numeric>

namespace MiniEngine::RenderGraph::Detail
{
void DependencyIndex::Clear()
{
    outOffsets.clear();
    inOffsets.clear();
    outEdges.clear();
    inEdges.clear();
}
void DependencyIndex::Build(std::uint32_t passCount, std::span<const DependencyEdge> edges)
{
    outOffsets.assign(static_cast<std::size_t>(passCount) + 1, 0);
    inOffsets.assign(static_cast<std::size_t>(passCount) + 1, 0);
    outEdges.resize(edges.size());
    inEdges.resize(edges.size());
    for (const auto& edge : edges)
    {
        if (edge.from >= passCount || edge.to >= passCount)
        {
            GraphDiagnostic error;
            error.stage = CompileStage::CreateDependencies;
            error.phase = GraphPhase::Compiling;
            error.pass = edge.from;
            error.resource = edge.resource;
            error.version = edge.version;
            error.message = "dependency endpoint is outside declared passes";
            error.cycle = {edge};
            throw GraphCompileError(std::move(error));
        }
        ++outOffsets[edge.from + 1];
        ++inOffsets[edge.to + 1];
    }
    std::partial_sum(outOffsets.begin(), outOffsets.end(), outOffsets.begin());
    std::partial_sum(inOffsets.begin(), inOffsets.end(), inOffsets.begin());
    auto outCursor = outOffsets, inCursor = inOffsets;
    for (std::uint32_t i = 0; i < edges.size(); ++i)
    {
        outEdges[outCursor[edges[i].from]++] = i;
        inEdges[inCursor[edges[i].to]++] = i;
    }
}
namespace
{
std::string NodeName(std::span<const std::string_view> names, std::uint32_t id, const char* prefix)
{
    return id < names.size() ? std::string(names[id]) : std::string(prefix) + std::to_string(id);
}
[[noreturn]] void ThrowCycle(const DependencyIndex& index, std::span<const DependencyEdge> edges,
                             std::span<const std::uint8_t> live, std::span<const std::uint32_t> indegree,
                             std::span<const std::string_view> passNames,
                             std::span<const std::string_view> resourceNames)
{
    // 仅在失败路径做逐起点 BFS；选择最短可用闭环，等长按稳定 edge 序确定。
    // 内部 fixture 也可传入乱序边；失败路径按语义排序邻接表，稳定选择同起点的等长闭环。
    auto sortedOutEdges = index.outEdges;
    for (std::size_t p = 0; p < live.size(); ++p)
        std::sort(sortedOutEdges.begin() + index.outOffsets[p], sortedOutEdges.begin() + index.outOffsets[p + 1],
                  [&](std::uint32_t left, std::uint32_t right) { return edges[left] < edges[right]; });
    std::vector<DependencyEdge> best;
    std::vector<std::uint32_t> parent(live.size()), queue;
    queue.reserve(live.size());
    for (std::uint32_t start = 0; start < live.size(); ++start)
    {
        if (!live[start] || indegree[start] == 0)
            continue;
        std::fill(parent.begin(), parent.end(), kInvalidGraphIndex);
        queue.clear();
        queue.push_back(start);
        parent[start] = static_cast<std::uint32_t>(edges.size());
        bool found = false;
        for (std::size_t q = 0; q < queue.size() && !found; ++q)
        {
            const auto from = queue[q];
            for (auto entry = index.outOffsets[from]; entry < index.outOffsets[from + 1]; ++entry)
            {
                const auto edgeId = sortedOutEdges[entry];
                const auto& edge = edges[edgeId];
                if (!live[edge.to] || indegree[edge.to] == 0)
                    continue;
                if (edge.to == start)
                {
                    std::vector<DependencyEdge> candidate{edge};
                    auto cursor = from;
                    while (cursor != start)
                    {
                        const auto& previous = edges[parent[cursor]];
                        candidate.push_back(previous);
                        cursor = previous.from;
                    }
                    std::reverse(candidate.begin(), candidate.end());
                    if (best.empty() || candidate.size() < best.size() ||
                        (candidate.size() == best.size() && candidate < best))
                        best = std::move(candidate);
                    found = true;
                    break;
                }
                if (parent[edge.to] == kInvalidGraphIndex)
                {
                    parent[edge.to] = edgeId;
                    queue.push_back(edge.to);
                }
            }
        }
    }
    GraphDiagnostic error;
    error.code = GraphErrorCode::Cycle;
    error.phase = GraphPhase::Compiling;
    error.stage = CompileStage::TopologicalSort;
    error.cycle = best;
    error.message = "dependency cycle";
    if (!best.empty())
    {
        error.pass = best.front().from;
        error.resource = best.front().resource;
        error.version = best.front().version;
        error.passName = NodeName(passNames, error.pass, "Pass");
        error.resourceName = NodeName(resourceNames, error.resource, "Resource");
        error.message += ": " + error.passName;
        for (const auto& edge : best)
            error.message += " -> " + NodeName(resourceNames, edge.resource, "Resource") + ".v" +
                             std::to_string(edge.version) + " -> " + NodeName(passNames, edge.to, "Pass");
    }
    throw GraphCycleError(std::move(error));
}
} // namespace
std::vector<std::uint32_t> StableTopologicalSort(const DependencyIndex& index, std::span<const DependencyEdge> edges,
                                                 std::span<const std::uint8_t> live,
                                                 std::span<const std::string_view> passNames,
                                                 std::span<const std::string_view> resourceNames)
{
    if (index.outOffsets.size() != live.size() + 1 || index.inOffsets.size() != live.size() + 1)
        throw GraphCompileError("dependency index/live mask size mismatch");
    std::vector<std::uint32_t> indegree(live.size(), 0), ready, order;
    ready.reserve(live.size());
    order.reserve(live.size());
    std::size_t expected = 0;
    for (const auto& edge : edges)
        if (live[edge.from] && live[edge.to])
            ++indegree[edge.to];
    for (std::uint32_t pass = 0; pass < live.size(); ++pass)
    {
        if (!live[pass])
            continue;
        ++expected;
        if (indegree[pass] == 0)
            ready.push_back(pass);
    }
    std::make_heap(ready.begin(), ready.end(), std::greater<>{});
    while (!ready.empty())
    {
        std::pop_heap(ready.begin(), ready.end(), std::greater<>{});
        const auto pass = ready.back();
        ready.pop_back();
        order.push_back(pass);
        for (auto entry = index.outOffsets[pass]; entry < index.outOffsets[pass + 1]; ++entry)
        {
            const auto& edge = edges[index.outEdges[entry]];
            if (live[edge.to] && --indegree[edge.to] == 0)
            {
                ready.push_back(edge.to);
                std::push_heap(ready.begin(), ready.end(), std::greater<>{});
            }
        }
    }
    if (order.size() != expected)
        ThrowCycle(index, edges, live, indegree, passNames, resourceNames);
    return order;
}
} // namespace MiniEngine::RenderGraph::Detail
