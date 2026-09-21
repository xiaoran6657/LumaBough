// ============================================================================
// D3D11Screenshot.cpp — 截图 readback 与图像比较的实现
// 里程碑：M4（07 篇「截图读取路径」「Diff 输出」）
// 职责：实现 3-slot staging ring（CopyResource → EVENT 查询 → Map → RowPitch 逐行
//       打包）、WIC PNG 原子写（tmp → 哈希 → rename，失败只清理临时文件）、
//       PNG 解码与纯 CPU 比较器。关键契约：
//   1. 后备缓冲只接受 R8G8B8A8/B8G8R8A8 UNORM、Sample.Count=1；其他格式失败并
//      报出实际值（07 篇：不静默转换未知格式）。
//   2. Map 前用 D3D11_QUERY_EVENT 等待，避免提交后立刻阻塞 GPU；本路径耗时不进
//      steady-state 帧基线。
//   3. BGRA 后备缓冲编码时选 GUID_WICPixelFormat32bppBGRA，RGBA 选 32bppRGBA——
//      WIC 按给定 GUID 解释字节序，通道顺序由此显式约定（07 篇 6）。
//   4. 原子写：先写 target.tmp 并 flush/commit 全部 WIC 对象，读回哈希，再
//      std::filesystem::rename；失败只删除 tmp，目标文件要么旧要么新，绝不半成品。
// 关联：engine/rhi/d3d11/src/D3D11Screenshot.h（契约）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（Enqueue/Poll 的调用时机）
// ============================================================================

#include "D3D11Screenshot.h"

#include "D3D11Error.h"

#include <MiniEngine/Assets/Sha256.h>

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace MiniEngine::Rhi::D3D11
{
namespace
{
using Microsoft::WRL::ComPtr;

// COM/WIC 工厂：进程内一个就够；首次调用时初始化 COM（Sandbox 主线程默认未初始化）。
ComPtr<IWICImagingFactory> WicFactory()
{
    // COINIT_MULTITHREADED：WIC 官方推荐；已初始化为其他模型时返回 RC_FALSE，同样可用。
    static const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)comInit; // 进程生命周期内不配对 Uninitialize（退出时由 OS 回收）

    static ComPtr<IWICImagingFactory> factory;
    if (factory == nullptr)
    {
        ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
                      "CoCreateInstance(CLSID_WICImagingFactory)");
    }
    return factory;
}

// 把紧密排列的像素缓冲编码为 PNG 并原子落盘；返回 PNG 字节的 SHA-256（hex）。
// pixelFormat 由调用方按实际字节序给出（RGBA/BGRA），WIC 不做任何转换。
std::string EncodePngAtomically(std::span<const std::uint8_t> pixels, const std::uint32_t width,
                                const std::uint32_t height, const GUID& pixelFormat,
                                const std::filesystem::path& target)
{
    const ComPtr<IWICImagingFactory> factory = WicFactory();

    // 1) 先写到 target.tmp：验证成功前目标路径不动（07 篇 8）。
    std::filesystem::path temporary = target;
    temporary += ".tmp";

    ComPtr<IWICStream> stream;
    ThrowIfFailed(factory->CreateStream(stream.ReleaseAndGetAddressOf()), "IWICImagingFactory::CreateStream");
    ThrowIfFailed(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE),
                  "IWICStream::InitializeFromFilename");

    ComPtr<IWICBitmapEncoder> encoder;
    ThrowIfFailed(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.ReleaseAndGetAddressOf()),
                  "IWICImagingFactory::CreateEncoder(PNG)");
    ThrowIfFailed(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    ThrowIfFailed(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(), properties.ReleaseAndGetAddressOf()),
                  "IWICBitmapEncoder::CreateNewFrame");
    ThrowIfFailed(frame->Initialize(properties.Get()), "IWICBitmapFrameEncode::Initialize");
    ThrowIfFailed(frame->SetSize(width, height), "IWICBitmapFrameEncode::SetSize");

    WICPixelFormatGUID establishedFormat = pixelFormat;
    ThrowIfFailed(frame->SetPixelFormat(&establishedFormat), "IWICBitmapFrameEncode::SetPixelFormat");

    // 编码器可能改写像素格式（本机 WIC PNG encoder 把 32bppRGBA 确立为 32bppBGRA）。
    // WritePixels 的字节序必须按**确立后**的格式解释：请求 RGBA、确立 BGRA 时在此
    // swizzle R/B；其他确立值一律失败（不静默转换通道顺序，07 篇 6）。
    std::span<const std::uint8_t> encodePixels = pixels;
    std::vector<std::uint8_t> swizzled;
    if (IsEqualGUID(establishedFormat, pixelFormat) == FALSE)
    {
        if (IsEqualGUID(establishedFormat, GUID_WICPixelFormat32bppBGRA) != FALSE &&
            IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppRGBA) != FALSE)
        {
            swizzled.assign(pixels.begin(), pixels.end());
            for (std::size_t offset = 0; offset < swizzled.size(); offset += 4U)
            {
                std::swap(swizzled[offset], swizzled[offset + 2U]);
            }
            encodePixels = swizzled;
        }
        else
        {
            std::ostringstream diagnostics;
            diagnostics << "WIC PNG encoder established an unsupported pixel format 0x" << std::hex
                        << establishedFormat.Data1;
            throw std::runtime_error{diagnostics.str()};
        }
    }

    ThrowIfFailed(frame->WritePixels(height, width * 4U, static_cast<UINT>(encodePixels.size()),
                                     const_cast<std::uint8_t*>(encodePixels.data())),
                  "IWICBitmapFrameEncode::WritePixels");
    ThrowIfFailed(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    ThrowIfFailed(encoder->Commit(), "IWICBitmapEncoder::Commit");
    ThrowIfFailed(stream->Commit(STGC_DEFAULT), "IWICStream::Commit");
    stream.Reset();
    encoder.Reset();
    frame.Reset();

    // 2) 读回临时文件计算 SHA-256（元数据 pngSha256 的来源；哈希对象即最终字节）。
    std::ifstream temporaryFile(temporary, std::ios::binary);
    if (!temporaryFile.is_open())
    {
        throw std::runtime_error{"failed to reopen the temporary PNG for hashing"};
    }
    const std::string pngBytes{std::istreambuf_iterator<char>(temporaryFile), std::istreambuf_iterator<char>()};
    temporaryFile.close();
    const auto* pngData = reinterpret_cast<const std::byte*>(pngBytes.data());
    const Assets::Sha256Digest digest = Assets::Sha256(std::span<const std::byte>(pngData, pngBytes.size()));

    // 3) 原子替换：同卷 rename；失败清理临时文件，不留半成品。
    std::error_code renameError;
    std::filesystem::rename(temporary, target, renameError);
    if (renameError)
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        std::ostringstream stream2;
        stream2 << "atomic rename of the capture failed: " << renameError.message();
        throw std::runtime_error{stream2.str()};
    }

    return Assets::ToHexDigest(digest);
}
} // namespace

void D3D11Screenshot::Create(ID3D11Device& device)
{
    D3D11_QUERY_DESC eventDesc{};
    eventDesc.Query = D3D11_QUERY_EVENT;
    for (Slot& slot : m_slots)
    {
        ThrowIfFailed(device.CreateQuery(&eventDesc, slot.completed.ReleaseAndGetAddressOf()),
                      "ID3D11Device::CreateQuery(D3D11_QUERY_EVENT)");
    }
}

void D3D11Screenshot::Release() noexcept
{
    for (Slot& slot : m_slots)
    {
        slot.staging.Reset();
        slot.completed.Reset();
        slot.pending = false;
        slot.target.clear();
    }
    m_nextSlot = 0;
    m_oldest = 0;
    m_pendingCount = 0;
}

bool D3D11Screenshot::AllIdle() const noexcept
{
    return m_pendingCount == 0;
}

bool D3D11Screenshot::Enqueue(ID3D11Device& device, ID3D11DeviceContext& context, ID3D11Texture2D& backBuffer,
                              const std::filesystem::path& target, std::string& error)
{
    if (m_pendingCount == m_slots.size())
    {
        error = "screenshot ring is full (3 pending captures)";
        return false;
    }

    D3D11_TEXTURE2D_DESC source{};
    backBuffer.GetDesc(&source);
    // 07 篇：交换链格式不受支持时立即失败并报出实际格式，不静默转换。
    if (source.SampleDesc.Count != 1U ||
        (source.Format != DXGI_FORMAT_R8G8B8A8_UNORM && source.Format != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        std::ostringstream stream;
        stream << "unsupported back buffer for capture: format=0x" << std::hex << source.Format
               << ", samples=" << source.SampleDesc.Count << " (expected R8G8B8A8/B8G8R8A8 UNORM without MSAA)";
        error = stream.str();
        return false;
    }

    D3D11_TEXTURE2D_DESC staging = source;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    // RowPitch/stride 以 UINT 传递（WritePixels、Map 行距），先做 checked 乘法：
    // 与 AssetManager 的 rowPitch 校验同款防御风格（M4-07 审查 §三.3）。
    if (source.Width > (std::numeric_limits<UINT>::max() / 4U))
    {
        std::ostringstream stream;
        stream << "back buffer width " << source.Width << " overflows the UINT byte stride";
        error = stream.str();
        return false;
    }

    Slot& slot = m_slots[m_nextSlot];
    ThrowIfFailed(device.CreateTexture2D(&staging, nullptr, slot.staging.ReleaseAndGetAddressOf()),
                  "ID3D11Device::CreateTexture2D(screenshot staging)");
    context.CopyResource(slot.staging.Get(), &backBuffer);
    context.End(slot.completed.Get()); // Begin 已在本帧前段的隐式状态外（EVENT 查询无需 Begin）
    slot.target = target;
    slot.pending = true;

    m_nextSlot = (m_nextSlot + 1U) % m_slots.size();
    ++m_pendingCount;
    return true;
}

D3D11Screenshot::PollResult D3D11Screenshot::Poll(ID3D11DeviceContext& context, ScreenshotResult& out,
                                                  std::string& error)
{
    if (m_pendingCount == 0)
    {
        return PollResult::Idle;
    }

    Slot& slot = m_slots[m_oldest];
    if (!slot.pending)
    {
        return PollResult::Idle; // 不变量：pendingCount>0 时最老 slot 必然 pending
    }

    BOOL done = FALSE;
    const HRESULT queryResult =
        context.GetData(slot.completed.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (queryResult == S_FALSE || (SUCCEEDED(queryResult) && done == FALSE))
    {
        return PollResult::Pending; // GPU 未完成：不阻塞（07 篇 4）
    }
    if (FAILED(queryResult))
    {
        error = "ID3D11DeviceContext::GetData(screenshot event) failed";
        slot.pending = false;
        slot.staging.Reset();
        --m_pendingCount;
        m_oldest = (m_oldest + 1U) % m_slots.size();
        return PollResult::Failed;
    }

    // GPU 已写完 staging：Map（此时不阻塞）并按 RowPitch 逐行打包（07 篇 5）。
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT mapResult = context.Map(slot.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult))
    {
        // 07 篇负向：staging Map 失败记录 HRESULT，不生成空 PNG。
        std::ostringstream stream;
        stream << "staging Map failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(mapResult);
        error = stream.str();
        slot.pending = false;
        slot.staging.Reset();
        --m_pendingCount;
        m_oldest = (m_oldest + 1U) % m_slots.size();
        return PollResult::Failed;
    }

    D3D11_TEXTURE2D_DESC description{};
    slot.staging->GetDesc(&description);
    const std::size_t rowBytes = static_cast<std::size_t>(description.Width) * 4U;
    if (mapped.RowPitch < rowBytes)
    {
        context.Unmap(slot.staging.Get(), 0);
        error = "staging RowPitch is smaller than the packed row size";
        slot.pending = false;
        slot.staging.Reset();
        --m_pendingCount;
        m_oldest = (m_oldest + 1U) % m_slots.size();
        return PollResult::Failed;
    }

    std::vector<std::uint8_t> packed(rowBytes * description.Height);
    for (std::uint32_t y = 0; y < description.Height; ++y)
    {
        const auto* sourceRow =
            static_cast<const std::byte*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        std::memcpy(packed.data() + static_cast<std::size_t>(y) * rowBytes, sourceRow, rowBytes);
    }
    context.Unmap(slot.staging.Get(), 0);

    // 07 篇 6（RGBA8/BGRA8 channel order）：BGRA 后备缓冲在读回后显式交换 R/B，
    // 统一转成 RGBA 再编码——WIC 的 PNG 编码器原生只支持 RGBA 排列（BGRA GUID 会被
    // 拒绝），通道顺序在 readback 这一层归一，比较器与 PNG 永远是 RGBA。
    if (description.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        for (std::size_t offset = 0; offset < packed.size(); offset += 4U)
        {
            std::swap(packed[offset], packed[offset + 2U]);
        }
    }

    try
    {
        out.pngPath = slot.target;
        out.width = description.Width;
        out.height = description.Height;
        out.pngSha256 = EncodePngAtomically(packed, description.Width, description.Height, GUID_WICPixelFormat32bppRGBA,
                                            slot.target);
    }
    catch (const std::exception& encodeError)
    {
        error = encodeError.what();
        slot.pending = false;
        slot.staging.Reset();
        --m_pendingCount;
        m_oldest = (m_oldest + 1U) % m_slots.size();
        return PollResult::Failed;
    }

    slot.pending = false;
    slot.staging.Reset();
    --m_pendingCount;
    m_oldest = (m_oldest + 1U) % m_slots.size();
    return PollResult::Completed;
}

ImageDiffMetrics CompareRgba8(const std::span<const std::uint8_t> golden, const std::span<const std::uint8_t> candidate,
                              const std::uint8_t changedThreshold)
{
    // 07 篇：输入为空、尺寸不同或非 RGBA8 必须在计算分位数前失败。
    if (golden.empty() || golden.size() != candidate.size() || golden.size() % 4 != 0)
    {
        throw std::invalid_argument{"CompareRgba8 requires equal, non-empty RGBA8 buffers"};
    }

    const std::size_t pixelCount = golden.size() / 4U;

    std::vector<double> errors;
    errors.reserve(pixelCount * 3U);
    double absoluteSum = 0.0;
    double squaredSum = 0.0;
    std::uint64_t changed = 0;

    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const std::size_t offset = pixel * 4U + channel;
            const int delta = std::abs(static_cast<int>(golden[offset]) - static_cast<int>(candidate[offset]));
            const double error = static_cast<double>(delta) / 255.0;
            errors.push_back(error);
            absoluteSum += error;
            squaredSum += error * error;
            changed += delta > static_cast<int>(changedThreshold) ? 1U : 0U;
        }
    }

    std::sort(errors.begin(), errors.end());
    // p99 = 第 ceil(0.99*N) 个样本（1-based）→ 下标 ceil(0.99*N)-1（至少 0）。
    const std::size_t p99Index = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(errors.size()))) - 1U;
    const std::size_t clampedIndex = std::min(p99Index, errors.size() - 1U);

    ImageDiffMetrics metrics;
    metrics.mae = absoluteSum / static_cast<double>(errors.size());
    metrics.rmse = std::sqrt(squaredSum / static_cast<double>(errors.size()));
    metrics.p99 = errors[clampedIndex];
    metrics.maxError = errors.back();
    metrics.changedRate = static_cast<double>(changed) / static_cast<double>(errors.size());
    metrics.sampleCount = errors.size();
    // maxError 像素坐标需要真实宽度（行主序），由带尺寸的入口（ComparePngFiles）回填。
    return metrics;
}

std::string MetadataIncomparableReason(const ScreenshotConfigIdentity& golden,
                                       const ScreenshotConfigIdentity& candidate)
{
    // 07 篇：比较器先比 schema 与关键配置；任何一项不同都报 INCOMPARABLE。
    // schemaVersion 排第一：schema 演进时旧 golden 必须被拒绝比较（而不是给出
    // 无意义的像素 PASS/FAIL）。
    if (golden.schemaVersion != candidate.schemaVersion)
    {
        return "schemaVersion differs: " + std::to_string(golden.schemaVersion) + " vs " +
               std::to_string(candidate.schemaVersion);
    }
    if (golden.scene != candidate.scene)
    {
        return "scene differs: '" + golden.scene + "' vs '" + candidate.scene + "'";
    }
    if (golden.width != candidate.width || golden.height != candidate.height)
    {
        return "resolution differs";
    }
    if (golden.fixedTick != candidate.fixedTick)
    {
        return "fixedTick differs";
    }
    if (golden.assetManifestSha256 != candidate.assetManifestSha256)
    {
        return "assetManifestSha256 differs";
    }
    if (golden.environmentArtifactSha256 != candidate.environmentArtifactSha256)
    {
        return "environmentArtifactSha256 differs";
    }
    if (golden.iblProfile != candidate.iblProfile)
    {
        return "iblProfile differs";
    }
    if (std::abs(golden.exposureEv - candidate.exposureEv) > 1.0e-6)
    {
        return "exposureEv differs";
    }
    if (golden.toneMapper != candidate.toneMapper)
    {
        return "toneMapper differs";
    }
    return {};
}

bool DecodePngToRgba8(const std::filesystem::path& path, std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                      std::uint32_t& height, std::string& error)
{
    try
    {
        const ComPtr<IWICImagingFactory> factory = WicFactory();
        ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand,
                                                         decoder.ReleaseAndGetAddressOf()),
                      "IWICImagingFactory::CreateDecoderFromFilename");
        ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()), "IWICBitmapDecoder::GetFrame");

        UINT decodedWidth = 0;
        UINT decodedHeight = 0;
        ThrowIfFailed(frame->GetSize(&decodedWidth, &decodedHeight), "IWICBitmapFrameDecode::GetSize");

        // 统一转换到 32bppRGBA：比较器只见一种通道顺序，消除源格式差异。
        ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf()),
                      "IWICImagingFactory::CreateFormatConverter");
        ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                            0.0, WICBitmapPaletteTypeCustom),
                      "IWICFormatConverter::Initialize");

        const std::size_t rowBytes = static_cast<std::size_t>(decodedWidth) * 4U;
        pixels.assign(rowBytes * decodedHeight, std::uint8_t{0});
        ThrowIfFailed(converter->CopyPixels(nullptr, static_cast<UINT>(rowBytes), static_cast<UINT>(pixels.size()),
                                            pixels.data()),
                      "IWICFormatConverter::CopyPixels");
        width = decodedWidth;
        height = decodedHeight;
        return true;
    }
    catch (const std::exception& decodeError)
    {
        error = decodeError.what();
        return false;
    }
}

bool ComparePngFiles(const std::filesystem::path& golden, const std::filesystem::path& candidate,
                     const std::uint8_t changedThreshold, ImageDiffMetrics& metrics, std::string& error)
{
    std::vector<std::uint8_t> goldenPixels;
    std::vector<std::uint8_t> candidatePixels;
    std::uint32_t goldenWidth = 0;
    std::uint32_t goldenHeight = 0;
    std::uint32_t candidateWidth = 0;
    std::uint32_t candidateHeight = 0;
    if (!DecodePngToRgba8(golden, goldenPixels, goldenWidth, goldenHeight, error))
    {
        error = "failed to decode golden PNG: " + error;
        return false;
    }
    if (!DecodePngToRgba8(candidate, candidatePixels, candidateWidth, candidateHeight, error))
    {
        error = "failed to decode candidate PNG: " + error;
        return false;
    }
    // 07 篇负向：尺寸不同必须 FAIL（不能静默缩放或裁剪）。
    if (goldenWidth != candidateWidth || goldenHeight != candidateHeight)
    {
        std::ostringstream stream;
        stream << "image sizes differ: golden " << goldenWidth << "x" << goldenHeight << ", candidate "
               << candidateWidth << "x" << candidateHeight;
        error = stream.str();
        return false;
    }

    metrics = CompareRgba8(goldenPixels, candidatePixels, changedThreshold);
    // 有真实宽度：反推 maxError 像素坐标（行主序）。
    const double worstThreshold = metrics.maxError;
    for (std::uint32_t y = 0; y < goldenHeight; ++y)
    {
        for (std::uint32_t x = 0; x < goldenWidth; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * goldenWidth + x) * 4U;
            int delta = 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                delta = std::max(delta, std::abs(static_cast<int>(goldenPixels[offset + channel]) -
                                                 static_cast<int>(candidatePixels[offset + channel])));
            }
            if (std::abs(static_cast<double>(delta) / 255.0 - worstThreshold) <= 1.0e-12)
            {
                metrics.worstX = x;
                metrics.worstY = y;
                return true;
            }
        }
    }
    return true;
}

bool WriteAbsDiffPng(const std::span<const std::uint8_t> golden, const std::span<const std::uint8_t> candidate,
                     const std::uint32_t width, const std::uint32_t height, const float scale,
                     const std::filesystem::path& target, std::string& error)
{
    if (golden.size() != candidate.size() || golden.size() != static_cast<std::size_t>(width) * height * 4U)
    {
        error = "abs diff requires equal RGBA8 buffers matching the given size";
        return false;
    }

    // 绝对差 × scale（07 篇 diff-abs-x8），alpha 固定 255；saturate 到 255。
    std::vector<std::uint8_t> diff(golden.size(), std::uint8_t{0});
    for (std::size_t index = 0; index < golden.size(); index += 4U)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const int delta =
                std::abs(static_cast<int>(golden[index + channel]) - static_cast<int>(candidate[index + channel]));
            const int amplified = std::min(255, static_cast<int>(static_cast<float>(delta) * scale));
            diff[index + channel] = static_cast<std::uint8_t>(amplified);
        }
        diff[index + 3U] = 255;
    }

    try
    {
        EncodePngAtomically(diff, width, height, GUID_WICPixelFormat32bppRGBA, target);
        return true;
    }
    catch (const std::exception& encodeError)
    {
        error = encodeError.what();
        return false;
    }
}
} // namespace MiniEngine::Rhi::D3D11
