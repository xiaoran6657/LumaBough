// ============================================================================
// AssetCache.cpp — Cache 布局、命中判定与反向依赖图实现
// 里程碑：M3-05
// 关联：docs/architecture/README.md 第 5、6、7 节
// ============================================================================
#include "AssetCache.h"

#include <MiniEngine/Assets/BakedReader.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

std::filesystem::path BuildCacheDirectory(const std::filesystem::path& outputRoot, const Sha256Digest& buildKey)
{
    const std::string hex = ToHexDigest(buildKey);
    // shard = 前两 hex。hex 必为 64 字符，故 substr 安全。
    return outputRoot / "cache" / hex.substr(0, 2) / hex;
}

std::filesystem::path BuildArtifactPath(const std::filesystem::path& cacheDirectory,
                                        const Sha256Digest& artifactHash, const std::string& stableSuffix,
                                        const std::string& extension)
{
    const std::string fileName = ToHexDigest(artifactHash) + "-" + stableSuffix + "." + extension;
    return cacheDirectory / fileName;
}

// ---------------------------------------------------------------------------
// 命中判定
// ---------------------------------------------------------------------------

std::optional<std::string> ReadArtifactBytes(const std::filesystem::path& path, std::string& error)
{
    error.clear();

    std::ifstream stream{path, std::ios::binary};
    if (!stream)
    {
        error = "Failed to open artifact: " + path.string();
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof())
    {
        error = "Failed to read artifact: " + path.string();
        return std::nullopt;
    }

    return buffer.str();
}

CacheHitResult EvaluateCacheHit(const CacheHitInput& input)
{
    CacheHitResult result;

    // (3) 文件存在且为普通文件。
    //     注意：is_regular_file 跟随 symlink；M3 禁止跟随 reparse point，
    //     因此在 Cooker 侧写入前就要拒绝符号链接目标，这里只做基本形状检查。
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(input.artifactPath, fileError) || fileError)
    {
        result.reason = "artifact is missing or not a regular file";
        return result;
    }

    std::string readError;
    const std::optional<std::string> bytes = ReadArtifactBytes(input.artifactPath, readError);
    if (!bytes.has_value())
    {
        result.reason = "artifact could not be read: " + readError;
        return result;
    }

    const std::span<const std::byte> fileBytes{
        reinterpret_cast<const std::byte*>(bytes->data()), bytes->size()};

    // (6) artifact SHA-256 等于 Manifest artifactHash。
    //     先于 Reader 校验整文件摘要，可把"任何字节损坏"一次性拦下。
    Sha256Digest actualHash{};
    if (!ComputeSha256(fileBytes, actualHash))
    {
        result.reason = "failed to compute artifact hash";
        return result;
    }
    if (actualHash != input.expectedArtifactHash)
    {
        result.reason = "artifactHash mismatch (corruption)";
        return result;
    }

    // (4) + (5) runtime Reader 通过，且 header BuildKey 等于期望。
    //     expectation.buildKey 让 Reader 直接比对 header 中的 32 字节。
    MiniEngine::Assets::BakedReadResult parsed;
    std::string parseError;
    if (!MiniEngine::Assets::BakedReader::Parse(
            fileBytes,
            MiniEngine::Assets::BakedReadExpectation{.kind = input.kind, .buildKey = input.expectedBuildKey},
            parsed, parseError))
    {
        result.reason = "reader rejected artifact: " + parseError;
        return result;
    }

    result.hit = true;
    return result;
}

// ---------------------------------------------------------------------------
// 反向依赖图
// ---------------------------------------------------------------------------

void ReverseDependencyGraph::AddAsset(const std::string& assetUri, std::vector<DependencyRecord> dependencies)
{
    SortDependencies(dependencies);
    m_assetToDependencies[assetUri] = std::move(dependencies);

    for (const DependencyRecord& record : m_assetToDependencies[assetUri])
    {
        std::vector<std::string>& owners = m_pathToAssets[record.normalizedPathOrName];
        if (std::find(owners.begin(), owners.end(), assetUri) == owners.end())
        {
            owners.push_back(assetUri);
        }
    }

    // 保持 owners 列表有序，保证 FindAffectedAssets 输出确定。
    for (auto& entry : m_pathToAssets)
    {
        std::sort(entry.second.begin(), entry.second.end());
    }
}

std::vector<std::string> ReverseDependencyGraph::FindAffectedAssets(const std::string_view changedPath) const
{
    const std::string normalized{NormalizeDependencyPath(changedPath)};
    const auto it = m_pathToAssets.find(normalized);
    if (it == m_pathToAssets.end())
    {
        return {};
    }
    return it->second;  // 已在 AddAsset 中排序
}

std::vector<std::string> ReverseDependencyGraph::GetAllAssets() const
{
    std::vector<std::string> assets;
    assets.reserve(m_assetToDependencies.size());
    for (const auto& entry : m_assetToDependencies)
    {
        assets.push_back(entry.first);
    }
    // std::map 已按键排序，此处顺序天然确定。
    return assets;
}
} // namespace MiniEngine::Tools
