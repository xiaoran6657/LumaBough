// ============================================================================
// D3D12Device.h — D3D12 Device 创建契约与诊断基线（M5-02 的对外面）
// 里程碑：M5（02 篇环境、Device 与诊断基线）
// 职责：以固定顺序创建可诊断的 DXGI/D3D12 Device——DRED → Debug Layer → GBV
//       → factory → adapter → device → feature 查询 → InfoQueue。顺序本身就是
//       契约：DRED/Debug Layer/GBV 都是进程级开关，晚于 D3D12CreateDevice 配置
//       会静默失效，因此本模块把"配置过晚"作为显式失败而不是假装已启用。
// 边界约定：本公共头的**代码不出现任何 D3D12/DXGI 类型名**（不含 d3d12.h/dxgi
//       头、不做前置声明；注释中提及接口名以说明契约，不构成类型依赖）。Device/
//       Factory 的跨层桥以 `void*` 不透明句柄表达，与 WindowsWindow::NativeHandle()
//       同款。D3D12/DXGI 类型只存在于 src/ 的实现与后端内部消费者中；组合根
//       （Sandbox）只消费值语义的 metadata 与 ValidationReport。HRESULT 以 32 位
//       原值穿越边界并注明编码。
// 关联：docs/architecture/README.md（创建顺序与模式表）
//       docs/architecture/DECISIONS.md（决策 1/5）
//       docs/architecture/DECISIONS.md（基线锚点）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
// ---------------------------------------------------------------------------
// 运行模式（02 篇「Debug 与运行模式」表）
// ---------------------------------------------------------------------------

// 模式由细粒度开关推导，而不是一个可独立指定的标志：这保证"模式"永远与实际的
// Device 配置一致，不存在"自称 profile 实际开着 GBV"的错位。
// Profile 模式（PIX markers + 无验证）随 M5-11 引入，02 阶段不开放。
enum class DeviceRunMode : std::uint8_t
{
    Debug = 0,  // --d3d12-debug：Debug Layer + DRED，日常正确性
    Gbv = 1,    // --d3d12-debug --gpu-validation：最终验证（会 patch shader，timing 不可作性能结论）
    Release = 2 // 无验证开关：性能基线
};

// Device 创建选项（与 Sandbox 的命令行开关一一对应）。
struct DeviceCreateOptions final
{
    bool debugLayer = false;    // --d3d12-debug：启用 D3D12 Debug Layer
    bool gpuValidation = false; // --gpu-validation：GPU-Based Validation（要求 debugLayer）
    bool dred = false;          // --dred：Device Removed Extended Data 强制开启（要求 debugLayer）
    bool warp = false;          // --warp：明确选择软件适配器（不会静默回退）
    // --adapter-luid=<low>-<high>（每段十进制或 0x 前缀十六进制）。
    // 指定后找不到该 LUID 必须**失败**，绝不回退到默认 adapter——
    // "点名不存在的卡却继续跑"会让性能与 parity 证据指向另一块硬件。
    bool hasAdapterLuid = false;
    std::uint32_t adapterLuidLow = 0;
    std::uint32_t adapterLuidHigh = 0;
};

// 由细粒度开关推导运行模式并校验互斥。
//
// 规则（02 篇模式表的机读版）：
//   - gpuValidation 蕴含 debugLayer（GBV 与调试层联动，单开 GBV 是配置错误）；
//   - dred 蕴含 debugLayer（DRED 数据由调试层产出，单开是无效配置）；
//   - warp 与 adapter-luid 互斥（一个选软件，一个点名硬件）；
//   - gpuValidation 优先于 debugLayer 决定模式名。
//
// 返回：error 非空表示组合非法（调用方必须失败，不得修正后继续）。
struct RunModeResolution final
{
    DeviceRunMode mode = DeviceRunMode::Release;
    std::string error; // 空字符串 = 合法
};

[[nodiscard]] RunModeResolution ResolveRunMode(const DeviceCreateOptions& options);

// 解析 `--adapter-luid` 的值：`<low>-<high>`，每段接受十进制或 0x 前缀十六进制。
//
// 参数：
//   text —— 待解析文本（不含选项名）
//   low  —— 成功时输出 LUID 的 LowPart
//   high —— 成功时输出 LUID 的 HighPart
// 返回：解析成功为 true；格式不符（缺 '-'、段为空、含非法字符、数值超出 32 位）为 false。
[[nodiscard]] bool ParseAdapterLuid(const std::string& text, std::uint32_t& low, std::uint32_t& high);

// BLOCKED 判定的纯函数（02 篇「Feature metadata」：SM6.0 / Root Signature baseline
// 不满足时明确 BLOCKED）。从 Create() 的能力查询中抽出为纯值函数，使其可以被
// 无 GPU 的单测覆盖——判定逻辑本身不应成为"写了但从未在本机执行"的路径。
// 参数取 CheckFeatureSupport 报告的最高支持值（D3D_SHADER_MODEL / D3D_ROOT_SIGNATURE_VERSION 编码）。
// 返回：空字符串 = 支持；非空 = BLOCKED 原因（可直接并入错误文本）。
[[nodiscard]] std::string ShaderModelBlockReason(std::uint32_t highestShaderModel);
[[nodiscard]] std::string RootSignatureBlockReason(std::uint32_t highestRootSignatureVersion);

// ---------------------------------------------------------------------------
// 元数据（adapter / feature / 模式，进入 metadata JSON 与验收记录）
// ---------------------------------------------------------------------------

// 选中 adapter 的身份与显存信息（02 篇「Adapter 选择」第 4 条）。
struct AdapterMetadata final
{
    std::string description; // DXGI_ADAPTER_DESC1::Description（UTF-8）
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t subSysId = 0;
    std::uint32_t revision = 0;
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;
    std::uint32_t luidLow = 0; // LUID 原值；复现实验用 --adapter-luid 传回
    std::uint32_t luidHigh = 0;
    bool isWarp = false; // WARP 必须显式标注，不允许静默软件渲染
};

// Device 能力（02 篇「Feature metadata」清单）。
struct FeatureMetadata final
{
    std::uint32_t resourceBindingTier = 0;         // D3D12_FEATURE_D3D12_OPTIONS.ResourceBindingTier（1..3）
    std::uint32_t tiledResourcesTier = 0;          // 仅记录，不因此扩展 M5
    bool typedUavLoadAdditionalFormats = false;    // 仅记录
    std::uint32_t highestShaderModel = 0;          // D3D_SHADER_MODEL 编码（0x60 = SM6.0）；低于 0x60 即 BLOCKED
    std::uint32_t rootSignatureHighestVersion = 0; // D3D_ROOT_SIGNATURE_VERSION 编码；低于 1_0 即 BLOCKED
    std::uint32_t maxGpuVirtualAddressBitsPerResource = 0; // 仅记录
};

// 创建结果的全量元数据：Sandbox 直接序列化为 JSON，验收按此核对。
struct DeviceMetadata final
{
    DeviceRunMode mode = DeviceRunMode::Release;
    bool debugLayerActive = false; // 实际生效值（请求失败会直接失败，不存在"请求了但没开"）
    bool gpuValidationActive = false;
    bool dredActive = false;
    AdapterMetadata adapter;
    FeatureMetadata features;
};

// ---------------------------------------------------------------------------
// InfoQueue 消息报告（02 篇「InfoQueue」：category/severity/ID/description 全量枚举）
// ---------------------------------------------------------------------------

// D3D12_MESSAGE_SEVERITY 的数值原样保存（CORRUPTION=0、ERROR=1、WARNING=2、
// INFO=3、MESSAGE=4）；不引入"兼容未来"的枚举翻译层（ADR-0006 决策 7）。
struct DxgiDebugMessage final
{
    std::uint32_t id = 0;
    std::uint32_t category = 0;
    std::uint32_t severity = 0;
    std::string description;
    bool isSummary = false;
    bool isLiveObject = false;
    std::string phase;
};

struct DxgiLiveReport final
{
    bool available = false;
    std::vector<DxgiDebugMessage> messages;
    std::size_t liveObjectCount = 0;
};
struct ValidationMessage final
{
    std::uint32_t category = 0;
    std::uint32_t severity = 0;
    std::uint32_t id = 0;
    std::string description;
};

// 一次 Gate 的全部消息。02 篇口径：WARNING 及以上零容忍；
// 只有已证明的系统噪声才允许按 message ID 过滤并写入 ADR，禁止整类 severity 过滤。
struct ValidationReport final
{
    std::vector<ValidationMessage> messages;

    // 判断报告是否触发零容忍失败：任一消息 severity <= WARNING（即非 INFO/MESSAGE）即失败。
    [[nodiscard]] bool HasFailure() const noexcept;
    // 消息条数（便于日志与 JSON 输出）。
    [[nodiscard]] std::size_t Size() const noexcept;
};

// ---------------------------------------------------------------------------
// Device（PImpl：d3d12.h 只出现在 src/）
// ---------------------------------------------------------------------------

class D3D12Device final
{
  public:
    // 按 02 篇固定顺序创建 Device：DRED → Debug Layer → GBV → factory →
    // adapter → device → feature → InfoQueue。内部先调用 ResolveRunMode，
    // 非法组合直接抛出，不做静默修正。
    //
    // 失败：任一步 HRESULT 失败抛 HResultError（文本含操作名与十六进制 HRESULT）；
    // SM6.0 或 Root Signature baseline 不满足时抛 std::runtime_error（文本含
    // "BLOCKED" 与实测能力值）；启用 Debug Layer 但调试层组件缺失时抛
    // HResultError，文本带 Graphics Tools 安装提示（不静默降级）。
    // 返回：就绪的 Device（InfoQueue 已清空，Gate 从零开始计数）。
    static std::unique_ptr<D3D12Device> Create(const DeviceCreateOptions& options);

    ~D3D12Device();
    D3D12Device(const D3D12Device&) = delete;
    D3D12Device& operator=(const D3D12Device&) = delete;

    // 全量元数据（adapter/feature/mode），生命周期与 Device 相同。
    [[nodiscard]] const DeviceMetadata& Metadata() const noexcept;

    // 保存最近一次移除诊断的 fence 口径；由渲染组合根在提交/完成点更新。
    void UpdateRemovalContext(std::uint64_t currentFence, std::uint64_t lastSubmittedFence,
                              std::uint64_t completedFence) noexcept;
    // 保存最近的 PIX/渲染事件，内部只保留最近 16 条。
    void RecordDiagnosticEvent(std::string_view eventName);
    void UpdateDiagnosticRevisions(std::string_view assetRevision, std::string_view shaderRevision);

    // 取走并清空 InfoQueue 的全部已存储消息（02 篇「每个 Gate 起点 Clear，
    // 终点枚举」的终点半边；起点由 Create 结束时的清空承担）。
    //
    // 语义注意：这不是逻辑常量——调用会消费（清空）InfoQueue 队列，因此
    // 特意声明为非 const；两次连续调用中第二次必然得到空报告。
    //
    // 失败：GetMessage 的 HRESULT 失败抛 HResultError（正常路径不会发生）。
    [[nodiscard]] ValidationReport DrainInfoQueue();

    // Device 是否已移除（每次调用查询 GetDeviceRemovedReason 并缓存结果）。
    [[nodiscard]] bool IsDeviceRemoved() const;

    // Device 移除原因：HRESULT 的 32 位原值（如 -2005270521 = 0x887A0006
    // = DXGI_ERROR_DEVICE_REMOVED）；未移除时为 0（S_OK）。
    [[nodiscard]] std::int32_t DeviceRemovedReason() const;

    // 输出 Device 移除诊断（02 篇「Device removed」四步：原因 HRESULT →
    // DRED breadcrumbs → page fault 分配链 → 由调用方停止提交并安全退出）。
    // 未移除时调用是无害 no-op（返回 S_OK 不输出）。
    void ReportDeviceRemoved() const;

    // 后端内部的原始设备桥：返回不透明句柄（实际为 ID3D12Device*），与
    // WindowsWindow::NativeHandle() 同款边界处理——**公共头不出现任何 D3D12/DXGI
    // 类型名**，转换只发生在后端内部消费者（D3D12Queue / D3D12Renderer）与设备级
    // 测试处（见 src/ 内的 static_cast）。句柄生命周期与 Device 相同。
    //
    // 组合根（Sandbox）不得把它当作 API 使用：组合根只经后端门面驱动渲染，
    // 所有 D3D12 操作仍封装在后端边界内（M6 的公共 RHI 面落地后本桥被取代）。
    [[nodiscard]] void* NativeDeviceHandle() const noexcept;

    // 创建本 Device 时使用的 DXGI factory 的不透明句柄（实际为 IDXGIFactory7*），
    // 带与调试层一致的 debug 标志：交换链需要它查询 tearing 能力并创建 swap chain。
    // 同上：只供后端内部模块与设备级测试使用。
    [[nodiscard]] void* NativeFactoryHandle() const noexcept;

    // 打印 live-object 报告（04 篇"退出无 live object"的取证入口）：
    // 经 ID3D12DebugDevice::ReportLiveDeviceObjects(DETAIL) 输出。
    //
    // 返回：真的输出了报告为 true；非 debug 模式（无调试层 → 无该接口）为 false。
    //   返回值必须被上层如实落进 metadata —— "报告是否发出"是事实，不能写成常量
    //   （非 debug 模式写 true 即为不实陈述，M5-04 二次审查 §二.1）。
    [[nodiscard]] bool ReportLiveDeviceObjects() const;
    // 进程级 DXGI live-object 报告；调用时机为所有 Device/Factory 对象释放之后。
    [[nodiscard]] static DxgiLiveReport ReportLiveDxgiObjects();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    explicit D3D12Device(std::unique_ptr<Impl> impl) noexcept;
};
} // namespace MiniEngine::Rhi::D3D12
