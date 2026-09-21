// ============================================================================
// WicImage.cpp — WIC 解码实现（PNG/JPEG → RGBA8）
// 里程碑：M3（03 篇 WIC 阶段）
// 职责：按编码字节前缀识别格式，WIC 单帧解码并转换到
//       GUID_WICPixelFormat32bppRGBA，执行 frame / dimension / 像素预算校验
//       与 checked 乘法；COM 初始化容错（可重入初始化）。
// 关联：Microsoft Learn：IWICImagingFactory / IWICBitmapDecoder（单帧解码）
//       tools/asset_cooker/src/WicImage.h（输出规格）
// ============================================================================

#include "WicImage.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace MiniEngine::Tools
{
namespace
{
// 03 篇 WIC 预算：编码字节与像素总量都必须有限。维度上限同时保护
// checked 乘法和后续 rowPitch*height 运算（引擎侧再次校验）。
inline constexpr std::uint32_t kMaxImageDimension = 16384;
inline constexpr std::uint64_t kMaxEncodedBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxPixelCount = 268435456ULL; // 16384*16384

std::string HResultText(const HRESULT hr)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<std::uint32_t>(static_cast<unsigned long>(hr));
    return stream.str();
}
} // namespace

bool DecodeImageBytes(const std::vector<std::byte>& encoded, DecodedImage& out, std::string& error)
{
    out = {};
    if (encoded.empty())
    {
        error = "empty image bytes";
        return false;
    }
    if (encoded.size() > kMaxEncodedBytes)
    {
        error = "image bytes exceed decode budget";
        return false;
    }

    // WIC 需要 COM；重复初始化返回 S_FALSE（可重入），不算失败。
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (comResult != S_OK && comResult != S_FALSE)
    {
        error = "CoInitializeEx failed: " + HResultText(comResult);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
    {
        error = "CoCreateInstance(WIC) failed";
        return false;
    }

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    {
        error = "CreateStreamOnHGlobal failed";
        return false;
    }
    ULONG written = 0;
    const HRESULT writeResult = stream->Write(encoded.data(), static_cast<ULONG>(encoded.size()), &written);
    if (FAILED(writeResult) || written != encoded.size())
    {
        error = "failed to copy image bytes into stream";
        return false;
    }
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
    {
        error = "failed to rewind image stream";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    const HRESULT createDecoderResult =
        factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(createDecoderResult))
    {
        error = "CreateDecoderFromStream failed: " + HResultText(createDecoderResult);
        return false;
    }

    UINT frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || frameCount != 1)
    {
        error = "image must contain exactly one frame";
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
    {
        error = "failed to get decoder frame";
        return false;
    }

    // 统一转 RGBA8：BMP/GIF 之类非 PNG/JPEG 输入同样收敛到 32bppRGBA。
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)))
    {
        error = "CreateFormatConverter failed";
        return false;
    }
    const HRESULT initResult = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                                     WICBitmapPaletteTypeCustom);
    if (FAILED(initResult))
    {
        error = "pixel format conversion failed: " + HResultText(initResult);
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)))
    {
        error = "failed to query image dimensions";
        return false;
    }
    if (width == 0 || height == 0 || width > kMaxImageDimension || height > kMaxImageDimension ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxPixelCount)
    {
        error = "image dimensions exceed decode budget";
        return false;
    }

    const std::uint64_t byteCount = static_cast<std::uint64_t>(width) * 4U * static_cast<std::uint64_t>(height);
    if (byteCount > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
    {
        error = "decoded size exceeds address space";
        return false;
    }

    out.width = width;
    out.height = height;
    out.rgba8.resize(static_cast<std::size_t>(byteCount));
    const HRESULT copyResult = converter->CopyPixels(nullptr, width * 4U, static_cast<UINT>(out.rgba8.size()),
                                                     reinterpret_cast<BYTE*>(out.rgba8.data()));
    if (FAILED(copyResult))
    {
        error = "CopyPixels failed: " + HResultText(copyResult);
        return false;
    }
    return true;
}
} // namespace MiniEngine::Tools
