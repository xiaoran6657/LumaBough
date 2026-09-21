// ============================================================================
// D3D12Device.cpp — Device 创建顺序、adapter 选择、feature 查询与 InfoQueue
// 里程碑：M5（02 篇环境、Device 与诊断基线；手抄清单第 1 条）
// 职责：按 02 篇冻结的顺序创建 Device：DRED → Debug Layer → GBV → factory →
//       adapter → device → feature → InfoQueue。顺序不可交换——前三者是进程级
//       开关，晚于 D3D12CreateDevice 配置即静默失效；因此任何"配置晚了"的路径
//       在这里根本不存在（先配置后创建，而不是创建后补开）。
// 失败语义：全部显式失败——非法开关组合抛 std::runtime_error、HRESULT 失败抛
//       HResultError（文本含操作名与十六进制值）、SM6.0/Root Signature 不满足抛
//       带 BLOCKED 字样的 std::runtime_error；绝不静默降级 WARP 或关闭验证。
// 关联：docs/architecture/README.md（创建顺序 / Adapter 选择）
//       docs/architecture/DECISIONS.md（决策 1/5）
// ============================================================================
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include "D3D12Diagnostics.h"

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// D3D12 Debug Layer 组件缺失（Graphics Tools / d3d12SDKLayers.dll 未安装）时的
// 提示文本：失败信息里直接给安装口径，避免按 M2 的经验再查一轮。
constexpr const char* kDebugLayerMissingHint =
    " (D3D12 debug layer components are missing; install the Graphics Tools optional feature "
    "or the matching Windows SDK d3d12SDKLayers.dll)";

// 逐个候选做能力探测：D3D12CreateDevice 传 nullptr 即"只探测不创建"。
bool AdapterSupportsDevice(IDXGIAdapter1* adapter)
{
    return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
}

// DXGI_ADAPTER_DESC1::Description（wchar[128]）→ UTF-8。
std::string AdapterDescriptionUtf8(const DXGI_ADAPTER_DESC1& desc)
{
    const std::wstring_view wide{desc.Description};
    if (wide.empty())
    {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// 从 adapter 收集 02 篇要求的全部身份字段。
AdapterMetadata CollectAdapterMetadata(IDXGIAdapter1* adapter, const bool isWarp)
{
    DXGI_ADAPTER_DESC1 desc{};
    ThrowIfFailed(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
    AdapterMetadata metadata;
    metadata.description = AdapterDescriptionUtf8(desc);
    metadata.vendorId = desc.VendorId;
    metadata.deviceId = desc.DeviceId;
    metadata.subSysId = desc.SubSysId;
    metadata.revision = desc.Revision;
    metadata.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
    metadata.sharedSystemMemoryBytes = desc.SharedSystemMemory;
    metadata.luidLow = desc.AdapterLuid.LowPart;
    metadata.luidHigh = desc.AdapterLuid.HighPart;
    metadata.isWarp = isWarp;
    return metadata;
}

// LUID 相等比较（DXGI_ADAPTER_DESC1::AdapterLuid 与 --adapter-luid 的匹配）。
// 注意 LUID::HighPart 是 long（有符号），显式转换后按位比较。
bool LuidMatches(const LUID& luid, const std::uint32_t low, const std::uint32_t high)
{
    return luid.LowPart == low && static_cast<std::uint32_t>(luid.HighPart) == high;
}
} // namespace

// ---------------------------------------------------------------------------
// 值语义：模式推导 / LUID 解析 / 报告判定
// ---------------------------------------------------------------------------

RunModeResolution ResolveRunMode(const DeviceCreateOptions& options)
{
    RunModeResolution resolution;
    // 互斥与蕴含规则必须在这里拦截：Create() 不做"帮你补一个开关"的静默修正，
    // 否则 metadata 记录的模式与 Device 实际配置会分叉。
    if (options.gpuValidation && !options.debugLayer)
    {
        resolution.error = "--gpu-validation requires --d3d12-debug (GBV works in conjunction with the debug layer)";
        return resolution;
    }
    if (options.dred && !options.debugLayer)
    {
        resolution.error = "--dred requires --d3d12-debug (DRED data is produced by the debug layer)";
        return resolution;
    }
    if (options.warp && options.hasAdapterLuid)
    {
        resolution.error = "--warp and --adapter-luid are mutually exclusive";
        return resolution;
    }

    resolution.mode = options.gpuValidation ? DeviceRunMode::Gbv
                      : options.debugLayer  ? DeviceRunMode::Debug
                                            : DeviceRunMode::Release;
    return resolution;
}

bool ParseAdapterLuid(const std::string& text, std::uint32_t& low, std::uint32_t& high)
{
    const std::size_t separator = text.find('-');
    if (separator == std::string::npos)
    {
        return false;
    }
    const std::string lowPart = text.substr(0, separator);
    const std::string highPart = text.substr(separator + 1);
    if (lowPart.empty() || highPart.empty())
    {
        return false;
    }

    const auto parsePart = [](const std::string& part, std::uint32_t& out)
    {
        // 只接受十进制或 0x/0X 前缀十六进制；刻意不用 strtoul 的自动进制语义
        // （它会把 "018" 当八进制），十进制段固定按 base=10 解析，前导 0 是合法输入。
        std::size_t begin = 0;
        int base = 10;
        if (part.size() > 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X'))
        {
            base = 16;
            begin = 2;
            for (const char c : part.substr(begin))
            {
                if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
                {
                    return false;
                }
            }
        }
        else
        {
            for (const char c : part)
            {
                if (std::isdigit(static_cast<unsigned char>(c)) == 0)
                {
                    return false;
                }
            }
        }
        unsigned long value = 0;
        try
        {
            value = std::stoul(part.substr(begin), nullptr, base);
        }
        catch (const std::exception&)
        {
            return false;
        }
        if (value > 0xFFFFFFFFUL)
        {
            return false;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    };

    return parsePart(lowPart, low) && parsePart(highPart, high);
}

std::string ShaderModelBlockReason(const std::uint32_t highestShaderModel)
{
    // M5 的 shader 全部走 DXC vs_6_0/ps_6_0，降级 shader 属于"未记录的切换"，
    // 因此低于 0x60 是硬性 BLOCKED 而不是"继续但少个特性"。
    if (highestShaderModel < 0x60U)
    {
        char text[16]{};
        std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(highestShaderModel));
        return "reports highest shader model " + std::string{text} +
               "; D3D12 backend requires SM 6.0 (0x60) for DXC vs_6_0/ps_6_0";
    }
    return {};
}

std::string RootSignatureBlockReason(const std::uint32_t highestRootSignatureVersion)
{
    // D3D_ROOT_SIGNATURE_VERSION_1_0 = 1；低于 1.0 说明连 baseline 都不支持。
    if (highestRootSignatureVersion < 1U)
    {
        return "does not support D3D12 root signature 1.0 baseline";
    }
    return {};
}

bool ValidationReport::HasFailure() const noexcept
{
    // D3D12_MESSAGE_SEVERITY：CORRUPTION=0、ERROR=1、WARNING=2、INFO=3、MESSAGE=4。
    // 零容忍 = 出现 INFO 之外的任何消息。
    for (const ValidationMessage& message : messages)
    {
        if (message.severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            return true;
        }
    }
    return false;
}

std::size_t ValidationReport::Size() const noexcept
{
    return messages.size();
}

// ---------------------------------------------------------------------------
// D3D12Device 实现
// ---------------------------------------------------------------------------

struct D3D12Device::Impl final
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    // 创建 Device 时所用的 factory（02 篇创建顺序第 4 步）：交换链要拿它查询
    // tearing 能力并创建 swap chain，因此必须与设备同源、带同样的 debug 标志。
    Microsoft::WRL::ComPtr<IDXGIFactory7> factory;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue; // 仅 debugLayerActive 时非空
    DeviceMetadata metadata;
    mutable std::int32_t cachedRemovedReason = 0; // S_OK；移除原因只查一次后缓存
    mutable bool removedQueried = false;
    Internal::DeviceRemovedContext removalContext;
};

D3D12Device::D3D12Device(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl))
{
}

D3D12Device::~D3D12Device() = default;

std::unique_ptr<D3D12Device> D3D12Device::Create(const DeviceCreateOptions& options)
{
    // 0) 开关组合校验：非法组合直接失败（metadata 的 mode 必须与实际配置一致）。
    const RunModeResolution mode = ResolveRunMode(options);
    if (!mode.error.empty())
    {
        throw std::runtime_error{"D3D12 device options invalid: " + mode.error};
    }

    // 1) DRED（先于 Device）：强制开启 breadcrumbs 与 page fault 采集。
    //    此刻还没有 Device，因此"创建后才发现没开"在这条路径上不可能发生。
    if (options.dred)
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)), "D3D12GetDebugInterface(DRED settings)");
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }

    // 2) Debug Layer + 3) GBV：同一进程级开关，GBV 必须在 Device 创建前置位。
    //    组件缺失（DXGI_ERROR_SDK_COMPONENT_MISSING）直接失败并提示安装，不静默降级。
    if (options.debugLayer)
    {
        ComPtr<ID3D12Debug> debug;
        const HRESULT debugHr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (FAILED(debugHr))
        {
            if (debugHr == DXGI_ERROR_SDK_COMPONENT_MISSING)
            {
                throw HResultError(debugHr,
                                   std::string{"D3D12GetDebugInterface(Debug layer)"} + kDebugLayerMissingHint);
            }
            throw HResultError(debugHr, "D3D12GetDebugInterface(Debug layer)");
        }
        debug->EnableDebugLayer();

        if (options.gpuValidation)
        {
            ComPtr<ID3D12Debug1> debug1;
            ThrowIfFailed(debug.As(&debug1), "Query ID3D12Debug1");
            debug1->SetEnableGPUBasedValidation(TRUE);
        }
        else
        {
            ComPtr<ID3D12Debug1> debug1;
            ThrowIfFailed(debug.As(&debug1), "Query ID3D12Debug1");
            debug1->SetEnableGPUBasedValidation(FALSE);
        }
    }

    // 4) Factory：debug 标志必须与调试层一致，否则 factory 拿不到诊断通道。
    const UINT factoryFlags = options.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0U;
    ComPtr<IDXGIFactory7> factory;
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    // 5) adapter 选择。三种策略互斥（ResolveRunMode 已保证）：
    //    --warp → EnumWarpAdapter；--adapter-luid → 精确匹配；默认 → 按偏好枚举。
    AdapterMetadata adapterMetadata;
    ComPtr<IDXGIAdapter1> selectedAdapter;
    if (options.warp)
    {
        // WARP 是显式选项而不是回退路径：EnumWarpAdapter 失败即失败。
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&selectedAdapter)), "IDXGIFactory4::EnumWarpAdapter");
        if (!AdapterSupportsDevice(selectedAdapter.Get()))
        {
            throw std::runtime_error{"WARP adapter cannot create a D3D12 device at feature level 11_0"};
        }
        adapterMetadata = CollectAdapterMetadata(selectedAdapter.Get(), true);
    }
    else if (options.hasAdapterLuid)
    {
        // 点名 adapter：找不到必须失败，并在错误里列出本机全部候选 LUID 方便修正。
        // 诊断文本统一 `<low>-<high>`（与 --adapter-luid 的输入契约同序——审查发现
        // 旧文本高低位颠倒，用户照抄候选回填会因对调而再次失败）。
        std::string candidates;
        for (UINT index = 0;; ++index)
        {
            // 枚举失败不等于枚举结束：NOT_FOUND 才终止，其它 HRESULT 显式失败。
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumHr = factory->EnumAdapters1(index, &candidate);
            if (enumHr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            ThrowIfFailed(enumHr, "IDXGIFactory7::EnumAdapters1");

            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(candidate->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
            candidates += " " + AdapterDescriptionUtf8(desc) +
                          " luid=" + std::to_string(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart)) + "-" +
                          std::to_string(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart));
            if (LuidMatches(desc.AdapterLuid, options.adapterLuidLow, options.adapterLuidHigh))
            {
                if (!AdapterSupportsDevice(candidate.Get()))
                {
                    throw std::runtime_error{"adapter luid " + std::to_string(options.adapterLuidLow) + "-" +
                                             std::to_string(options.adapterLuidHigh) +
                                             " was found but cannot create a D3D12 device at feature level 11_0"};
                }
                selectedAdapter = candidate;
                adapterMetadata = CollectAdapterMetadata(candidate.Get(), false);
                break;
            }
        }
        if (selectedAdapter == nullptr)
        {
            throw std::runtime_error{"adapter luid " + std::to_string(options.adapterLuidLow) + "-" +
                                     std::to_string(options.adapterLuidHigh) + " not found; available:" + candidates};
        }
    }
    else
    {
        // 默认策略：按 HIGH_PERFORMANCE 偏好枚举，跳过软件适配器（除非显式 --warp）。
        // 不用"显存最大"这类隐式启发；找不到硬件 adapter 就失败并提示 --warp。
        bool foundHardware = false;
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumHr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                       IID_PPV_ARGS(&candidate));
            if (enumHr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            ThrowIfFailed(enumHr, "IDXGIFactory6::EnumAdapterByGpuPreference");

            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(candidate->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
            if ((desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0)
            {
                continue; // 软件适配器只属于显式 --warp 路径
            }
            if (AdapterSupportsDevice(candidate.Get()))
            {
                selectedAdapter = candidate;
                adapterMetadata = CollectAdapterMetadata(candidate.Get(), false);
                foundHardware = true;
                break;
            }
        }
        if (!foundHardware)
        {
            throw std::runtime_error{
                "no hardware adapter that supports D3D12 feature level 11_0 was found; pass --warp to opt in to "
                "software rendering explicitly"};
        }
    }

    // 6) Device：feature level 固定 11_0（baseline），能力差异交给 feature 查询披露。
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                  "D3D12CreateDevice");

    // 7) feature 查询：02 篇清单（OPTIONS / ROOT_SIGNATURE / SM6.0），不足即 BLOCKED。
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->factory = factory;
    impl->metadata.mode = mode.mode;
    impl->metadata.debugLayerActive = options.debugLayer;
    impl->metadata.gpuValidationActive = options.gpuValidation;
    impl->metadata.dredActive = options.dred;
    impl->metadata.adapter = adapterMetadata;

    D3D12_FEATURE_DATA_D3D12_OPTIONS optionsData{};
    ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &optionsData, sizeof(optionsData)),
                  "ID3D12Device::CheckFeatureSupport(D3D12_OPTIONS)");
    impl->metadata.features.resourceBindingTier = static_cast<std::uint32_t>(optionsData.ResourceBindingTier);
    impl->metadata.features.tiledResourcesTier = static_cast<std::uint32_t>(optionsData.TiledResourcesTier);
    impl->metadata.features.typedUavLoadAdditionalFormats = optionsData.TypedUAVLoadAdditionalFormats != 0;
    impl->metadata.features.maxGpuVirtualAddressBitsPerResource =
        static_cast<std::uint32_t>(optionsData.MaxGPUVirtualAddressBitsPerResource);

    // SM：请求 6.0，runtime 返回它支持的最高模型；低于 6.0 是硬性 BLOCKED
    // （M5 的 shader 全部走 DXC vs_6_0/ps_6_0，降级 shader 属于"未记录的切换"）。
    // BLOCKED 判定抽为纯函数（ShaderModelBlockReason），可被无 GPU 的单测覆盖。
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
    shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)),
                  "ID3D12Device::CheckFeatureSupport(D3D12_SHADER_MODEL)");
    impl->metadata.features.highestShaderModel = static_cast<std::uint32_t>(shaderModel.HighestShaderModel);
    if (const std::string blockReason = ShaderModelBlockReason(impl->metadata.features.highestShaderModel);
        !blockReason.empty())
    {
        throw std::runtime_error{"BLOCKED: adapter \"" + adapterMetadata.description + "\": " + blockReason};
    }

    // Root Signature：请求 1.1 baseline；runtime 在不支持时以失败返回并填最高支持版本，
    // 因此失败也要读回 HighestVersion 再判定（低于 1.0 才是 BLOCKED）。
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSignature{};
    rootSignature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    const HRESULT rootSignatureHr =
        device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rootSignature, sizeof(rootSignature));
    if (FAILED(rootSignatureHr) && rootSignature.HighestVersion == 0)
    {
        // 连版本号都没填出来：能力查询本身失败，按 BLOCKED 报告而不是猜一个值。
        throw std::runtime_error{"BLOCKED: CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE) failed with HRESULT " +
                                 std::to_string(static_cast<std::int32_t>(rootSignatureHr))};
    }
    impl->metadata.features.rootSignatureHighestVersion = static_cast<std::uint32_t>(rootSignature.HighestVersion);
    if (const std::string blockReason = RootSignatureBlockReason(impl->metadata.features.rootSignatureHighestVersion);
        !blockReason.empty())
    {
        throw std::runtime_error{"BLOCKED: adapter \"" + adapterMetadata.description + "\": " + blockReason};
    }

    // 8) InfoQueue：break-on（仅当有调试器附着，避免无人值守进程被断点挂死）、
    //    清空历史消息（Gate 起点）、为 Device 设置稳定名称。
    if (options.debugLayer)
    {
        ThrowIfFailed(device.As(&impl->infoQueue), "Query ID3D12InfoQueue");
        ThrowIfFailed(impl->infoQueue->SetMessageCountLimit(16384U), "ID3D12InfoQueue::SetMessageCountLimit");
        if (IsDebuggerPresent() != 0)
        {
            ThrowIfFailed(impl->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE),
                          "ID3D12InfoQueue::SetBreakOnSeverity(CORRUPTION)");
            ThrowIfFailed(impl->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE),
                          "ID3D12InfoQueue::SetBreakOnSeverity(ERROR)");
            ThrowIfFailed(impl->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE),
                          "ID3D12InfoQueue::SetBreakOnSeverity(WARNING)");
        }
        impl->infoQueue->ClearStoredMessages();
    }
    Internal::SetDebugName(device.Get(), L"M5.D3D12.Device");

    // 创建期自证日志：验收/取证按这行确认模式与 adapter，不靠肉眼猜测。
    char vendorText[16]{};
    std::snprintf(vendorText, sizeof(vendorText), "0x%04X", static_cast<unsigned>(adapterMetadata.vendorId));
    char smText[16]{};
    std::snprintf(smText, sizeof(smText), "0x%X", static_cast<unsigned>(impl->metadata.features.highestShaderModel));
    MiniEngine::WriteLog(MiniEngine::LogLevel::Info,
                         "d3d12 device created: mode=" + std::to_string(static_cast<int>(mode.mode)) +
                             " debugLayer=" + std::string{options.debugLayer ? "on" : "off"} +
                             " gbv=" + std::string{options.gpuValidation ? "on" : "off"} +
                             " dred=" + std::string{options.dred ? "on" : "off"} +
                             " warp=" + std::string{adapterMetadata.isWarp ? "true" : "false"} + " adapter=\"" +
                             adapterMetadata.description + "\" vendor=" + vendorText + " sm=" + smText);

    return std::unique_ptr<D3D12Device>(new D3D12Device(std::move(impl)));
}

void D3D12Device::UpdateRemovalContext(const std::uint64_t currentFence, const std::uint64_t lastSubmittedFence,
                                       const std::uint64_t completedFence) noexcept
{
    m_impl->removalContext.currentFence = currentFence;
    m_impl->removalContext.lastSubmittedFence = lastSubmittedFence;
    m_impl->removalContext.completedFence = completedFence;
}

void D3D12Device::RecordDiagnosticEvent(const std::string_view eventName)
{
    if (eventName.empty())
        return;
    auto& events = m_impl->removalContext.recentPixEvents;
    events.emplace_back(eventName);
    if (events.size() > 16U)
        events.erase(events.begin());
}
void D3D12Device::UpdateDiagnosticRevisions(const std::string_view assetRevision, const std::string_view shaderRevision)
{
    m_impl->removalContext.assetRevision = assetRevision;
    m_impl->removalContext.shaderRevision = shaderRevision;
}
const DeviceMetadata& D3D12Device::Metadata() const noexcept
{
    return m_impl->metadata;
}

ValidationReport D3D12Device::DrainInfoQueue()
{
    ValidationReport report;
    Internal::DrainInfoQueue(m_impl->infoQueue.Get(), report);
    return report;
}

bool D3D12Device::IsDeviceRemoved() const
{
    return DeviceRemovedReason() != 0;
}

std::int32_t D3D12Device::DeviceRemovedReason() const
{
    // 移除原因在移除后不变，查询一次后缓存：后续帧内反复查询不再产生开销，
    // 也保证日志与 metadata 报告的是同一个值。
    if (!m_impl->removedQueried)
    {
        const HRESULT reason = m_impl->device->GetDeviceRemovedReason();
        m_impl->cachedRemovedReason = static_cast<std::int32_t>(reason);
        // S_OK 只说明当前健康，不能缓存后永久跳过未来的移除查询。
        m_impl->removedQueried = FAILED(reason);
    }
    return m_impl->cachedRemovedReason;
}

void D3D12Device::ReportDeviceRemoved() const
{
    const std::int32_t reason = DeviceRemovedReason();
    if (reason != 0)
    {
        Internal::ReportDeviceRemoved(m_impl->device.Get(), static_cast<HRESULT>(reason), m_impl->removalContext);
    }
}

void* D3D12Device::NativeDeviceHandle() const noexcept
{
    // 不透明句柄：公共头不认识 D3D12 类型，转换责任在调用方（后端内部消费者与
    // 设备级测试），见 D3D12Queue/Renderer 与 tests 处的 static_cast。
    return m_impl->device.Get();
}

void* D3D12Device::NativeFactoryHandle() const noexcept
{
    return m_impl->factory.Get();
}

DxgiLiveReport D3D12Device::ReportLiveDxgiObjects()
{
    DxgiLiveReport report;
    Microsoft::WRL::ComPtr<IDXGIInfoQueue> infoQueue;
    Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&infoQueue))) ||
        FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug))))
        return report;
    ThrowIfFailed(infoQueue->SetMessageCountLimit(DXGI_DEBUG_DXGI, 16384U), "IDXGIInfoQueue::SetMessageCountLimit");
    const auto ensureNoDiscardedMessages = [&infoQueue]()
    {
        const UINT64 discarded = infoQueue->GetNumMessagesDiscardedByMessageCountLimit(DXGI_DEBUG_ALL);
        if (discarded != 0U)
            throw std::runtime_error{"DXGI InfoQueue discarded " + std::to_string(discarded) + " message(s)"};
    };
    const auto collect = [&report, &infoQueue](const char* phase)
    {
        const UINT64 count = infoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T bytes = 0;
            ThrowIfFailed(infoQueue->GetMessage(DXGI_DEBUG_ALL, index, nullptr, &bytes),
                          "IDXGIInfoQueue::GetMessage(size)");
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(storage.data());
            ThrowIfFailed(infoQueue->GetMessage(DXGI_DEBUG_ALL, index, message, &bytes), "IDXGIInfoQueue::GetMessage");
            DxgiDebugMessage entry;
            entry.id = static_cast<std::uint32_t>(message->ID);
            entry.category = static_cast<std::uint32_t>(message->Category);
            entry.severity = static_cast<std::uint32_t>(message->Severity);
            entry.description = message->pDescription != nullptr ? message->pDescription : "<empty>";
            entry.phase = phase;
            entry.isSummary = entry.description.find("Live Object Summary") != std::string::npos;
            const auto firstText = entry.description.find_first_not_of(" \t\r\n");
            entry.isLiveObject = !entry.isSummary && firstText != std::string::npos &&
                                 entry.description.compare(firstText, 5U, "Live ") == 0;
            MiniEngine::WriteLog(MiniEngine::LogLevel::Info,
                                 std::string{"dxgi live report phase="} + entry.phase +
                                     " id=" + std::to_string(entry.id) + " category=" + std::to_string(entry.category) +
                                     " severity=" + std::to_string(entry.severity) + " " + entry.description);
            report.messages.push_back(std::move(entry));
            if (report.messages.back().isLiveObject && report.messages.back().phase == "live-report")
                ++report.liveObjectCount;
        }
    };
    collect("before-live-report");
    ensureNoDiscardedMessages();
    infoQueue->ClearStoredMessages(DXGI_DEBUG_ALL);
    ThrowIfFailed(debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_DETAIL), "IDXGIDebug1::ReportLiveObjects");
    collect("live-report");
    ensureNoDiscardedMessages();
    infoQueue->ClearStoredMessages(DXGI_DEBUG_ALL);
    report.available = true;
    return report;
}
bool D3D12Device::ReportLiveDeviceObjects() const
{
    return Internal::ReportLiveDeviceObjects(m_impl->device.Get());
}
} // namespace MiniEngine::Rhi::D3D12
