#include "ManifestWriter.h"

#include "BuildKey.h"
#include "CookSession.h"

#include <map>
#include <string_view>

namespace MiniEngine::Tools
{
namespace
{
// Manifest JSON 的最小转义。路径统一 '/' 分隔以减少转义并保持确定性。
void AppendJsonString(std::string& out, const std::string_view text)
{
    out.push_back('"');
    for (const char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    out.push_back('"');
}
} // namespace

std::string BuildManifestJson(const std::vector<CookedArtifactRecord>& records, const std::string& profile)
{
    std::string json = "{\"assets\":[";
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        if (index != 0)
        {
            json.push_back(',');
        }
        const CookedArtifactRecord& record = records[index];
        // AppendJsonString 自带一对引号；前缀只写到 ':' 为止，避免双重引号。
        json += "{\"artifactHash\":\"" + ToHexDigest(record.artifactHash) + "\",\"artifactPath\":";
        AppendJsonString(json, record.artifactPath);
        json += ",\"assetUri\":";
        AppendJsonString(json, record.assetUri);
        json += ",\"buildKey\":\"" + ToHexDigest(record.buildKey) + "\",\"fileSize\":" +
                std::to_string(record.fileSize) + ",\"kind\":\"" + record.kind + "\"}";
    }
    json += "],\"profile\":";
    AppendJsonString(json, profile);
    json += ",\"schemaVersion\":1}";
    return json;
}

std::vector<std::string> FindDuplicateAssetUris(const std::vector<CookedArtifactRecord>& records)
{
    // std::map 按 key 字节序遍历，报告顺序确定（与 manifest 条目排序口径一致）。
    std::map<std::string, int> counts;
    for (const CookedArtifactRecord& record : records)
    {
        ++counts[record.assetUri];
    }

    std::vector<std::string> duplicates;
    for (const auto& [uri, count] : counts)
    {
        if (count > 1)
        {
            duplicates.push_back(uri);
        }
    }
    return duplicates;
}
} // namespace MiniEngine::Tools
