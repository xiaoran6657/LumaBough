// ============================================================================
// AsyncAssetLoaderTests.cpp — M7-06 异步加载流水线的去重/取消/失败/乱序/背压
// 里程碑：M7-06（异步资产加载流水线）
// 职责（对应 M7-A18/A19）：
//   * 合并：同 (AssetId, revision) 只读一次、只 decode 一次；Ready 直接复用；
//   * 乱序：高 revision 先完成时，旧请求必须落 Stale，PublishedRevision 不被覆盖；
//   * 失败：文件缺失/0 字节/截断头/截断载荷/hash 不符/schema 不支持/尺寸超限/
//           decode 被拒/读取消/上传前取消——每一种都必须保留旧 Ready 资源；
//   * 背压：I/O 准入（请求数）、CpuReady 停车区、上传队列三处满载都给出可复现结果；
//   * 线程归属：I/O 只发生在专用线程（用可闸门来源证明），decode 走执行器端口。
// 测试设计：真实临时目录 + 真实 Registry/BakedReader/解码器（不 mock 生产逻辑），
//   只把两个"环境"换成可控实现：CookedFileSource（闸门/故障）与 DecodeDispatcher
//   （手动驱动完成顺序）。这样既不牺牲覆盖面，也不依赖 sleep 竞速。
// 关联：engine/assets/async/include/MiniEngine/Assets/AsyncAssetLoader.h
//       docs/architecture/README.md「第 1/5 步」
// ============================================================================

#include <MiniEngine/Assets/AsyncAssetLoader.h>
#include <MiniEngine/Assets/TaskSystemDecodeDispatcher.h>

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/PbrVertex.h> // kMeshFormatVersion（.memesh v2 的 flags 载波）
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Tasks/Task.h> // Tasks::TaskFunction
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
#include <thread>
#include <vector>

namespace
{
using namespace MiniEngine;
using namespace MiniEngine::Assets;

std::uint32_t g_testDirCounter = 0;

std::filesystem::path MakeTempAssetDir()
{
    static const std::uint64_t processBase =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    ++g_testDirCounter;
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("MiniEngineAsyncAssetTests-" + std::to_string(processBase) + "-" + std::to_string(g_testDirCounter));
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

std::vector<std::byte> ToBytes(const std::string& text)
{
    const auto span = std::as_bytes(std::span{text.data(), text.size()});
    return {span.begin(), span.end()};
}

// 最小合法 .memesh：64 字节 header（+ 可选 1 字节 payload）。与 AssetManagerTests
// 的构造器同源：wire 布局无 padding，因此逐偏移写。
std::vector<std::byte> MakeMeshArtifact(const Sha256Digest& buildKey, const std::byte payloadTag,
                                        const std::uint32_t flags = kMeshFormatVersion, const bool headerOnly = false)
{
    const std::size_t size = headerOnly ? kBakedHeaderSize : kBakedHeaderSize + 1;
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
    write16(6, static_cast<std::uint16_t>(BakedAssetKind::Mesh));
    write32(8, static_cast<std::uint32_t>(kBakedHeaderSize));
    write32(12, 0); // chunkCount：M3 阶段的 header-only 约定
    write64(16, static_cast<std::uint64_t>(size));
    std::copy(buildKey.begin(), buildKey.end(), bytes.begin() + 24);
    write32(56, flags);
    write32(60, 0);
    if (!headerOnly)
    {
        bytes[kBakedHeaderSize] = payloadTag;
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// 可闸门 / 可故障注入的真实目录来源（I/O 线程调用）
// ---------------------------------------------------------------------------
class TestFileSource final : public CookedFileSource
{
  public:
    explicit TestFileSource(std::filesystem::path root) : m_real(std::move(root))
    {
    }

    // 让某个相对路径的下一次读取阻塞，直到 Release()（制造"读中取消"与确定性顺序）。
    void GateRead(const std::string& relativePath)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_gated.insert(relativePath);
        m_released = false;
        m_blocked = 0; // 每次重新布闸都从头计"已阻塞"
    }

    void ReleaseGate()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_cv.notify_all();
    }

    // 让某个路径的读取直接返回指定错误（模拟权限/沙箱等来源级失败）。
    void ForceError(const std::string& relativePath, const AssetLoadErrorCode code)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_forcedErrors[relativePath] = code;
    }

    // 等待"某个被闸门的读取已经开始"（证明阻塞发生在 I/O 线程，而不是调用线程）。
    bool WaitUntilBlocked(const std::uint32_t timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return m_blocked > 0; });
    }

    [[nodiscard]] std::uint32_t ReadCount() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_reads;
    }

    CookedFileRead Read(const std::string& cookedRelativePath, const std::size_t maxBytes) override
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_reads;
            if (const auto forced = m_forcedErrors.find(cookedRelativePath); forced != m_forcedErrors.end())
            {
                CookedFileRead result;
                result.error = forced->second;
                result.errorText = "forced source error";
                return result;
            }
            if (m_gated.contains(cookedRelativePath) && !m_released)
            {
                ++m_blocked;
                m_cv.notify_all();
                m_cv.wait(lock,
                          [this, &cookedRelativePath] { return m_released || !m_gated.contains(cookedRelativePath); });
            }
        }
        return m_real.Read(cookedRelativePath, maxBytes);
    }

  private:
    DirectoryCookedFileSource m_real;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::set<std::string> m_gated;
    std::map<std::string, AssetLoadErrorCode> m_forcedErrors;
    bool m_released = false;
    std::uint32_t m_reads = 0;
    std::uint32_t m_blocked = 0;
};

// ---------------------------------------------------------------------------
// 手动 decode 执行器：测试决定"什么时候执行 decode"（FIFO），可拒绝提交
// ---------------------------------------------------------------------------
class ManualDecodeDispatcher final : public DecodeDispatcher
{
  public:
    [[nodiscard]] bool SubmitDecode(Tasks::TaskFunction entry, void* context) noexcept override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_rejectSubmissions)
        {
            return false;
        }
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

    void SetRejectSubmissions(const bool reject)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_rejectSubmissions = reject;
    }

    [[nodiscard]] std::size_t PendingCount() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending.size();
    }

    bool WaitForPending(const std::size_t expected, const std::uint32_t timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [this, expected] { return m_pending.size() >= expected; });
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
    bool m_rejectSubmissions = false;
};

// ---------------------------------------------------------------------------
// 夹具：真实目录 + 真实 Registry + 可控 I/O 与 decode 执行器
// ---------------------------------------------------------------------------
struct PublishedArtifact final
{
    std::string uri;
    AssetId id{};
    std::string relativePath;
};

struct AsyncLoaderFixture final
{
    std::filesystem::path dir;
    AsyncAssetLoaderConfig config{};
    std::uint32_t workerCount = 1;
    Sha256Digest buildKey{};

    std::vector<std::string> entryJson;
    std::vector<PublishedArtifact> published;

    std::unique_ptr<MiniEngine::Tasks::TaskSystem> tasks;
    std::unique_ptr<TestFileSource> source;
    std::unique_ptr<ManualDecodeDispatcher> dispatcher;
    std::optional<AssetRegistry> registry;
    std::unique_ptr<AsyncAssetLoader> loader;

    explicit AsyncLoaderFixture(const AsyncAssetLoaderConfig& cfg = {}, const std::uint32_t workers = 1)
        : dir(MakeTempAssetDir()), config(cfg), workerCount(workers), buildKey(HashText("mesh-build-key"))
    {
    }

    ~AsyncLoaderFixture()
    {
        // 顺序与构造相反：loader（I/O 线程 + decode）先关，再撤依赖。
        loader.reset();
        registry.reset();
        dispatcher.reset();
        source.reset();
        tasks.reset();
    }

    // 写盘一个 mesh artifact 并登记 manifest 条目（hash 与 fileSize 由真实字节计算）。
    PublishedArtifact AddMeshArtifact(const std::string& uri, const std::byte tag,
                                      const std::uint32_t flags = kMeshFormatVersion, const bool headerOnly = false)
    {
        return AddArtifact(uri, AssetKind::Mesh, MakeMeshArtifact(buildKey, tag, flags, headerOnly));
    }

    PublishedArtifact AddArtifact(const std::string& uri, const AssetKind kind, std::vector<std::byte> bytes)
    {
        const std::string artifactPath = "cache/aa/" + ToHexDigest(artifactHashOf(bytes)) + "-artifact.bin";
        EXPECT_TRUE(WriteFileBytes(dir / artifactPath, bytes));
        const std::string kindText = kind == AssetKind::Mesh      ? "mesh"
                                     : kind == AssetKind::Texture ? "texture"
                                     : kind == AssetKind::World   ? "world"
                                                                  : "material";
        entryJson.push_back("{\"artifactHash\":\"" + ToHexDigest(artifactHashOf(bytes)) + "\",\"artifactPath\":\"" +
                            artifactPath + "\",\"assetUri\":\"" + uri + "\",\"buildKey\":\"" + ToHexDigest(buildKey) +
                            "\",\"fileSize\":" + std::to_string(bytes.size()) + ",\"kind\":\"" + kindText + "\"}");
        published.push_back(PublishedArtifact{uri, DeriveAssetId(uri), artifactPath});
        return published.back();
    }

    // 写 manifest → 解析 Registry → 装配 loader（所有依赖齐全后才建立 loader）。
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
        EXPECT_TRUE(WriteFileBytes(dir / "manifest.json", ToBytes(manifest)));

        std::string error;
        registry = AssetRegistry::ParseAndValidate(ToBytes(manifest), dir, error);
        ASSERT_TRUE(registry.has_value()) << error;

        MiniEngine::Tasks::TaskSystemConfig taskConfig;
        taskConfig.workerCount = workerCount;
        tasks = std::make_unique<MiniEngine::Tasks::TaskSystem>(taskConfig);
        source = std::make_unique<TestFileSource>(dir);
        dispatcher = std::make_unique<ManualDecodeDispatcher>();
        loader = std::make_unique<AsyncAssetLoader>(config, *tasks, *source, *dispatcher, *registry);
    }

    [[nodiscard]] PublishedArtifact Artifact(const std::string& uri) const
    {
        static const PublishedArtifact kEmpty{};
        for (const PublishedArtifact& artifact : published)
        {
            if (artifact.uri == uri)
            {
                return artifact;
            }
        }
        ADD_FAILURE() << "unknown artifact " << uri;
        return kEmpty;
    }

    [[nodiscard]] RequestId Request(const AssetId& id, const std::uint64_t revision,
                                    const LoadPriority priority = LoadPriority::Visible)
    {
        RequestId request = 0;
        const RequestResult result = loader->Request(id, revision, priority, request);
        EXPECT_TRUE(result == RequestResult::Accepted || result == RequestResult::Coalesced)
            << static_cast<int>(result);
        return request;
    }

    // 驱动一个请求走完整条流水线（含上传）：用于"先让旧资源 Ready"的前置。
    bool RunToReady(const RequestId request)
    {
        if (!loader->WaitForCpuReady(request, 5000))
        {
            return false;
        }
        for (int guard = 0; guard < 64; ++guard)
        {
            const auto state = loader->Query(request);
            if (!state.has_value())
            {
                return false;
            }
            if (*state == AssetLoadState::Ready)
            {
                return true;
            }
            if (IsFailureOrCancellation(*state))
            {
                return false;
            }
            loader->Pump();
            const auto ticket = loader->BeginNextUpload();
            if (!ticket.has_value())
            {
                return false;
            }
            loader->CompleteUpload(ticket->requestId, true);
        }
        return false;
    }

    // 驱动到终态（失败用例：失败可能发生在任一阶段）。
    bool RunUntilTerminal(const RequestId request, const std::uint32_t timeoutMs = 5000)
    {
        for (int guard = 0; guard < 256; ++guard)
        {
            const auto state = loader->Query(request);
            if (!state.has_value())
            {
                return false;
            }
            if (IsTerminal(*state))
            {
                return true;
            }
            static_cast<void>(loader->WaitForCpuReady(request, timeoutMs));
            loader->Pump();
        }
        return false;
    }

  private:
    [[nodiscard]] static Sha256Digest artifactHashOf(const std::vector<std::byte>& bytes)
    {
        return Sha256(std::span<const std::byte>(bytes));
    }
};

// 故障清单（文档第 5 步要求的注入项；每一项都必须保留旧 Ready 资源）。
enum class LoadFault
{
    MissingFile,
    ZeroByte,
    TruncatedPayload,
    HashMismatch,
    TruncatedHeader,
    UnsupportedSchema,
    DecodeTooLarge,
    ForcedReadError,
    PathRejected,
    DecodeRejected
};

const char* ToString(const LoadFault fault)
{
    switch (fault)
    {
    case LoadFault::MissingFile:
        return "missing-file";
    case LoadFault::ZeroByte:
        return "zero-byte";
    case LoadFault::TruncatedPayload:
        return "truncated-payload";
    case LoadFault::HashMismatch:
        return "hash-mismatch";
    case LoadFault::TruncatedHeader:
        return "truncated-header";
    case LoadFault::UnsupportedSchema:
        return "unsupported-schema";
    case LoadFault::DecodeTooLarge:
        return "decode-too-large";
    case LoadFault::ForcedReadError:
        return "forced-read-error";
    case LoadFault::PathRejected:
        return "path-rejected";
    case LoadFault::DecodeRejected:
        return "decode-rejected";
    }
    return "unknown";
}

std::vector<LoadFault> AllLoadFaults()
{
    return {LoadFault::MissingFile,    LoadFault::ZeroByte,        LoadFault::TruncatedPayload,
            LoadFault::HashMismatch,   LoadFault::TruncatedHeader, LoadFault::UnsupportedSchema,
            LoadFault::DecodeTooLarge, LoadFault::ForcedReadError, LoadFault::PathRejected,
            LoadFault::DecodeRejected};
}
} // namespace

// ---------------------------------------------------------------------------
// 合并（M7-A18）
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, CoalescesSameAssetAndRevision)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    // 闸门：让第一个请求停在 Reading，证明"合并发生在读取之前"。
    fixture.source->GateRead(artifact.relativePath);

    RequestId first = 0;
    EXPECT_EQ(fixture.loader->Request(artifact.id, 7, LoadPriority::Prefetch, first), RequestResult::Accepted);
    ASSERT_TRUE(fixture.source->WaitUntilBlocked());

    RequestId second = 0;
    EXPECT_EQ(fixture.loader->Request(artifact.id, 7, LoadPriority::Critical, second), RequestResult::Coalesced);
    EXPECT_EQ(first, second);
    // 合并请求把已有请求提升到高优先级（不复制）。
    const auto record = fixture.loader->Record(second);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->priority, LoadPriority::Critical);

    fixture.source->ReleaseGate();
    ASSERT_TRUE(fixture.RunToReady(first));
    EXPECT_EQ(fixture.source->ReadCount(), 1U); // 只读一次
    EXPECT_EQ(fixture.loader->Stats().requestsCoalesced, 1U);
    // 同一个 (AssetId, revision) 已 Ready：再请求直接复用。
    RequestId third = 0;
    EXPECT_EQ(fixture.loader->Request(artifact.id, 7, LoadPriority::Visible, third), RequestResult::AlreadyReady);
    EXPECT_EQ(third, second);
    EXPECT_EQ(fixture.source->ReadCount(), 1U);
}

TEST(AsyncAssetLoaderTests, CancelWaiterKeepsSharedRequestAliveUntilLastWaiterLeaves)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();
    fixture.source->GateRead(artifact.relativePath);

    RequestId first = 0;
    EXPECT_EQ(fixture.loader->Request(artifact.id, 1, LoadPriority::Visible, first), RequestResult::Accepted);
    ASSERT_TRUE(fixture.source->WaitUntilBlocked());
    RequestId second = 0;
    EXPECT_EQ(fixture.loader->Request(artifact.id, 1, LoadPriority::Visible, second), RequestResult::Coalesced);

    // 释放一个 waiter 不取消共享请求。
    fixture.loader->CancelWaiter(first);
    fixture.source->ReleaseGate();
    ASSERT_TRUE(fixture.RunToReady(second));
    EXPECT_EQ(fixture.loader->Stats().cancelsRequested, 0U);

    // 第二个 waiter 也离开：请求已 Ready，取消不影响已发布状态。
    fixture.loader->CancelWaiter(second);
    EXPECT_EQ(*fixture.loader->Query(second), AssetLoadState::Ready);
}

// ---------------------------------------------------------------------------
// 乱序 revision（M7-A18）
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, OlderCompletionCannotReplaceNewRevision)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    // rev7 先进入 Uploading（尚未完成），随后才提出 rev8 —— 两个上传同时在飞。
    const RequestId oldRequest = fixture.Request(artifact.id, 7, LoadPriority::Visible);
    ASSERT_TRUE(fixture.loader->WaitForCpuReady(oldRequest));
    const auto oldUpload = fixture.loader->BeginNextUpload();
    ASSERT_TRUE(oldUpload.has_value());
    EXPECT_EQ(oldUpload->requestId, oldRequest);

    const RequestId newRequest = fixture.Request(artifact.id, 8, LoadPriority::Critical);
    ASSERT_TRUE(fixture.loader->WaitForCpuReady(newRequest));
    const auto newUpload = fixture.loader->BeginNextUpload();
    ASSERT_TRUE(newUpload.has_value());
    EXPECT_EQ(newUpload->requestId, newRequest);

    // 新版本先完成：旧版本的上传即使随后成功，也不得覆盖已发布的 revision。
    fixture.loader->CompleteUpload(newUpload->requestId, true);
    EXPECT_EQ(*fixture.loader->Query(newRequest), AssetLoadState::Ready);
    EXPECT_EQ(fixture.loader->PublishedRevision(artifact.id), 8U);

    fixture.loader->CompleteUpload(oldUpload->requestId, true);
    EXPECT_EQ(*fixture.loader->Query(oldRequest), AssetLoadState::Stale);
    EXPECT_EQ(fixture.loader->PublishedRevision(artifact.id), 8U);
    const auto oldRecord = fixture.loader->Record(oldRequest);
    ASSERT_TRUE(oldRecord.has_value());
    EXPECT_EQ(oldRecord->error, AssetLoadErrorCode::Stale);
}

// 反向顺序：旧版本先完成上传，但新版本已在飞 —— 旧完成同样不得发布。
TEST(AsyncAssetLoaderTests, SupersededUploadNeverPublishesBeforeNewRevisionArrives)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    const RequestId oldRequest = fixture.Request(artifact.id, 7, LoadPriority::Visible);
    ASSERT_TRUE(fixture.loader->WaitForCpuReady(oldRequest));
    const auto oldUpload = fixture.loader->BeginNextUpload();
    ASSERT_TRUE(oldUpload.has_value());

    const RequestId newRequest = fixture.Request(artifact.id, 8, LoadPriority::Critical);
    ASSERT_TRUE(fixture.loader->WaitForCpuReady(newRequest));

    fixture.loader->CompleteUpload(oldUpload->requestId, true); // 旧的上传"成功"了
    EXPECT_EQ(*fixture.loader->Query(oldRequest), AssetLoadState::Stale);
    EXPECT_EQ(fixture.loader->PublishedRevision(artifact.id), 0U); // 尚未发布任何版本

    const auto newUpload = fixture.loader->BeginNextUpload();
    ASSERT_TRUE(newUpload.has_value());
    fixture.loader->CompleteUpload(newUpload->requestId, true);
    EXPECT_EQ(*fixture.loader->Query(newRequest), AssetLoadState::Ready);
    EXPECT_EQ(fixture.loader->PublishedRevision(artifact.id), 8U);
}

TEST(AsyncAssetLoaderTests, SupersededRequestNeverPublishesPayload)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    // rev7 的读完成但 decode 还没跑；此时提出 rev8 → rev7 的完成必须被判 Stale。
    fixture.source->GateRead(artifact.relativePath);
    const RequestId stale = fixture.Request(artifact.id, 7, LoadPriority::Prefetch);
    ASSERT_TRUE(fixture.source->WaitUntilBlocked());
    fixture.source->ReleaseGate();
    ASSERT_TRUE(fixture.dispatcher->WaitForPending(1));

    const RequestId fresh = fixture.Request(artifact.id, 8, LoadPriority::Critical);
    fixture.dispatcher->RunAll();
    fixture.loader->Pump();

    EXPECT_EQ(*fixture.loader->Query(stale), AssetLoadState::Stale);
    ASSERT_TRUE(fixture.RunToReady(fresh));
    EXPECT_EQ(fixture.loader->PublishedRevision(artifact.id), 8U);
    // Stale 请求没有留下 payload（不占用 CpuReady/上传队列预算）。
    const auto staleRecord = fixture.loader->Record(stale);
    ASSERT_TRUE(staleRecord.has_value());
    EXPECT_EQ(staleRecord->state, AssetLoadState::Stale);
}

// ---------------------------------------------------------------------------
// 失败注入（M7-A18）
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, EveryFailurePreservesOldReadyResource)
{
    for (const LoadFault fault : AllLoadFaults())
    {
        AsyncLoaderFixture fixture;
        // A：稳定的旧资源（header-only，64 字节）。B：故障注入用的第二个资产。
        const PublishedArtifact stable = fixture.AddMeshArtifact("meshes/demo/stable", std::byte{1}, kMeshFormatVersion,
                                                                 /*headerOnly=*/true);
        std::string faultUri = "meshes/demo/stable"; // 默认对同一资产的 rev2 注入文件级故障
        bool sameAssetFault = true;
        switch (fault)
        {
        case LoadFault::TruncatedHeader:
            fixture.AddArtifact("meshes/demo/broken-header", AssetKind::Mesh,
                                std::vector<std::byte>(40, std::byte{0U}));
            faultUri = "meshes/demo/broken-header";
            sameAssetFault = false;
            break;
        case LoadFault::UnsupportedSchema:
            fixture.AddMeshArtifact("meshes/demo/bad-schema", std::byte{2}, /*flags=*/0);
            faultUri = "meshes/demo/bad-schema";
            sameAssetFault = false;
            break;
        case LoadFault::DecodeTooLarge:
            // A 是 64 字节；限制 64 字节时 A 通过、B（65 字节）在 decode 阶段被拒。
            fixture.config.maxDecodedAssetBytes = 64;
            fixture.AddMeshArtifact("meshes/demo/too-big", std::byte{3});
            faultUri = "meshes/demo/too-big";
            sameAssetFault = false;
            break;
        default:
            break;
        }
        fixture.Finalize();

        const PublishedArtifact stableArtifact = fixture.Artifact(stable.uri);
        const RequestId baseline = fixture.Request(stableArtifact.id, 1);
        ASSERT_TRUE(fixture.RunToReady(baseline)) << ToString(fault);
        ASSERT_EQ(fixture.loader->PublishedRevision(stableArtifact.id), 1U) << ToString(fault);

        // ---- 注入故障 ----
        const PublishedArtifact faultArtifact = fixture.Artifact(faultUri);
        const std::filesystem::path faultPath = fixture.dir / faultArtifact.relativePath;
        switch (fault)
        {
        case LoadFault::MissingFile:
            std::filesystem::remove(faultPath);
            break;
        case LoadFault::ZeroByte:
            ASSERT_TRUE(WriteFileBytes(faultPath, {}));
            break;
        case LoadFault::TruncatedPayload:
        {
            const auto bytes = MakeMeshArtifact(fixture.buildKey, std::byte{9});
            ASSERT_TRUE(WriteFileBytes(faultPath, std::vector<std::byte>(bytes.begin(), bytes.end() - 1)));
            break;
        }
        case LoadFault::HashMismatch:
        {
            auto bytes = MakeMeshArtifact(fixture.buildKey, std::byte{9});
            bytes.back() = std::byte{0xEE}; // 尺寸不变，内容变了
            ASSERT_TRUE(WriteFileBytes(faultPath, bytes));
            break;
        }
        case LoadFault::ForcedReadError:
            fixture.source->ForceError(faultArtifact.relativePath, AssetLoadErrorCode::ReadFailed);
            break;
        case LoadFault::PathRejected:
            fixture.source->ForceError(faultArtifact.relativePath, AssetLoadErrorCode::PathRejected);
            break;
        case LoadFault::DecodeRejected:
            fixture.dispatcher->SetRejectSubmissions(true);
            break;
        case LoadFault::TruncatedHeader:
        case LoadFault::UnsupportedSchema:
        case LoadFault::DecodeTooLarge:
            break; // 畸形产物在 Finalize 前已写好
        }

        RequestId failing = 0;
        const std::uint64_t revision = sameAssetFault ? 2U : 1U;
        EXPECT_EQ(fixture.loader->Request(faultArtifact.id, revision, LoadPriority::Visible, failing),
                  RequestResult::Accepted)
            << ToString(fault);
        ASSERT_TRUE(fixture.RunUntilTerminal(failing)) << ToString(fault);

        const auto record = fixture.loader->Record(failing);
        ASSERT_TRUE(record.has_value()) << ToString(fault);
        EXPECT_TRUE(IsFailureOrCancellation(record->state)) << ToString(fault) << " -> " << ToString(record->state);
        EXPECT_NE(record->error, AssetLoadErrorCode::None) << ToString(fault);
        // 旧 Ready 资源仍有效；没有半完成状态泄漏。
        EXPECT_EQ(fixture.loader->PublishedRevision(stableArtifact.id), 1U) << ToString(fault);
        EXPECT_EQ(fixture.loader->LiveRequestCount(), 0U) << ToString(fault);
    }
}

TEST(AsyncAssetLoaderTests, RequestRejectsUnknownAssetAndOversizedArtifact)
{
    AsyncLoaderFixture fixture;
    AsyncAssetLoaderConfig config;
    config.maxSingleAssetBytes = 32; // 任一 64 字节 artifact 都会被读取前拒绝
    fixture.config = config;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    RequestId request = 0;
    EXPECT_EQ(fixture.loader->Request(DeriveAssetId("meshes/demo/missing"), 1, LoadPriority::Visible, request),
              RequestResult::Invalid);
    EXPECT_EQ(fixture.loader->Request(artifact.id, 1, LoadPriority::Visible, request), RequestResult::TooLarge);
    EXPECT_EQ(fixture.source->ReadCount(), 0U); // 两者都在读取前被拒绝
    EXPECT_EQ(fixture.loader->Stats().rejectedInvalid, 1U);
    EXPECT_EQ(fixture.loader->Stats().rejectedTooLarge, 1U);
}

TEST(AsyncAssetLoaderTests, DecodeRejectionIsReportedAndCounted)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();
    fixture.dispatcher->SetRejectSubmissions(true);

    const RequestId request = fixture.Request(artifact.id, 1);
    ASSERT_TRUE(fixture.RunUntilTerminal(request));
    const auto record = fixture.loader->Record(request);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->error, AssetLoadErrorCode::DecodeRejected);
    EXPECT_EQ(fixture.loader->Stats().decodeRejected, 1U);
}

// ---------------------------------------------------------------------------
// 取消（M7-A18）
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, CancellingTheLastWaiterCancelsDuringRead)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();
    fixture.source->GateRead(artifact.relativePath);

    const RequestId request = fixture.Request(artifact.id, 1);
    ASSERT_TRUE(fixture.source->WaitUntilBlocked());
    fixture.loader->CancelWaiter(request); // 唯一 waiter 离开 → 请求取消
    fixture.source->ReleaseGate();

    ASSERT_TRUE(fixture.RunUntilTerminal(request));
    EXPECT_EQ(*fixture.loader->Query(request), AssetLoadState::Cancelled);
    EXPECT_EQ(fixture.loader->Stats().cancelsRequested, 1U);
    // 读入的字节记入 cancel waste（"取消浪费"证据），并不进入 payload。
    EXPECT_EQ(fixture.loader->Stats().cancelWasteBytes, 65U);
    EXPECT_EQ(fixture.loader->PublishedRevision(artifact.id), 0U);
}

TEST(AsyncAssetLoaderTests, CancellingWhileDecodingAndBeforeUploadLeavesNoPayload)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    // 情况一：decode 已提交但尚未执行 → 取消后 decode 任务必须落 Cancelled。
    const RequestId decoding = fixture.Request(artifact.id, 1);
    ASSERT_TRUE(fixture.dispatcher->WaitForPending(1));
    fixture.loader->CancelWaiter(decoding);
    fixture.dispatcher->RunAll();
    fixture.loader->Pump();
    EXPECT_EQ(*fixture.loader->Query(decoding), AssetLoadState::Cancelled);

    // 情况二：已进入上传队列 → 取消后必须在上传取件时落 Cancelled，而不是被上传。
    // （WaitForCpuReady 会 Pump，因此这里观察到的是 UploadQueued 而不是 CpuReady。）
    const RequestId queued = fixture.Request(artifact.id, 2);
    ASSERT_TRUE(fixture.loader->WaitForCpuReady(queued));
    EXPECT_EQ(*fixture.loader->Query(queued), AssetLoadState::UploadQueued);
    fixture.loader->CancelWaiter(queued);
    fixture.loader->Pump();
    // 上传取件必须丢弃被取消的条目（队列里只有它，因此取件返回空）。
    EXPECT_FALSE(fixture.loader->BeginNextUpload().has_value());
    EXPECT_EQ(*fixture.loader->Query(queued), AssetLoadState::Cancelled);
    const auto queuedRecord = fixture.loader->Record(queued);
    ASSERT_TRUE(queuedRecord.has_value());
    EXPECT_EQ(queuedRecord->timestamps.uploadStartNs, 0U); // 上传阶段从未开始
}

// ---------------------------------------------------------------------------
// 背压（M7-A19）
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, CapacityOneUsesDocumentedBackpressure)
{
    AsyncAssetLoaderConfig config;
    config.maxIoRequests = 1;
    AsyncLoaderFixture fixture(config);
    const PublishedArtifact first = fixture.AddMeshArtifact("meshes/demo/a", std::byte{1});
    const PublishedArtifact second = fixture.AddMeshArtifact("meshes/demo/b", std::byte{2});
    fixture.Finalize();
    fixture.source->GateRead(first.relativePath);

    RequestId accepted = 0;
    EXPECT_EQ(fixture.loader->Request(first.id, 1, LoadPriority::Visible, accepted), RequestResult::Accepted);
    ASSERT_TRUE(fixture.source->WaitUntilBlocked());
    // 准入计数覆盖"排队 + 读取中"：即使队列本身为空，也不能无限放行。
    RequestId rejected = 0;
    EXPECT_EQ(fixture.loader->Request(second.id, 1, LoadPriority::Visible, rejected), RequestResult::QueueFull);
    EXPECT_EQ(fixture.loader->Stats().rejectedQueueFull, 1U);
    EXPECT_EQ(fixture.loader->Stats().ioAdmittedHighWater, 1U);

    fixture.source->ReleaseGate();
    ASSERT_TRUE(fixture.RunToReady(accepted));
    // 释放名额后可以再准入（背压是"暂时拒绝"，不是永久失败）。
    RequestId retried = 0;
    EXPECT_EQ(fixture.loader->Request(second.id, 1, LoadPriority::Visible, retried), RequestResult::Accepted);
    ASSERT_TRUE(fixture.RunToReady(retried));
}

TEST(AsyncAssetLoaderTests, CpuReadyAndUploadQueuesApplyBackpressure)
{
    AsyncAssetLoaderConfig config;
    config.maxCpuReadyRequests = 1;     // 停车区只放一个
    config.maxUploadQueuedRequests = 1; // 上传队列只放一个
    AsyncLoaderFixture fixture(config);
    const PublishedArtifact first = fixture.AddMeshArtifact("meshes/demo/a", std::byte{1});
    const PublishedArtifact second = fixture.AddMeshArtifact("meshes/demo/b", std::byte{2});
    fixture.Finalize();

    // 第一步：A 停在 CpuReady（停车区容量 1，且不 Pump）；B 的 decode 完成但停车区满
    // → 保持 Decoding（payload 已在请求上，不丢、不阻塞 decode 线程）。
    const RequestId a = fixture.Request(first.id, 1);
    ASSERT_TRUE(fixture.dispatcher->WaitForPending(1));
    fixture.dispatcher->RunAll();
    EXPECT_EQ(*fixture.loader->Query(a), AssetLoadState::CpuReady);
    const RequestId b = fixture.Request(second.id, 1);
    ASSERT_TRUE(fixture.dispatcher->WaitForPending(1));
    fixture.dispatcher->RunAll();
    EXPECT_EQ(*fixture.loader->Query(b), AssetLoadState::Decoding);

    // 第二个 Pump 才把 B 发布（A 已在第一次 Pump 中离开停车区）。
    fixture.loader->Pump();
    EXPECT_EQ(*fixture.loader->Query(a), AssetLoadState::UploadQueued);
    fixture.loader->Pump();
    EXPECT_EQ(*fixture.loader->Query(b), AssetLoadState::CpuReady);
    EXPECT_EQ(fixture.loader->Stats().cpuReadyHighWater, 1U);
    EXPECT_EQ(fixture.loader->Stats().uploadQueueHighWater, 1U);

    // 上传队列容量 1：A 在队列里时 B 只能停在 CpuReady。
    const auto firstTicket = fixture.loader->BeginNextUpload();
    ASSERT_TRUE(firstTicket.has_value());
    EXPECT_EQ(*fixture.loader->Query(b), AssetLoadState::CpuReady);

    // 完成 A 并回收 → 名额释放，B 才能进上传队列。
    fixture.loader->CompleteUpload(firstTicket->requestId, true);
    fixture.loader->ReleaseRequest(firstTicket->requestId);
    fixture.loader->Pump();
    EXPECT_EQ(*fixture.loader->Query(b), AssetLoadState::UploadQueued);
    const auto secondTicket = fixture.loader->BeginNextUpload();
    ASSERT_TRUE(secondTicket.has_value());
    fixture.loader->CompleteUpload(secondTicket->requestId, true);
    EXPECT_EQ(*fixture.loader->Query(b), AssetLoadState::Ready);
}

// ---------------------------------------------------------------------------
// 记录保留上限与释放契约（M7-LOAD-RECORDS）
// ---------------------------------------------------------------------------
// 契约是"终态请求采样完 timestamps 后释放"。在途请求的记录仍被 I/O/decode/上传阶段持有：
// 对它调用 ReleaseRequest 必须被**拒绝**（原先会静默删掉在途请求的状态机）。
TEST(AsyncAssetLoaderTests, ReleaseRequestOfLiveRequestIsRejectedAndKeepsRecord)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/live", std::byte{3});
    fixture.Finalize();

    const RequestId live = fixture.Request(artifact.id, 1);
    ASSERT_TRUE(fixture.loader->Query(live).has_value());

    fixture.loader->ReleaseRequest(live);
    EXPECT_EQ(fixture.loader->Stats().releaseRejectedLive, 1U);
    EXPECT_EQ(fixture.loader->Stats().recordsReleased, 0U);
    EXPECT_EQ(fixture.loader->RetainedRecordCount(), 1U);
    EXPECT_TRUE(fixture.loader->Query(live).has_value());

    // 到达终态后同一个调用生效——此前那次被拒绝的调用没有破坏流水线。
    ASSERT_TRUE(fixture.RunToReady(live));
    fixture.loader->ReleaseRequest(live);
    EXPECT_EQ(fixture.loader->Stats().recordsReleased, 1U);
    EXPECT_EQ(fixture.loader->RetainedRecordCount(), 0U);
    EXPECT_FALSE(fixture.loader->Query(live).has_value());
}

// 长跑护栏：没有上限时记录（含已发布但未采样的 payload）随请求数线性增长（M7-LOAD-RECORDS）。
// 上限只淘汰**终态**记录，且不影响已发布 revision / GPU 资源 token（那是两张独立的表）。
TEST(AsyncAssetLoaderTests, RetainedRecordsAreEvictedAtCapAndPublishedStateSurvives)
{
    AsyncAssetLoaderConfig config;
    config.maxRetainedRecords = 2;
    AsyncLoaderFixture fixture(config);
    std::vector<PublishedArtifact> artifacts;
    for (int index = 0; index < 4; ++index)
    {
        artifacts.push_back(fixture.AddMeshArtifact("meshes/demo/cap" + std::to_string(index),
                                                   static_cast<std::byte>(index + 1)));
    }
    fixture.Finalize();

    for (std::size_t index = 0; index < artifacts.size(); ++index)
    {
        const RequestId request = fixture.Request(artifacts[index].id, 1);
        ASSERT_TRUE(fixture.loader->WaitForCpuReady(request));
        fixture.loader->Pump();
        const auto ticket = fixture.loader->BeginNextUpload();
        ASSERT_TRUE(ticket.has_value());
        const UploadCommitResult committed =
            fixture.loader->CommitUpload(ticket->requestId, static_cast<UploadResourceToken>(100u + index));
        ASSERT_TRUE(committed.committed);
        fixture.loader->Pump(); // 保留上限在帧点结算
    }

    EXPECT_EQ(fixture.loader->RetainedRecordCount(), 2U);
    EXPECT_EQ(fixture.loader->Stats().recordsEvicted, 2U);
    EXPECT_EQ(fixture.loader->Stats().recordsReleased, 0U);

    for (std::size_t index = 0; index < artifacts.size(); ++index)
    {
        EXPECT_EQ(fixture.loader->PublishedRevision(artifacts[index].id), 1U);
        EXPECT_EQ(fixture.loader->PublishedResource(artifacts[index].id),
                  static_cast<UploadResourceToken>(100u + index));
    }
}

// ---------------------------------------------------------------------------
// 关闭（M7-A17/A18）
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, ShutdownDrainLeavesEveryRequestTerminal)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact first = fixture.AddMeshArtifact("meshes/demo/a", std::byte{1});
    const PublishedArtifact second = fixture.AddMeshArtifact("meshes/demo/b", std::byte{2});
    fixture.Finalize();

    const RequestId ready = fixture.Request(first.id, 1);
    ASSERT_TRUE(fixture.RunToReady(ready));
    const RequestId queued = fixture.Request(second.id, 1);

    fixture.loader->Shutdown(/*drain=*/true);
    // 关闭后不再准入。
    RequestId afterShutdown = 0;
    EXPECT_EQ(fixture.loader->Request(first.id, 9, LoadPriority::Visible, afterShutdown), RequestResult::Stopping);

    for (const RequestId request : {ready, queued})
    {
        const auto record = fixture.loader->Record(request);
        ASSERT_TRUE(record.has_value());
        EXPECT_TRUE(IsTerminal(record->state)) << ToString(record->state);
    }
    EXPECT_EQ(fixture.loader->LiveRequestCount(), 0U);
    // 幂等：二次关闭是 no-op。
    fixture.loader->Shutdown(true);
    EXPECT_EQ(fixture.loader->Stats().shutdowns, 1U);
}

TEST(AsyncAssetLoaderTests, ShutdownWithoutDrainLeavesNoActiveRequest)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/a", std::byte{1});
    fixture.Finalize();

    // 不布闸门：请求可能停在 Queued（未开始）或 Reading（读已返回）。
    // 两条路径都必须收敛到终态——drain=false 的契约是"取消一切未发布的东西"。
    const RequestId request = fixture.Request(artifact.id, 1);
    fixture.loader->Shutdown(/*drain=*/false);

    const auto record = fixture.loader->Record(request);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(IsTerminal(record->state)) << ToString(record->state);
    EXPECT_EQ(*fixture.loader->Query(request), AssetLoadState::Cancelled);
    EXPECT_EQ(fixture.loader->LiveRequestCount(), 0U);
}

// ---------------------------------------------------------------------------
// 观测（第 6 步）：时间戳/记录/聚合
// ---------------------------------------------------------------------------
TEST(AsyncAssetLoaderTests, MetricsCoverEveryStageAndKeepRequestIdentity)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    const PublishedArtifact failing = fixture.AddMeshArtifact("meshes/demo/bad", std::byte{8}, /*flags=*/0);
    fixture.Finalize();

    const RequestId ready = fixture.Request(artifact.id, 1);
    ASSERT_TRUE(fixture.RunToReady(ready));
    const RequestId failed = fixture.Request(failing.id, 1);
    ASSERT_TRUE(fixture.RunUntilTerminal(failed));

    const auto records = fixture.loader->CollectRecords();
    ASSERT_EQ(records.size(), 2U);
    const auto readyRecord = fixture.loader->Record(ready);
    ASSERT_TRUE(readyRecord.has_value());
    // 时间戳单调：enqueue ≤ readStart ≤ readEnd ≤ decodeStart ≤ decodeEnd ≤ uploadStart ≤ ready。
    const LoadTimestamps& stamps = readyRecord->timestamps;
    EXPECT_GT(stamps.enqueueNs, 0U);
    EXPECT_LE(stamps.enqueueNs, stamps.readStartNs);
    EXPECT_LE(stamps.readStartNs, stamps.readEndNs);
    EXPECT_LE(stamps.readEndNs, stamps.decodeStartNs);
    EXPECT_LE(stamps.decodeStartNs, stamps.decodeEndNs);
    EXPECT_LE(stamps.decodeEndNs, stamps.uploadStartNs);
    EXPECT_LE(stamps.uploadStartNs, stamps.readyNs);
    EXPECT_EQ(readyRecord->bytes, 65U);

    const AsyncLoadMetrics metrics = fixture.loader->Metrics();
    EXPECT_EQ(metrics.readyRequests, 1U);
    EXPECT_EQ(metrics.failedRequests, 1U);
    EXPECT_GT(metrics.readyP50Ms, 0.0);
    EXPECT_GT(metrics.bytesRead, 0U);
    EXPECT_GT(metrics.readBandwidthMiBPerSecond, 0.0);
    EXPECT_EQ(metrics.maxConcurrentReads, 1U); // 单 I/O 线程基线
}

// request-to-ready 时序：多请求穿过全部阶段后，逐请求时间戳与聚合分位数都可追溯。
// 注意：单测里的绝对毫秒数不是生产延迟证据（真实延迟在 M7-07/M7-10 的 run 里测）。
TEST(AsyncAssetLoaderTests, RequestToReadyTimingIsAggregatedAcrossManyRequests)
{
    AsyncLoaderFixture fixture({}, /*workers=*/2);
    std::vector<PublishedArtifact> artifacts;
    for (int index = 0; index < 24; ++index)
    {
        artifacts.push_back(
            fixture.AddMeshArtifact("meshes/demo/box-" + std::to_string(index), static_cast<std::byte>(index + 1)));
    }
    fixture.Finalize();

    TaskSystemDecodeDispatcher production(*fixture.tasks);
    AsyncAssetLoader loader(fixture.config, *fixture.tasks, *fixture.source, production, *fixture.registry);

    std::vector<RequestId> requests;
    requests.reserve(artifacts.size());
    for (const PublishedArtifact& artifact : artifacts)
    {
        RequestId request = 0;
        ASSERT_EQ(loader.Request(artifact.id, 1, LoadPriority::Visible, request), RequestResult::Accepted);
        requests.push_back(request);
    }

    // render thread 语义的帧点循环：Pump → 取一件上传 → 完成；没有可上传件时等某个请求推进
    // （等待线程帮助执行 decode）。循环按墙钟设定上界，不靠自旋次数——自旋会让 I/O 线程
    // 落后于测试线程，把"加载没完成"伪装成"聚合不对"。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::size_t completed = 0;
    while (completed < requests.size() && std::chrono::steady_clock::now() < deadline)
    {
        loader.Pump();
        if (const auto ticket = loader.BeginNextUpload(); ticket.has_value())
        {
            loader.CompleteUpload(ticket->requestId, true);
            ++completed;
            continue;
        }
        bool waited = false;
        for (const RequestId request : requests)
        {
            const auto state = loader.Query(request);
            if (!state.has_value() || IsTerminal(*state) || *state == AssetLoadState::CpuReady ||
                *state == AssetLoadState::UploadQueued)
            {
                continue; // 已终态/已就绪/已在队列：换下一个
            }
            static_cast<void>(loader.WaitForCpuReady(request, 200));
            waited = true;
            break;
        }
        if (!waited)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 全部在停车区：让 Pump 推进
        }
    }
    EXPECT_EQ(completed, requests.size());
    EXPECT_EQ(loader.LiveRequestCount(), 0U);

    const AsyncLoadMetrics metrics = loader.Metrics();
    EXPECT_EQ(metrics.readyRequests, static_cast<std::uint32_t>(requests.size()));
    EXPECT_EQ(metrics.failedRequests, 0U);
    EXPECT_EQ(metrics.bytesRead, 65U * static_cast<std::uint64_t>(requests.size()));
    EXPECT_GE(metrics.readyP95Ms, metrics.readyP50Ms);
    EXPECT_GE(metrics.readyP99Ms, metrics.readyP95Ms);
    EXPECT_GE(metrics.ioQueueHighWater, 1U);

    const std::vector<AsyncLoadRecord> records = loader.CollectRecords();
    ASSERT_EQ(records.size(), requests.size());
    for (const AsyncLoadRecord& record : records)
    {
        EXPECT_EQ(record.state, AssetLoadState::Ready);
        EXPECT_LT(record.timestamps.enqueueNs, record.timestamps.readyNs);
        EXPECT_NE(record.ioThreadId, 0U);
        EXPECT_NE(record.decodeThreadId, 0U);
    }
    loader.Shutdown(true);
}

// A17 线程归属 trace：读文件的线程、执行 decode 的线程、上传线程必须可区分。
TEST(AsyncAssetLoaderTests, ThreadOwnershipTraceSeparatesIoDecodeAndUpload)
{
    AsyncLoaderFixture fixture;
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    const RequestId request = fixture.Request(artifact.id, 1);
    ASSERT_TRUE(fixture.RunToReady(request));

    const auto record = fixture.loader->Record(request);
    ASSERT_TRUE(record.has_value());
    const std::uint64_t ioThreadId = fixture.loader->IoThreadId();
    ASSERT_NE(ioThreadId, 0U);
    // 阻塞读只发生在专用 I/O 线程上。
    EXPECT_EQ(record->ioThreadId, ioThreadId);
    // decode 不在 I/O 线程上执行（手动执行器 → 测试主线程；生产执行器 → worker/主线程）。
    ASSERT_NE(record->decodeThreadId, 0U);
    EXPECT_NE(record->decodeThreadId, record->ioThreadId);
    EXPECT_FALSE(record->decodeExecutedOnWorker); // 手动执行器不在 worker 上
    // 上传阶段由调用方线程（render thread 语义）驱动，时间戳顺序与线程归属一致。
    EXPECT_GE(record->timestamps.uploadStartNs, record->timestamps.decodeEndNs);
}

// decode 在 compute 侧执行：使用生产执行器（TaskSystem）验证端到端线程归属。
TEST(AsyncAssetLoaderTests, ProductionDispatcherRunsDecodeThroughTaskSystem)
{
    AsyncLoaderFixture fixture({}, /*workers=*/2);
    const PublishedArtifact artifact = fixture.AddMeshArtifact("meshes/demo/box", std::byte{7});
    fixture.Finalize();

    // 用生产执行器替换手动执行器，其余依赖不变。
    TaskSystemDecodeDispatcher production(*fixture.tasks);
    AsyncAssetLoader loader(fixture.config, *fixture.tasks, *fixture.source, production, *fixture.registry);

    RequestId request = 0;
    ASSERT_EQ(loader.Request(artifact.id, 1, LoadPriority::Visible, request), RequestResult::Accepted);
    ASSERT_TRUE(loader.WaitForCpuReady(request, 5000)); // 帮助执行 decode（main-help）
    loader.Pump();
    const auto ticket = loader.BeginNextUpload();
    ASSERT_TRUE(ticket.has_value());
    loader.CompleteUpload(ticket->requestId, true);
    EXPECT_EQ(*loader.Query(request), AssetLoadState::Ready);

    const auto record = loader.Record(request);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->state == AssetLoadState::Ready);
    // 线程归属证据：decode 由 compute 侧（worker 或帮助执行的线程）完成，
    // 绝不可能是 I/O 线程——I/O 线程从不等待 decode 组。
    EXPECT_GT(record->timestamps.decodeStartNs, record->timestamps.readEndNs);
    loader.Shutdown(true);
}
