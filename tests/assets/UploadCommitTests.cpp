// ============================================================================
// UploadCommitTests.cpp — M7-07 上传预算、原子提交与退休
// 里程碑：M7-07（上传预算、热重载与背压）
// 职责（对应 M7-A20）：
//   * fence 未完成不得发布：Uploading 期间旧 revision/旧资源继续可用；
//   * 每帧三约束（bytes / CPU time / 请求数）+ 热重载份额同时生效，失败不计入 usage；
//   * 关闭（含上传中退出）后：上传队列空、在飞空、临时资源全部退休；
//   * 取消/过期/依赖漂移的提交在 fence 之后照样被拒绝，且不留半提交资源；
//   * priority + aging 决策可复现并进 trace；每 30 帧热重载 10,000 帧无半提交状态。
// 测试设计：真实临时目录 + 真实 Registry/BakedReader/解码器 + 真实 loader；
//   只把 GPU 侧换成 FakeUploadSink（可手动完成 fence、可注入创建失败/字节误差），
//   这样预算与提交事务的判定完全不依赖 GPU，也不需要 sleep 竞速。
// 关联：engine/assets/async/include/MiniEngine/Assets/AssetUploadCoordinator.h
//       docs/architecture/README.md
// ============================================================================

#include <MiniEngine/Assets/AssetUploadCoordinator.h>
#include <MiniEngine/Assets/AsyncAssetLoader.h>
#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Tasks/Task.h>
#include <MiniEngine/Tasks/TaskSystem.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace
{
using namespace MiniEngine;
using namespace MiniEngine::Assets;

constexpr std::size_t kMiB = 1024u * 1024u;

std::uint32_t g_testDirCounter = 0;

std::filesystem::path MakeTempDir()
{
    static const std::uint64_t processBase =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    ++g_testDirCounter;
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("MiniEngineUploadCommitTests-" + std::to_string(processBase) + "-" + std::to_string(g_testDirCounter));
    std::filesystem::create_directories(dir);
    return dir;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

Sha256Digest HashText(const std::string& text)
{
    Sha256Builder builder;
    Sha256Digest digest{};
    static_cast<void>(builder.Append(std::as_bytes(std::span{text.data(), text.size()})));
    static_cast<void>(builder.Finish(digest));
    return digest;
}

// 精确 N 字节的 World 产物：assets 层对 World 只做 header 校验并交付原始字节，
// 因此 decoded payload 的 ByteSize == 文件大小（预算算术可复算）。
std::vector<std::byte> MakeWorldArtifact(const Sha256Digest& buildKey, const std::size_t totalBytes)
{
    const std::size_t size = totalBytes < kBakedHeaderSize ? kBakedHeaderSize : totalBytes;
    std::vector<std::byte> bytes(size, std::byte{0U});
    const auto write16 = [&bytes](const std::size_t offset, const std::uint16_t value)
    {
        bytes[offset] = static_cast<std::byte>(value & 0xFFU);
        bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    };
    const auto write32 = [&bytes](const std::size_t offset, const std::uint32_t value)
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
        }
    };
    const auto write64 = [&bytes](const std::size_t offset, const std::uint64_t value)
    {
        for (std::size_t i = 0; i < 8; ++i)
        {
            bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
        }
    };
    bytes[0] = static_cast<std::byte>('M');
    bytes[1] = static_cast<std::byte>('E');
    bytes[2] = static_cast<std::byte>('A');
    bytes[3] = static_cast<std::byte>('3');
    write16(4, kBakedFormatVersion);
    write16(6, static_cast<std::uint16_t>(BakedAssetKind::World));
    write32(8, static_cast<std::uint32_t>(kBakedHeaderSize));
    write32(12, 0); // chunkCount：World 不在此层解析 chunk
    write64(16, static_cast<std::uint64_t>(size));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    // 载荷内容非零，便于将来做内容哈希比对；只影响字节值，不影响尺寸。
    for (std::size_t offset = kBakedHeaderSize; offset < size; ++offset)
    {
        bytes[offset] = static_cast<std::byte>(offset & 0xFFU);
    }
    return bytes;
}

// 手动 decode 执行器：测试决定什么时候执行 decode（FIFO）。
class ManualDecodeDispatcher final : public DecodeDispatcher
{
  public:
    [[nodiscard]] bool SubmitDecode(Tasks::TaskFunction entry, void* context) noexcept override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(Pending{entry, context});
        m_cv.notify_all();
        return true;
    }

    void WaitDecodes() noexcept override
    {
        RunAll();
    }

    void RunAll()
    {
        for (;;)
        {
            Pending task{};
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (m_pending.empty())
                {
                    return;
                }
                task = m_pending.front();
                m_pending.erase(m_pending.begin());
            }
            task.entry(task.context);
        }
    }

  private:
    struct Pending final
    {
        Tasks::TaskFunction entry = nullptr;
        void* context = nullptr;
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Pending> m_pending;
};

// ---------------------------------------------------------------------------
// 假上传端口：可手动完成 fence、可注入创建失败与"实际字节 ≠ 估计字节"
// ---------------------------------------------------------------------------
struct CreatedUpload final
{
    RequestId requestId = 0;
    UploadResourceToken resource = 0;
    std::uint64_t fence = 0;
    std::size_t byteCount = 0;
    AssetKind kind = AssetKind::World;
    std::uint64_t revision = 0;
};

class FakeUploadSink final : public UploadSink
{
  public:
    bool failNextCreate = false;
    std::size_t actualBytesScalePercent = 100; // 100 = 实际上报 == 估计
    bool immediateFence = false;               // true → completionFence = 0（同步后端语义）

    [[nodiscard]] UploadResult CreateAndUpload(const UploadRequestDescription& description) override
    {
        UploadResult result;
        if (failNextCreate)
        {
            failNextCreate = false;
            result.success = false;
            result.errorText = "injected create failure";
            ++createFailures;
            return result;
        }
        result.success = true;
        result.resource = m_nextResource++;
        result.actualBytes = description.byteCount * actualBytesScalePercent / 100u;
        result.completionFence = immediateFence ? 0U : m_nextFence++;
        CreatedUpload created;
        created.requestId = description.requestId;
        created.resource = result.resource;
        created.fence = result.completionFence;
        created.byteCount = description.byteCount;
        created.kind = description.kind;
        created.revision = description.revision;
        m_created.push_back(created);
        m_live.insert(result.resource);
        return result;
    }

    [[nodiscard]] bool IsFenceComplete(const std::uint64_t fence) const noexcept override
    {
        return fence == 0 || m_completed.contains(fence);
    }

    void DeferDestroy(const UploadResourceToken resource) noexcept override
    {
        if (m_live.erase(resource) > 0)
        {
            ++m_deferredCount;
            m_deferred.push_back(resource);
        }
    }

    void ReleaseAll() noexcept override
    {
        m_deferredCount += static_cast<std::uint32_t>(m_live.size());
        for (const UploadResourceToken resource : m_live)
        {
            m_deferred.push_back(resource);
        }
        m_live.clear();
    }

    // ---- 测试驱动 ----
    void CompleteFenceFor(const RequestId request)
    {
        for (const CreatedUpload& created : m_created)
        {
            if (created.requestId == request && created.fence != 0)
            {
                m_completed.insert(created.fence);
                return;
            }
        }
        ADD_FAILURE() << "no fence recorded for request " << request;
    }

    void CompleteAllFences()
    {
        for (const CreatedUpload& created : m_created)
        {
            if (created.fence != 0)
            {
                m_completed.insert(created.fence);
            }
        }
    }

    [[nodiscard]] std::uint64_t FenceFor(const RequestId request) const
    {
        for (const CreatedUpload& created : m_created)
        {
            if (created.requestId == request)
            {
                return created.fence;
            }
        }
        return 0;
    }

    [[nodiscard]] std::size_t LiveResourceCount() const noexcept
    {
        return m_live.size();
    }
    [[nodiscard]] std::uint32_t DeferredCount() const noexcept
    {
        return m_deferredCount;
    }
    [[nodiscard]] const std::vector<CreatedUpload>& Created() const noexcept
    {
        return m_created;
    }

    std::uint32_t createFailures = 0;

  private:
    std::vector<CreatedUpload> m_created;
    std::set<UploadResourceToken> m_live;
    std::set<std::uint64_t> m_completed;
    std::vector<UploadResourceToken> m_deferred;
    UploadResourceToken m_nextResource = 1;
    std::uint64_t m_nextFence = 1;
    std::uint32_t m_deferredCount = 0;
};

// ---------------------------------------------------------------------------
// 夹具：真实依赖 + 假 sink + 协调器
// ---------------------------------------------------------------------------
struct UploadFixture final
{
    std::filesystem::path dir;
    AsyncAssetLoaderConfig config{};
    Sha256Digest buildKey{};
    std::vector<std::string> entryJson;
    std::vector<std::pair<std::string, AssetId>> assets;

    std::unique_ptr<Tasks::TaskSystem> tasks;
    std::unique_ptr<DirectoryCookedFileSource> source;
    std::unique_ptr<ManualDecodeDispatcher> dispatcher;
    std::optional<AssetRegistry> registry;
    std::unique_ptr<AsyncAssetLoader> loader;
    FakeUploadSink sink;
    std::unique_ptr<AssetUploadCoordinator> coordinator;

    UploadFixture() : dir(MakeTempDir()), buildKey(HashText("upload-build-key"))
    {
    }

    ~UploadFixture()
    {
        // 顺序与构造相反：协调器 → loader（I/O 线程与 decode）→ 依赖。
        coordinator.reset();
        loader.reset();
        registry.reset();
        dispatcher.reset();
        source.reset();
        tasks.reset();
    }

    // 写盘一个 totalBytes 字节的 World 产物并登记 manifest 条目。
    AssetId AddWorldArtifact(const std::string& uri, const std::size_t totalBytes)
    {
        const std::vector<std::byte> bytes = MakeWorldArtifact(buildKey, totalBytes);
        Sha256Builder builder;
        Sha256Digest digest{};
        static_cast<void>(builder.Append(std::span<const std::byte>(bytes)));
        static_cast<void>(builder.Finish(digest));
        const std::string artifactPath = "cache/aa/" + ToHexDigest(digest) + "-artifact.bin";
        EXPECT_TRUE(WriteFileBytes(dir / artifactPath, bytes));
        entryJson.push_back("{\"artifactHash\":\"" + ToHexDigest(digest) + "\",\"artifactPath\":\"" + artifactPath +
                            "\",\"assetUri\":\"" + uri + "\",\"buildKey\":\"" + ToHexDigest(buildKey) +
                            "\",\"fileSize\":" + std::to_string(bytes.size()) + ",\"kind\":\"world\"}");
        const AssetId id = DeriveAssetId(uri);
        assets.emplace_back(uri, id);
        return id;
    }

    void Finalize()
    {
        std::string entries;
        for (std::size_t index = 0; index < entryJson.size(); ++index)
        {
            if (index != 0)
            {
                entries += ",";
            }
            entries += entryJson[index];
        }
        const std::string manifest = "{\"assets\":[" + entries + "],\"profile\":\"windows-d3d11\",\"schemaVersion\":1}";
        EXPECT_TRUE(WriteFileBytes(dir / "manifest.json", ToAsciiBytes(manifest)));

        std::string error;
        registry = AssetRegistry::ParseAndValidate(ToAsciiBytes(manifest), dir, error);
        ASSERT_TRUE(registry.has_value()) << error;

        Tasks::TaskSystemConfig taskConfig;
        taskConfig.workerCount = 1;
        tasks = std::make_unique<Tasks::TaskSystem>(taskConfig);
        source = std::make_unique<DirectoryCookedFileSource>(dir);
        dispatcher = std::make_unique<ManualDecodeDispatcher>();
        loader = std::make_unique<AsyncAssetLoader>(config, *tasks, *source, *dispatcher, *registry);
        coordinator = std::make_unique<AssetUploadCoordinator>(*loader, sink, UploadCoordinatorConfig{});
    }

    static std::vector<std::byte> ToAsciiBytes(const std::string& text)
    {
        const auto span = std::as_bytes(std::span{text.data(), text.size()});
        return {span.begin(), span.end()};
    }

    // 请求 + 驱动到 UploadQueued（读 + decode + Pump），不推进上传阶段。
    RequestId QueueUpload(const AssetId& id, const std::uint64_t revision,
                          const LoadPriority priority = LoadPriority::Visible, const std::uint64_t dependencyHash = 0)
    {
        RequestId request = 0;
        const RequestResult result = loader->Request(id, revision, priority, dependencyHash, request);
        EXPECT_TRUE(result == RequestResult::Accepted || result == RequestResult::Coalesced)
            << static_cast<int>(result);
        EXPECT_TRUE(loader->WaitForCpuReady(request, 5000));
        loader->Pump();
        return request;
    }

    // 先登记 N 个产物（Finalize 之前调用），再按需 QueueUploads。
    void AddWorldArtifacts(const std::size_t count, const std::size_t bytesEach)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            static_cast<void>(AddWorldArtifact("world/bulk-" + std::to_string(index), bytesEach));
        }
    }

    void QueueUploads()
    {
        for (const auto& [uri, id] : assets)
        {
            static_cast<void>(QueueUpload(id, 1));
        }
    }

    // 让某资产拥有"已发布资源"（热重载前置）：走完整流水线并提交一次。
    void InstallReadyRevision(const AssetId& id, const std::uint64_t revision)
    {
        const RequestId request = QueueUpload(id, revision);
        sink.immediateFence = true;
        ProcessFrame(HugeBudget());
        RetireCompleted();
        sink.immediateFence = false;
        ASSERT_EQ(loader->PublishedRevision(id), revision);
        ASSERT_EQ(loader->Query(request).value_or(AssetLoadState::Unloaded), AssetLoadState::Ready);
    }

    [[nodiscard]] static UploadBudget HugeBudget()
    {
        UploadBudget budget;
        budget.maxBytes = 256u * kMiB;
        budget.maxCpuTime = std::chrono::milliseconds(1000);
        budget.maxRequests = 256;
        budget.reloadBytesPercent = 100;
        return budget;
    }

    UploadUsage ProcessFrame(const UploadBudget& budget)
    {
        return coordinator->ProcessFrame(budget);
    }

    std::uint32_t RetireCompleted()
    {
        return coordinator->RetireCompletedUploads();
    }

    void CompleteFence(const RequestId request)
    {
        sink.CompleteFenceFor(request);
    }

    void ShutdownAndCompleteGpu()
    {
        coordinator->Shutdown();
        loader->Shutdown(true);
    }

    [[nodiscard]] std::uint64_t CurrentRevision(const AssetId& id) const
    {
        return loader->PublishedRevision(id);
    }
    [[nodiscard]] std::optional<AssetLoadState> State(const RequestId request) const
    {
        return loader->Query(request);
    }
    [[nodiscard]] std::size_t PendingUploads() const
    {
        return loader->PendingUploads().size();
    }
    [[nodiscard]] std::uint32_t InFlightUploads() const
    {
        return coordinator->InFlightUploads();
    }
    [[nodiscard]] std::size_t LiveTemporaryResources() const
    {
        return sink.LiveResourceCount();
    }
    [[nodiscard]] bool OldResourceIsDeferred() const
    {
        return sink.DeferredCount() > 0;
    }
};

// ---------------------------------------------------------------------------
// A20：fence 之后才发布
// ---------------------------------------------------------------------------
TEST(UploadCommitTests, DoesNotPublishBeforeFenceCompletes)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", 4u * kMiB);
    fixture.Finalize();
    fixture.InstallReadyRevision(asset, 7);

    const RequestId request = fixture.QueueUpload(asset, 8);
    const UploadUsage usage = fixture.ProcessFrame(UploadBudget{8u * kMiB, std::chrono::milliseconds(100), 16, 100});

    EXPECT_EQ(usage.requests, 1U);
    EXPECT_EQ(usage.bytes, 4u * kMiB);
    EXPECT_EQ(fixture.CurrentRevision(asset), 7U); // fence 未完成：旧 revision 仍是当前
    EXPECT_EQ(fixture.State(request).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);

    fixture.CompleteFence(request);
    EXPECT_EQ(fixture.RetireCompleted(), 1U);
    EXPECT_EQ(fixture.CurrentRevision(asset), 8U);
    EXPECT_EQ(fixture.State(request).value_or(AssetLoadState::Unloaded), AssetLoadState::Ready);
    EXPECT_TRUE(fixture.OldResourceIsDeferred());
    // 同一事务里换：已发布 revision 有资源，且只剩新资源一份。
    EXPECT_NE(fixture.loader->PublishedResource(asset), 0U);
    EXPECT_EQ(fixture.LiveTemporaryResources(), 1U);
    EXPECT_EQ(fixture.InFlightUploads(), 0U);
}

TEST(UploadCommitTests, BudgetBoundsNormalRequests)
{
    UploadFixture fixture;
    fixture.AddWorldArtifacts(/*count=*/10, /*bytesEach=*/kMiB);
    fixture.Finalize();
    fixture.QueueUploads();

    UploadBudget budget;
    budget.maxBytes = 4u * kMiB;
    budget.maxCpuTime = std::chrono::milliseconds(100);
    budget.maxRequests = 10;
    const UploadUsage usage = fixture.ProcessFrame(budget);

    EXPECT_LE(usage.bytes, 4u * kMiB);
    EXPECT_EQ(usage.requests, 4U);
    EXPECT_EQ(fixture.PendingUploads(), 6U);
    EXPECT_EQ(fixture.InFlightUploads(), 4U);
    EXPECT_GE(fixture.coordinator->Stats().budgetStops, 1U);

    // 下一帧继续：同样的预算再吃 4 个，剩下 2 个。
    const UploadUsage second = fixture.ProcessFrame(budget);
    EXPECT_EQ(second.requests, 4U);
    EXPECT_EQ(fixture.PendingUploads(), 2U);
}

TEST(UploadCommitTests, RequestsPerFrameAndCpuConstraintsAlsoApply)
{
    UploadFixture fixture;
    fixture.AddWorldArtifacts(/*count=*/6, /*bytesEach=*/kMiB);
    fixture.Finalize();
    fixture.QueueUploads();

    UploadBudget budget;
    budget.maxBytes = 64u * kMiB;
    budget.maxCpuTime = std::chrono::milliseconds(100);
    budget.maxRequests = 2; // 请求数是本帧的主导约束
    const UploadUsage usage = fixture.ProcessFrame(budget);

    EXPECT_EQ(usage.requests, 2U);
    EXPECT_EQ(fixture.PendingUploads(), 4U);

    // CPU 时间为 0 的预算：一个请求都进不去（三约束同时生效）。
    UploadBudget noCpu;
    noCpu.maxBytes = 64u * kMiB;
    noCpu.maxCpuTime = std::chrono::microseconds{0};
    noCpu.maxRequests = 8;
    const UploadUsage blocked = fixture.ProcessFrame(noCpu);
    EXPECT_EQ(blocked.requests, 0U);
    EXPECT_EQ(fixture.PendingUploads(), 4U);
}

TEST(UploadCommitTests, FirstRequestMayExceedBudgetAndFailureDoesNotConsumeUsage)
{
    UploadFixture fixture;
    const AssetId large = fixture.AddWorldArtifact("world/large", 16u * kMiB);
    const AssetId small = fixture.AddWorldArtifact("world/small", kMiB);
    const AssetId recovery = fixture.AddWorldArtifact("world/recovery", kMiB);
    fixture.Finalize();

    const RequestId largeRequest = fixture.QueueUpload(large, 1);
    const RequestId smallRequest = fixture.QueueUpload(small, 1);
    static_cast<void>(fixture.QueueUpload(recovery, 1));

    UploadBudget budget;
    budget.maxBytes = 2u * kMiB; // 小于大资产：首个请求例外让它可以被上传
    budget.maxRequests = 8;
    const UploadUsage usage = fixture.ProcessFrame(budget);

    EXPECT_EQ(usage.requests, 1U);
    EXPECT_EQ(usage.bytes, 16u * kMiB);
    EXPECT_EQ(usage.estimatedOverrunBytes, 14u * kMiB);
    EXPECT_GE(fixture.coordinator->Stats().singleRequestOverruns, 1U);
    // 大资产吃满本帧后，其余留到下一帧（不会被饿死）。
    EXPECT_EQ(fixture.PendingUploads(), 2U);
    EXPECT_EQ(fixture.State(largeRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);

    // 创建失败不计入 usage（同一帧的后续候选照常使用剩余预算），失败请求落 Failed。
    fixture.sink.failNextCreate = true;
    const UploadUsage failedFrame =
        fixture.ProcessFrame(UploadBudget{8u * kMiB, std::chrono::milliseconds(100), 8, 100});
    EXPECT_EQ(failedFrame.requests, 1U); // 只剩 recovery 成功
    EXPECT_EQ(failedFrame.bytes, kMiB);
    EXPECT_EQ(fixture.coordinator->Stats().uploadsFailed, 1U);
    EXPECT_TRUE(IsFailureOrCancellation(fixture.State(smallRequest).value_or(AssetLoadState::Unloaded)));
    EXPECT_EQ(fixture.PendingUploads(), 0U);
}

TEST(UploadCommitTests, ShutdownDuringUploadRetiresEverything)
{
    UploadFixture fixture;
    fixture.AddWorldArtifacts(/*count=*/16, /*bytesEach=*/kMiB);
    fixture.Finalize();
    fixture.QueueUploads();
    ASSERT_EQ(fixture.ProcessFrame(UploadBudget{16u * kMiB, std::chrono::milliseconds(100), 16, 100}).requests, 16U);

    fixture.ShutdownAndCompleteGpu();

    EXPECT_EQ(fixture.PendingUploads(), 0U);
    EXPECT_EQ(fixture.InFlightUploads(), 0U);
    EXPECT_EQ(fixture.LiveTemporaryResources(), 0U);
    EXPECT_TRUE(fixture.coordinator->ShutdownCalled());
    EXPECT_GE(fixture.coordinator->Stats().shutdownCancels, 16U);
    // 关闭不发布半完成状态：没有任何资产被标成 Ready。
    for (const auto& [uri, id] : fixture.assets)
    {
        EXPECT_EQ(fixture.CurrentRevision(id), 0U) << uri;
        EXPECT_EQ(fixture.loader->PublishedResource(id), 0U) << uri;
    }
}

TEST(UploadCommitTests, CancelledOrStaleUploadCannotCommitAfterFence)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", kMiB);
    fixture.Finalize();
    fixture.InstallReadyRevision(asset, 7);

    const RequestId request = fixture.QueueUpload(asset, 8);
    ASSERT_EQ(fixture.ProcessFrame(UploadBudget{4u * kMiB, std::chrono::milliseconds(100), 4, 100}).requests, 1U);

    fixture.loader->CancelWaiter(request); // 最后一个 waiter 离开 → 请求取消
    fixture.CompleteFence(request);
    EXPECT_EQ(fixture.RetireCompleted(), 1U);

    EXPECT_EQ(fixture.CurrentRevision(asset), 7U);
    EXPECT_NE(fixture.State(request).value_or(AssetLoadState::Unloaded), AssetLoadState::Ready);
    EXPECT_EQ(fixture.LiveTemporaryResources(), 1U); // 只剩已发布的 rev7 资源
    EXPECT_EQ(fixture.coordinator->Stats().commitsRejected, 1U);
}

TEST(UploadCommitTests, NewerRequestedRevisionBlocksCommitOfOlderUpload)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", kMiB);
    fixture.Finalize();
    fixture.InstallReadyRevision(asset, 7);

    const RequestId older = fixture.QueueUpload(asset, 8);
    ASSERT_EQ(fixture.ProcessFrame(UploadBudget{4u * kMiB, std::chrono::milliseconds(100), 4, 100}).requests, 1U);
    // 应用已经要求 rev9：rev8 的完成不得覆盖它。
    static_cast<void>(fixture.QueueUpload(asset, 9));

    fixture.CompleteFence(older);
    EXPECT_EQ(fixture.RetireCompleted(), 1U);
    EXPECT_EQ(fixture.CurrentRevision(asset), 7U); // rev9 还没提交，rev8 也不得发布
    EXPECT_EQ(fixture.State(older).value_or(AssetLoadState::Unloaded), AssetLoadState::Stale);
}

TEST(UploadCommitTests, DependencyMismatchBlocksCommit)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", kMiB);
    fixture.Finalize();
    fixture.InstallReadyRevision(asset, 7);

    const RequestId request = fixture.QueueUpload(asset, 8, LoadPriority::Visible, /*dependencyHash=*/0xAAAAu);
    ASSERT_EQ(fixture.ProcessFrame(UploadBudget{4u * kMiB, std::chrono::milliseconds(100), 4, 100}).requests, 1U);
    // 依赖组合变了（例如引用的 texture 换了版本）：同一 revision 的声明被更新。
    static_cast<void>(fixture.QueueUpload(asset, 8, LoadPriority::Visible, /*dependencyHash=*/0xBBBBu));

    fixture.CompleteFence(request);
    EXPECT_EQ(fixture.RetireCompleted(), 1U);
    EXPECT_EQ(fixture.CurrentRevision(asset), 7U); // 不发布混合依赖组合
    EXPECT_NE(fixture.State(request).value_or(AssetLoadState::Unloaded), AssetLoadState::Ready);

    // 用更新后的依赖声明重试：这次可以提交。
    const RequestId retry = fixture.QueueUpload(asset, 8, LoadPriority::Visible, /*dependencyHash=*/0xBBBBu);
    ASSERT_EQ(fixture.ProcessFrame(UploadBudget{4u * kMiB, std::chrono::milliseconds(100), 4, 100}).requests, 1U);
    fixture.CompleteFence(retry);
    EXPECT_EQ(fixture.RetireCompleted(), 1U);
    EXPECT_EQ(fixture.CurrentRevision(asset), 8U);
}

// ---------------------------------------------------------------------------
// A20：优先级、aging 与公平份额
// ---------------------------------------------------------------------------
TEST(UploadCommitTests, PriorityOrdersUploadsWithinAFrame)
{
    UploadFixture fixture;
    const AssetId prefetch = fixture.AddWorldArtifact("world/prefetch", kMiB);
    const AssetId visible = fixture.AddWorldArtifact("world/visible", kMiB);
    const AssetId critical = fixture.AddWorldArtifact("world/critical", kMiB);
    fixture.Finalize();

    const RequestId prefetchRequest = fixture.QueueUpload(prefetch, 1, LoadPriority::Prefetch);
    const RequestId visibleRequest = fixture.QueueUpload(visible, 1, LoadPriority::Visible);
    const RequestId criticalRequest = fixture.QueueUpload(critical, 1, LoadPriority::Critical);
    // aging 阈值 10 s 且禁用提升：本帧顺序完全由优先级决定。
    fixture.coordinator = std::make_unique<AssetUploadCoordinator>(
        *fixture.loader, fixture.sink,
        UploadCoordinatorConfig{std::chrono::seconds(10), /*maxAgingSteps=*/0, 512, 128});

    UploadBudget budget;
    budget.maxBytes = 64u * kMiB;
    budget.maxRequests = 1; // 每帧只放行一个：顺序就是优先级顺序
    EXPECT_EQ(fixture.ProcessFrame(budget).requests, 1U);
    EXPECT_EQ(fixture.State(criticalRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);
    EXPECT_EQ(fixture.State(prefetchRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::UploadQueued);

    EXPECT_EQ(fixture.ProcessFrame(budget).requests, 1U);
    EXPECT_EQ(fixture.State(visibleRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);

    EXPECT_EQ(fixture.ProcessFrame(budget).requests, 1U);
    EXPECT_EQ(fixture.State(prefetchRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);
    EXPECT_EQ(fixture.coordinator->Stats().agingPromotions, 0U); // 阈值很大：本帧没有 aging
}

TEST(UploadCommitTests, AgingPreventsStarvationAndIsTraced)
{
    UploadFixture fixture;
    const AssetId oldest = fixture.AddWorldArtifact("world/oldest", kMiB);
    const AssetId newest = fixture.AddWorldArtifact("world/newest", kMiB);
    fixture.Finalize();

    // 先入队的反而是低优先级（Prefetch），后入队的是 Critical。
    const RequestId oldestRequest = fixture.QueueUpload(oldest, 1, LoadPriority::Prefetch);
    const RequestId newestRequest = fixture.QueueUpload(newest, 1, LoadPriority::Critical);
    // aging 阈值 1 ns：任何等待都立即提升到最高档 → 先入队者先被处理（防饥饿）。
    fixture.coordinator = std::make_unique<AssetUploadCoordinator>(
        *fixture.loader, fixture.sink, UploadCoordinatorConfig{std::chrono::nanoseconds(1), 2, 512, 128});

    UploadBudget budget;
    budget.maxBytes = 64u * kMiB;
    budget.maxRequests = 1;
    EXPECT_EQ(fixture.ProcessFrame(budget).requests, 1U);
    EXPECT_EQ(fixture.State(oldestRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);
    EXPECT_EQ(fixture.State(newestRequest).value_or(AssetLoadState::Unloaded), AssetLoadState::UploadQueued);
    EXPECT_GE(fixture.coordinator->Stats().agingPromotions, 1U);

    bool traced = false;
    for (const UploadDecision& decision : fixture.coordinator->Decisions())
    {
        if (decision.reason != nullptr && std::string(decision.reason) == "aging" && decision.agingSteps > 0)
        {
            traced = true;
        }
    }
    EXPECT_TRUE(traced) << "aging 决策必须进 trace";
}

TEST(UploadCommitTests, ReloadShareCannotStarveStreaming)
{
    UploadFixture fixture;
    const AssetId reloadA = fixture.AddWorldArtifact("world/reload-a", 2u * kMiB);
    const AssetId reloadB = fixture.AddWorldArtifact("world/reload-b", 2u * kMiB);
    const AssetId stream = fixture.AddWorldArtifact("world/stream", 2u * kMiB);
    fixture.Finalize();
    fixture.InstallReadyRevision(reloadA, 1);
    fixture.InstallReadyRevision(reloadB, 1);

    // 两个热重载（各 2 MiB）先入队，streaming 请求最后入队。
    const RequestId a = fixture.QueueUpload(reloadA, 2);
    const RequestId b = fixture.QueueUpload(reloadB, 2);
    const RequestId s = fixture.QueueUpload(stream, 1);

    UploadBudget budget;
    budget.maxBytes = 4u * kMiB;
    budget.maxCpuTime = std::chrono::milliseconds(100);
    budget.maxRequests = 16;
    budget.reloadBytesPercent = 50; // 热重载最多 2 MiB/帧
    const UploadUsage usage = fixture.ProcessFrame(budget);

    EXPECT_EQ(usage.bytes, 4u * kMiB);
    EXPECT_EQ(usage.reloadBytes, 2u * kMiB); // 第二个热重载被份额挡下
    EXPECT_EQ(usage.streamBytes, 2u * kMiB); // streaming 拿到自己的份额
    EXPECT_EQ(usage.requests, 2U);
    EXPECT_GE(fixture.coordinator->Stats().fairnessHolds, 1U);
    EXPECT_EQ(fixture.State(a).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);
    EXPECT_EQ(fixture.State(s).value_or(AssetLoadState::Unloaded), AssetLoadState::Uploading);
    EXPECT_EQ(fixture.State(b).value_or(AssetLoadState::Unloaded), AssetLoadState::UploadQueued);
}

TEST(UploadCommitTests, EstimatedVersusActualBytesAreBothRecorded)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", kMiB);
    fixture.Finalize();
    fixture.sink.actualBytesScalePercent = 150; // 实际上传 1.5 MiB

    static_cast<void>(fixture.QueueUpload(asset, 1));
    const UploadUsage usage = fixture.ProcessFrame(UploadBudget{8u * kMiB, std::chrono::milliseconds(100), 8, 100});

    EXPECT_EQ(usage.bytes, kMiB * 3u / 2u);
    const UploadCoordinatorStats stats = fixture.coordinator->Stats();
    EXPECT_EQ(stats.estimatedBytes, kMiB);
    EXPECT_EQ(stats.actualBytes, kMiB * 3u / 2u);
    EXPECT_EQ(stats.estimatedErrorBytes, kMiB / 2u);
}

TEST(UploadCommitTests, EventsAreDispatchedAtFramePointsNotFromWorkers)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", kMiB);
    fixture.Finalize();
    fixture.InstallReadyRevision(asset, 1);
    std::vector<UploadEvent> installEvents;
    static_cast<void>(fixture.coordinator->DrainEvents(installEvents)); // 首次发布的事件先派发掉

    const RequestId request = fixture.QueueUpload(asset, 2);
    ASSERT_EQ(fixture.ProcessFrame(fixture.HugeBudget()).requests, 1U);
    fixture.CompleteFence(request);
    ASSERT_EQ(fixture.RetireCompleted(), 1U);

    std::vector<UploadEvent> events;
    EXPECT_EQ(fixture.coordinator->DrainEvents(events), 1U);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().kind, UploadEventKind::Reloaded);
    EXPECT_EQ(events.front().revision, 2U);
    EXPECT_EQ(events.front().requestId, request);
    EXPECT_NE(events.front().resource, 0U);
    // 派发后队列为空（同一事件不会重复派发）。
    EXPECT_EQ(fixture.coordinator->DrainEvents(events), 0U);
}

// ---------------------------------------------------------------------------
// A20 压力：每 30 帧热重载 10,000 帧，不允许出现半提交资源
// ---------------------------------------------------------------------------
TEST(UploadCommitTests, HotReloadEveryThirtyFramesKeepsTenThousandFramesHalfCommitFree)
{
    UploadFixture fixture;
    const AssetId asset = fixture.AddWorldArtifact("world/level", 64u * 1024u);
    fixture.Finalize();
    fixture.InstallReadyRevision(asset, 1);
    fixture.sink.immediateFence = true; // 等价 D3D11 的同步完成语义：当帧即可提交
    std::vector<UploadEvent> events;
    static_cast<void>(fixture.coordinator->DrainEvents(events)); // 首次发布的事件先派发掉
    events.clear();

    UploadBudget budget = fixture.HugeBudget();
    constexpr std::uint32_t kFrames = 10000;
    constexpr std::uint32_t kCadence = 30;
    std::uint64_t committedRevision = 1;
    std::uint32_t reloads = 0;

    for (std::uint32_t frame = 1; frame <= kFrames; ++frame)
    {
        if (frame % kCadence == 0)
        {
            ++committedRevision;
            static_cast<void>(fixture.QueueUpload(asset, committedRevision));
            ++reloads;
        }
        static_cast<void>(fixture.ProcessFrame(budget));
        fixture.RetireCompleted();

        // 每帧不变量：已发布 revision 与已发布资源必须成对出现（同一事务里换），
        // 且同一资产最多只有一份存活资源（旧的在提交时进入延迟退休）。
        const std::uint64_t published = fixture.CurrentRevision(asset);
        ASSERT_GE(published, 1U);
        ASSERT_NE(fixture.loader->PublishedResource(asset), 0U);
        ASSERT_EQ(fixture.InFlightUploads(), 0U);
        ASSERT_EQ(fixture.LiveTemporaryResources(), 1U);
    }

    EXPECT_EQ(reloads, kFrames / kCadence);
    EXPECT_EQ(fixture.CurrentRevision(asset), committedRevision);
    EXPECT_EQ(fixture.loader->Stats().reloadCommits, reloads);
    EXPECT_EQ(fixture.loader->Stats().commitsRejected, 0U);
    EXPECT_EQ(fixture.coordinator->Stats().eventsDropped, 0U);
    // 事件与提交次数一致：每次热重载恰好一个 Reloaded。
    static_cast<void>(fixture.coordinator->DrainEvents(events));
    ASSERT_EQ(events.size(), reloads);
    for (const UploadEvent& event : events)
    {
        EXPECT_EQ(event.kind, UploadEventKind::Reloaded);
    }
}
} // namespace
