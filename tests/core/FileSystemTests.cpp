#include <MiniEngine/Core/FileSystem.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
class TemporaryDirectory
{
  public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name =
            "MiniEngineCoreTests-" + std::to_string(timestamp) + "-" + std::to_string(sequence.fetch_add(1));

        m_path = std::filesystem::temp_directory_path() / name;
        std::error_code errorCode;
        std::filesystem::create_directories(m_path, errorCode);

        if (errorCode)
        {
            throw std::system_error{errorCode, "failed to create temporary test directory"};
        }
    }

    ~TemporaryDirectory()
    {
        std::error_code errorCode;
        std::filesystem::remove_all(m_path, errorCode);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

TEST(FileSystemTests, ReadsExactBinaryBytes)
{
    TemporaryDirectory temporaryDirectory;
    const auto filePath = temporaryDirectory.Path() / "bytes.bin";
    constexpr std::array<std::uint8_t, 3> expected{0x00, 0x7F, 0xFF};

    {
        std::ofstream output{filePath, std::ios::binary};
        ASSERT_TRUE(output.is_open());
        output.write(reinterpret_cast<const char*>(expected.data()), static_cast<std::streamsize>(expected.size()));
        ASSERT_TRUE(output.good());
    }

    const MiniEngine::BinaryFileResult result = MiniEngine::ReadBinaryFile(filePath);

    ASSERT_TRUE(result.Succeeded()) << result.error;
    ASSERT_EQ(result.bytes.size(), expected.size());

    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        EXPECT_EQ(result.bytes[index], static_cast<std::byte>(expected[index]));
    }
}

TEST(FileSystemTests, TreatsEmptyFileAsSuccessfulRead)
{
    TemporaryDirectory temporaryDirectory;
    const auto filePath = temporaryDirectory.Path() / "empty.bin";

    {
        std::ofstream output{filePath, std::ios::binary};
        ASSERT_TRUE(output.is_open());
    }

    const MiniEngine::BinaryFileResult result = MiniEngine::ReadBinaryFile(filePath);

    EXPECT_TRUE(result.Succeeded()) << result.error;
    EXPECT_TRUE(result.bytes.empty());
    EXPECT_TRUE(result.error.empty());
}

TEST(FileSystemTests, ReportsMissingFileWithoutPartialData)
{
    TemporaryDirectory temporaryDirectory;
    const auto filePath = temporaryDirectory.Path() / "missing.bin";

    const MiniEngine::BinaryFileResult result = MiniEngine::ReadBinaryFile(filePath);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(result.bytes.empty());
    EXPECT_FALSE(result.error.empty());
    EXPECT_NE(result.error.find("missing.bin"), std::string::npos);
}
} // namespace