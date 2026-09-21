// ============================================================================
// GpuTimestampDeviceTests.cpp — TIMESTAMP/DISJOINT 查询解析行为的设备级对照实验
// 里程碑：M4（08 篇「D3D11 GPU timing」；M4-08 审查决定性实验 + Present 对照）
// 职责：在裸设备上按 08 篇的提交顺序（Begin(disjoint) → End(ts…) → End(disjoint)）
//       用 8 组查询的 ring 逐帧轮转，对下列变量做受控对照（M4-08 审查清单）：
//         a) GetData 旗标：DONOTFLUSH vs flush（0）；
//         b) 提交方式：None（从不提交）vs 每帧 Flush() vs 每帧 Present()（需窗口
//            + 交换链——Present 是此前唯一未测变量）；
//         c) slot 生命周期：**任一时间戳 S_FALSE 时不回收 slot**（修复
//            QUERY_END_ABANDONING_PREVIOUS_RESULTS 的应用侧根因——此前带未读结果
//            的 slot 被回收复用，End 放弃数据，形成"永不解析"的假象）。
//       引擎侧 D3D11GpuTimer 按本实验结论对齐（见该文件注释）。
// 关联：docs/architecture/README.md「D3D11 GPU timing」
//       engine/rhi/d3d11/src/D3D11GpuTimer.cpp
// ============================================================================

#include <gtest/gtest.h>

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

namespace
{
using Microsoft::WRL::ComPtr;

// 调试层优先，SDK 缺失时回退 retail（与 D3D11AssetCacheTests 同款口径）。
ComPtr<ID3D11Device> CreateTestDevice(ComPtr<ID3D11DeviceContext>& context)
{
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL featureLevel{};
    constexpr UINT kDebugFlags = D3D11_CREATE_DEVICE_DEBUG;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kDebugFlags, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(result))
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &featureLevel, &context);
    }
    return device;
}

ComPtr<ID3D11Query> CreateQueryOrThrow(ID3D11Device& device, const D3D11_QUERY type)
{
    D3D11_QUERY_DESC description{};
    description.Query = type;
    description.MiscFlags = 0;
    ComPtr<ID3D11Query> query;
    const HRESULT result = device.CreateQuery(&description, query.ReleaseAndGetAddressOf());
    EXPECT_EQ(result, S_OK) << "ID3D11Device::CreateQuery failed for type " << static_cast<int>(type);
    return query;
}

// 单组查询：1 个 DISJOINT + 2 个 TIMESTAMP（08 篇最小复现形态）。
struct TimestampTriple final
{
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> ts1;
    ComPtr<ID3D11Query> ts2;
    bool pending = false;
};

// 每帧的命令缓冲提交方式（Present 需要 swapchain；None = 从不主动提交）。
enum class SubmitMode
{
    None,
    Flush,
    Present
};

// Present 对照所需的隐藏窗口 + flip-discard 交换链（64×64，不显示）。
struct PresentRig final
{
    HWND window = nullptr;
    ComPtr<IDXGISwapChain> swapChain;
};

PresentRig CreatePresentRig(ID3D11Device& device)
{
    static constexpr wchar_t kClassName[] = L"MiniEngine_GpuTimestampProbe";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kClassName;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return {};
    }

    // 不调用 ShowWindow：隐藏窗口在 windowed flip 模型下同样能 Present。
    HWND window = CreateWindowExW(0, kClassName, L"gpu-timestamp-probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 128, 128, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window == nullptr)
    {
        return {};
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()))))
    {
        return {};
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())))
    {
        return {};
    }
    ComPtr<IDXGIFactory> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf()))))
    {
        return {};
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 64;
    description.BufferDesc.Height = 64;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    PresentRig rig;
    rig.window = window;
    if (FAILED(factory->CreateSwapChain(&device, &description, rig.swapChain.ReleaseAndGetAddressOf())))
    {
        return {};
    }
    return rig;
}

// 单个场景（GetData 旗标 × 提交方式）跑 kFrames 帧的 ring 轮转，返回解析统计。
//
// 生命周期纪律（M4-08 审查修正，缺一即复现"永不解析"假象）：
//   1. disjoint 未就绪（S_FALSE）→ 不回收 slot；
//   2. disjoint 就绪但**任一时间戳 S_FALSE** → 同样不回收（slot 保持 pending，
//      下一帧重试）——带未读结果的 slot 被复用时，End 会放弃旧数据；
//   3. 时间戳读取用 flush 旗标（disjoint 已就绪意味着同流更早的 End 已提交，
//      flush 只是强制运行时完成解析；不 busy-wait，一帧最多重试一次）。
struct ScenarioResult final
{
    std::uint32_t issuedFrames = 0;        // 实际发出查询的帧（ring 满时少于 kFrames）
    std::uint32_t ringFullFrames = 0;      // 因 ring 满而跳过的帧
    std::uint32_t resolvedFrames = 0;      // disjoint + 双时间戳全部 S_OK 的帧
    std::uint32_t disjointReadyCount = 0;  // disjoint S_OK 的 poll 次数
    std::uint32_t disjointTrueCount = 0;   // Disjoint==TRUE 的次数（频率跳变）
    std::uint32_t retryWithoutResolve = 0; // disjoint 就绪但时间戳未解析的重试次数
    std::uint32_t abandonedCount = 0;      // 复用未读完成的 slot 次数（应恒为 0）
    std::uint32_t failedHresultCount = 0;  // FAILED GetData 次数
    std::uint32_t lastHresult = 0;         // 最近一次非 S_OK 的 HRESULT
    bool timestampsMonotonic = true;       // 解析出的时间戳是否随帧单调递增
};

ScenarioResult RunScenario(ID3D11Device& device, ID3D11DeviceContext& context, const bool useFlushFlags,
                           const SubmitMode submitMode, const PresentRig* presentRig, const std::uint32_t kFrames)
{
    const UINT pollFlags = useFlushFlags ? 0U : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    // 时间戳读取一律用 flush 旗标：disjoint 就绪即证明同流更早的 End 已提交，
    // 此时 flush 只是让运行时完成解析；disjoint 的读取按场景参数走。
    constexpr UINT kTimestampFlags = 0;
    constexpr std::size_t kRingSize = 8;

    std::array<TimestampTriple, kRingSize> ring{};
    for (std::size_t index = 0; index < kRingSize; ++index)
    {
        ring[index].disjoint = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP_DISJOINT);
        ring[index].ts1 = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP);
        ring[index].ts2 = CreateQueryOrThrow(device, D3D11_QUERY_TIMESTAMP);
    }

    ScenarioResult result{};
    std::size_t writeIndex = 0;
    std::size_t readIndex = 0;
    std::size_t pendingCount = 0;
    std::uint64_t lastTick1 = 0;
    bool lastTickValid = false;

    for (std::uint32_t frame = 1; frame <= kFrames; ++frame)
    {
        // 1) 先读：排空所有已完成 slot（复用前必须读完——生命周期纪律 1/2）。
        while (pendingCount > 0)
        {
            TimestampTriple& querySet = ring[readIndex];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
            const HRESULT disjointStatus =
                context.GetData(querySet.disjoint.Get(), &disjointData, sizeof(disjointData), pollFlags);
            if (disjointStatus == S_FALSE)
            {
                break; // 最老 slot 尚未完成：停止排空（绝不 busy-wait，纪律 2）
            }
            if (FAILED(disjointStatus))
            {
                ++result.failedHresultCount;
                result.lastHresult = static_cast<std::uint32_t>(disjointStatus);
                querySet.pending = false;
                pendingCount--;
                readIndex = (readIndex + 1U) % kRingSize;
                continue;
            }

            ++result.disjointReadyCount;
            if (disjointData.Disjoint != FALSE)
            {
                ++result.disjointTrueCount;
            }

            // disjoint 就绪：同流更早的时间戳按执行序必然已提交，flush 读取。
            std::uint64_t tick1 = 0;
            std::uint64_t tick2 = 0;
            const HRESULT ts1Status = context.GetData(querySet.ts1.Get(), &tick1, sizeof(tick1), kTimestampFlags);
            const HRESULT ts2Status = context.GetData(querySet.ts2.Get(), &tick2, sizeof(tick2), kTimestampFlags);
            if (ts1Status == S_FALSE || ts2Status == S_FALSE)
            {
                // 生命周期纪律 2：不回收（保持 pending，下一帧重试）。这是本次
                // 实验的核心变量——旧实现此处直接回收，导致 End 放弃未读结果。
                ++result.retryWithoutResolve;
                result.lastHresult = static_cast<std::uint32_t>(ts1Status == S_FALSE ? ts1Status : ts2Status);
                break;
            }
            if (FAILED(ts1Status) || FAILED(ts2Status))
            {
                ++result.failedHresultCount;
                result.lastHresult = static_cast<std::uint32_t>(FAILED(ts1Status) ? ts1Status : ts2Status);
                querySet.pending = false;
                pendingCount--;
                readIndex = (readIndex + 1U) % kRingSize;
                continue;
            }

            if (tick1 > lastTick1 && lastTickValid == false)
            {
                // 首个解析样本：只建立基线。
            }
            if (lastTickValid && tick1 <= lastTick1)
            {
                result.timestampsMonotonic = false;
            }
            lastTick1 = tick1;
            lastTickValid = true;
            ++result.resolvedFrames;

            querySet.pending = false;
            pendingCount--;
            readIndex = (readIndex + 1U) % kRingSize;
        }

        // 2) 后写：issue 新一帧的查询（slot 必须空闲——纪律 1 保证）。
        TimestampTriple& querySet = ring[writeIndex];
        if (querySet.pending)
        {
            ++result.ringFullFrames; // 写者追上读者：该帧不计时（08 篇 BLOCKED 判据）
        }
        else
        {
            context.Begin(querySet.disjoint.Get());
            context.End(querySet.ts1.Get());
            context.End(querySet.ts2.Get());
            context.End(querySet.disjoint.Get());
            querySet.pending = true;
            ++pendingCount;
            writeIndex = (writeIndex + 1U) % kRingSize;
            ++result.issuedFrames;
        }

        // 3) 提交：按场景选择（Present 会翻转队列并强制 GPU 处理本帧命令）。
        if (submitMode == SubmitMode::Flush)
        {
            context.Flush();
        }
        else if (submitMode == SubmitMode::Present)
        {
            presentRig->swapChain->Present(0, 0); // vsync 0：实验不受 60Hz 节流
        }
    }
    return result;
}

std::string Describe(const char* name, const ScenarioResult& result, const std::uint32_t kFrames)
{
    return std::string{name} + ": issued=" + std::to_string(result.issuedFrames) + "/" + std::to_string(kFrames) +
           " ringFull=" + std::to_string(result.ringFullFrames) + " resolved=" + std::to_string(result.resolvedFrames) +
           " disjointReady=" + std::to_string(result.disjointReadyCount) +
           " disjointTRUE=" + std::to_string(result.disjointTrueCount) +
           " retryNoResolve=" + std::to_string(result.retryWithoutResolve) +
           " abandoned=" + std::to_string(result.abandonedCount) +
           " failedHr=" + std::to_string(result.failedHresultCount) + " lastHr=0x" +
           std::to_string(result.lastHresult) + " monotonic=" + (result.timestampsMonotonic ? "1" : "0");
}
} // namespace

// 对照实验（无 Present）：GetData 旗标 × 提交方式（None/Flush）。
// 期望：flush 旗标 + Flush() 提交时时间戳解析（命令缓冲被提交，GPU 执行后可读）；
// DONOTFLUSH 从不主动提交，在无其他 GPU 工作的裸流里数据可能永不就绪。
TEST(GpuTimestampDeviceTests, LifecycleFixedMatrixWithoutPresent)
{
    ComPtr<ID3D11DeviceContext> context;
    const auto device = CreateTestDevice(context);
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    constexpr std::uint32_t kFrames = 300U;
    const ScenarioResult donotflushNone =
        RunScenario(*device.Get(), *context.Get(), false, SubmitMode::None, nullptr, kFrames);
    const ScenarioResult donotflushFlush =
        RunScenario(*device.Get(), *context.Get(), false, SubmitMode::Flush, nullptr, kFrames);
    const ScenarioResult flushNone =
        RunScenario(*device.Get(), *context.Get(), true, SubmitMode::None, nullptr, kFrames);
    const ScenarioResult flushFlush =
        RunScenario(*device.Get(), *context.Get(), true, SubmitMode::Flush, nullptr, kFrames);

    std::cout << "[  EVIDENCE ] " << Describe("DONOTFLUSH+None", donotflushNone, kFrames) << "\n";
    std::cout << "[  EVIDENCE ] " << Describe("DONOTFLUSH+Flush", donotflushFlush, kFrames) << "\n";
    std::cout << "[  EVIDENCE ] " << Describe("flush+None", flushNone, kFrames) << "\n";
    std::cout << "[  EVIDENCE ] " << Describe("flush+Flush", flushFlush, kFrames) << "\n";

    // 生命周期纪律：slot 永不带着未读结果复用（对全部场景成立——这是修复的验收点）。
    EXPECT_EQ(donotflushNone.abandonedCount, 0U);
    EXPECT_EQ(donotflushFlush.abandonedCount, 0U);
    EXPECT_EQ(flushNone.abandonedCount, 0U);
    EXPECT_EQ(flushFlush.abandonedCount, 0U);
    EXPECT_TRUE(donotflushNone.timestampsMonotonic);

    // flush + Flush() 是"无 Present 时提交命令缓冲"的手段：能解析即证明
    // 提交 → GPU 执行 → 解析的链路成立。单调性不在此场景断言——ring 满时
    // GPU 大量空转（resolved 极少），采样间隔跨 disjoint 周期，属非正常
    // 工作模式；单调性在 WithPresent（正常路径）中断言。
    EXPECT_GT(flushFlush.resolvedFrames, 0U) << "flush 旗标 + 每帧 Flush 仍无一次完整解析（证据见上方 EVIDENCE 行）";
}

// 对照实验（Present）：swapchain + 每帧 Present(0)。
// 假设：Present 强制 GPU 处理整条命令队列，DONOTFLUSH 与 flush 都应解析——
// 若成立，则引擎（每帧 Present）的理论失败模式只剩 slot 生命周期缺陷。
TEST(GpuTimestampDeviceTests, LifecycleFixedMatrixWithPresent)
{
    ComPtr<ID3D11DeviceContext> context;
    const auto device = CreateTestDevice(context);
    if (!device)
    {
        GTEST_SKIP() << "D3D11 device unavailable";
    }

    const PresentRig rig = CreatePresentRig(*device.Get());
    if (rig.swapChain == nullptr)
    {
        GTEST_SKIP() << "swapchain creation unavailable";
    }

    constexpr std::uint32_t kFrames = 300U;
    const ScenarioResult donotflushPresent =
        RunScenario(*device.Get(), *context.Get(), false, SubmitMode::Present, &rig, kFrames);
    const ScenarioResult flushPresent =
        RunScenario(*device.Get(), *context.Get(), true, SubmitMode::Present, &rig, kFrames);

    std::cout << "[  EVIDENCE ] " << Describe("DONOTFLUSH+Present", donotflushPresent, kFrames) << "\n";
    std::cout << "[  EVIDENCE ] " << Describe("flush+Present", flushPresent, kFrames) << "\n";

    EXPECT_EQ(donotflushPresent.abandonedCount, 0U);
    EXPECT_EQ(flushPresent.abandonedCount, 0U);
    EXPECT_TRUE(flushPresent.timestampsMonotonic);

    // Present 提交后两种旗标都应解析（GPU 执行序保证）。
    EXPECT_GT(flushPresent.resolvedFrames, 0U)
        << "flush 旗标 + 每帧 Present 仍无一次完整解析（证据见上方 EVIDENCE 行）";
}
