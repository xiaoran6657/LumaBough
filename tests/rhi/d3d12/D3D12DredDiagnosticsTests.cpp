// ============================================================================
// D3D12DredDiagnosticsTests.cpp — DRED formatter 的 CPU 侧结构契约
// 说明：测试只构造原生 DRED 输出，验证格式化与边界；不制造真实 TDR 或设备移除。
// ============================================================================
#include "D3D12Diagnostics.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

using namespace MiniEngine::Rhi::D3D12::Internal;

TEST(D3D12DredDiagnosticsTests, FormatsReasonContextBreadcrumbAndPageFault)
{
    std::array<D3D12_AUTO_BREADCRUMB_OP, 3> history{D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED,
                                                    D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
                                                    D3D12_AUTO_BREADCRUMB_OP_DISPATCH};
    UINT completed = 2;
    D3D12_AUTO_BREADCRUMB_NODE node{};
    node.pCommandQueueDebugNameA = "graphics queue";
    node.pCommandListDebugNameA = "forward list";
    node.BreadcrumbCount = static_cast<UINT>(history.size());
    node.pLastBreadcrumbValue = &completed;
    node.pCommandHistory = history.data();
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    crumbs.pHeadAutoBreadcrumbNode = &node;

    D3D12_DRED_ALLOCATION_NODE existing{};
    existing.ObjectNameA = "HDR target";
    existing.AllocationType = D3D12_DRED_ALLOCATION_TYPE_RESOURCE;
    D3D12_DRED_ALLOCATION_NODE freed{};
    freed.ObjectNameA = "old upload";
    freed.AllocationType = D3D12_DRED_ALLOCATION_TYPE_HEAP;
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    fault.PageFaultVA = 0x1234;
    fault.pHeadExistingAllocationNode = &existing;
    fault.pHeadRecentFreedAllocationNode = &freed;

    DeviceRemovedContext context{42, 41, 40, {"Shadow", "ToneMap"}, "asset-r7", "shader-r12"};
    const std::string report = FormatDeviceRemovedReport(E_FAIL, &crumbs, &fault, context);
    EXPECT_NE(report.find("0x80004005"), std::string::npos);
    EXPECT_NE(report.find("queue=graphics queue"), std::string::npos);
    EXPECT_NE(report.find("list=forward list"), std::string::npos);
    EXPECT_NE(report.find("lastOperation=1:ResourceBarrier"), std::string::npos);
    EXPECT_NE(report.find("faultVA=0x0000000000001234"), std::string::npos);
    EXPECT_NE(report.find("name=HDR target"), std::string::npos);
    EXPECT_NE(report.find("name=old upload"), std::string::npos);
    EXPECT_NE(report.find("asset-r7"), std::string::npos);
    EXPECT_NE(report.find("ToneMap"), std::string::npos);
}

TEST(D3D12DredDiagnosticsTests, UsesCompletedMinusOneAndCapsHistoryAtDredRingSize)
{
    std::array<D3D12_AUTO_BREADCRUMB_OP, 2> history{D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED,
                                                    D3D12_AUTO_BREADCRUMB_OP_DISPATCH};
    UINT completed = 65537;
    D3D12_AUTO_BREADCRUMB_NODE node{};
    node.BreadcrumbCount = 65537;
    node.pLastBreadcrumbValue = &completed;
    node.pCommandHistory = history.data();
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    crumbs.pHeadAutoBreadcrumbNode = &node;
    const std::string report = FormatDeviceRemovedReport(E_FAIL, &crumbs, nullptr, {});
    EXPECT_NE(report.find("lastOperation=0:DrawInstanced"), std::string::npos)
        << "DRED 历史环索引必须使用 (completed-1) % 65536，且不可越界";
}

TEST(D3D12DredDiagnosticsTests, ReportsMissingDataAndTraversalTruncationExplicitly)
{
    DeviceRemovedContext context;
    const std::string missing = FormatDeviceRemovedReport(DXGI_ERROR_DEVICE_REMOVED, nullptr, nullptr, context);
    EXPECT_NE(missing.find("breadcrumbs=<unavailable>"), std::string::npos);
    EXPECT_NE(missing.find("pageFault=<unavailable>"), std::string::npos);
    EXPECT_NE(missing.find("assetRevision=<missing>"), std::string::npos);

    D3D12_AUTO_BREADCRUMB_NODE first{};
    D3D12_AUTO_BREADCRUMB_NODE second{};
    first.pNext = &second;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    crumbs.pHeadAutoBreadcrumbNode = &first;
    const std::string capped = FormatDeviceRemovedReport(S_OK, &crumbs, nullptr, {}, 1, 1);
    EXPECT_NE(capped.find("breadcrumbs=<truncated>"), std::string::npos);
}

TEST(D3D12DredDiagnosticsTests, RejectsOutOfRangeCounterWithoutReadingHistory)
{
    const D3D12_AUTO_BREADCRUMB_OP operation = D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED;
    UINT completed = 2U;
    D3D12_AUTO_BREADCRUMB_NODE node{};
    node.BreadcrumbCount = 1U;
    node.pCommandHistory = &operation;
    node.pLastBreadcrumbValue = &completed;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT output{};
    output.pHeadAutoBreadcrumbNode = &node;
    EXPECT_NE(FormatDeviceRemovedReport(E_FAIL, &output, nullptr, {}).find("lastOperation=<unavailable>"),
              std::string::npos);
    completed = 0U;
    EXPECT_NE(FormatDeviceRemovedReport(E_FAIL, &output, nullptr, {}).find("lastOperation=<unavailable>"),
              std::string::npos);
}

TEST(D3D12DredDiagnosticsTests, ExactTraversalLimitDoesNotClaimTruncation)
{
    D3D12_AUTO_BREADCRUMB_NODE node{};
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT output{};
    output.pHeadAutoBreadcrumbNode = &node;
    D3D12_DRED_ALLOCATION_NODE allocation{};
    allocation.ObjectNameA = "one allocation";
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    fault.pHeadExistingAllocationNode = &allocation;
    EXPECT_EQ(FormatDeviceRemovedReport(E_FAIL, &output, &fault, {}, 1U, 1U).find("<truncated>"), std::string::npos);
    allocation.pNext = &allocation;
    EXPECT_NE(FormatDeviceRemovedReport(E_FAIL, &output, &fault, {}, 1U, 1U).find("pageFault existing=<truncated>"),
              std::string::npos);
}
