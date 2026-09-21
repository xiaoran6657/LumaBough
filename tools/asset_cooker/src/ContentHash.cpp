// ============================================================================
// ContentHash.cpp — 文件级 ContentHash 实现
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 2 节
// ============================================================================
#include "ContentHash.h"

#include <cstddef>
#include <fstream>
#include <span>
#include <vector>

namespace MiniEngine::Tools
{
namespace
{
// 固定分块大小。不要为了"减少系统调用"而把它放大到接近文件大小——
// 大文件整体读入会破坏流式保证。
constexpr std::size_t kHashChunkSize = 1024U * 1024U;  // 1 MiB
}  // namespace

bool ComputeFileHash(const std::filesystem::path& path, Sha256Digest& digest, std::string& error)
{
    error.clear();
    digest = Sha256Digest{};

    std::ifstream stream{path, std::ios::binary};
    if (!stream)
    {
        error = "Failed to open file for hashing: " + path.string();
        return false;
    }

    Sha256Builder builder;
    if (!builder.IsReady())
    {
        error = "SHA-256 builder is not ready";
        return false;
    }

    std::vector<char> buffer(kHashChunkSize);
    for (;;)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

        // gcount() 是本次实际读入字节数；短读本身不是错误，只要随后命中 eof。
        const std::streamsize read = stream.gcount();
        if (read > 0)
        {
            const std::span<const char> chunk{buffer.data(), static_cast<std::size_t>(read)};
            if (!builder.AppendBytes(std::as_bytes(chunk)))
            {
                error = "Failed to append file chunk to hash: " + path.string();
                return false;
            }
        }

        if (stream.eof())
        {
            break;
        }
        if (stream.fail())
        {
            // 既没到 eof 又 fail：真实读取错误（或被外部截断/替换）。
            error = "File read failed during hashing: " + path.string();
            return false;
        }
    }

    if (!builder.Finish(digest))
    {
        error = "Failed to finish hash: " + path.string();
        return false;
    }

    return true;
}
}  // namespace MiniEngine::Tools
