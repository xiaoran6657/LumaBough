#include <MiniEngine/Rhi/RhiValidation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <utility>
#include <vector>

using namespace MiniEngine::Rhi;

namespace
{
template <class Action> RhiError ErrorFrom(Action&& action)
{
    try
    {
        action();
        ADD_FAILURE() << "expected RhiValidationError";
    }
    catch (const RhiValidationError& error)
    {
        return error.Error();
    }
    return {};
}

std::vector<std::byte> Bytes(std::uint64_t count)
{
    return std::vector<std::byte>(static_cast<std::size_t>(count));
}

std::uint64_t BytesPerPixel(Format format)
{
    switch (format)
    {
    case Format::Rgba8Unorm:
    case Format::Rgba8UnormSrgb:
    case Format::Rg16Float:
    case Format::R32Float:
    case Format::D32Float:
        return 4;
    case Format::Rgba16Float:
        return 8;
    case Format::Unknown:
    case Format::Count:
        return 0;
    }
    return 0;
}

std::pair<std::uint32_t, std::uint32_t> MipExtent(const TextureDesc& desc, std::uint16_t mip)
{
    auto width = desc.extent.width;
    auto height = desc.extent.height;
    for (std::uint16_t level = 0; level < mip; ++level)
    {
        if (width > 1)
        {
            width /= 2;
        }
        if (height > 1)
        {
            height /= 2;
        }
    }
    return {width, height};
}

struct TexturePayload final
{
    std::vector<std::vector<std::byte>> storage;
    std::vector<TextureSubresourceData> subresources;
};

TexturePayload MakePayload(const TextureDesc& desc, bool layerMajor)
{
    TexturePayload payload;
    const auto count = static_cast<std::size_t>(desc.arrayLayers) * desc.mipLevels;
    payload.storage.reserve(count);
    payload.subresources.reserve(count);
    auto add = [&](std::uint16_t mip)
    {
        const auto [width, height] = MipExtent(desc, mip);
        const auto rowBytes = static_cast<std::uint64_t>(width) * BytesPerPixel(desc.format);
        const auto footprint = rowBytes * (height - 1U) + rowBytes;
        payload.storage.push_back(Bytes(footprint));
        payload.subresources.push_back({payload.storage.back(), rowBytes, footprint});
    };
    if (layerMajor)
    {
        for (std::uint16_t layer = 0; layer < desc.arrayLayers; ++layer)
        {
            for (std::uint16_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                add(mip);
            }
        }
    }
    else
    {
        for (std::uint16_t mip = 0; mip < desc.mipLevels; ++mip)
        {
            for (std::uint16_t layer = 0; layer < desc.arrayLayers; ++layer)
            {
                add(mip);
            }
        }
    }
    return payload;
}

TextureDesc UploadTexture2D(std::uint32_t width = 1, std::uint32_t height = 1, std::uint16_t mips = 1)
{
    TextureDesc desc;
    desc.extent = {width, height};
    desc.mipLevels = mips;
    desc.format = Format::Rgba8Unorm;
    desc.usage = TextureUsage::CopyDestination;
    return desc;
}
} // namespace

TEST(RhiUploadValidationTests, BufferInitialDataAllowsEmptyAndExactPayload)
{
    const BufferDesc desc{16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "initial"};
    EXPECT_NO_THROW(ValidateBufferInitialData(desc, {}));
    const auto bytes = Bytes(desc.size);
    EXPECT_NO_THROW(ValidateBufferInitialData(desc, bytes));
}

TEST(RhiUploadValidationTests, BufferInitialDataRejectsOversizeAndReadbackPayload)
{
    BufferDesc desc{16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "initial"};
    const auto before = desc;
    const auto tooLarge = Bytes(17);
    EXPECT_EQ(ErrorFrom([&] { ValidateBufferInitialData(desc, tooLarge); }).code, RhiErrorCode::InvalidArgument);
    EXPECT_EQ(desc, before);

    desc = {16, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "readback"};
    const auto data = Bytes(1);
    EXPECT_EQ(ErrorFrom([&] { ValidateBufferInitialData(desc, data); }).code, RhiErrorCode::InvalidArgument);
    EXPECT_EQ(desc, (BufferDesc{16, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "readback"}));
    EXPECT_NO_THROW(ValidateBufferInitialData(desc, {}));
}

TEST(RhiUploadValidationTests, BufferUploadAcceptsGpuCopyDestinationAndCpuUploadMemory)
{
    const auto bytes = Bytes(4);
    EXPECT_NO_THROW(ValidateBufferUpload({16, BufferUsage::CopyDestination, MemoryDomain::GpuOnly, "gpu"}, 4, bytes));
    EXPECT_NO_THROW(ValidateBufferUpload({16, BufferUsage::Vertex, MemoryDomain::CpuToGpu, "cpu"}, 4, bytes));
    EXPECT_EQ(
        ErrorFrom([&] { ValidateBufferUpload({16, BufferUsage::Vertex, MemoryDomain::GpuOnly, "missing"}, 0, bytes); })
            .code,
        RhiErrorCode::InvalidArgument);
    EXPECT_EQ(
        ErrorFrom(
            [&] { ValidateBufferUpload({16, BufferUsage::CopyDestination, MemoryDomain::GpuToCpu, "read"}, 0, bytes); })
            .code,
        RhiErrorCode::InvalidArgument);
}

TEST(RhiUploadValidationTests, BufferUploadRejectsEmptyAndOverflowingRanges)
{
    const BufferDesc desc{16, BufferUsage::CopyDestination, MemoryDomain::GpuOnly, "range"};
    EXPECT_EQ(ErrorFrom([&] { ValidateBufferUpload(desc, 0, {}); }).code, RhiErrorCode::InvalidArgument);
    const auto bytes = Bytes(1);
    EXPECT_EQ(ErrorFrom([&] { ValidateBufferUpload(desc, std::numeric_limits<std::uint64_t>::max(), bytes); }).code,
              RhiErrorCode::InvalidArgument);
    EXPECT_EQ(ErrorFrom([&] { ValidateBufferUpload(desc, 16, bytes); }).code, RhiErrorCode::InvalidArgument);
    const auto tooLarge = Bytes(2);
    EXPECT_EQ(ErrorFrom([&] { ValidateBufferUpload(desc, 15, tooLarge); }).code, RhiErrorCode::InvalidArgument);
}

TEST(RhiUploadValidationTests, TextureUploadAcceptsTightPitchesForEverySupportedFormat)
{
    for (const auto format : {Format::Rgba8Unorm, Format::Rgba8UnormSrgb, Format::Rg16Float, Format::Rgba16Float,
                              Format::R32Float, Format::D32Float})
    {
        auto desc = UploadTexture2D(2, 3);
        desc.format = format;
        const auto rowBytes = static_cast<std::uint64_t>(2) * BytesPerPixel(format);
        const auto footprint = rowBytes * 2 + rowBytes;
        auto bytes = Bytes(footprint);
        const TextureSubresourceData subresource{bytes, rowBytes, footprint};
        EXPECT_NO_THROW(ValidateTextureUpload(desc, std::array{subresource})) << static_cast<int>(format);
    }
}

TEST(RhiUploadValidationTests, TextureUploadAllowsRowAndSlicePaddingOutsideActualBytes)
{
    const auto desc = UploadTexture2D(3, 2);
    auto bytes = Bytes(28);
    const TextureSubresourceData subresource{bytes, 16, 32};
    EXPECT_NO_THROW(ValidateTextureUpload(desc, std::array{subresource}));
}

TEST(RhiUploadValidationTests, TextureUploadRequiresLayerMajorCompleteCubeMipChain)
{
    TextureDesc desc = UploadTexture2D(8, 8, 3);
    desc.dimension = TextureDimension::TextureCube;
    desc.arrayLayers = 6;
    desc.format = Format::Rgba16Float;
    auto valid = MakePayload(desc, true);
    EXPECT_NO_THROW(ValidateTextureUpload(desc, valid.subresources));

    auto mipMajor = MakePayload(desc, false);
    EXPECT_EQ(ErrorFrom([&] { ValidateTextureUpload(desc, mipMajor.subresources); }).code,
              RhiErrorCode::InvalidArgument);
}

TEST(RhiUploadValidationTests, TextureUploadRejectsMissingCopyDestinationOrSubresources)
{
    auto desc = UploadTexture2D(2, 2, 2);
    auto payload = MakePayload(desc, true);
    desc.usage = TextureUsage::Sampled;
    EXPECT_EQ(ErrorFrom([&] { ValidateTextureUpload(desc, payload.subresources); }).code,
              RhiErrorCode::InvalidArgument);

    desc = UploadTexture2D(2, 2, 2);
    payload = MakePayload(desc, true);
    payload.subresources.pop_back();
    EXPECT_EQ(ErrorFrom([&] { ValidateTextureUpload(desc, payload.subresources); }).code,
              RhiErrorCode::InvalidArgument);
}

TEST(RhiUploadValidationTests, TextureUploadRejectsPitchAndByteFootprintViolations)
{
    const auto desc = UploadTexture2D(3, 2);
    const auto validBytes = Bytes(28);
    EXPECT_EQ(
        ErrorFrom([&] { ValidateTextureUpload(desc, std::array{TextureSubresourceData{validBytes, 11, 28}}); }).code,
        RhiErrorCode::InvalidArgument);
    EXPECT_EQ(
        ErrorFrom([&] { ValidateTextureUpload(desc, std::array{TextureSubresourceData{validBytes, 16, 27}}); }).code,
        RhiErrorCode::InvalidArgument);
    const auto shortBytes = Bytes(27);
    EXPECT_EQ(
        ErrorFrom([&] { ValidateTextureUpload(desc, std::array{TextureSubresourceData{shortBytes, 16, 28}}); }).code,
        RhiErrorCode::InvalidArgument);
}
