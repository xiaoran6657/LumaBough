// ============================================================================
// D3D12PsoFactoryDeviceTests.cpp — 真 GPU 上的 PSO 创建、缓存与热重载事务
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移）
// 职责：用 **tools/shader_compiler 真实产出的 DXIL**（CMake 在测试前编译）验证：
//       baseline PSO 全部可创建且零调试层消息、相同 key 命中缓存不重复创建、
//       格式表与 PSO desc 一致（通过创建成功 + PassFormats 断言间接证明）、
//       mirrored 是独立对象、热重载事务 Commit 后旧 PSO 走 fence 延迟释放、
//       Abort 保留旧集。
// 为什么用产物而不是在测试里编译 shader：这是"工具 → 运行时"的真实路径；
//       在测试内再编译一次会掩盖"产物缺失/过期"这类问题（08 篇验收要求帧内不编译）。
// 环境：需要 D3D12 硬件或 WARP；需要 MINIENGINE_D3D12_SHADER_DIR 指向编译产物。
// 关联：docs/architecture/README.md（PSO lifecycle / hot reload）
//       tools/shader_compiler（产物的唯一来源）
// ============================================================================
#include "D3D12PsoFactory.h"
#include "D3D12RootSignature.h"

#include <MiniEngine/Rhi/D3D12/D3D12Device.h>

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using MiniEngine::Rhi::D3D12::D3D12PsoFactory;
using MiniEngine::Rhi::D3D12::PassKind;
using MiniEngine::Rhi::D3D12::PsoKey;
using MiniEngine::Rhi::D3D12::PsoShaderSet;

#ifndef MINIENGINE_D3D12_SHADER_DIR
#error "MINIENGINE_D3D12_SHADER_DIR must be defined by the test target (see tests/rhi/d3d12/CMakeLists.txt)"
#endif

namespace
{
// 读取 tools/shader_compiler 产出的 .dxil（文件名约定：<stem>.<entry>.<stage>.dxil）。
std::vector<std::byte> LoadShader(const std::string& fileName)
{
    const std::string path = std::string{MINIENGINE_D3D12_SHADER_DIR} + "/" + fileName;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        throw std::runtime_error{"missing shader artifact: " + path +
                                 " (run MiniEngineShaderCompiler before the tests)"};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size <= 0 || !file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        throw std::runtime_error{"cannot read shader artifact: " + path};
    }
    return bytes;
}

// 一个 pass 的 shader 集（与 PsoShaderSet 对应；Shadow 没有 PS）。
PsoShaderSet LoadPassShaders(const std::string& stem, const bool hasPixelShader)
{
    PsoShaderSet shaders;
    shaders.vertexShader = LoadShader(stem + ".VSMain.vs.dxil");
    if (hasPixelShader)
    {
        shaders.pixelShader = LoadShader(stem + ".PSMain.ps.dxil");
    }
    return shaders;
}

struct PsoHarness final
{
    std::unique_ptr<MiniEngine::Rhi::D3D12::D3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    D3D12PsoFactory factory;

    PsoHarness()
    {
        MiniEngine::Rhi::D3D12::DeviceCreateOptions options;
        options.debugLayer = true;
        device = MiniEngine::Rhi::D3D12::D3D12Device::Create(options);

        MiniEngine::Rhi::D3D12::RootSignatureFacts facts{};
        rootSignature = MiniEngine::Rhi::D3D12::CreateM5RootSignature(
            *static_cast<ID3D12Device*>(device->NativeDeviceHandle()), facts);
        factory.Initialize(*static_cast<ID3D12Device*>(device->NativeDeviceHandle()), *rootSignature.Get(), 0U);
    }

    // 创建当前 baseline 集：Shadow/PbrOpaque/Skybox 各含镜像变体，另含 ToneMap、四个 IBL
    // 生成 pass、TriangleSmoke，共 13 个 PSO。
    void CreateBaselineSet(const std::uint64_t shaderRevision = 0U)
    {
        const auto makeKey = [shaderRevision](const PassKind pass, const bool mirrored)
        {
            PsoKey key;
            key.pass = pass;
            key.mirrored = mirrored;
            key.shaderRevision = shaderRevision;
            key.rootSignatureRevision = 0U;
            return key;
        };

        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::Shadow, false), LoadPassShaders("ShadowDepth", false)));
        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::Shadow, true), LoadPassShaders("ShadowDepth", false)));
        static_cast<void>(
            factory.GetOrCreate(makeKey(PassKind::PbrOpaque, false), LoadPassShaders("PbrForward", true)));
        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::PbrOpaque, true), LoadPassShaders("PbrForward", true)));
        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::Skybox, false), LoadPassShaders("Skybox", true)));
        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::Skybox, true), LoadPassShaders("Skybox", true)));
        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::ToneMap, false), LoadPassShaders("ToneMap", true)));
        static_cast<void>(
            factory.GetOrCreate(makeKey(PassKind::EquirectToCube, false), LoadPassShaders("EquirectToCube", true)));
        static_cast<void>(
            factory.GetOrCreate(makeKey(PassKind::Irradiance, false), LoadPassShaders("IrradianceConvolution", true)));
        static_cast<void>(factory.GetOrCreate(makeKey(PassKind::EnvironmentDownsample, false),
                                              PsoShaderSet{LoadShader("IrradianceConvolution.VSMain.vs.dxil"),
                                                           LoadShader("IrradianceConvolution.PSDownsample.ps.dxil")}));
        static_cast<void>(
            factory.GetOrCreate(makeKey(PassKind::Prefilter, false), LoadPassShaders("PrefilterEnvironment", true)));
        static_cast<void>(
            factory.GetOrCreate(makeKey(PassKind::BrdfLut, false), LoadPassShaders("IntegrateBrdf", true)));
        static_cast<void>(
            factory.GetOrCreate(makeKey(PassKind::TriangleSmoke, false), LoadPassShaders("TriangleSmoke", true)));
    }
};
} // namespace

// baseline 集：13 个 PSO 全部创建成功、pending 为 0、零调试层消息。
TEST(D3D12PsoFactoryDeviceTests, CreatesBaselineSetWithZeroDebugMessages)
{
    PsoHarness harness;
    harness.CreateBaselineSet();

    EXPECT_EQ(harness.factory.CachedPsoCount(), 13U) << "9 个 pass，其中 Shadow/Pbr/Skybox 各含镜像变体";
    EXPECT_EQ(harness.factory.PendingPsoCount(), 0U) << "baseline 前 pendingPsoCount 必须为 0";
    EXPECT_EQ(harness.factory.Stats().created, 13U);
    EXPECT_EQ(harness.factory.Stats().failedCreations, 0U);
    EXPECT_GT(harness.factory.Stats().creationMicroseconds, 0U) << "08 篇要求记录 PSO creation 时间";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure()) << "PSO 创建不得产生调试层消息";
}

// 相同 key 必须命中缓存：同一个指针、created 不增长。
TEST(D3D12PsoFactoryDeviceTests, IdenticalKeyHitsTheCache)
{
    PsoHarness harness;
    const PsoShaderSet shaders = LoadPassShaders("ToneMap", true);
    PsoKey key;
    key.pass = PassKind::ToneMap;

    ID3D12PipelineState& first = harness.factory.GetOrCreate(key, shaders);
    const std::uint64_t createdAfterFirst = harness.factory.Stats().created;
    ID3D12PipelineState& second = harness.factory.GetOrCreate(key, shaders);

    EXPECT_EQ(&first, &second) << "同一 key 必须返回同一对象";
    EXPECT_EQ(harness.factory.Stats().created, createdAfterFirst) << "命中缓存不得再创建";
    EXPECT_GE(harness.factory.Stats().cacheHits, 1U);
    EXPECT_EQ(harness.factory.CachedPsoCount(), 1U);
}

// mirrored 与 normal 必须是两个独立 PSO（08 篇：镜像用独立 winding 的 PSO）。
TEST(D3D12PsoFactoryDeviceTests, MirroredUsesASeparatePipelineState)
{
    PsoHarness harness;
    const PsoShaderSet shaders = LoadPassShaders("PbrForward", true);

    PsoKey normal;
    normal.pass = PassKind::PbrOpaque;
    PsoKey mirrored = normal;
    mirrored.mirrored = true;

    ID3D12PipelineState& normalPso = harness.factory.GetOrCreate(normal, shaders);
    ID3D12PipelineState& mirroredPso = harness.factory.GetOrCreate(mirrored, shaders);

    EXPECT_NE(&normalPso, &mirroredPso) << "mirrored 必须是独立对象（不能在 draw 前改状态）";
    EXPECT_EQ(harness.factory.CachedPsoCount(), 2U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 创建失败必须显式抛出并带上定位信息（08 篇：Create 失败保留 HRESULT）。
// D3D12 没有 PSO 序列化错误 blob，因此异常文本 + 调试层输出就是唯一的诊断来源。
TEST(D3D12PsoFactoryDeviceTests, FailedCreationIsReportedWithPassContext)
{
    PsoHarness harness;

    PsoKey key;
    key.pass = PassKind::ToneMap;

    PsoShaderSet broken;
    broken.vertexShader = std::vector<std::byte>(64U, std::byte{0xAB}); // 非法的"字节码"
    broken.pixelShader = std::vector<std::byte>(64U, std::byte{0xAB});

    std::string message;
    bool threw = false;
    try
    {
        static_cast<void>(harness.factory.GetOrCreate(key, broken));
    }
    catch (const std::exception& exception)
    {
        threw = true;
        message = exception.what();
    }
    EXPECT_TRUE(threw) << "非法字节码必须失败，不允许静默给出一个坏 PSO";
    EXPECT_NE(message.find("CreateGraphicsPipelineState failed"), std::string::npos) << message;
    EXPECT_NE(message.find("HRESULT"), std::string::npos) << "异常文本必须带 HRESULT：" << message;
    EXPECT_NE(message.find("ToneMap"), std::string::npos) << "异常文本必须带 pass 名：" << message;
    EXPECT_EQ(harness.factory.Stats().failedCreations, 1U);
    EXPECT_EQ(harness.factory.CachedPsoCount(), 0U) << "失败的创建不得进入缓存";

    // 空 VS 在参数校验阶段就被拒绝（不进 D3D），与"创建失败"是两条不同的路径。
    PsoShaderSet empty;
    EXPECT_THROW(static_cast<void>(harness.factory.GetOrCreate(key, empty)), std::invalid_argument);
}

// 热重载事务：Commit 后新集生效，旧 PSO 挂 fence 延迟释放（fence 前不释放）。
TEST(D3D12PsoFactoryDeviceTests, HotReloadCommitRetiresOldPsosByFence)
{
    PsoHarness harness;
    harness.CreateBaselineSet(0U);
    const std::size_t baselineCount = harness.factory.CachedPsoCount();

    harness.factory.BeginHotReload(1U);
    EXPECT_TRUE(harness.factory.IsHotReloadPending());
    harness.CreateBaselineSet(1U); // 同一批 pass，但 shaderRevision = 1 → 全新 key
    EXPECT_EQ(harness.factory.PendingPsoCount(), baselineCount);
    EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount) << "提交前旧集必须仍然有效";

    harness.factory.CommitHotReload(10U);
    EXPECT_FALSE(harness.factory.IsHotReloadPending());
    EXPECT_EQ(harness.factory.PendingPsoCount(), 0U);
    EXPECT_EQ(harness.factory.DeferredRelease().PendingCount(), baselineCount) << "旧 PSO 全部进入延迟释放队列";
    // 关键不变量（审查 F-1）：Commit 是整体替换，当前集必须**恰好**等于替换集。
    // 修复前这里是 2 × baselineCount——旧 key 以 pipelineState==nullptr 的空条目形式残留。
    EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount) << "Commit 后不得残留旧 key 的空条目";

    static_cast<void>(harness.factory.DeferredRelease().Reclaim(9U));
    EXPECT_EQ(harness.factory.DeferredRelease().PendingCount(), baselineCount) << "fence 未完成不得释放";
    static_cast<void>(harness.factory.DeferredRelease().Reclaim(10U));
    EXPECT_EQ(harness.factory.DeferredRelease().PendingCount(), 0U) << "fence 完成后全部释放";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 连续多轮热重载：当前集大小必须**每轮都回到 baseline**（审查 F-1 的判别性用例）。
// 修复前每轮都会把上一轮 key 的空条目留在 m_current 里，CachedPsoCount 逐轮 +baseline
// （13 → 26 → 39），同时第二次起每轮多出 baseline 条虚假 "pso retired" 日志。
TEST(D3D12PsoFactoryDeviceTests, RepeatedHotReloadKeepsCachedCountStable)
{
    PsoHarness harness;
    harness.CreateBaselineSet(0U);
    const std::size_t baselineCount = harness.factory.CachedPsoCount();
    ASSERT_EQ(baselineCount, 13U) << "当前 baseline 必须包含全部 13 个 key";

    std::uint64_t retireFence = 10U;
    for (std::uint64_t cycle = 1U; cycle <= 3U; ++cycle)
    {
        harness.factory.BeginHotReload(cycle);
        harness.CreateBaselineSet(cycle);
        EXPECT_EQ(harness.factory.PendingPsoCount(), baselineCount) << "第 " << cycle << " 轮替换集必须完整";

        harness.factory.CommitHotReload(retireFence);
        EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount)
            << "第 " << cycle << " 轮 Commit 后当前集膨胀（旧 key 未移除）";
        EXPECT_EQ(harness.factory.RetiredPendingCount(), baselineCount) << "第 " << cycle << " 轮旧集全部延迟释放";

        static_cast<void>(harness.factory.DeferredRelease().Reclaim(retireFence));
        EXPECT_EQ(harness.factory.RetiredPendingCount(), 0U) << "第 " << cycle << " 轮 fence 完成后全部回收";
        retireFence += 10U;
    }

    // 已退休 revision 的 key 不得以 null 形式留在缓存里：查询它必须走"未命中 → 创建"的正常
    // 路径。修复前这里会命中空条目，`GetOrCreate` 解引用 nullptr（未定义行为）。
    //
    // 依赖声明（审查 N-1 的下游护栏）：这条语义意味着**拿着已退休 revision 的调用方不会被
    // 工厂告警**，而是静默创建。执法点在调用方而不在工厂——Sandbox 结算
    // `pso.unexpectedCreations != 0` 时直接以退出码 3 失败
    // （`samples/sandbox_d3d12/main.cpp` 的 RunPresentation）。工厂只保证"不返回 nullptr"。
    PsoKey staleKey;
    staleKey.pass = PassKind::ToneMap;
    staleKey.shaderRevision = 0U; // baseline 那一轮的 key，已被三轮替换全部退休
    const std::uint64_t createdBefore = harness.factory.Stats().created;
    ID3D12PipelineState& recreated = harness.factory.GetOrCreate(staleKey, LoadPassShaders("ToneMap", true));
    // 不写 `EXPECT_NE(&recreated, nullptr)`：引用不可能为空，该断言恒真、零判别力（审查已指出）。
    // 有判别力的是下面两条——"这次查询走了创建路径"与"新对象接管了该 key"。
    EXPECT_EQ(harness.factory.Stats().created, createdBefore + 1U) << "旧 key 已移除，查询必须走创建路径";
    EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount + 1U);
    EXPECT_EQ(&harness.factory.GetOrCreate(staleKey, LoadPassShaders("ToneMap", true)), &recreated)
        << "新建的 PSO 必须接管该 key（再次查询命中同一对象）";
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 被覆盖的同名 key 旧 PSO 也必须走 fence 延迟释放（审查 P3-2 的判别性用例）：
// 生产路径 revision 严格递增、key 全不重叠，因此这个情形只能由"revision 没变却发起
// 事务"构造——但它必须同样安全（覆盖 map 条目会立即析构旧 ComPtr，而旧 PSO 可能
// 仍被在飞命令列表引用）。修复前本用例的 RetiredPendingCount 是 0。
TEST(D3D12PsoFactoryDeviceTests, OverwrittenKeysAreAlsoRetiredByFence)
{
    PsoHarness harness;

    PsoKey key;
    key.pass = PassKind::ToneMap;
    const PsoShaderSet shaders = LoadPassShaders("ToneMap", true);

    static_cast<void>(harness.factory.GetOrCreate(key, shaders));
    EXPECT_EQ(harness.factory.CachedPsoCount(), 1U);

    harness.factory.BeginHotReload(0U); // 同一 revision：pending 的 key 与 current 完全相同
    static_cast<void>(harness.factory.GetOrCreate(key, shaders));
    EXPECT_EQ(harness.factory.PendingPsoCount(), 1U);
    EXPECT_EQ(harness.factory.CachedPsoCount(), 1U);

    harness.factory.CommitHotReload(10U);
    EXPECT_EQ(harness.factory.RetiredPendingCount(), 1U) << "被覆盖的旧 PSO 不得同步释放，必须进延迟释放队列";
    EXPECT_EQ(harness.factory.CachedPsoCount(), 1U) << "新对象接管该 key";

    static_cast<void>(harness.factory.DeferredRelease().Reclaim(9U));
    EXPECT_EQ(harness.factory.RetiredPendingCount(), 1U) << "fence 未完成不得释放";
    static_cast<void>(harness.factory.DeferredRelease().Reclaim(10U));
    EXPECT_EQ(harness.factory.RetiredPendingCount(), 0U);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}

// 热重载失败：Abort 保留旧集，且不产生任何延迟释放项。
TEST(D3D12PsoFactoryDeviceTests, HotReloadAbortKeepsTheOldSet)
{
    PsoHarness harness;
    harness.CreateBaselineSet(0U);
    const std::size_t baselineCount = harness.factory.CachedPsoCount();

    harness.factory.BeginHotReload(1U);
    harness.CreateBaselineSet(1U);
    harness.factory.AbortHotReload();

    EXPECT_FALSE(harness.factory.IsHotReloadPending());
    EXPECT_EQ(harness.factory.PendingPsoCount(), 0U);
    EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount) << "旧集完好";
    EXPECT_EQ(harness.factory.DeferredRelease().PendingCount(), 0U) << "Abort 不应产生延迟释放";

    // 旧集仍可使用（再次 GetOrCreate 命中缓存）。
    PsoKey key;
    key.pass = PassKind::ToneMap;
    const std::uint64_t createdBefore = harness.factory.Stats().created;
    static_cast<void>(harness.factory.GetOrCreate(key, LoadPassShaders("ToneMap", true)));
    EXPECT_EQ(harness.factory.Stats().created, createdBefore) << "旧集命中缓存，未重新创建";
}

// 空替换集必须被拒绝（审查 N-1）：`Begin` 之后不创建任何 PSO 就 Commit，在
// "Commit 后当前集 == 替换集"的语义下会把当前集**清空**（CachedPsoCount()==0）。
// 修复前该情形留下 baselineCount 个空条目——同样是坏状态，但"看起来还有 13 个"，
// 更难被 API 使用者察觉，所以这里要求显式失败而不是安静降级。
TEST(D3D12PsoFactoryDeviceTests, CommitHotReloadRejectsAnEmptyReplacementSet)
{
    PsoHarness harness;
    harness.CreateBaselineSet(0U);
    const std::size_t baselineCount = harness.factory.CachedPsoCount();
    ASSERT_EQ(baselineCount, 13U);

    harness.factory.BeginHotReload(1U);
    // 故意不创建任何 PSO：空替换集不是合法事务。
    EXPECT_THROW(harness.factory.CommitHotReload(10U), std::logic_error);
    EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount) << "失败的事务不得清空当前集";
    EXPECT_EQ(harness.factory.DeferredRelease().PendingCount(), 0U) << "失败的事务不得退休任何旧 PSO";
    EXPECT_TRUE(harness.factory.IsHotReloadPending()) << "抛出不改变事务状态，调用方可补齐或 Abort";

    // 补齐替换集后，同一事务可以正常提交（拒绝的是空集，不是这次事务）。
    harness.CreateBaselineSet(1U);
    harness.factory.CommitHotReload(10U);
    EXPECT_EQ(harness.factory.CachedPsoCount(), baselineCount) << "补齐后提交仍是整体替换";
    EXPECT_EQ(harness.factory.RetiredPendingCount(), baselineCount);
    EXPECT_FALSE(harness.device->DrainInfoQueue().HasFailure());
}
