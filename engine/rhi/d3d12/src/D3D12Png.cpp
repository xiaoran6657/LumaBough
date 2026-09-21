// ============================================================================
// D3D12Png.cpp — 与 M4 同口径的 WIC 编码、通道转换和原子发布。
// 里程碑：M5-09。
// 职责：把已完成 fence 的 CPU 像素编码为 PNG，不把 D3D11 后端链接到 D3D12。
// 关联：engine/rhi/d3d11/src/D3D11Screenshot.cpp。
// ============================================================================
#include "D3D12Png.h"
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
namespace MiniEngine::Rhi::D3D12
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

std::string WritePng(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
                     const std::filesystem::path& target)
{
    if (width == 0 || height == 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
    {
        throw std::invalid_argument("PNG dimensions and RGBA bytes disagree");
    }
    if (!target.parent_path().empty())
    {
        std::filesystem::create_directories(target.parent_path());
    }
    return EncodePngAtomically(rgba, width, height, GUID_WICPixelFormat32bppRGBA, target);
}
} // namespace MiniEngine::Rhi::D3D12
