// ============================================================================
// BuildKey.h — artifact-specific BuildKey（versioned binary preimage）
// 里程碑：M3-05
// 职责：把"配方 + 依赖 + 工具/格式/转换规则"编码为 versioned 二进制 preimage，
//       再用 Sha256Builder 得到 32 字节 BuildKey。BuildKey 决定 Cache 目录，
//       并写入 BakedHeader.buildKey 供运行时比对。
// 关联：docs/architecture/README.md 第 3 节
// ============================================================================
#pragma once

#include "DependencyRecord.h"
#include "Sha256.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// preimage 的 schema 版本。改动 preimage 布局时必须递增，
// 否则新旧 Cooker 会对同一输入算出同一个 key 而内容语义已变。
inline constexpr std::uint32_t kBuildKeySchemaVersion = 1;

// 工具与规则身份：进入 BuildKey，使"换工具版本"必然失效。
// 这些值本身就是依赖，不要在 ComputeBuildKey 内部硬编码到哈希之外。
inline constexpr const char* kCookerVersion = "0.1.0-m3";
inline constexpr const char* kImporterName = "fastgltf/0.9.0";   // M3-05 起固定
inline constexpr const char* kImporterSettings = "rh-to-lh=x-reflection-v1;coord=lh;texcoord0-only";
inline constexpr const char* kWicPolicyVersion = "m3-rgba8-v1";
inline constexpr const char* kBakedFormatRule = "format/memesh=1;format/metex=1;format/meworld=1";

struct BuildKeyInputs final
{
    std::uint32_t buildKeySchemaVersion{kBuildKeySchemaVersion};
    std::string cookerVersion{kCookerVersion};

    // 必须与要写入的 artifact header version 一致（kBakedFormatVersion）。
    std::uint32_t bakedFormatVersion{1};

    std::string profile;                  // recipe.profile
    std::string canonicalAssetUri;        // recipe.assetRoot
    std::string canonicalRecipeBytes;     // EncodeCanonicalRecipe(recipe) 的产物

    // 必须已 SortDependencies 排序；ComputeBuildKey 会再校验排序。
    std::vector<DependencyRecord> dependencies;

    std::string importerName{kImporterName};
    std::string importerSettings{kImporterSettings};
};

// 计算 BuildKey。
//
// preimage 布局（严格按字段顺序，全部小端；字符串一律 u32 LE 长度 + UTF-8）：
//   ASCII "MiniEngineBuildKey"
//   u8 0
//   u32 buildKeySchemaVersion LE
//   length+UTF8 cookerVersion
//   u32 bakedFormatVersion LE
//   length+UTF8 profile
//   length+UTF8 canonicalAssetUri
//   length+canonical recipe bytes
//   u32 dependencyCount LE
//   for each dependency sorted by (kind,path):
//       u8 kind
//       length+UTF8 normalized path
//       32 bytes contentHash
//   length+UTF8 importerName
//   length+UTF8 importerSettings
//
// 为什么用 length-prefixed 而不是拼接字符串：避免 profile="ab"+uri="c"
// 与 profile="a"+uri="bc" 这类不同语义得到同一 preimage。
//
// 禁止：不要把 JSON 拼接或 key=value 字符串直接 hash。
[[nodiscard]] bool ComputeBuildKey(const BuildKeyInputs& inputs, Sha256Digest& digest, std::string& error);

// 32 字节 digest → 64 位小写 hex（无 0x 前缀）。用于 Cache 目录名与日志。
[[nodiscard]] std::string ToHexDigest(const Sha256Digest& digest);
}  // namespace MiniEngine::Tools