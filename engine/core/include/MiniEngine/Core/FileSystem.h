#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace MiniEngine
{
struct BinaryFileResult
{
    std::vector<std::byte> bytes;
    std::string error;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return error.empty();
    }

    explicit operator bool() const noexcept
    {
        return Succeeded();
    }
};

[[nodiscard]] BinaryFileResult ReadBinaryFile(const std::filesystem::path& path);
} // namespace MiniEngine