#include <MiniEngine/Core/FileSystem.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>

namespace MiniEngine
{
namespace
{
BinaryFileResult MakeError(const std::filesystem::path& path, const std::string_view reason)
{
    std::ostringstream stream;
    stream << "Failed to read '" << path.string() << "': " << reason;
    return {.bytes = {}, .error = stream.str()};
}
} // namespace

BinaryFileResult ReadBinaryFile(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, errorCode);

    if (errorCode)
    {
        return MakeError(path, errorCode.message());
    }

    if (fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        return MakeError(path, "File is too large for size_t");
    }

    if (fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
    {
        return MakeError(path, "File is too large for streamsize");
    }

    std::ifstream input{path, std::ios::binary};

    if (!input.is_open())
    {
        return MakeError(path, "unable to open file");
    }

    BinaryFileResult result;
    result.bytes.resize(static_cast<std::size_t>(fileSize));

    if (!result.bytes.empty())
    {
        input.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(result.bytes.size()));

        if (!input)
        {
            return MakeError(path, "incomplete read");
        }
    }

    return result;
}
} // namespace MiniEngine