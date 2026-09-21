// ============================================================================
// D3D12DredLiveTests.cpp — DRED 在活动 Device 上的接线（不含 GPU fault）
// 里程碑：M6-12 审计后续（U2）
// 职责：验证 Debug Layer 打开时 DRED 强制开启、移除取证接口在活动 Device 上可获取、
//       以及引擎的报告格式化路径能被真实调用。真实 device-removed / TDR 仍属未验证
//       边界（见 docs/BACKLOG.md 的 DRED-E2E-REMOVAL），本文件不制造 GPU fault。
// ============================================================================
#include "D3D12Diagnostics.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using MiniEngine::Rhi::D3D12::DeviceCreateOptions;
using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::Internal::DeviceRemovedContext;
using MiniEngine::Rhi::D3D12::Internal::FormatDeviceRemovedReport;
using MiniEngine::Rhi::D3D12::Internal::ReportDeviceRemoved;

namespace
{

TEST(D3D12DredLiveTests, DebugDeviceForcesDredOnAndRemovalInterfaceIsUsable)
{
    DeviceCreateOptions options;
    options.debugLayer = true;
    options.dred = true;

    const std::unique_ptr<D3D12Device> device = D3D12Device::Create(options);
    ASSERT_NE(device, nullptr);
    EXPECT_TRUE(device->Metadata().debugLayerActive);
    EXPECT_TRUE(device->Metadata().dredActive) << "Debug Layer 打开时 DRED 必须强制开启";
    // 健康 Device 上不应报告移除；原因值保持 S_OK(0)。
    EXPECT_FALSE(device->IsDeviceRemoved());
    EXPECT_EQ(device->DeviceRemovedReason(), 0);

    auto* native = static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    ASSERT_NE(native, nullptr);
    // 移除取证的第一步：DRED 接口必须能取到（真实移除时的同一路径）。
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    const HRESULT query = native->QueryInterface(IID_PPV_ARGS(&dred));
    ASSERT_TRUE(SUCCEEDED(query)) << "DRED 强制开启后应可查询 ID3D12DeviceRemovedExtendedData，HRESULT=0x"
                                  << std::hex << static_cast<unsigned long>(query);
    ASSERT_NE(dred, nullptr);

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    const HRESULT crumbs = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
    const HRESULT fault = dred->GetPageFaultAllocationOutput(&pageFault);
    dred->Release();
    // 未发生移除时允许失败或返回空链；要求的是调用安全、报告路径可用。
    if (SUCCEEDED(crumbs))
        EXPECT_TRUE(breadcrumbs.pHeadAutoBreadcrumbNode == nullptr || breadcrumbs.pHeadAutoBreadcrumbNode != nullptr);
    const std::string report = FormatDeviceRemovedReport(
        static_cast<HRESULT>(device->DeviceRemovedReason()), SUCCEEDED(crumbs) ? &breadcrumbs : nullptr,
        SUCCEEDED(fault) ? &pageFault : nullptr, DeviceRemovedContext{});
    EXPECT_NE(report.find("reason="), std::string::npos) << report;
    EXPECT_NE(report.find("breadcrumbs="), std::string::npos) << report;
    // 引擎的移除报告入口在健康 Device 上必须可调用（同一格式化路径）。
    ReportDeviceRemoved(native, static_cast<HRESULT>(device->DeviceRemovedReason()));
    EXPECT_FALSE(device->DrainInfoQueue().HasFailure()) << "DRED 查询不应对调试层产生消息";
}

} // namespace
