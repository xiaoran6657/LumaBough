// ============================================================================
// D3D12FrameReadback.cpp — timestamp query 与 footprint 截图回读实现
// 里程碑：M5（09 篇输出一致性；08 篇 GPU timestamp）
// 职责：实现固定 query slice、fence 标记、逐行 footprint 复制和 CPU RGBA8 打包。
//       GPU 等待、资源 barrier、Renderer 接线及 PNG 编码均由调用方承担。
// 关联：engine/rhi/d3d12/src/D3D12FrameReadback.h
//       engine/rhi/d3d12/src/D3D12Common.h
//       docs/architecture/README.md
// ============================================================================
#include "D3D12FrameReadback.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
constexpr std::uint32_t kQueriesPerPass = 2U;
constexpr std::uint32_t kQueriesPerFrame = kD3D12FrameReadbackPassCount * kQueriesPerPass;
constexpr std::uint64_t kTimestampBytesPerFrame = static_cast<std::uint64_t>(kQueriesPerFrame) * sizeof(std::uint64_t);

// 对尺寸和行距做 checked 算术，避免异常输入在资源或 Map 范围中回绕。
std::uint64_t CheckedMultiply(const std::uint64_t left, const std::uint64_t right, const char* context)
{
    if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right)
    {
        throw std::overflow_error{context};
    }
    return left * right;
}

std::uint64_t CheckedAdd(const std::uint64_t left, const std::uint64_t right, const char* context)
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
    {
        throw std::overflow_error{context};
    }
    return left + right;
}

bool IsBgraFormat(const DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool IsRgba8Format(const DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

std::uint32_t QueryIndex(const std::uint32_t frameIndex, const std::uint32_t passIndex, const bool end)
{
    return frameIndex * kQueriesPerFrame + passIndex * kQueriesPerPass + (end ? 1U : 0U);
}

std::uint64_t QueryOffset(const std::uint32_t passIndex, const bool end)
{
    return static_cast<std::uint64_t>(passIndex * kQueriesPerPass + (end ? 1U : 0U)) * sizeof(std::uint64_t);
}

// D3D12 Map 不负责同步；只有已完成的非零 fence 才允许映射。
bool MayMap(const std::uint64_t completedFence, const std::uint64_t fence)
{
    return fence != 0U && completedFence >= fence;
}
} // namespace

void D3D12FrameReadback::Initialize(ID3D12Device& device, ID3D12CommandQueue& queue)
{
    if (m_initialized)
    {
        throw std::logic_error{"D3D12FrameReadback::Initialize called twice"};
    }

    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDescription.Count = kD3D12FrameReadbackCount * kQueriesPerFrame;
    queryDescription.NodeMask = 0U;
    ThrowIfFailed(device.CreateQueryHeap(&queryDescription, IID_PPV_ARGS(&m_timestampHeap)),
                  "ID3D12Device::CreateQueryHeap(timestamp)");

    ThrowIfFailed(m_timestampHeap->SetName(L"M5.TimestampQueryHeap"), "timestamp query name");
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    readbackHeap.CreationNodeMask = 1U;
    readbackHeap.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC timestampDescription{};
    timestampDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    timestampDescription.Alignment = 0U;
    timestampDescription.Width = kTimestampBytesPerFrame;
    timestampDescription.Height = 1U;
    timestampDescription.DepthOrArraySize = 1U;
    timestampDescription.MipLevels = 1U;
    timestampDescription.Format = DXGI_FORMAT_UNKNOWN;
    timestampDescription.SampleDesc.Count = 1U;
    timestampDescription.SampleDesc.Quality = 0U;
    timestampDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    timestampDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

    for (FrameSlot& slot : m_frames)
    {
        ThrowIfFailed(device.CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &timestampDescription,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&slot.timestampReadback)),
                      "ID3D12Device::CreateCommittedResource(timestamp readback)");
    }

    for (std::size_t index = 0; index < m_frames.size(); ++index)
        ThrowIfFailed(
            m_frames[index].timestampReadback->SetName((L"M5.TimestampReadback." + std::to_wstring(index)).c_str()),
            "timestamp readback name");

    ThrowIfFailed(queue.GetTimestampFrequency(&m_timestampFrequency), "ID3D12CommandQueue::GetTimestampFrequency");
    if (m_timestampFrequency == 0U)
    {
        throw std::runtime_error{"D3D12 timestamp frequency is zero"};
    }
    m_initialized = true;
}

D3D12FrameReadback::FrameSlot& D3D12FrameReadback::RequireFrame(const std::uint32_t frameIndex)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12FrameReadback used before Initialize"};
    }
    if (frameIndex >= kD3D12FrameReadbackCount)
    {
        throw std::out_of_range{"D3D12FrameReadback frame index out of range"};
    }
    return m_frames[frameIndex];
}

void D3D12FrameReadback::BeginFrame(const std::uint32_t frameIndex, const std::uint64_t completedFence,
                                    const std::uint64_t frameNumber)
{
    FrameSlot& slot = RequireFrame(frameIndex);
    if (slot.recording)
    {
        throw std::logic_error{"D3D12FrameReadback::BeginFrame called while frame slot is recording"};
    }
    if (slot.fence != 0U && completedFence < slot.fence)
    {
        throw std::logic_error{"D3D12FrameReadback frame slot fence is not complete"};
    }
    if (slot.fence != 0U)
    {
        ConsumeTimestamp(slot, frameIndex, completedFence);
    }

    slot.fence = 0U;
    slot.frameNumber = frameNumber;
    slot.recording = true;
    slot.resolved = false;
    slot.began.fill(false);
    slot.ended.fill(false);
}

void D3D12FrameReadback::Poll(const std::uint64_t completedFence)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12FrameReadback used before Initialize"};
    }
    for (std::uint32_t frameIndex = 0U; frameIndex < kD3D12FrameReadbackCount; ++frameIndex)
    {
        FrameSlot& slot = m_frames[frameIndex];
        if (slot.recording || slot.fence == 0U || completedFence < slot.fence)
        {
            continue;
        }
        ConsumeTimestamp(slot, frameIndex, completedFence);
        slot.fence = 0U;
        slot.frameNumber = 0U;
        slot.resolved = false;
        slot.began.fill(false);
        slot.ended.fill(false);
    }
}

void D3D12FrameReadback::BeginPass(ID3D12GraphicsCommandList& list, const std::uint32_t frameIndex,
                                   const std::uint32_t passIndex)
{
    FrameSlot& slot = RequireFrame(frameIndex);
    if (passIndex >= kD3D12FrameReadbackPassCount)
    {
        throw std::out_of_range{"D3D12FrameReadback pass index out of range"};
    }
    if (!slot.recording || slot.began[passIndex] || slot.ended[passIndex])
    {
        throw std::logic_error{"D3D12FrameReadback::BeginPass has invalid pass state"};
    }
    list.EndQuery(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(frameIndex, passIndex, false));
    slot.began[passIndex] = true;
}

void D3D12FrameReadback::EndPass(ID3D12GraphicsCommandList& list, const std::uint32_t frameIndex,
                                 const std::uint32_t passIndex)
{
    FrameSlot& slot = RequireFrame(frameIndex);
    if (passIndex >= kD3D12FrameReadbackPassCount)
    {
        throw std::out_of_range{"D3D12FrameReadback pass index out of range"};
    }
    if (!slot.recording || !slot.began[passIndex] || slot.ended[passIndex])
    {
        throw std::logic_error{"D3D12FrameReadback::EndPass has invalid pass state"};
    }
    list.EndQuery(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(frameIndex, passIndex, true));
    slot.ended[passIndex] = true;
}

void D3D12FrameReadback::Resolve(ID3D12GraphicsCommandList& list, const std::uint32_t frameIndex)
{
    FrameSlot& slot = RequireFrame(frameIndex);
    if (!slot.recording || slot.resolved)
    {
        throw std::logic_error{"D3D12FrameReadback::Resolve has invalid frame state"};
    }

    for (std::uint32_t passIndex = 0U; passIndex < kD3D12FrameReadbackPassCount; ++passIndex)
    {
        if (slot.began[passIndex] && slot.ended[passIndex])
        {
            list.ResolveQueryData(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                  QueryIndex(frameIndex, passIndex, false), kQueriesPerPass,
                                  slot.timestampReadback.Get(), QueryOffset(passIndex, false));
        }
    }
    slot.resolved = true;
}

void D3D12FrameReadback::Commit(const std::uint32_t frameIndex, const std::uint64_t fence)
{
    FrameSlot& slot = RequireFrame(frameIndex);
    if (fence == 0U)
    {
        throw std::invalid_argument{"D3D12FrameReadback::Commit requires a non-zero fence"};
    }
    if (!slot.recording || !slot.resolved || slot.fence != 0U)
    {
        throw std::logic_error{"D3D12FrameReadback::Commit has invalid frame state"};
    }
    slot.fence = fence;
    slot.recording = false;
}

void D3D12FrameReadback::ConsumeTimestamp(FrameSlot& slot, const std::uint32_t frameIndex,
                                          const std::uint64_t completedFence)
{
    if (!MayMap(completedFence, slot.fence))
    {
        throw std::logic_error{"D3D12FrameReadback timestamp Map requires a completed non-zero fence"};
    }

    D3D12_RANGE readRange{};
    readRange.Begin = 0U;
    readRange.End = static_cast<SIZE_T>(kTimestampBytesPerFrame);
    std::byte* mapped = nullptr;
    ThrowIfFailed(slot.timestampReadback->Map(0U, &readRange, reinterpret_cast<void**>(&mapped)),
                  "timestamp readback Map");
    if (mapped == nullptr)
    {
        slot.timestampReadback->Unmap(0U, nullptr);
        throw std::runtime_error{"timestamp readback Map returned a null pointer"};
    }

    D3D12FrameTiming timing;
    timing.frameIndex = frameIndex;
    timing.frameNumber = slot.frameNumber;
    for (std::uint32_t passIndex = 0U; passIndex < kD3D12FrameReadbackPassCount; ++passIndex)
    {
        if (slot.began[passIndex] && slot.ended[passIndex])
        {
            std::uint64_t begin = 0U;
            std::uint64_t end = 0U;
            std::memcpy(&begin, mapped + QueryOffset(passIndex, false), sizeof(begin));
            std::memcpy(&end, mapped + QueryOffset(passIndex, true), sizeof(end));
            if (end >= begin)
            {
                timing.passMilliseconds[passIndex] =
                    static_cast<double>(end - begin) * 1000.0 / static_cast<double>(m_timestampFrequency);
                timing.valid[passIndex] = true;
            }
        }
    }
    slot.timestampReadback->Unmap(0U, nullptr);
    m_completedTimings.push_back(timing);
}

void D3D12FrameReadback::RecordScreenshot(ID3D12Device& device, ID3D12GraphicsCommandList& list,
                                          ID3D12Resource& resource, const std::uint32_t width,
                                          const std::uint32_t height)
{
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12FrameReadback used before Initialize"};
    }
    if (!CanRecordScreenshot())
    {
        throw std::logic_error{"D3D12FrameReadback screenshot ring is full or has an uncommitted copy"};
    }
    auto& slot = m_screenshots[m_screenshotWriteIndex];
    if (width == 0U || height == 0U)
    {
        throw std::invalid_argument{"D3D12FrameReadback screenshot dimensions must be non-zero"};
    }

    const D3D12_RESOURCE_DESC description = resource.GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.Width != width ||
        description.Height != height || description.DepthOrArraySize != 1U || description.MipLevels != 1U ||
        description.SampleDesc.Count != 1U || (!IsRgba8Format(description.Format) && !IsBgraFormat(description.Format)))
    {
        throw std::invalid_argument{"D3D12FrameReadback screenshot resource must be a single-sample RGBA8 texture2D"};
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    device.GetCopyableFootprints(&description, 0U, 1U, 0U, &footprint, &rowCount, &rowSize, &totalBytes);
    const std::uint64_t rowBytes = CheckedMultiply(width, 4U, "screenshot row byte count overflow");
    const std::uint64_t footprintEnd =
        CheckedAdd(footprint.Offset,
                   CheckedAdd(CheckedMultiply(footprint.Footprint.RowPitch, height - 1U,
                                              "screenshot footprint byte count overflow"),
                              rowBytes, "screenshot final row overflow"),
                   "screenshot footprint offset overflow");
    if (rowCount < height || rowSize != rowBytes || footprint.Footprint.RowPitch < rowBytes || totalBytes == 0U ||
        footprintEnd > totalBytes)
    {
        throw std::invalid_argument{"D3D12FrameReadback returned an invalid RGBA8 footprint"};
    }

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    readbackHeap.CreationNodeMask = 1U;
    readbackHeap.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = totalBytes;
    bufferDescription.Height = 1U;
    bufferDescription.DepthOrArraySize = 1U;
    bufferDescription.MipLevels = 1U;
    bufferDescription.Format = DXGI_FORMAT_UNKNOWN;
    bufferDescription.SampleDesc.Count = 1U;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
                  "ID3D12Device::CreateCommittedResource(screenshot readback)");
    ThrowIfFailed(readback->SetName((L"M5.ScreenshotReadback." + std::to_wstring(m_screenshotWriteIndex)).c_str()),
                  "screenshot readback name");

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = &resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0U;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list.CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);

    slot.readback = std::move(readback);
    slot.footprint = footprint;
    slot.width = width;
    slot.height = height;
    slot.totalBytes = totalBytes;
    slot.fence = 0U;
    slot.bgra = IsBgraFormat(description.Format);
    slot.committed = false;
    slot.pending = true;
    ++m_screenshotCount;
}

void D3D12FrameReadback::CommitScreenshot(const std::uint64_t fence)
{
    auto& slot = m_screenshots[m_screenshotWriteIndex];
    if (!m_initialized)
    {
        throw std::logic_error{"D3D12FrameReadback used before Initialize"};
    }
    if (fence == 0U)
    {
        throw std::invalid_argument{"D3D12FrameReadback::CommitScreenshot requires a non-zero fence"};
    }
    if (!slot.pending || slot.committed)
    {
        throw std::logic_error{"D3D12FrameReadback::CommitScreenshot has no uncommitted screenshot"};
    }
    slot.fence = fence;
    slot.committed = true;
    m_screenshotWriteIndex = (m_screenshotWriteIndex + 1U) % m_screenshots.size();
}

std::optional<D3D12Rgba8Image> D3D12FrameReadback::TryReadScreenshot(const std::uint64_t completedFence)
{
    auto& slot = m_screenshots[m_screenshotReadIndex];
    if (!slot.pending || !slot.committed || !MayMap(completedFence, slot.fence))
    {
        return std::nullopt;
    }

    // GetCopyableFootprints 的总大小不含最后一行尾部 padding。
    const std::uint64_t rowBytes = CheckedMultiply(slot.width, 4U, "screenshot row byte count overflow");
    const std::uint64_t readBytes = CheckedAdd(
        CheckedMultiply(slot.footprint.Footprint.RowPitch, slot.height - 1U, "screenshot read range overflow"),
        rowBytes, "screenshot final row overflow");
    const std::uint64_t readEnd = CheckedAdd(slot.footprint.Offset, readBytes, "screenshot read range overflow");
    if (readEnd > slot.totalBytes || slot.footprint.Footprint.RowPitch < rowBytes)
    {
        throw std::runtime_error{"D3D12FrameReadback screenshot read range exceeds the readback buffer"};
    }

    D3D12_RANGE readRange{};
    readRange.Begin = static_cast<SIZE_T>(slot.footprint.Offset);
    readRange.End = static_cast<SIZE_T>(readEnd);
    std::byte* mapped = nullptr;
    ThrowIfFailed(slot.readback->Map(0U, &readRange, reinterpret_cast<void**>(&mapped)), "screenshot readback Map");
    if (mapped == nullptr)
    {
        slot.readback->Unmap(0U, nullptr);
        throw std::runtime_error{"screenshot readback Map returned a null pointer"};
    }

    const std::size_t packedRowBytes = static_cast<std::size_t>(rowBytes);
    D3D12Rgba8Image image;
    image.width = slot.width;
    image.height = slot.height;
    image.pixels.resize(packedRowBytes * static_cast<std::size_t>(slot.height));
    for (std::uint32_t row = 0U; row < slot.height; ++row)
    {
        const std::byte* sourceRow =
            mapped + slot.footprint.Offset + static_cast<std::size_t>(row) * slot.footprint.Footprint.RowPitch;
        std::uint8_t* destinationRow = image.pixels.data() + static_cast<std::size_t>(row) * packedRowBytes;
        std::memcpy(destinationRow, sourceRow, packedRowBytes);
        if (slot.bgra)
        {
            for (std::size_t pixel = 0U; pixel < packedRowBytes; pixel += 4U)
            {
                std::swap(destinationRow[pixel], destinationRow[pixel + 2U]);
            }
        }
    }
    slot.readback->Unmap(0U, nullptr);

    slot.readback.Reset();
    slot.footprint = {};
    slot.width = 0U;
    slot.height = 0U;
    slot.totalBytes = 0U;
    slot.fence = 0U;
    slot.bgra = false;
    slot.committed = false;
    slot.pending = false;
    --m_screenshotCount;
    m_screenshotReadIndex = (m_screenshotReadIndex + 1U) % m_screenshots.size();
    return image;
}

std::vector<D3D12FrameTiming> D3D12FrameReadback::TakeTimings() noexcept
{
    std::vector<D3D12FrameTiming> result;
    result.swap(m_completedTimings);
    return result;
}

bool D3D12FrameReadback::IsScreenshotPending() const noexcept
{
    return m_screenshotCount != 0U;
}
bool D3D12FrameReadback::CanRecordScreenshot() const noexcept
{
    return m_initialized && m_screenshotCount < m_screenshots.size() && !m_screenshots[m_screenshotWriteIndex].pending;
}
} // namespace MiniEngine::Rhi::D3D12
