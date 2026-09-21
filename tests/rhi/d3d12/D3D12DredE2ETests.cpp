// ============================================================================
// D3D12DredE2ETests.cpp — 真实 device-removed 故障注入下的 DRED 端到端取证
// 授权：LEGACY/用户于 2026-09-16 批准（BACKLOG DRED-E2E-REMOVAL）；仅在本机
//       显式设置 MINIENGINE_DRED_E2E=1 时执行，否则跳过。TDR/页错误会复位
//       显示驱动（桌面闪烁数秒），因此绝不进入默认全量套件。
// 方案：越界 UAV 写入（dispatch 一个读取远超分配范围的 trigger 索引的 CS），
//       触发 GPU 页错误 → 设备移除 → DRED breadcrumbs + page fault 链 + 报告。
// ============================================================================
#include "D3D12Diagnostics.h"
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <gtest/gtest.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <cstdlib>
#endif

using MiniEngine::Rhi::D3D12::DeviceCreateOptions;
using MiniEngine::Rhi::D3D12::D3D12Device;
using MiniEngine::Rhi::D3D12::Internal::DeviceRemovedContext;
using MiniEngine::Rhi::D3D12::Internal::FormatDeviceRemovedReport;

namespace
{

#pragma warning(push)
#pragma warning(disable : 4996)
const char* SafeEnv(const char* name)
{
    return std::getenv(name);
}
#pragma warning(pop)

constexpr char kFaultyComputeShader[] = R"(
RWStructuredBuffer<uint> g_output : register(u0);
StructuredBuffer<uint>   g_trigger : register(t0);
[numthreads(1,1,1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    // trigger 缓冲只含 0x10000000（2.56 亿元素 × 4B ≈ 越界 1 GB），读到的索引
    // 远超 4 字节 victim 分配 → GPU 页错误 → 设备移除。
    g_output[g_trigger[0]] = tid.x;
}
)";

std::unique_ptr<D3D12Device> CreateDebugDevice()
{
    DeviceCreateOptions options;
    options.debugLayer = true;
    options.dred = true;
    return D3D12Device::Create(options);
}

 Microsoft::WRL::ComPtr<ID3DBlob> CompileFaultyShader()
{
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT compiled = D3DCompile(kFaultyComputeShader, std::size(kFaultyComputeShader) - 1,
                                        "M6Audit.FaultyCS", nullptr, nullptr, "CSMain", "cs_5_0",
                                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    if (FAILED(compiled))
        return nullptr;
    return bytecode;
}

} // namespace

TEST(D3D12DredE2E, OutOfBoundsUavWriteRemovesDeviceAndProducesDredEvidence)
{
    if (const char* gate = SafeEnv("MINIENGINE_DRED_E2E");
        gate == nullptr || std::string(gate) != "1")
    {
        GTEST_SKIP() << "真实 TDR/页错误会复位显示驱动；设置 MINIENGINE_DRED_E2E=1 才执行（用户授权的取证运行）";
    }

    const std::unique_ptr<D3D12Device> device = CreateDebugDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_TRUE(device->Metadata().dredActive);
    EXPECT_FALSE(device->IsDeviceRemoved());
    auto* native = static_cast<ID3D12Device*>(device->NativeDeviceHandle());
    ASSERT_NE(native, nullptr);

    const Microsoft::WRL::ComPtr<ID3DBlob> shader = CompileFaultyShader();
    ASSERT_NE(shader, nullptr) << "故障 CS 编译失败";

    // 根签名：两个根描述符（UAV + SRV），无堆。
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].Descriptor.RegisterSpace = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC signatureDesc{};
    signatureDesc.NumParameters = 2;
    signatureDesc.pParameters = parameters;
    signatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> signatureErrors;
    ASSERT_TRUE(SUCCEEDED(D3D12SerializeRootSignature(&signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &signature, &signatureErrors)));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    ASSERT_TRUE(SUCCEEDED(native->CreateRootSignature(0, signature->GetBufferPointer(),
                                                      signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature))));

    // victim（UAV，4 字节）与 trigger（SRV，4 字节，值为 0x10000000）。
    constexpr std::uint32_t kOutOfBoundsIndex = 0x10000000U;
    D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC victimDesc{};
    victimDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    victimDesc.Width = 4;
    victimDesc.Height = 1;
    victimDesc.DepthOrArraySize = 1;
    victimDesc.MipLevels = 1;
    victimDesc.SampleDesc.Count = 1;
    victimDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    victimDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> victim;
    ASSERT_TRUE(SUCCEEDED(native->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &victimDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                          IID_PPV_ARGS(&victim))));
    victim->SetName(L"M6Audit.PageFaultVictim");
    D3D12_RESOURCE_DESC triggerDesc = victimDesc;
    triggerDesc.Width = 4;
    triggerDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> trigger;
    ASSERT_TRUE(SUCCEEDED(native->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &triggerDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&trigger))));
    trigger->SetName(L"M6Audit.PageFaultTrigger");
    constexpr std::uint32_t triggerValue = kOutOfBoundsIndex;
    void* mapped = nullptr;
    ASSERT_TRUE(SUCCEEDED(trigger->Map(0, nullptr, &mapped)));
    std::memcpy(mapped, &triggerValue, sizeof(triggerValue));
    trigger->Unmap(0, nullptr);

    D3D12_SHADER_BYTECODE shaderBytecode{shader->GetBufferPointer(), shader->GetBufferSize()};
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS = shaderBytecode;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    ASSERT_TRUE(SUCCEEDED(native->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline))));

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ASSERT_TRUE(SUCCEEDED(native->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))));
    queue->SetName(L"M6Audit.Queue");
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    ASSERT_TRUE(SUCCEEDED(native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    ASSERT_TRUE(SUCCEEDED(native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                                    pipeline.Get(), IID_PPV_ARGS(&commands))));
    commands->SetName(L"M6Audit.FaultList");
    commands->SetComputeRootSignature(rootSignature.Get());
    commands->SetComputeRootUnorderedAccessView(0, victim->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(1, trigger->GetGPUVirtualAddress());
    commands->Dispatch(1, 1, 1);
    commands->Close();

    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    ASSERT_TRUE(SUCCEEDED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(event, nullptr);
    // 先把 event 关联到 fence。注意唤醒语义有二义性：正常完成把信号置为 1；
    // 设备移除会把 fence 信号置为 UINT64_MAX（ID3D12Device5::RemoveDevice 语义），
    // 同样唤醒等待者——因此"何时唤醒"不能区分两种路径（见下方 completedAtWake 记录）。
    ASSERT_TRUE(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
    ASSERT_TRUE(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    // TdrDelay 默认 2s：GPU 挂起 ⇒ 20s 内不会因正常完成而唤醒。最终判定只依据
    // "设备移除 + DRED 输出"，等待结果与唤醒时序只入证据不作判定。
    const DWORD waited = WaitForSingleObject(event, 20000);
    // 唤醒时的完成值：1 ⇒ dispatch 正常完成；UINT64_MAX ⇒ 设备移除信号；其他值视为异常。
    const UINT64 completedAtWake = fence->GetCompletedValue();
    CloseHandle(event);
    bool removed = false;
    for (int poll = 0; poll < 150 && !removed; ++poll)
    {
        removed = device->IsDeviceRemoved();
        if (!removed)
            Sleep(100);
    }
    ASSERT_TRUE(removed) << "故障 dispatch 后 35s 内设备未被移除（waited=" << waited
                         << "）——故障注入无效";

    // 设备必须真的被移除；0x887A0006 = DXGI_ERROR_DEVICE_HUNG（winerror.h；
    // 注意 0x887A0005 才是 DXGI_ERROR_DEVICE_REMOVED，0x887A0007 是 DEVICE_RESET）。
    EXPECT_TRUE(device->IsDeviceRemoved());
    const std::int32_t reason = device->DeviceRemovedReason();
    EXPECT_EQ(reason, static_cast<std::int32_t>(0x887A0006U)) << "移除原因 HRESULT=0x" << std::hex << reason;

    // DRED 取证：接口可获取；breadcrumbs 与 page fault 链至少一路有数据。
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    ASSERT_TRUE(SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&dred))));
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    const bool haveBreadcrumbs = SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs));
    const bool havePageFault = SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault));
    EXPECT_TRUE(haveBreadcrumbs || havePageFault) << "DRED 两路输出都缺失";
    UINT breadcrumbNodes = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = haveBreadcrumbs ? breadcrumbs.pHeadAutoBreadcrumbNode : nullptr;
         node != nullptr; node = node->pNext, ++breadcrumbNodes)
    {
    }
    UINT existingAllocations = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* node = havePageFault ? pageFault.pHeadExistingAllocationNode : nullptr;
         node != nullptr; node = node->pNext, ++existingAllocations)
    {
    }
    UINT recentFreed = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* node = havePageFault ? pageFault.pHeadRecentFreedAllocationNode : nullptr;
         node != nullptr; node = node->pNext, ++recentFreed)
    {
    }

    DeviceRemovedContext context;
    context.currentFence = 1;
    context.lastSubmittedFence = 1;
    context.assetRevision = "m6-audit-dred-e2e";
    const std::string report = FormatDeviceRemovedReport(static_cast<HRESULT>(reason),
                                                         haveBreadcrumbs ? &breadcrumbs : nullptr,
                                                         havePageFault ? &pageFault : nullptr, context);
    EXPECT_NE(report.find("reason=0x887A0006"), std::string::npos) << report;
    if (haveBreadcrumbs && breadcrumbNodes > 0)
        EXPECT_NE(report.find("breadcrumb["), std::string::npos) << report;
    if (havePageFault)
        EXPECT_NE(report.find("pageFault faultVA="), std::string::npos) << report;

    // 证据落盘（工具脚本设置 MINIENGINE_DRED_EVIDENCE_OUT）。
    std::ostringstream payload;
    payload << "{\n  \"reasonHresult\": \"" << std::hex << std::uppercase << static_cast<unsigned>(reason) << "\",\n"
            << "  \"fenceCompletedAtWake\": \"" << std::dec << completedAtWake << "\",\n"
            << "  \"breadcrumbNodes\": " << breadcrumbNodes << ",\n"
            << "  \"pageFaultExistingAllocations\": " << existingAllocations << ",\n"
            << "  \"pageFaultRecentFreedAllocations\": " << recentFreed << ",\n"
            << "  \"haveBreadcrumbsOutput\": " << (haveBreadcrumbs ? "true" : "false") << ",\n"
            << "  \"havePageFaultOutput\": " << (havePageFault ? "true" : "false") << ",\n"
            << "  \"waitResult\": " << waited << ",\n"
            << "  \"gpuName\": \"" << [](const D3D12Device& d) {
                   std::string name;
                   for (wchar_t character : d.Metadata().adapter.description)
                       name.push_back(static_cast<char>(character));
                   return name;
               }(*device) << "\"\n}\n";
    if (const char* evidence = SafeEnv("MINIENGINE_DRED_EVIDENCE_OUT"))
    {
        const std::filesystem::path path(evidence);
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << payload.str()
                                              << "--- formatted report ---\n" << report;
    }
    std::cout << "[DRED-E2E] nodes=" << breadcrumbNodes << " existing=" << existingAllocations
              << " recentFreed=" << recentFreed << " havePageFault=" << havePageFault << std::endl;
    SUCCEED() << "真实 device-removed 取证完成";
    if (dred)
        dred->Release();
}
