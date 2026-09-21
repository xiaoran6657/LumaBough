// ============================================================================
// D3D12DeviceTests.cpp — 模式互斥 / LUID 解析 / 零容忍报告 + 设备级创建 Gate
// 里程碑：M5（02 篇环境、Device 与诊断基线）
// 职责：两层测试。CPU 侧锁定 ResolveRunMode 的互斥规则、ParseAdapterLuid 的
//       严格解析与 ValidationReport 的零容忍判定——这些是"配置自证"的地基，
//       配置错位会让所有取证指向错误的模式。设备侧用真实 GPU 验证创建顺序
//       （DRED→Debug Layer→GBV 先于 D3D12CreateDevice）与 feature 披露，
//       并负向验证"点名的 LUID 不存在必须失败而不是回退"。
// 环境：设备级测试需要本机存在 D3D12 硬件或 WARP；这与仓库既有的 D3D11
//       设备级测试（AssetCache/GpuTimestamp/PipelineStatistics）同一口径。
// 关联：docs/architecture/README.md（本步验证清单）
// ============================================================================
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using MiniEngine::Rhi::D3D12::DeviceCreateOptions;
using MiniEngine::Rhi::D3D12::DeviceRunMode;
using MiniEngine::Rhi::D3D12::ParseAdapterLuid;
using MiniEngine::Rhi::D3D12::ResolveRunMode;
using MiniEngine::Rhi::D3D12::RootSignatureBlockReason;
using MiniEngine::Rhi::D3D12::ShaderModelBlockReason;
using MiniEngine::Rhi::D3D12::ValidationMessage;
using MiniEngine::Rhi::D3D12::ValidationReport;

namespace
{
// 构造一个最小 DeviceCreateOptions 的便捷入口（默认 release 语义）。
DeviceCreateOptions MinimalOptions()
{
    return DeviceCreateOptions{};
}
} // namespace

// 模式推导：开关组合必须唯一确定模式，且蕴含规则不被静默修正。
TEST(D3D12RunModeTests, DerivesModeFromFlags)
{
    EXPECT_EQ(ResolveRunMode(MinimalOptions()).mode, DeviceRunMode::Release);

    DeviceCreateOptions debug = MinimalOptions();
    debug.debugLayer = true;
    EXPECT_EQ(ResolveRunMode(debug).mode, DeviceRunMode::Debug);
    EXPECT_TRUE(ResolveRunMode(debug).error.empty());

    DeviceCreateOptions gbv = debug;
    gbv.gpuValidation = true;
    EXPECT_EQ(ResolveRunMode(gbv).mode, DeviceRunMode::Gbv);
    EXPECT_TRUE(ResolveRunMode(gbv).error.empty());
}

// 互斥与蕴含：非法组合必须给出错误文本，调用方失败而不是"帮忙补开关"。
TEST(D3D12RunModeTests, RejectsInvalidCombinationsWithExplicitError)
{
    DeviceCreateOptions gbvOnly = MinimalOptions();
    gbvOnly.gpuValidation = true;
    const auto gbvResolution = ResolveRunMode(gbvOnly);
    EXPECT_FALSE(gbvResolution.error.empty()) << "GBV 单开（无调试层）必须是显式配置错误";

    DeviceCreateOptions dredOnly = MinimalOptions();
    dredOnly.dred = true;
    const auto dredResolution = ResolveRunMode(dredOnly);
    EXPECT_FALSE(dredResolution.error.empty()) << "DRED 单开（无调试层）必须是显式配置错误";

    DeviceCreateOptions warpAndLuid = MinimalOptions();
    warpAndLuid.warp = true;
    warpAndLuid.hasAdapterLuid = true;
    const auto warpResolution = ResolveRunMode(warpAndLuid);
    EXPECT_FALSE(warpResolution.error.empty()) << "--warp 与 --adapter-luid 互斥";
}

// LUID 解析：只接受 <low>-<high> 的十进制或 0x 十六进制，宽容前缀必须拒绝。
TEST(D3D12AdapterLuidTests, ParsesDecimalAndHexPairsStrictly)
{
    std::uint32_t low = 0;
    std::uint32_t high = 0;

    EXPECT_TRUE(ParseAdapterLuid("123-456", low, high));
    EXPECT_EQ(low, 123U);
    EXPECT_EQ(high, 456U);

    EXPECT_TRUE(ParseAdapterLuid("0x1A2B-0x0000ABCD", low, high));
    EXPECT_EQ(low, 0x1A2BU);
    EXPECT_EQ(high, 0xABCDU);

    EXPECT_TRUE(ParseAdapterLuid("0-0", low, high)) << "全零 LUID 是合法输入（找不到是创建阶段的错误）";

    EXPECT_FALSE(ParseAdapterLuid("123", low, high)) << "缺少 '-' 不是可解析的 LUID";
    EXPECT_FALSE(ParseAdapterLuid("123-", low, high)) << "空的高段必须拒绝";
    EXPECT_FALSE(ParseAdapterLuid("-123", low, high)) << "空的低段必须拒绝";
    EXPECT_FALSE(ParseAdapterLuid("12 3-4", low, high)) << "内嵌空白必须拒绝";
    EXPECT_TRUE(ParseAdapterLuid("018-1", low, high))
        << "前导 0 按十进制解释（解析固定 base=10，不用 strtoul 自动进制）";
    EXPECT_EQ(low, 18U);
    EXPECT_FALSE(ParseAdapterLuid("0b101-1", low, high)) << "0b 前缀不在契约内（0x 之后缀必须拒绝）";
    EXPECT_FALSE(ParseAdapterLuid("xyz-1", low, high)) << "非数字必须拒绝";
    EXPECT_FALSE(ParseAdapterLuid("99999999999-1", low, high)) << "超出 32 位必须拒绝而不是截断";
}

// 零容忍判定：WARNING 及以上失败，INFO/MESSAGE 不失败（02 篇口径）。
TEST(D3D12ValidationReportTests, FailsOnWarningAndAboveOnly)
{
    ValidationReport empty;
    EXPECT_FALSE(empty.HasFailure());
    EXPECT_EQ(empty.Size(), 0U);

    ValidationReport infoOnly;
    infoOnly.messages.push_back(ValidationMessage{0U, 3U, 1U, "info message"}); // INFO
    EXPECT_FALSE(infoOnly.HasFailure()) << "INFO 不触发零容忍失败";

    ValidationReport warning;
    warning.messages.push_back(ValidationMessage{0U, 2U, 2U, "warning message"}); // WARNING
    EXPECT_TRUE(warning.HasFailure());

    ValidationReport corruption;
    corruption.messages.push_back(ValidationMessage{0U, 0U, 3U, "corruption message"}); // CORRUPTION
    EXPECT_TRUE(corruption.HasFailure());
}

// BLOCKED 判定（纯函数）：本机硬件永远支持 SM6.0，Create 里的 BLOCKED 分支
// 不可达；判定逻辑抽为纯值函数后在这里穷举边界，避免"写了但从未执行"（审查意见）。
TEST(D3D12FeatureBlockReasonTests, ShaderModelBlockedBelow60Only)
{
    EXPECT_TRUE(ShaderModelBlockReason(0x60U).empty()) << "SM 6.0 恰好达标";
    EXPECT_TRUE(ShaderModelBlockReason(0x62U).empty()) << "更高模型必须放行（不能要求恰好等于 6.0）";

    const std::string blocked = ShaderModelBlockReason(0x50U);
    EXPECT_NE(blocked.find("shader model"), std::string::npos) << "错误文本必须指明是 shader model 不足";
    EXPECT_NE(blocked.find("0x50"), std::string::npos) << "错误文本必须给出实测值";
}

TEST(D3D12FeatureBlockReasonTests, RootSignatureBlockedBelow10Only)
{
    EXPECT_TRUE(RootSignatureBlockReason(1U).empty());
    EXPECT_TRUE(RootSignatureBlockReason(2U).empty()) << "1.1（编码 2）必须放行";

    const std::string blocked = RootSignatureBlockReason(0U);
    EXPECT_NE(blocked.find("root signature 1.0 baseline"), std::string::npos) << blocked;
}

// ---------------------------------------------------------------------------
// 设备级测试：真实创建路径（GPU/软件适配器皆可）
// ---------------------------------------------------------------------------

// 创建 Gate（debug 模式）：固定顺序 + feature 披露 + InfoQueue 零消息。
TEST(D3D12DeviceDeviceTests, CreateGateWithDebugLayerPassesZeroTolerance)
{
    DeviceCreateOptions options = MinimalOptions();
    options.debugLayer = true;
    options.dred = true;

    const std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device =
        MiniEngine::Rhi::D3D12::D3D12Device::Create(options);

    const MiniEngine::Rhi::D3D12::DeviceMetadata& metadata = device->Metadata();
    EXPECT_EQ(metadata.mode, DeviceRunMode::Debug);
    EXPECT_TRUE(metadata.debugLayerActive);
    EXPECT_TRUE(metadata.dredActive);
    EXPECT_FALSE(metadata.gpuValidationActive);
    EXPECT_FALSE(metadata.adapter.isWarp) << "默认路径必须选择硬件 adapter，绝不静默 WARP";
    EXPECT_FALSE(metadata.adapter.description.empty());
    EXPECT_NE(metadata.adapter.vendorId, 0U) << "硬件 adapter 必有非零 VendorId";

    // Feature 披露：SM6.0 与 Root Signature baseline 是 M5 的硬门槛（BLOCKED 判据）。
    EXPECT_GE(metadata.features.highestShaderModel, 0x60U) << "SM6.0 不满足应 BLOCKED 而不是继续";
    EXPECT_GE(metadata.features.rootSignatureHighestVersion, 1U);
    EXPECT_GE(metadata.features.resourceBindingTier, 1U);
    EXPECT_LE(metadata.features.resourceBindingTier, 3U);

    // 创建 Gate：没有录制任何命令，健康路径必须是零消息（02 篇零容忍）。
    const MiniEngine::Rhi::D3D12::ValidationReport report = device->DrainInfoQueue();
    EXPECT_FALSE(report.HasFailure()) << "创建期出现 WARNING 及以上消息（零容忍失败）";
    EXPECT_TRUE(device->Metadata().debugLayerActive);
}

// GBV 模式：GBV 标志必须真实生效（metadata 自证），且创建 Gate 同样零消息。
TEST(D3D12DeviceDeviceTests, CreateGateWithGpuBasedValidationPasses)
{
    DeviceCreateOptions options = MinimalOptions();
    options.debugLayer = true;
    options.gpuValidation = true;
    options.dred = true;

    const std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device =
        MiniEngine::Rhi::D3D12::D3D12Device::Create(options);

    EXPECT_EQ(device->Metadata().mode, DeviceRunMode::Gbv);
    EXPECT_TRUE(device->Metadata().gpuValidationActive);
    EXPECT_TRUE(device->Metadata().debugLayerActive);
    EXPECT_FALSE(device->DrainInfoQueue().HasFailure());
}

// WARP：显式 opt-in 后 metadata 必须明确 isWarp=true（不允许静默软件渲染）。
TEST(D3D12DeviceDeviceTests, WarpIsExplicitInMetadata)
{
    DeviceCreateOptions options = MinimalOptions();
    options.warp = true;

    const std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device =
        MiniEngine::Rhi::D3D12::D3D12Device::Create(options);

    EXPECT_TRUE(device->Metadata().adapter.isWarp);
    EXPECT_EQ(device->Metadata().mode, DeviceRunMode::Release);
    EXPECT_FALSE(device->Metadata().debugLayerActive);
}

// 负向：点名的 LUID 不存在必须失败（错误文本含候选清单），绝不静默回退默认 adapter。
TEST(D3D12DeviceDeviceTests, UnknownAdapterLuidFailsInsteadOfFallingBack)
{
    DeviceCreateOptions options = MinimalOptions();
    options.hasAdapterLuid = true;
    // 硬件 LUID 的 HighPart 几乎总为 0，LowPart 是持续递增的计数；一个"从未分配"
    // 的巨大 LowPart + HighPart=7 是安全的不存在组合。
    options.adapterLuidLow = 0xDEADBEEFU;
    options.adapterLuidHigh = 7U;

    try
    {
        static_cast<void>(MiniEngine::Rhi::D3D12::D3D12Device::Create(options));
        FAIL() << "点名的 LUID 不存在时必须失败";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message{error.what()};
        EXPECT_NE(message.find("not found"), std::string::npos) << "错误文本必须说明 LUID 未找到：" << message;
        EXPECT_NE(message.find("available:"), std::string::npos) << "错误文本必须列出候选 adapter：" << message;
    }
}

// 负向：非法开关组合在创建前被拒绝（RunModeResolution 的错误不能被 Create 吞掉）。
TEST(D3D12DeviceDeviceTests, InvalidOptionCombinationFailsBeforeDeviceCreation)
{
    DeviceCreateOptions options = MinimalOptions();
    options.gpuValidation = true; // 缺 --d3d12-debug

    EXPECT_THROW(static_cast<void>(MiniEngine::Rhi::D3D12::D3D12Device::Create(options)), std::runtime_error);
}

TEST(D3D12DeviceDeviceTests, DxgiLiveGateDetectsRetainedDeviceThenClearsAfterRelease)
{
    using MiniEngine::Rhi::D3D12::D3D12Device;
    DeviceCreateOptions options;
    options.debugLayer = true;
    auto device = D3D12Device::Create(options);
    const auto retained = D3D12Device::ReportLiveDxgiObjects();
    ASSERT_TRUE(retained.available);
    EXPECT_GT(retained.liveObjectCount, 0U) << "有意保留 Device/Factory 时必须检出 live 对象";
    device.reset();
    const auto released = D3D12Device::ReportLiveDxgiObjects();
    ASSERT_TRUE(released.available);
    EXPECT_EQ(released.liveObjectCount, 0U) << "全部释放后不得把之前快照误判成当前泄漏";
    for (const auto& message : released.messages)
    {
        EXPECT_GT(message.severity, 2U) << message.description;
    }
}
