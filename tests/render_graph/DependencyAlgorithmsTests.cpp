#include "DependencyAlgorithms.h"
#include <algorithm>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <vector>

namespace MiniEngine::RenderGraph::Detail
{
namespace
{
DependencyEdge Edge(std::uint32_t from, std::uint32_t to, std::uint32_t resource, std::uint32_t version, Hazard hazard,
                    bool contributes = true)
{
    return {from, to, resource, version, hazard, contributes};
}

std::vector<std::uint8_t> AllLive(std::size_t count)
{
    return std::vector<std::uint8_t>(count, 1);
}

} // namespace

TEST(DependencyAlgorithms, EmptyAcyclicGraphKeepsDeclarationOrder)
{
    DependencyIndex index;
    const std::vector<DependencyEdge> edges;
    const auto live = AllLive(4);
    const std::array<std::string_view, 4> passNames{"P0", "P1", "P2", "P3"};
    const std::array<std::string_view, 1> resourceNames{"R0"};
    index.Build(4, edges);

    EXPECT_EQ(StableTopologicalSort(index, edges, live, passNames, resourceNames),
              (std::vector<std::uint32_t>{0, 1, 2, 3}));
}

TEST(DependencyAlgorithms, ProducesTopologicalOrderWithoutUsingDeclarationOrder)
{
    DependencyIndex index;
    const std::vector<DependencyEdge> edges{
        Edge(2, 0, 0, 1, Hazard::ReadAfterWrite),
        Edge(0, 1, 0, 2, Hazard::WriteAfterRead, false),
    };
    const auto live = AllLive(4);
    const std::array<std::string_view, 4> passNames{"P0", "P1", "P2", "P3"};
    const std::array<std::string_view, 1> resourceNames{"R0"};
    index.Build(4, edges);

    EXPECT_EQ(StableTopologicalSort(index, edges, live, passNames, resourceNames),
              (std::vector<std::uint32_t>{2, 0, 1, 3}));
}

TEST(DependencyAlgorithms, UsesDeclarationIndexForStableReadyQueueTies)
{
    DependencyIndex index;
    // Pass 3 变为 ready 时，独立的 pass 4 已经 ready；此处由声明索引而非边插入顺序决定平局。
    const std::vector<DependencyEdge> edges{
        Edge(2, 3, 0, 1, Hazard::ReadAfterWrite),
    };
    const auto live = AllLive(5);
    const std::array<std::string_view, 5> passNames{"P0", "P1", "P2", "P3", "P4"};
    const std::array<std::string_view, 2> resourceNames{"R0", "R1"};
    index.Build(5, edges);

    EXPECT_EQ(StableTopologicalSort(index, edges, live, passNames, resourceNames),
              (std::vector<std::uint32_t>{0, 1, 2, 3, 4}));
}

TEST(DependencyAlgorithms, CountsParallelRawWarWawEdgesInIndegree)
{
    DependencyIndex index;
    // P0 发出的两条边是不同 hazard；P3 必须等 P0 和 P4 都处理完后才能解除阻塞。
    const std::vector<DependencyEdge> edges{
        Edge(0, 3, 0, 1, Hazard::ReadAfterWrite),
        Edge(4, 3, 1, 2, Hazard::WriteAfterRead, false),
        Edge(0, 3, 2, 3, Hazard::WriteAfterWrite, false),
    };
    const auto live = AllLive(5);
    const std::array<std::string_view, 5> passNames{"P0", "P1", "P2", "P3", "P4"};
    const std::array<std::string_view, 3> resourceNames{"R0", "R1", "R2"};
    index.Build(5, edges);

    EXPECT_EQ(StableTopologicalSort(index, edges, live, passNames, resourceNames),
              (std::vector<std::uint32_t>{0, 1, 2, 4, 3}));
}

TEST(DependencyAlgorithms, IgnoresCyclesOutsideLiveSubgraph)
{
    DependencyIndex index;
    const std::vector<DependencyEdge> edges{
        Edge(0, 1, 0, 1, Hazard::ReadAfterWrite),
        Edge(1, 0, 0, 2, Hazard::WriteAfterWrite, false),
        Edge(1, 2, 1, 0, Hazard::WriteAfterRead, false),
        Edge(2, 3, 2, 1, Hazard::ReadAfterWrite),
    };
    const std::vector<std::uint8_t> live{0, 0, 1, 1};
    const std::array<std::string_view, 4> passNames{"DeadA", "DeadB", "LiveA", "LiveB"};
    const std::array<std::string_view, 3> resourceNames{"DeadR", "BridgeR", "LiveR"};
    index.Build(4, edges);

    EXPECT_EQ(StableTopologicalSort(index, edges, live, passNames, resourceNames), (std::vector<std::uint32_t>{2, 3}));
}

TEST(DependencyAlgorithms, ReportsShortestStableLiveCycleWithStructuredDiagnostic)
{
    DependencyIndex index;
    // 输入边顺序刻意打乱；存在两个长度为二的环和一个长度为三的环，稳定结果应选择 P0 的环。
    const std::vector<DependencyEdge> edges{
        Edge(1, 0, 1, 2, Hazard::WriteAfterRead, false),  Edge(2, 3, 2, 3, Hazard::ReadAfterWrite),
        Edge(3, 2, 3, 4, Hazard::WriteAfterWrite, false), Edge(6, 4, 6, 7, Hazard::ReadAfterWrite),
        Edge(4, 5, 4, 5, Hazard::WriteAfterRead, false),  Edge(5, 6, 5, 6, Hazard::WriteAfterWrite, false),
        Edge(0, 1, 0, 1, Hazard::ReadAfterWrite),
    };
    const auto live = AllLive(7);
    const std::array<std::string_view, 7> passNames{"P0", "P1", "P2", "P3", "P4", "P5", "P6"};
    const std::array<std::string_view, 7> resourceNames{"R0", "R1", "R2", "R3", "R4", "R5", "R6"};
    index.Build(7, edges);

    try
    {
        (void)StableTopologicalSort(index, edges, live, passNames, resourceNames);
        FAIL() << "expected a live cycle";
    }
    catch (const GraphCycleError& error)
    {
        const auto& diagnostic = error.Diagnostic();
        EXPECT_EQ(diagnostic.code, GraphErrorCode::Cycle);
        EXPECT_EQ(diagnostic.phase, GraphPhase::Compiling);
        EXPECT_EQ(diagnostic.stage, CompileStage::TopologicalSort);
        EXPECT_EQ(diagnostic.pass, 0U);
        EXPECT_EQ(diagnostic.resource, 0U);
        EXPECT_EQ(diagnostic.version, 1U);
        EXPECT_EQ(diagnostic.passName, "P0");
        EXPECT_EQ(diagnostic.resourceName, "R0");
        ASSERT_EQ(diagnostic.cycle.size(), 2U);
        EXPECT_EQ(diagnostic.cycle[0], Edge(0, 1, 0, 1, Hazard::ReadAfterWrite));
        EXPECT_EQ(diagnostic.cycle[1], Edge(1, 0, 1, 2, Hazard::WriteAfterRead, false));
        EXPECT_NE(diagnostic.message.find("P0"), std::string::npos);
        EXPECT_NE(diagnostic.message.find("R0.v1"), std::string::npos);
    }
    catch (const std::exception& error)
    {
        FAIL() << "unexpected exception type: " << error.what();
    }
}

TEST(DependencyAlgorithms, ReportsLiveSelfDependencyAsOneEdgeCycle)
{
    DependencyIndex index;
    const std::vector<DependencyEdge> edges{Edge(1, 1, 0, 9, Hazard::WriteAfterWrite, false)};
    const auto live = AllLive(2);
    const std::array<std::string_view, 2> passNames{"P0", "Self"};
    const std::array<std::string_view, 1> resourceNames{"R0"};
    index.Build(2, edges);

    try
    {
        (void)StableTopologicalSort(index, edges, live, passNames, resourceNames);
        FAIL() << "expected a self dependency cycle";
    }
    catch (const GraphCycleError& error)
    {
        const auto& diagnostic = error.Diagnostic();
        EXPECT_EQ(diagnostic.code, GraphErrorCode::Cycle);
        EXPECT_EQ(diagnostic.stage, CompileStage::TopologicalSort);
        ASSERT_EQ(diagnostic.cycle.size(), 1U);
        EXPECT_EQ(diagnostic.cycle.front(), edges.front());
        EXPECT_EQ(diagnostic.passName, "Self");
        EXPECT_EQ(diagnostic.resourceName, "R0");
    }
    catch (const std::exception& error)
    {
        FAIL() << "unexpected exception type: " << error.what();
    }
}

TEST(DependencyAlgorithms, SameOriginEqualLengthCyclesIgnoreInputPermutation)
{
    std::vector<DependencyEdge> edges{
        Edge(0, 1, 0, 1, Hazard::ReadAfterWrite), Edge(1, 0, 1, 1, Hazard::ReadAfterWrite),
        Edge(0, 2, 2, 1, Hazard::ReadAfterWrite), Edge(2, 0, 3, 1, Hazard::ReadAfterWrite)};
    std::sort(edges.begin(), edges.end());
    const auto live = AllLive(3);
    const std::array<std::string_view, 3> names{"P0", "P1", "P2"};
    const std::array<std::string_view, 4> resourceNames{"R0", "R1", "R2", "R3"};
    std::size_t permutations = 0;
    do
    {
        DependencyIndex index;
        index.Build(3, edges);
        try
        {
            (void)StableTopologicalSort(index, edges, live, names, resourceNames);
            FAIL() << "cycle accepted";
        }
        catch (const GraphCycleError& error)
        {
            ASSERT_EQ(error.Diagnostic().cycle.size(), 2U);
            EXPECT_EQ(error.Diagnostic().cycle[0], Edge(0, 1, 0, 1, Hazard::ReadAfterWrite));
            EXPECT_EQ(error.Diagnostic().cycle[1], Edge(1, 0, 1, 1, Hazard::ReadAfterWrite));
        }
        ++permutations;
    } while (std::next_permutation(edges.begin(), edges.end()));
    EXPECT_EQ(permutations, 24U);
}
} // namespace MiniEngine::RenderGraph::Detail
