// ============================================================================
// D3D11LifetimeDeviceTests.cpp — M6-03 D3D11 真实资源生命周期证据
//
// 这里只使用裸 D3D11 device/context、DeferredRegistry 和 D3D11_QUERY_EVENT，
// 证明公共 Destroy/Collect 语义能由 D3D11 immediate context 的真实完成点驱动：
//   upload(staging WRITE) -> CopyResource(default) -> CopyResource(readback)
//   -> EVENT query -> Map(readback)
//
// 提前释放实验单独创建 Debug device。D3D11 immediate runtime 按契约保留命令
// 引用，因而“提交后立刻 Release”可能没有 debug 错误；测试只记录实际
// InfoQueue message，不把预期中的错误伪造成诊断。提前 DestroyCompleted 是隔离的引擎负例，
// 必须在真实 EVENT 完成被接纳前被 d3d11 归因的验证错误拒绝。
// ============================================================================

#include "DeferredRegistry.h"

#include <MiniEngine/Rhi/RhiError.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <gtest/gtest.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using MiniEngine::Rhi::BufferHandle;
using MiniEngine::Rhi::DeferredRegistry;
using MiniEngine::Rhi::RhiErrorCode;
using MiniEngine::Rhi::RhiValidationError;

constexpr UINT kCopyByteCount = 256U;
constexpr std::uint32_t kEventTimeoutMs = 5000U;
constexpr std::uint32_t kNativeRetirementCount = 32U;
constexpr std::uint32_t kCpuRetirementCount = 10000U;

struct DeviceRun final
{
    const char* name;
    D3D_DRIVER_TYPE driverType;
};

void PrintTo(const DeviceRun& value, std::ostream* stream)
{
    *stream << value.name;
}

struct DeviceRig final
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
};

// 只创建 Debug device；不能用 retail fallback 冒充本步的 debug 证据。
std::optional<DeviceRig> CreateDebugDevice(const D3D_DRIVER_TYPE driverType, HRESULT& failure)
{
    DeviceRig rig;
    constexpr D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    constexpr UINT flags = D3D11_CREATE_DEVICE_DEBUG;
    HRESULT result =
        D3D11CreateDevice(nullptr, driverType, nullptr, flags, featureLevels,
                          static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
                          rig.device.ReleaseAndGetAddressOf(), &rig.featureLevel, rig.context.ReleaseAndGetAddressOf());
    // 老 runtime 可能拒绝带 11_1 的数组；仍保持 Debug flag，重试 11_0。
    if (FAILED(result))
    {
        constexpr D3D_FEATURE_LEVEL featureLevel11_0[] = {D3D_FEATURE_LEVEL_11_0};
        result = D3D11CreateDevice(nullptr, driverType, nullptr, flags, featureLevel11_0, 1U, D3D11_SDK_VERSION,
                                   rig.device.ReleaseAndGetAddressOf(), &rig.featureLevel,
                                   rig.context.ReleaseAndGetAddressOf());
    }
    failure = result;
    if (FAILED(result) || rig.device == nullptr || rig.context == nullptr)
    {
        return std::nullopt;
    }
    return rig;
}

std::string Hex(const HRESULT result)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << static_cast<unsigned long>(static_cast<std::uint32_t>(result));
    return stream.str();
}

struct DebugMessage final
{
    D3D11_MESSAGE_ID id = D3D11_MESSAGE_ID_UNKNOWN;
    D3D11_MESSAGE_SEVERITY severity = D3D11_MESSAGE_SEVERITY_INFO;
    std::string description;
};

ComPtr<ID3D11InfoQueue> GetInfoQueue(ID3D11Device& device)
{
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device.QueryInterface(IID_PPV_ARGS(queue.ReleaseAndGetAddressOf()))) || queue == nullptr)
    {
        return {};
    }
    queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
    queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
    return queue;
}

std::vector<DebugMessage> ReadMessages(ID3D11InfoQueue& queue, const bool clear)
{
    std::vector<DebugMessage> messages;
    const UINT64 count = queue.GetNumStoredMessages();
    messages.reserve(static_cast<std::size_t>(count));
    for (UINT64 index = 0; index < count; ++index)
    {
        SIZE_T length = 0;
        if (FAILED(queue.GetMessage(index, nullptr, &length)) || length < sizeof(D3D11_MESSAGE))
        {
            continue;
        }
        std::vector<std::byte> bytes(length);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        if (FAILED(queue.GetMessage(index, message, &length)))
        {
            continue;
        }
        messages.push_back(
            {message->ID, message->Severity, message->pDescription == nullptr ? "" : message->pDescription});
    }
    if (clear)
    {
        queue.ClearStoredMessages();
    }
    return messages;
}

std::vector<DebugMessage> Problems(const std::vector<DebugMessage>& messages)
{
    std::vector<DebugMessage> result;
    for (const auto& message : messages)
    {
        if (message.severity <= D3D11_MESSAGE_SEVERITY_WARNING)
        {
            result.push_back(message);
        }
    }
    return result;
}

void ExpectNoDebugProblems(ID3D11InfoQueue& queue, const char* run)
{
    const auto problems = Problems(ReadMessages(queue, true));
    for (const auto& message : problems)
    {
        ADD_FAILURE() << "D3D11 " << run << " unexpected severity=" << static_cast<int>(message.severity)
                      << " messageId=" << static_cast<unsigned>(message.id) << ": " << message.description;
    }
    EXPECT_TRUE(problems.empty()) << "normal lifecycle group requires zero warning/error/corruption";
}

// 正常诊断已单独检查；此显式 census 允许报告仍由测试持有的 device/context/query。
// 必须看见 LIVE_DEVICE 才认定报告有效，同时拒绝仍有外部引用的 Buffer。
void ExpectNoLiveBuffers(ID3D11Device& device, ID3D11InfoQueue& queue, const char* run)
{
    ComPtr<ID3D11Debug> debug;
    ASSERT_HRESULT_SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(&debug)));
    ASSERT_HRESULT_SUCCEEDED(
        debug->ReportLiveDeviceObjects(static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL)));
    bool reportedDevice = false;
    std::size_t buffers = 0;
    for (const auto& message : ReadMessages(queue, true))
    {
        reportedDevice |= message.id == D3D11_MESSAGE_ID_LIVE_DEVICE;
        if (message.id == D3D11_MESSAGE_ID_LIVE_BUFFER || message.id == D3D11_MESSAGE_ID_LIVE_BUFFER_WIN7)
        {
            ++buffers;
            ADD_FAILURE() << message.description;
        }
    }
    EXPECT_TRUE(reportedDevice);
    EXPECT_EQ(buffers, 0U);
    std::cout << "M6_D11_CENSUS run=" << run << " deviceReport=" << reportedDevice << " liveBuffers=" << buffers
              << '\n';
}

std::vector<std::byte> MakePattern(const std::uint32_t seed)
{
    std::vector<std::byte> bytes(kCopyByteCount);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        const auto value =
            static_cast<unsigned char>((seed * 37U + static_cast<std::uint32_t>(index) * 13U + 0x5AU) & 0xFFU);
        bytes[index] = static_cast<std::byte>(value);
    }
    return bytes;
}

ComPtr<ID3D11Buffer> CreateBuffer(ID3D11Device& device, const D3D11_USAGE usage, const UINT cpuAccessFlags,
                                  const UINT bindFlags)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = kCopyByteCount;
    description.Usage = usage;
    description.BindFlags = bindFlags;
    description.CPUAccessFlags = cpuAccessFlags;

    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device.CreateBuffer(&description, nullptr, buffer.ReleaseAndGetAddressOf())))
    {
        return {};
    }
    return buffer;
}

bool UploadStaging(ID3D11DeviceContext& context, ID3D11Buffer& buffer, const std::vector<std::byte>& bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context.Map(&buffer, 0, D3D11_MAP_WRITE, 0, &mapped)))
    {
        return false;
    }
    std::memcpy(mapped.pData, bytes.data(), bytes.size());
    context.Unmap(&buffer, 0);
    return true;
}

bool ReadbackStaging(ID3D11DeviceContext& context, ID3D11Buffer& buffer, std::vector<std::byte>& bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context.Map(&buffer, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        return false;
    }
    std::memcpy(bytes.data(), mapped.pData, bytes.size());
    context.Unmap(&buffer, 0);
    return true;
}

enum class EventWaitStatus
{
    Completed,
    Failed,
    TimedOut
};

struct EventWaitResult final
{
    EventWaitStatus status = EventWaitStatus::TimedOut;
    HRESULT result = S_FALSE;
    std::uint64_t polls = 0;
};

// EVENT query 是本文件唯一的 native completion 来源。超时保证坏设备/坏提交
// 不会把测试变成无限等待；Sleep(1) 让轮询占用主要由 GPU 工作决定。
EventWaitResult WaitForEvent(ID3D11DeviceContext& context, ID3D11Query& query,
                             const std::uint32_t timeoutMs = kEventTimeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    EventWaitResult result{};
    for (;;)
    {
        ++result.polls;
        result.result = context.GetData(&query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result.result == S_OK)
        {
            result.status = EventWaitStatus::Completed;
            return result;
        }
        if (FAILED(result.result))
        {
            result.status = EventWaitStatus::Failed;
            return result;
        }
        if (GetTickCount64() >= deadline)
        {
            result.status = EventWaitStatus::TimedOut;
            return result;
        }
        Sleep(1);
    }
}

struct PayloadLifetime final
{
    std::atomic<std::uint32_t> destroyed{0};
    std::atomic<std::uint32_t> destroyedBeforeNativeCompletion{0};
    std::atomic<bool> nativeEventCompleted{false};
};

struct Payload final
{
    ComPtr<ID3D11Buffer> buffer;
    std::shared_ptr<PayloadLifetime> lifetime;

    Payload(ComPtr<ID3D11Buffer> value, std::shared_ptr<PayloadLifetime> lifetimeValue) noexcept
        : buffer(std::move(value)), lifetime(std::move(lifetimeValue))
    {
    }
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    Payload(Payload&&) noexcept = default;
    Payload& operator=(Payload&&) noexcept = default;
    ~Payload() noexcept
    {
        if (lifetime == nullptr)
        {
            return;
        }
        lifetime->destroyed.fetch_add(1U, std::memory_order_release);
        if (!lifetime->nativeEventCompleted.load(std::memory_order_acquire))
        {
            lifetime->destroyedBeforeNativeCompletion.fetch_add(1U, std::memory_order_release);
        }
    }
};

static_assert(std::is_nothrow_move_constructible_v<Payload>);
static_assert(std::is_nothrow_move_assignable_v<Payload>);
static_assert(std::is_nothrow_destructible_v<Payload>);

template <class Registry> void ExpectStale(Registry& registry, const BufferHandle handle)
{
    try
    {
        static_cast<void>(registry.Get(handle));
        ADD_FAILURE() << "Destroy must invalidate a public handle immediately";
    }
    catch (const RhiValidationError& error)
    {
        EXPECT_EQ(error.Error().code, RhiErrorCode::InvalidHandle);
        EXPECT_EQ(error.Error().operation, "Get");
        EXPECT_EQ(error.Error().objectType, "Buffer");
        EXPECT_EQ(error.Error().backend, "d3d11");
    }
}

template <class Registry> void ExpectDoubleDestroy(Registry& registry, const BufferHandle handle)
{
    try
    {
        registry.Destroy(handle);
        ADD_FAILURE() << "double Destroy must fail on the stale handle";
    }
    catch (const RhiValidationError& error)
    {
        EXPECT_EQ(error.Error().code, RhiErrorCode::InvalidHandle);
        EXPECT_EQ(error.Error().operation, "Get");
        EXPECT_EQ(error.Error().objectType, "Buffer");
        EXPECT_EQ(error.Error().backend, "d3d11");
    }
}

void ExpectStats(const MiniEngine::Rhi::RegistryCounts& stats, const std::size_t alive, const std::size_t retiring,
                 const std::size_t slots, const std::size_t exhausted)
{
    EXPECT_EQ(stats.alive, alive);
    EXPECT_EQ(stats.retiring, retiring);
    EXPECT_EQ(stats.slots, slots);
    EXPECT_EQ(stats.exhausted, exhausted);
}

class D3D11LifetimeDeviceTests : public testing::TestWithParam<DeviceRun>
{
};

TEST_P(D3D11LifetimeDeviceTests, DestroyWaitsForEventCompletionAndReusesSlot)
{
    const DeviceRun run = GetParam();
    HRESULT createResult = S_OK;
    auto rig = CreateDebugDevice(run.driverType, createResult);
    ASSERT_HRESULT_SUCCEEDED(createResult) << run.name << " D3D11 Debug device creation failed: " << Hex(createResult);
    ASSERT_TRUE(rig.has_value());
    ASSERT_NE(rig->device, nullptr);
    ASSERT_NE(rig->context, nullptr);

    const auto queue = GetInfoQueue(*rig->device.Get());
    ASSERT_NE(queue, nullptr) << "Debug device did not expose ID3D11InfoQueue";
    queue->ClearStoredMessages();

    const auto expected = MakePattern(7U);
    auto upload = CreateBuffer(*rig->device.Get(), D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, 0);
    auto target = CreateBuffer(*rig->device.Get(), D3D11_USAGE_DEFAULT, 0, D3D11_BIND_VERTEX_BUFFER);
    auto readback = CreateBuffer(*rig->device.Get(), D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0);
    ASSERT_NE(upload, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(readback, nullptr);
    ASSERT_TRUE(UploadStaging(*rig->context.Get(), *upload.Get(), expected));

    // 第一段 copy 在 registry 建立前录制；第二段 copy 通过 registry payload 取 native resource。
    rig->context->CopyResource(target.Get(), upload.Get());
    auto lifetime = std::make_shared<PayloadLifetime>();
    DeferredRegistry<BufferHandle, Payload> registry("Buffer", "d3d11");
    const BufferHandle handle = registry.Create(Payload(std::move(target), lifetime), "M6-03/event-buffer");
    ASSERT_NE(registry.Get(handle).buffer, nullptr);
    rig->context->CopyResource(readback.Get(), registry.Get(handle).buffer.Get());

    ComPtr<ID3D11Query> event;
    D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    ASSERT_HRESULT_SUCCEEDED(rig->device->CreateQuery(&queryDescription, event.ReleaseAndGetAddressOf()));
    constexpr std::uint64_t submission = 1U;
    registry.MarkUsed(handle, submission);
    rig->context->End(event.Get());
    rig->context->Flush();

    registry.Destroy(handle);
    ExpectStale(registry, handle);
    ExpectDoubleDestroy(registry, handle);
    EXPECT_EQ(lifetime->destroyed.load(std::memory_order_acquire), 0U);
    ExpectStats(registry.Stats(), 0U, 1U, 1U, 0U);

    // 即使 query 很快完成，Collect(0) 也不能越过最后一次 MarkUsed。
    registry.Collect(0U);
    EXPECT_EQ(lifetime->destroyed.load(std::memory_order_acquire), 0U);
    ExpectStats(registry.Stats(), 0U, 1U, 1U, 0U);

    const EventWaitResult completion = WaitForEvent(*rig->context.Get(), *event.Get());
    ASSERT_EQ(completion.status, EventWaitStatus::Completed)
        << run.name << " event query did not complete: status=" << static_cast<int>(completion.status)
        << " hr=" << Hex(completion.result) << " polls=" << completion.polls;
    lifetime->nativeEventCompleted.store(true, std::memory_order_release);

    registry.Collect(submission);
    EXPECT_EQ(lifetime->destroyed.load(std::memory_order_acquire), 1U)
        << "Payload destruction is tied to Collect after the native EVENT completion";
    EXPECT_EQ(lifetime->destroyedBeforeNativeCompletion.load(std::memory_order_acquire), 0U);
    ExpectStats(registry.Stats(), 0U, 0U, 1U, 0U);

    // 已安全回收的 slot 只能以新 generation 复用；旧 public handle 仍 stale。
    auto replacement = CreateBuffer(*rig->device.Get(), D3D11_USAGE_DEFAULT, 0, D3D11_BIND_VERTEX_BUFFER);
    ASSERT_NE(replacement, nullptr);
    auto replacementLifetime = std::make_shared<PayloadLifetime>();
    replacementLifetime->nativeEventCompleted.store(true, std::memory_order_release);
    const BufferHandle reused =
        registry.Create(Payload(std::move(replacement), replacementLifetime), "M6-03/reused-buffer");
    EXPECT_EQ(reused.Index(), handle.Index());
    EXPECT_NE(reused.Generation(), handle.Generation());
    EXPECT_EQ(registry.Stats().alive, 1U);
    ExpectStale(registry, handle);
    registry.Destroy(reused);
    registry.Collect(submission);
    EXPECT_EQ(replacementLifetime->destroyed.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(replacementLifetime->destroyedBeforeNativeCompletion.load(std::memory_order_acquire), 0U);
    ExpectStats(registry.Stats(), 0U, 0U, 1U, 0U);

    std::vector<std::byte> actual(kCopyByteCount);
    ASSERT_TRUE(ReadbackStaging(*rig->context.Get(), *readback.Get(), actual));
    EXPECT_EQ(actual, expected) << "upload -> copy -> EVENT -> readback bytes must match exactly";

    rig->context->ClearState();
    rig->context->Flush();
    ExpectNoDebugProblems(*queue.Get(), run.name);
    upload.Reset();
    readback.Reset();
    rig->context->Flush();
    ExpectNoLiveBuffers(*rig->device.Get(), *queue.Get(), run.name);
}

TEST_P(D3D11LifetimeDeviceTests, BoundedNativeRetirementLoopKeepsAllReadbacksCorrect)
{
    const DeviceRun run = GetParam();
    HRESULT createResult = S_OK;
    auto rig = CreateDebugDevice(run.driverType, createResult);
    ASSERT_HRESULT_SUCCEEDED(createResult) << run.name << " D3D11 Debug device creation failed: " << Hex(createResult);
    ASSERT_TRUE(rig.has_value());
    const auto queue = GetInfoQueue(*rig->device.Get());
    ASSERT_NE(queue, nullptr);
    queue->ClearStoredMessages();

    DeferredRegistry<BufferHandle, Payload> registry("Buffer", "d3d11");
    std::vector<BufferHandle> handles;
    std::vector<std::shared_ptr<PayloadLifetime>> lifetimes;
    std::vector<ComPtr<ID3D11Buffer>> readbacks;
    std::vector<std::vector<std::byte>> expected;
    handles.reserve(kNativeRetirementCount);
    lifetimes.reserve(kNativeRetirementCount);
    readbacks.reserve(kNativeRetirementCount);
    expected.reserve(kNativeRetirementCount);

    for (std::uint32_t index = 0; index < kNativeRetirementCount; ++index)
    {
        auto upload = CreateBuffer(*rig->device.Get(), D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, 0);
        auto target = CreateBuffer(*rig->device.Get(), D3D11_USAGE_DEFAULT, 0, D3D11_BIND_VERTEX_BUFFER);
        auto readback = CreateBuffer(*rig->device.Get(), D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0);
        ASSERT_NE(upload, nullptr);
        ASSERT_NE(target, nullptr);
        ASSERT_NE(readback, nullptr);
        expected.push_back(MakePattern(index + 100U));
        ASSERT_TRUE(UploadStaging(*rig->context.Get(), *upload.Get(), expected.back()));
        rig->context->CopyResource(target.Get(), upload.Get());

        auto lifetime = std::make_shared<PayloadLifetime>();
        const BufferHandle handle = registry.Create(Payload(std::move(target), lifetime), "M6-03/loop-buffer");
        rig->context->CopyResource(readback.Get(), registry.Get(handle).buffer.Get());
        registry.MarkUsed(handle, static_cast<std::uint64_t>(index) + 1U);
        handles.push_back(handle);
        lifetimes.push_back(std::move(lifetime));
        readbacks.push_back(std::move(readback));
        // upload 只需要活到 CopyResource 调用返回；D3D11 命令引用由 runtime 管理。
    }

    ComPtr<ID3D11Query> event;
    D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    ASSERT_HRESULT_SUCCEEDED(rig->device->CreateQuery(&queryDescription, event.ReleaseAndGetAddressOf()));
    rig->context->End(event.Get());
    rig->context->Flush();

    for (const BufferHandle handle : handles)
    {
        registry.Destroy(handle);
    }
    ExpectStats(registry.Stats(), 0U, kNativeRetirementCount, kNativeRetirementCount, 0U);
    for (const auto& lifetime : lifetimes)
    {
        EXPECT_EQ(lifetime->destroyed.load(std::memory_order_acquire), 0U);
    }
    registry.Collect(0U);
    ExpectStats(registry.Stats(), 0U, kNativeRetirementCount, kNativeRetirementCount, 0U);

    const EventWaitResult completion = WaitForEvent(*rig->context.Get(), *event.Get());
    ASSERT_EQ(completion.status, EventWaitStatus::Completed)
        << run.name << " bounded retirement EVENT did not complete: hr=" << Hex(completion.result)
        << " polls=" << completion.polls;
    for (const auto& lifetime : lifetimes)
    {
        lifetime->nativeEventCompleted.store(true, std::memory_order_release);
    }
    registry.Collect(kNativeRetirementCount);
    ExpectStats(registry.Stats(), 0U, 0U, kNativeRetirementCount, 0U);
    for (const auto& lifetime : lifetimes)
    {
        EXPECT_EQ(lifetime->destroyed.load(std::memory_order_acquire), 1U);
        EXPECT_EQ(lifetime->destroyedBeforeNativeCompletion.load(std::memory_order_acquire), 0U);
    }

    for (std::uint32_t index = 0; index < kNativeRetirementCount; ++index)
    {
        std::vector<std::byte> actual(kCopyByteCount);
        ASSERT_TRUE(ReadbackStaging(*rig->context.Get(), *readbacks[index].Get(), actual));
        EXPECT_EQ(actual, expected[index]) << "bounded native retirement readback index=" << index;
    }
    rig->context->ClearState();
    rig->context->Flush();
    ExpectNoDebugProblems(*queue.Get(), run.name);
    readbacks.clear();
    rig->context->Flush();
    ExpectNoLiveBuffers(*rig->device.Get(), *queue.Get(), run.name);
}

INSTANTIATE_TEST_SUITE_P(M6, D3D11LifetimeDeviceTests,
                         testing::Values(DeviceRun{"HardwareDebug", D3D_DRIVER_TYPE_HARDWARE},
                                         DeviceRun{"WarpDebug", D3D_DRIVER_TYPE_WARP}),
                         [](const testing::TestParamInfo<DeviceRun>& info) { return info.param.name; });

struct CpuPayload final
{
    std::uint32_t value = 0;
    explicit CpuPayload(const std::uint32_t valueIn) noexcept : value(valueIn)
    {
    }
    CpuPayload(CpuPayload&&) noexcept = default;
    CpuPayload& operator=(CpuPayload&&) noexcept = default;
    CpuPayload(const CpuPayload&) = delete;
    CpuPayload& operator=(const CpuPayload&) = delete;
};

static_assert(std::is_nothrow_move_constructible_v<CpuPayload>);
static_assert(std::is_nothrow_destructible_v<CpuPayload>);

TEST(DeferredRegistryCpuTests, TenThousandRetirementsHaveBoundedSlots)
{
    DeferredRegistry<BufferHandle, CpuPayload> registry("Buffer", "cpu");
    std::unordered_set<BufferHandle> seen;
    seen.reserve(kCpuRetirementCount);

    for (std::uint32_t index = 0; index < kCpuRetirementCount; ++index)
    {
        const std::uint64_t serial = static_cast<std::uint64_t>(index) + 1U;
        const BufferHandle handle = registry.Create(CpuPayload(index));
        ASSERT_TRUE(seen.insert(handle).second);
        EXPECT_EQ(registry.Get(handle).value, index);
        registry.MarkUsed(handle, serial);
        registry.Destroy(handle);
        EXPECT_EQ(registry.Stats().retiring, 1U);
        registry.Collect(serial);
        EXPECT_EQ(registry.Stats().alive, 0U);
        EXPECT_EQ(registry.Stats().retiring, 0U);
        EXPECT_EQ(registry.Stats().slots, 1U);
        EXPECT_EQ(registry.Stats().exhausted, 0U);
    }

    EXPECT_EQ(seen.size(), kCpuRetirementCount);
    EXPECT_EQ(registry.Stats().slots, 1U);
}

class D3D11EarlyReleaseNegativeTests : public testing::TestWithParam<DeviceRun>
{
};

TEST_P(D3D11EarlyReleaseNegativeTests, ReleaseRetentionAndPrematureRetirementAreDistinguished)
{
    const DeviceRun run = GetParam();
    HRESULT createResult = S_OK;
    auto rig = CreateDebugDevice(run.driverType, createResult);
    ASSERT_HRESULT_SUCCEEDED(createResult) << run.name << " D3D11 Debug device creation failed: " << Hex(createResult);
    ASSERT_TRUE(rig.has_value());
    const auto queue = GetInfoQueue(*rig->device.Get());
    ASSERT_NE(queue, nullptr);
    queue->ClearStoredMessages();

    // 所有 copy 已进入 immediate context 后释放应用引用；不假造 release-only debug error。
    const auto expected = MakePattern(991U);
    auto upload = CreateBuffer(*rig->device.Get(), D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, 0);
    auto target = CreateBuffer(*rig->device.Get(), D3D11_USAGE_DEFAULT, 0, D3D11_BIND_VERTEX_BUFFER);
    auto readback = CreateBuffer(*rig->device.Get(), D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0);
    ASSERT_NE(upload, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(readback, nullptr);
    ASSERT_TRUE(UploadStaging(*rig->context.Get(), *upload.Get(), expected));
    rig->context->CopyResource(target.Get(), upload.Get());
    rig->context->CopyResource(readback.Get(), target.Get());
    upload.Reset();
    target.Reset();

    ComPtr<ID3D11Query> releaseEvent;
    D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    ASSERT_HRESULT_SUCCEEDED(rig->device->CreateQuery(&queryDescription, releaseEvent.ReleaseAndGetAddressOf()));
    rig->context->End(releaseEvent.Get());
    rig->context->Flush();
    const EventWaitResult releaseCompletion = WaitForEvent(*rig->context.Get(), *releaseEvent.Get());
    ASSERT_EQ(releaseCompletion.status, EventWaitStatus::Completed)
        << "release-only event did not complete: hr=" << Hex(releaseCompletion.result)
        << " polls=" << releaseCompletion.polls;
    std::vector<std::byte> actual(kCopyByteCount);
    ASSERT_TRUE(ReadbackStaging(*rig->context.Get(), *readback.Get(), actual));
    EXPECT_EQ(actual, expected);

    const auto releaseProblems = Problems(ReadMessages(*queue.Get(), true));
    std::cout << "M6_D3D11_EARLY_RELEASE run=" << run.name << " completion=EVENT"
              << " releaseOnlyDiagnostics=" << releaseProblems.size() << std::endl;
    for (const auto& message : releaseProblems)
    {
        std::cout << "M6_D3D11_EARLY_RELEASE messageId=" << static_cast<unsigned>(message.id)
                  << " severity=" << static_cast<int>(message.severity)
                  << " cause=release-only: " << message.description << std::endl;
    }
    if (releaseProblems.empty())
    {
        std::cout << "M6_D3D11_EARLY_RELEASE limitation=immediate-runtime-retained-command-reference";
    }

    EXPECT_TRUE(releaseProblems.empty());
    std::cout << '\n';
    // 引擎不能依赖 COM 暗中保留引用：未接纳完成证明时立即释放必须失败。
    DeferredRegistry<BufferHandle, Payload> registry("Buffer", "d3d11");
    auto completionState = std::make_shared<PayloadLifetime>();
    auto guarded = CreateBuffer(*rig->device.Get(), D3D11_USAGE_DEFAULT, 0, D3D11_BIND_VERTEX_BUFFER);
    ASSERT_NE(guarded, nullptr);
    auto handle = registry.Create(Payload(std::move(guarded), completionState), "M6-03/early-destroy");
    rig->context->CopyResource(registry.Get(handle).buffer.Get(), readback.Get());
    registry.MarkUsed(handle, 1);
    rig->context->End(releaseEvent.Get());
    rig->context->Flush();
    try
    {
        registry.DestroyCompleted(handle);
        FAIL() << "immediate destruction without backend completion must fail";
    }
    catch (const RhiValidationError& error)
    {
        EXPECT_EQ(error.Error().code, RhiErrorCode::InvalidState);
        EXPECT_EQ(error.Error().operation, "DestroyCompleted");
        EXPECT_EQ(error.Error().backend, "d3d11");
        EXPECT_EQ(error.Error().objectType, "Buffer");
        std::cout << "M6_D3D11_EARLY_RELEASE run=" << run.name
                  << " engineDiagnostic=InvalidState operation=DestroyCompleted backend=" << error.Error().backend
                  << " lastUse=1 acceptedCompletion=0\n";
    }
    EXPECT_EQ(registry.Stats().alive, 1U);
    EXPECT_EQ(completionState->destroyed.load(), 0U);
    const auto guardedCompletion = WaitForEvent(*rig->context.Get(), *releaseEvent.Get());
    ASSERT_EQ(guardedCompletion.status, EventWaitStatus::Completed);
    completionState->nativeEventCompleted.store(true);
    registry.Collect(1);
    registry.DestroyCompleted(handle);
    EXPECT_EQ(registry.Stats().alive, 0U);
    EXPECT_EQ(completionState->destroyed.load(), 1U);
    EXPECT_EQ(completionState->destroyedBeforeNativeCompletion.load(), 0U);
    ExpectNoDebugProblems(*queue.Get(), run.name);
}

INSTANTIATE_TEST_SUITE_P(M6, D3D11EarlyReleaseNegativeTests,
                         testing::Values(DeviceRun{"HardwareDebug", D3D_DRIVER_TYPE_HARDWARE},
                                         DeviceRun{"WarpDebug", D3D_DRIVER_TYPE_WARP}),
                         [](const testing::TestParamInfo<DeviceRun>& info) { return info.param.name; });
} // namespace
