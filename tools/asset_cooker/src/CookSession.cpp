// ============================================================================
// CookSession.cpp — Cooker 四命令与烘焙管线的实现
// 里程碑：M3-04 / M3-05 / watch（07 篇）/ 审计 3.1
// 职责：按阶段状态机推进 validate / cook / inspect / watch：Recipe 解析、
//       source 沙箱、Khronos validator 门禁、glTF 导入、四类 writer 序列化、
//       ReopenVerify、原子发布与 Manifest 快照；watch 以 DirectoryWatcherWin32
//       的事件为 hint、200ms 防抖后整体重 cook，失败批次不发布 Manifest
//      （last-known-good 保持），overflow 触发全量重扫。
// 关联：tools/asset_cooker/src/CookSession.h（阶段与退出码契约）
//       docs/architecture/DECISIONS.md §2、§9
// ============================================================================

#include "CookSession.h"


#include <MiniEngine/Assets/BakedReader.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "AssetCache.h"
#include "AtomicPublish.h"
#include "BakedWriter.h"
#include "BuildKey.h"
#include "CanonicalRecipe.h"
#include "ContentHash.h"
#include "DependencyRecord.h"
#include <MiniEngine/Assets/AssetRegistry.h>
#include <MiniEngine/Platform/Windows/DirectoryWatcherWin32.h>

#include "GltfGeometry.h"
#include "GltfImportAdapter.h"
#include "HdrImage.h"
#include "ManifestWriter.h"
#include "MaterialArtifactWriter.h"
#include "MeshArtifactWriter.h"
#include "MeworldWriter.h"
#include "TextureArtifactWriter.h"
#include "TextureMipBuilder.h"
#include "WicImage.h"

#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/TextureFormatV2.h>
#include <MiniEngine/Core/FileSystem.h> // 审计 4：inspect 直读收敛到 Core

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>

namespace MiniEngine::Tools
{
namespace
{
// 从文件头前 8 字节解析 artifact kind（offset 6 处的 u16 little-endian）。
// 只用于 inspect 构造 expectation；完整结构校验仍由 BakedReader 完成。
std::optional<MiniEngine::Assets::BakedAssetKind> ReadHeaderKind(const std::vector<std::byte>& fileBytes)
{
    if (fileBytes.size() < 8)
    {
        return std::nullopt;
    }
    if (fileBytes[0] != std::byte{'M'} || fileBytes[1] != std::byte{'E'} || fileBytes[2] != std::byte{'A'} ||
        fileBytes[3] != std::byte{'3'})
    {
        return std::nullopt;
    }

    const std::uint16_t kind =
        static_cast<std::uint16_t>(std::to_integer<unsigned char>(fileBytes[6])) |
        (static_cast<std::uint16_t>(std::to_integer<unsigned char>(fileBytes[7])) << 8U);
    switch (static_cast<MiniEngine::Assets::BakedAssetKind>(kind))
    {
        case MiniEngine::Assets::BakedAssetKind::Mesh:
        case MiniEngine::Assets::BakedAssetKind::Texture:
        case MiniEngine::Assets::BakedAssetKind::World:
        case MiniEngine::Assets::BakedAssetKind::Material:
            return static_cast<MiniEngine::Assets::BakedAssetKind>(kind);
        default:
            return std::nullopt;
    }
}

// texture usage 的稳定名称（assetUri fragment 与发布文件名共用，保证确定性）。
const char* TextureUsageName(const MiniEngine::Assets::TextureUsage usage)
{
    using Usage = MiniEngine::Assets::TextureUsage;
    switch (usage)
    {
        case Usage::BaseColor:
            return "baseColor";
        case Usage::MetallicRoughness:
            return "metallicRoughness";
        case Usage::Normal:
            return "normal";
        case Usage::Occlusion:
            return "occlusion";
        case Usage::Emissive:
            return "emissive";
        case Usage::HdrEnvironment:
            return "hdrEnvironment";
    }
    return "unknown";
}

// mesh/texture/manifest writer 已抽取为独立模块（BakedWriter/MeshArtifactWriter/
// TextureArtifactWriter/ManifestWriter，审计 3.1 结构性重构）；本文件不再持有
// artifact 序列化细节，BuildMeshArtifact/BuildTextureArtifact/BuildManifestJson
// 调用点见 CookSession::CookMesh/CookTexture/BakeWorld 与 PublishManifest。

bool WriteBytesToFile(const std::filesystem::path& path, const std::span<const std::byte> bytes)
{
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

const char* StageName(const CookStage stage)
{
    switch (stage)
    {
        case CookStage::Discovered:
            return "Discovered";
        case CookStage::ParseRecipe:
            return "ParseRecipe";
        case CookStage::ResolveSource:
            return "ResolveSource";
        case CookStage::ValidateSource:
            return "ValidateSource";
        case CookStage::KhronosValidate:
            return "KhronosValidate";
        case CookStage::Import:
            return "Import";
        case CookStage::Serialize:
            return "Serialize";
        case CookStage::ReopenVerify:
            return "ReopenVerify";
        case CookStage::PublishArtifacts:
            return "PublishArtifacts";
        case CookStage::PublishManifest:
            return "PublishManifest";
    }
    return "Unknown";
}
} // namespace

CookSession::CookSession(CookCommandLine commandLine) : m_commandLine{std::move(commandLine)}
{
}

void CookSession::LogStage(const CookStage stage, const std::string_view recipe, const std::string_view assetUri,
                           const std::string_view message) const
{
    std::cout << '[' << StageName(stage) << "] recipe=" << recipe;
    if (!assetUri.empty())
    {
        std::cout << " asset=" << assetUri;
    }
    std::cout << ' ' << message << '\n';
}

int CookSession::Run()
{
    LogStage(CookStage::Discovered, m_commandLine.recipe.string(), m_commandLine.recipeRoot.string(),
             "command accepted");

    if (m_commandLine.command == "validate")
    {
        return RunValidate();
    }
    if (m_commandLine.command == "cook")
    {
        return RunCook();
    }
    if (m_commandLine.command == "inspect")
    {
        return RunInspect();
    }
    if (m_commandLine.command == "watch")
    {
        return RunWatch();
    }

    LogStage(CookStage::Discovered, "", "", "unknown command");
    return 2;
}

std::optional<std::filesystem::path> CookSession::ResolveValidator() const
{
    // 解析顺序：--validator 显式参数 → MGE_GLTF_VALIDATOR 环境变量回退。
    // 环境变量是 tools/Run-*.ps1 脚本的文档口径（STATUS 环境备忘引用它）；
    // 直接调用 Cooker 的场景此前被静默忽略，导致退出码 3 与脚本文档不一致
    //（M4-04 审查定位）。缺省且 env 未设置/文件不存在时仍返回 nullopt（CLI 契约）。
    std::filesystem::path candidate = m_commandLine.validator;
    if (candidate.empty())
    {
        char validatorBuffer[1024]{};
        if (GetEnvironmentVariableA("MGE_GLTF_VALIDATOR", validatorBuffer,
                                    static_cast<DWORD>(sizeof(validatorBuffer))) == 0)
        {
            return std::nullopt;
        }
        candidate = validatorBuffer;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(candidate, fileError))
    {
        return std::nullopt;
    }
    return candidate;
}

int CookSession::RunKhronosValidator(const std::filesystem::path& source,
                                     const std::filesystem::path& report) const
{
    const std::optional<std::filesystem::path> validator = ResolveValidator();
    if (!validator.has_value())
    {
        LogStage(CookStage::KhronosValidate, m_commandLine.recipe.string(), source.string(),
                 "gltf-validator not found (set --validator)");
        return 3;
    }

    std::error_code dirError;
    std::filesystem::create_directories(report.parent_path(), dirError);

    // 子进程 cwd 会被设为 source.parent_path()；若 source 是相对路径，
    // validator 会在新 cwd 下解析它而找不到文件（M3-05 冒烟实测 IO_ERROR）。
    // 因此传给 validator 的必须是绝对路径。
    std::error_code absoluteError;
    const std::filesystem::path absoluteSource = std::filesystem::absolute(source, absoluteError);
    if (absoluteError)
    {
        LogStage(CookStage::KhronosValidate, m_commandLine.recipe.string(), source.string(),
                 "failed to resolve absolute source path");
        return 6;
    }

    const std::wstring commandLine =
        L"\"" + validator->wstring() + L"\" -o -r \"" + absoluteSource.wstring() + L"\"";

    // CreateFileW 默认句柄不可继承；子进程经 STARTF_USESTDHANDLES 接收 stdout 时
    // 必须能继承该句柄，否则 validator 写 stdout 失败。bInheritHandle=TRUE 使其可继承。
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE reportFile = CreateFileW(report.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reportFile == INVALID_HANDLE_VALUE)
    {
        LogStage(CookStage::KhronosValidate, m_commandLine.recipe.string(), source.string(),
                 "failed to open report file");
        return 6;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = reportFile;
    startupInfo.hStdError = reportFile;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION processInfo{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        source.parent_path().c_str(), &startupInfo, &processInfo))
    {
        CloseHandle(reportFile);
        LogStage(CookStage::KhronosValidate, m_commandLine.recipe.string(), source.string(),
                 "failed to launch gltf-validator");
        return 6;
    }

    CloseHandle(reportFile);
    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode{};
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    LogStage(CookStage::KhronosValidate, m_commandLine.recipe.string(), source.string(),
             "gltf-validator exit=" + std::to_string(exitCode));
    return exitCode == 0 ? 0 : 3;
}

int CookSession::RunValidate()
{
    LogStage(CookStage::ParseRecipe, m_commandLine.recipe.string(), "", "parsing recipe");

    std::string error;
    const std::optional<Recipe> recipe = ParseRecipeFile(m_commandLine.recipe, error);
    if (!recipe.has_value())
    {
        LogStage(CookStage::ParseRecipe, m_commandLine.recipe.string(), "", "recipe parse failed: " + error);
        return 2;
    }

    LogStage(CookStage::ResolveSource, m_commandLine.recipe.string(), recipe->assetRoot, "resolving source path");
    const std::optional<std::filesystem::path> resolved =
        ResolveSourceWithinRoot(m_commandLine.sourceRoot, recipe->source);
    if (!resolved.has_value())
    {
        LogStage(CookStage::ResolveSource, m_commandLine.recipe.string(), recipe->assetRoot,
                 "source escapes source-root");
        return 2;
    }

    LogStage(CookStage::ValidateSource, m_commandLine.recipe.string(), recipe->assetRoot, resolved->string());
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(*resolved, fileError))
    {
        LogStage(CookStage::ValidateSource, m_commandLine.recipe.string(), recipe->assetRoot,
                 "source file is missing");
        return 3;
    }

    return RunKhronosValidator(*resolved, m_commandLine.report);
}

int CookSession::RunCook()
{
    std::error_code dirError;
    if (!std::filesystem::is_directory(m_commandLine.recipeRoot, dirError))
    {
        LogStage(CookStage::Discovered, "", m_commandLine.recipeRoot.string(), "recipe root is not a directory");
        return 2;
    }

    // 枚举 recipeRoot 下的 *.json 并按路径排序：处理顺序即 Manifest 条目顺序，
    // 是双目录确定性的一部分。
    std::vector<std::filesystem::path> recipes;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{m_commandLine.recipeRoot, dirError})
    {
        if (dirError)
        {
            LogStage(CookStage::Discovered, "", m_commandLine.recipeRoot.string(),
                     "failed to scan recipe root: " + dirError.message());
            return 2;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            recipes.push_back(entry.path());
        }
    }
    std::sort(recipes.begin(), recipes.end());

    if (recipes.empty())
    {
        LogStage(CookStage::Discovered, "", m_commandLine.recipeRoot.string(), "no recipes found; nothing to cook");
        return 0;
    }

    if (m_commandLine.deterministic)
    {
        LogStage(CookStage::Discovered, "", m_commandLine.outputRoot.string(),
                 "deterministic mode: outputs contain no timestamps, PIDs, or absolute paths");
    }

    std::vector<CookedArtifactRecord> collectedArtifacts;
    for (const std::filesystem::path& recipePath : recipes)
    {
        std::string error;
        const std::optional<Recipe> recipe = ParseRecipeFile(recipePath, error);
        if (!recipe.has_value())
        {
            LogStage(CookStage::ParseRecipe, recipePath.filename().string(), "", "recipe parse failed: " + error);
            return 2;
        }

        LogStage(CookStage::ResolveSource, recipePath.filename().string(), recipe->assetRoot,
                 "source=" + recipe->source);
        const std::optional<std::filesystem::path> resolved =
            ResolveSourceWithinRoot(m_commandLine.sourceRoot, recipe->source);
        if (!resolved.has_value())
        {
            LogStage(CookStage::ResolveSource, recipePath.filename().string(), recipe->assetRoot,
                     "source escapes source-root");
            return 2;
        }

        std::error_code fileError;
        if (!std::filesystem::is_regular_file(*resolved, fileError))
        {
            LogStage(CookStage::ValidateSource, recipePath.filename().string(), recipe->assetRoot,
                     "source file is missing: " + resolved->string());
            return 3;
        }

        const int validateResult = RunKhronosValidator(*resolved, m_commandLine.outputRoot / "reports" /
                                                                      (recipePath.stem().string() + ".json"));
        if (validateResult != 0)
        {
            return validateResult;
        }

        const int cookResult = CookRecipe(recipePath, *recipe, *resolved, collectedArtifacts);
        if (cookResult != 0)
        {
            return cookResult;
        }
    }

    // 所有 recipe 的 artifact 都成功发布后，最后发布 Manifest snapshot。
    return PublishManifest(collectedArtifacts);
}

int CookSession::RunInspect()
{
    // 审计 4：RunInspect 的 ifstream 直读收敛到 MiniEngine::ReadBinaryFile（Core 唯一实现）；
    // 空文件读取成功、由下方 ReadHeaderKind 判定为无效 header（退出码 5）。
    const MiniEngine::BinaryFileResult file = MiniEngine::ReadBinaryFile(m_commandLine.artifact);
    if (!file.Succeeded())
    {
        LogStage(CookStage::Discovered, "", m_commandLine.artifact.string(),
                 "artifact file is missing: " + file.error);
        return 2;
    }
    const std::vector<std::byte>& fileBytes = file.bytes;

    const std::optional<MiniEngine::Assets::BakedAssetKind> kind = ReadHeaderKind(fileBytes);
    if (!kind.has_value())
    {
        LogStage(CookStage::ReopenVerify, "", m_commandLine.artifact.string(),
                 "artifact header kind is missing or invalid");
        return 5;
    }

    MiniEngine::Assets::BakedReadResult result;
    std::string error;
    if (!MiniEngine::Assets::BakedReader::Parse(
            fileBytes, MiniEngine::Assets::BakedReadExpectation{.kind = *kind}, result, error))
    {
        LogStage(CookStage::ReopenVerify, "", m_commandLine.artifact.string(), "reader rejected artifact: " + error);
        return 5;
    }

    const char* kindName = *kind == MiniEngine::Assets::BakedAssetKind::Mesh    ? "Mesh"
                           : *kind == MiniEngine::Assets::BakedAssetKind::Texture ? "Texture"
                                                                                 : "World";
    std::cout << "type=" << kindName << " formatVersion=" << result.header.version << '\n';
    std::cout << "buildKey=<64-hex-not-displayed>" << '\n';
    std::cout << "fileSize=" << result.header.fileSize << '\n';
    std::cout << "chunks:\n";
    for (const MiniEngine::Assets::BakedChunk& chunk : result.chunks)
    {
        std::cout << "  " << std::string_view{chunk.type.data(), chunk.type.size()} << " offset=" << chunk.offset
                  << " size=" << chunk.size << " count=" << chunk.elementCount << " stride=" << chunk.stride << '\n';
    }
    std::cout << "validation=PASS\n";
    return 0;
}

int CookSession::RunWatch()
{
    using MiniEngine::Platform::DirectoryWatchEvent;
    using MiniEngine::Platform::DirectoryWatcherWin32;

    // 先做一次完整初始 cook（结果失败也继续 watch：文件保存中/缺失属可恢复状态，
    // 源变化会再次触发；失败批次不发布 Manifest，旧 last-good 保持）。
    const int initialResult = RunCook();
    LogStage(CookStage::Discovered, "", m_commandLine.outputRoot.string(),
             initialResult == 0 ? "watch started (initial cook OK)"
                                : "watch started (initial cook failed; retry on source changes)");

    // sourceRoot 与 recipeRoot 下所有目录都建 watcher：改文件可能发生在任意子目录。
    const auto collectDirectories = [](const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> directories;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
        {
            return directories;
        }
        directories.push_back(root);
        for (std::filesystem::recursive_directory_iterator iterator{root,
                                                                    std::filesystem::directory_options::skip_permission_denied,
                                                                    ec};
             iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (iterator->is_directory(ec) && !ec)
            {
                directories.push_back(iterator->path());
            }
        }
        return directories;
    };

    const auto rebuildWatchers = [&]()
    {
        std::vector<std::filesystem::path> directories = collectDirectories(m_commandLine.sourceRoot);
        for (const std::filesystem::path& directory : collectDirectories(m_commandLine.recipeRoot))
        {
            if (std::find(directories.begin(), directories.end(), directory) == directories.end())
            {
                directories.push_back(directory);
            }
        }
        std::vector<std::unique_ptr<DirectoryWatcherWin32>> watchers;
        for (const std::filesystem::path& directory : directories)
        {
            try
            {
                watchers.push_back(std::make_unique<DirectoryWatcherWin32>(directory));
            }
            catch (const std::system_error& exception)
            {
                LogStage(CookStage::Discovered, "", directory.string(),
                         std::string{"watch setup failed: "} + exception.what());
            }
        }
        return watchers;
    };

    const auto pollInto = [](std::vector<std::unique_ptr<DirectoryWatcherWin32>>& watchers, bool& changed,
                             bool& fullRescan)
    {
        for (const auto& watcher : watchers)
        {
            for (const DirectoryWatchEvent& event : watcher->PollEvents())
            {
                if (!event.relativePaths.empty())
                {
                    changed = true;
                }
                if (event.fullRescanRequired)
                {
                    fullRescan = true;
                }
            }
        }
    };

    constexpr std::chrono::milliseconds kQuietWindow{200};
    std::vector<std::unique_ptr<DirectoryWatcherWin32>> watchers = rebuildWatchers();
    for (;;)
    {
        bool changed = false;
        bool fullRescan = false;
        pollInto(watchers, changed, fullRescan);
        if (!changed && !fullRescan)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{150});
            continue;
        }

        // debounce：200ms 无新事件才触发一次重 cook（07 篇 "200ms后开始 batch"）。
        bool quiet = false;
        while (!quiet)
        {
            quiet = true;
            for (std::chrono::milliseconds waited{0}; waited < kQuietWindow; waited += std::chrono::milliseconds{20})
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                bool more = false;
                bool moreFull = false;
                pollInto(watchers, more, moreFull);
                if (more || moreFull)
                {
                    quiet = false;
                    fullRescan = fullRescan || moreFull;
                    break;
                }
            }
        }

        LogStage(CookStage::Discovered, "", "",
                 fullRescan ? "change detected (full rescan); re-cooking" : "change detected; re-cooking");
        const int result = RunCook();
        LogStage(CookStage::Discovered, "", "",
                 result == 0 ? "watch cook completed"
                             : "watch cook failed; manifest left unchanged (last-known-good retained)");
        // 重建 watcher 集合：新 recipe/子目录会在这之后被纳入监听。
        watchers = rebuildWatchers();
    }
    return 0;
}

// ---- CookRecipe 三阶段拆分（审计 3.1）----
// 共享上下文：Import 阶段产物 + 日志/URI 上下文。仅在本文件定义；指针都指向
// CookRecipe 的栈对象，任何阶段不得在作用域外保存。
struct CookSession::RecipeCookScope final
{
    std::string recipeName;                        // 日志：recipe 文件名
    std::string assetUri;                          // recipe.assetRoot
    const Recipe* recipe{};
    const std::string* canonicalJson{};            // EncodeCanonicalRecipe 输出
    const std::vector<DependencyRecord>* dependencies{};
    const ImportResult* import{};
    std::vector<CookedArtifactRecord>* collectedArtifacts{};
};

int CookSession::CookRecipe(const std::filesystem::path& recipePath, const Recipe& recipe,
                            const std::filesystem::path& resolvedSource,
                            std::vector<CookedArtifactRecord>& collectedArtifacts)
{
    const std::string recipeName = recipePath.filename().string();
    const std::string assetUri = recipe.assetRoot;

    // ---- Import 阶段：收集依赖 ----
    LogStage(CookStage::Import, recipeName, assetUri, "collecting dependencies");

    std::vector<DependencyRecord> dependencies;

    // (a) source glTF 文件
    Sha256Digest sourceHash{};
    std::string hashError;
    if (!ComputeFileHash(resolvedSource, sourceHash, hashError))
    {
        LogStage(CookStage::Import, recipeName, assetUri, "source hash failed: " + hashError);
        return 4;
    }
    DependencyRecord sourceDep;
    sourceDep.kind = DependencyKind::Source;
    sourceDep.normalizedPathOrName = NormalizeDependencyPath(resolvedSource.lexically_relative(m_commandLine.sourceRoot).generic_string());
    sourceDep.contentHash = sourceHash;
    dependencies.push_back(std::move(sourceDep));

    // (b) recipe 依赖：ContentHash 取 canonical bytes 的摘要，而非源文件原始字节。
    // 失效矩阵（M3-05 第 8 节）要求"Recipe 只改 whitespace → 无"：行尾/缩进变化
    // 不得触发重建。若哈希原始字节，git 的 eol 归一化或编辑器行尾差异都会改变
    // BuildKey（Step 9 实测：CRLF→LF 归一化导致 key 漂移）。
    const auto canonical = EncodeCanonicalRecipe(recipe);
    if (!canonical.has_value())
    {
        LogStage(CookStage::ParseRecipe, recipeName, assetUri, "canonical recipe encoding failed");
        return 2;
    }

    Sha256Digest canonicalRecipeHash{};
    if (!ComputeSha256(std::as_bytes(std::span{canonical->data(), canonical->size()}), canonicalRecipeHash))
    {
        LogStage(CookStage::Serialize, recipeName, assetUri, "failed to hash canonical recipe bytes");
        return 7;
    }

    DependencyRecord recipeDep;
    recipeDep.kind = DependencyKind::Recipe;
    recipeDep.normalizedPathOrName = NormalizeDependencyPath(recipeName);
    recipeDep.contentHash = canonicalRecipeHash;
    dependencies.push_back(std::move(recipeDep));

    // (c) 工具链身份（换 cooker 版本必须全量失效）
    dependencies.push_back(MakeToolDependency("tool/asset-cooker", kCookerVersion));

    // (d) glTF 解析：source 内容 → 引擎 primitive；失败即终止并列出诊断。
    ImportResult importResult;
    DiagnosticSink importDiagnostics;
    if (!ImportGltf(resolvedSource, m_commandLine.sourceRoot, importResult, importDiagnostics))
    {
        for (const DiagnosticRecord& record : importDiagnostics.records)
        {
            LogStage(CookStage::Import, recipeName, assetUri, record.context + ": " + record.message);
        }
        LogStage(CookStage::Import, recipeName, assetUri, "glTF import rejected");
        return 4; // importer 失败码（0/2/3/4/5/6/7 契约）
    }
    if (importResult.primitives.empty())
    {
        LogStage(CookStage::Import, recipeName, assetUri, "glTF contains no renderable primitives");
        return 4;
    }

    // (e) 外部 buffer/image 依赖（02 篇：所有依赖进入 Manifest 与 BuildKey）。
    for (const std::string& externalPath : importResult.externalDependencyPaths)
    {
        DependencyRecord externalDep;
        externalDep.kind = DependencyKind::Source;
        externalDep.normalizedPathOrName = NormalizeDependencyPath(externalPath);
        Sha256Digest externalHash{};
        std::string externalError;
        if (!ComputeFileHash(m_commandLine.sourceRoot / externalPath, externalHash, externalError))
        {
            LogStage(CookStage::Import, recipeName, assetUri, "external dependency hash failed: " + externalError);
            return 4;
        }
        externalDep.contentHash = externalHash;
        dependencies.push_back(std::move(externalDep));
    }

    // (f) environment source（M4-04）：.hdr 内容 hash 进 BuildKey——recook/换
    // HDRI 必然触发 environment artifact 重建（失效矩阵同款纪律）。
    if (recipe.environment.has_value())
    {
        const auto resolvedEnvironment =
            ResolveSourceWithinRoot(m_commandLine.sourceRoot, recipe.environment->source);
        if (!resolvedEnvironment.has_value())
        {
            LogStage(CookStage::Import, recipeName, assetUri,
                     "environment source escapes source-root: " + recipe.environment->source);
            return 2;
        }
        Sha256Digest environmentHash{};
        std::string environmentHashError;
        if (!ComputeFileHash(*resolvedEnvironment, environmentHash, environmentHashError))
        {
            LogStage(CookStage::Import, recipeName, assetUri,
                     "environment source hash failed: " + environmentHashError);
            return 4;
        }
        DependencyRecord environmentDep;
        environmentDep.kind = DependencyKind::Source;
        environmentDep.normalizedPathOrName =
            NormalizeDependencyPath(resolvedEnvironment->lexically_relative(m_commandLine.sourceRoot).generic_string());
        environmentDep.contentHash = environmentHash;
        dependencies.push_back(std::move(environmentDep));
    }

    // ---- Import 完成：Serialize/Bake 三阶段交给拆分出的成员函数（审计 3.1）----
    // RecipeCookScope 携带 Import 产物与日志上下文。顺序（mesh → texture → world）
    // 与既有实现一致，因此 Manifest 条目顺序与日志顺序保持确定性不变。
    RecipeCookScope scope;
    scope.recipeName = recipeName;
    scope.assetUri = assetUri;
    scope.recipe = &recipe;
    scope.canonicalJson = &*canonical;
    scope.dependencies = &dependencies;
    scope.import = &importResult;
    scope.collectedArtifacts = &collectedArtifacts;

    if (const int meshResult = CookMesh(scope); meshResult != 0)
    {
        return meshResult;
    }
    if (const int textureResult = CookTexture(scope); textureResult != 0)
    {
        return textureResult;
    }
    if (const int materialResult = CookMaterials(scope); materialResult != 0)
    {
        return materialResult;
    }
    if (const int environmentResult = CookEnvironment(scope); environmentResult != 0)
    {
        return environmentResult;
    }
    if (const int worldResult = BakeWorld(scope); worldResult != 0)
    {
        return worldResult;
    }
    return 0;
}

int CookSession::CookMesh(const RecipeCookScope& scope)
{
    const std::string& recipeName = scope.recipeName;
    const std::string& assetUri = scope.assetUri;
    const Recipe& recipe = *scope.recipe;
    const std::string& canonical = *scope.canonicalJson;
    const std::vector<DependencyRecord>& dependencies = *scope.dependencies;
    const ImportResult& importResult = *scope.import;

    // ---- Serialize 阶段：每个 primitive 一个 BuildKey + artifact ----
    // primitive 稳定身份（mesh/x/primitive/y，02 篇 node key 规则）作为额外依赖
    // 参与 BuildKey：同一 glTF 的多个 primitive 因此各有不同 key 与 artifact。
    for (std::size_t primitiveIndex = 0; primitiveIndex < importResult.primitives.size(); ++primitiveIndex)
    {
        const ImportedPrimitive& imported = importResult.primitives[primitiveIndex];
        const std::string primitiveKey = imported.stableKey;
        const std::string recordAssetUri =
            importResult.primitives.size() == 1 ? assetUri : assetUri + "#" + primitiveKey;

        std::vector<DependencyRecord> primitiveDependencies = dependencies;
        DependencyRecord identityDep;
        identityDep.kind = DependencyKind::Recipe;
        identityDep.normalizedPathOrName = primitiveKey;
        Sha256Digest identityHash{};
        if (!ComputeSha256(std::as_bytes(std::span{primitiveKey.data(), primitiveKey.size()}), identityHash))
        {
            LogStage(CookStage::Serialize, recipeName, recordAssetUri, "failed to hash primitive identity");
            return 7;
        }
        identityDep.contentHash = identityHash;
        primitiveDependencies.push_back(std::move(identityDep));

        BuildKeyInputs inputs;
        inputs.profile = recipe.profile;
        inputs.canonicalAssetUri = recordAssetUri;
        inputs.canonicalRecipeBytes = canonical;
        inputs.dependencies = primitiveDependencies; // ComputeBuildKey 内部会排序+校验

        Sha256Digest buildKey{};
        std::string keyError;
        if (!ComputeBuildKey(inputs, buildKey, keyError))
        {
            LogStage(CookStage::Serialize, recipeName, recordAssetUri, "build key failed: " + keyError);
            return 7;
        }

        LogStage(CookStage::Serialize, recipeName, recordAssetUri,
                 "primitive " + primitiveKey + " build key=" + ToHexDigest(buildKey).substr(0, 12) + "...");

        std::vector<std::byte> meshBytes;
        std::string meshError;
        if (!BuildMeshArtifact(buildKey, imported.mesh, meshBytes, meshError))
        {
            LogStage(CookStage::Serialize, recipeName, recordAssetUri, ".memesh build failed: " + meshError);
            return 7;
        }
        const int publishCode = PublishArtifact(scope, recordAssetUri, "mesh", MiniEngine::Assets::BakedAssetKind::Mesh,
                                                "memesh", buildKey, meshBytes, "mesh-" + std::to_string(primitiveIndex));
        if (publishCode != 0)
        {
            return publishCode;
        }
    }
    return 0;
}

int CookSession::CookTexture(const RecipeCookScope& scope)
{
    const std::string& recipeName = scope.recipeName;
    const std::string& assetUri = scope.assetUri;
    const Recipe& recipe = *scope.recipe;
    const std::string& canonical = *scope.canonicalJson;
    const std::vector<DependencyRecord>& dependencies = *scope.dependencies;
    const ImportResult& importResult = *scope.import;

    // ---- Texture bake v2：每个 (image, usage) 组合一个 .metex ----
    // usage 由引用它的材质 role 决定（同一 image 被 baseColor 与 occlusion 同时
    // 引用会产出两份 artifact，因为 format/colorSpace/mip 滤波语义不同）。
    // 外部 image 文件已在 Import (e) 记录内容哈希；Data URI/GLB 内嵌图片字节随
    // source 一起哈希，改动像素必然改变 BuildKey → 精确失效。
    std::map<std::size_t, std::set<MiniEngine::Assets::TextureUsage>> usagesByImage;
    const auto recordUsage = [&](const std::optional<std::size_t>& imageIndex,
                                 const MiniEngine::Assets::TextureUsage usage)
    {
        if (imageIndex.has_value())
        {
            usagesByImage[*imageIndex].insert(usage);
        }
    };
    for (const ImportedPrimitive& primitive : importResult.primitives)
    {
        if (!primitive.material.has_value())
        {
            continue;
        }
        const ImportedMaterial& material = *primitive.material;
        recordUsage(material.baseColorImageIndex, MiniEngine::Assets::TextureUsage::BaseColor);
        recordUsage(material.metallicRoughnessImageIndex, MiniEngine::Assets::TextureUsage::MetallicRoughness);
        recordUsage(material.normalImageIndex, MiniEngine::Assets::TextureUsage::Normal);
        recordUsage(material.occlusionImageIndex, MiniEngine::Assets::TextureUsage::Occlusion);
        recordUsage(material.emissiveImageIndex, MiniEngine::Assets::TextureUsage::Emissive);
    }

    for (const auto& [imageIndex, usages] : usagesByImage)
    {
        const ImportedImage& importedImage = importResult.images[imageIndex];

        DecodedImage decoded;
        std::string decodeError;
        if (!DecodeImageBytes(importedImage.embeddedBytes, decoded, decodeError))
        {
            LogStage(CookStage::Import, recipeName, assetUri + "#image/" + std::to_string(imageIndex),
                     "WIC decode failed: " + decodeError);
            return 4;
        }

        for (const MiniEngine::Assets::TextureUsage usage : usages)
        {
            const std::string imageKey = "image/" + std::to_string(imageIndex) + "/" + TextureUsageName(usage);
            const std::string recordAssetUri = assetUri + "#" + imageKey;

            std::vector<DependencyRecord> imageDependencies = dependencies;
            DependencyRecord identityDep;
            identityDep.kind = DependencyKind::Recipe;
            identityDep.normalizedPathOrName = imageKey;
            Sha256Digest identityHash{};
            if (!ComputeSha256(std::as_bytes(std::span{imageKey.data(), imageKey.size()}), identityHash))
            {
                LogStage(CookStage::Serialize, recipeName, recordAssetUri, "failed to hash image identity");
                return 7;
            }
            identityDep.contentHash = identityHash;
            imageDependencies.push_back(std::move(identityDep));

            BuildKeyInputs inputs;
            inputs.profile = recipe.profile;
            inputs.canonicalAssetUri = recordAssetUri;
            inputs.canonicalRecipeBytes = canonical;
            inputs.dependencies = imageDependencies; // ComputeBuildKey 内部会排序+校验

            Sha256Digest buildKey{};
            std::string keyError;
            if (!ComputeBuildKey(inputs, buildKey, keyError))
            {
                LogStage(CookStage::Serialize, recipeName, recordAssetUri, "build key failed: " + keyError);
                return 7;
            }

            LogStage(CookStage::Serialize, recipeName, recordAssetUri,
                     "image " + imageKey + " " + std::to_string(decoded.width) + "x" + std::to_string(decoded.height) +
                         " build key=" + ToHexDigest(buildKey).substr(0, 12) + "...");

            // 角色化 mip：滤波域由 usage 决定（sRGB 线性往返 / normal 重归一化 / 线性平均）。
            BuiltTextureMips mips;
            std::string mipError;
            if (!BuildTextureMipChain(decoded.width, decoded.height, usage, decoded.rgba8, mips, mipError))
            {
                LogStage(CookStage::Serialize, recipeName, recordAssetUri, "mip chain failed: " + mipError);
                return 7;
            }

            std::vector<std::byte> textureBytes;
            std::string textureError;
            if (!BuildTextureArtifact(buildKey, decoded.width, decoded.height,
                                      MiniEngine::Assets::TexturePixelFormat::Rgba8Unorm,
                                      usage == MiniEngine::Assets::TextureUsage::BaseColor ||
                                              usage == MiniEngine::Assets::TextureUsage::Emissive
                                          ? MiniEngine::Assets::TextureColorSpace::Srgb
                                          : MiniEngine::Assets::TextureColorSpace::Linear,
                                      usage, mips, textureBytes, textureError))
            {
                LogStage(CookStage::Serialize, recipeName, recordAssetUri, ".metex build failed: " + textureError);
                return 7;
            }
            const int publishCode = PublishArtifact(scope, recordAssetUri, "texture",
                                                    MiniEngine::Assets::BakedAssetKind::Texture, "metex", buildKey,
                                                    textureBytes, "image-" + std::to_string(imageIndex) + "-" +
                                                                      TextureUsageName(usage));
            if (publishCode != 0)
            {
                return publishCode;
            }
        }
    }
    return 0;
}

// CookMaterials：每个 glTF material 一个 .memat v1（身份 = assetUri#material/<index>）；
// 无材质的 primitive 引用合成 default 材质（assetUri#material/default，全默认因子、
// 无贴图）——渲染端 t0–t4 绑定真实 fallback SRV，不再需要 M3 的合成白贴图。
int CookSession::CookMaterials(const RecipeCookScope& scope)
{
    const std::string& recipeName = scope.recipeName;
    const std::string& assetUri = scope.assetUri;
    const Recipe& recipe = *scope.recipe;
    const std::string& canonical = *scope.canonicalJson;
    const std::vector<DependencyRecord>& dependencies = *scope.dependencies;
    const ImportResult& importResult = *scope.import;

    // 同一 glTF materialIndex 被多个 primitive 共享时只烘焙一份（首个出现的
    // ImportedMaterial 即权威内容——同一 index 的材质在 glTF 内是同一份）。
    std::set<std::size_t> materialIndices;
    bool needDefaultMaterial = false;
    for (const ImportedPrimitive& primitive : importResult.primitives)
    {
        if (primitive.gltfMaterialIndex.has_value())
        {
            materialIndices.insert(*primitive.gltfMaterialIndex);
        }
        else
        {
            needDefaultMaterial = true;
        }
    }

    const auto imageUriFor = [&](const std::size_t imageIndex, const MiniEngine::Assets::TextureUsage usage)
        -> std::string
    {
        return assetUri + "#image/" + std::to_string(imageIndex) + "/" + TextureUsageName(usage);
    };

    // 通用发布：uri/identity 前缀由调用点给出（材质与 default 两类）。
    const auto publishMaterial = [&](const std::string& materialKey, const MiniEngine::Assets::MaterialAsset& asset,
                                     const std::string& filePrefix) -> int
    {
        const std::string recordAssetUri = assetUri + "#" + materialKey;

        std::vector<DependencyRecord> materialDependencies = dependencies;
        DependencyRecord identityDep;
        identityDep.kind = DependencyKind::Recipe;
        identityDep.normalizedPathOrName = materialKey;
        Sha256Digest identityHash{};
        if (!ComputeSha256(std::as_bytes(std::span{materialKey.data(), materialKey.size()}), identityHash))
        {
            LogStage(CookStage::Serialize, recipeName, recordAssetUri, "failed to hash material identity");
            return 7;
        }
        identityDep.contentHash = identityHash;
        materialDependencies.push_back(std::move(identityDep));

        BuildKeyInputs inputs;
        inputs.profile = recipe.profile;
        inputs.canonicalAssetUri = recordAssetUri;
        inputs.canonicalRecipeBytes = canonical;
        inputs.dependencies = materialDependencies;

        Sha256Digest buildKey{};
        std::string keyError;
        if (!ComputeBuildKey(inputs, buildKey, keyError))
        {
            LogStage(CookStage::Serialize, recipeName, recordAssetUri, "build key failed: " + keyError);
            return 7;
        }

        std::vector<std::byte> materialBytes;
        std::string materialError;
        if (!BuildMaterialArtifact(buildKey, asset, materialBytes, materialError))
        {
            LogStage(CookStage::Serialize, recipeName, recordAssetUri, ".memat build failed: " + materialError);
            return 7;
        }
        const int publishCode =
            PublishArtifact(scope, recordAssetUri, "material", MiniEngine::Assets::BakedAssetKind::Material, "memat",
                            buildKey, materialBytes, filePrefix);
        if (publishCode != 0)
        {
            return publishCode;
        }
        return 0;
    };

    const auto fillAssetTextures = [&](MiniEngine::Assets::MaterialAsset& asset, const ImportedMaterial& material)
    {
        if (material.baseColorImageIndex.has_value())
        {
            asset.baseColorTexture =
                MiniEngine::Assets::DeriveAssetId(imageUriFor(*material.baseColorImageIndex,
                                                              MiniEngine::Assets::TextureUsage::BaseColor));
        }
        if (material.metallicRoughnessImageIndex.has_value())
        {
            asset.metallicRoughnessTexture =
                MiniEngine::Assets::DeriveAssetId(imageUriFor(*material.metallicRoughnessImageIndex,
                                                              MiniEngine::Assets::TextureUsage::MetallicRoughness));
        }
        if (material.normalImageIndex.has_value())
        {
            asset.normalTexture =
                MiniEngine::Assets::DeriveAssetId(imageUriFor(*material.normalImageIndex,
                                                              MiniEngine::Assets::TextureUsage::Normal));
        }
        if (material.occlusionImageIndex.has_value())
        {
            asset.occlusionTexture =
                MiniEngine::Assets::DeriveAssetId(imageUriFor(*material.occlusionImageIndex,
                                                              MiniEngine::Assets::TextureUsage::Occlusion));
        }
        if (material.emissiveImageIndex.has_value())
        {
            asset.emissiveTexture =
                MiniEngine::Assets::DeriveAssetId(imageUriFor(*material.emissiveImageIndex,
                                                              MiniEngine::Assets::TextureUsage::Emissive));
        }
    };

    for (const std::size_t materialIndex : materialIndices)
    {
        const ImportedMaterial* source = nullptr;
        for (const ImportedPrimitive& primitive : importResult.primitives)
        {
            if (primitive.gltfMaterialIndex == materialIndex)
            {
                source = &*primitive.material;
                break;
            }
        }

        MiniEngine::Assets::MaterialAsset asset;
        for (int component = 0; component < 4; ++component)
        {
            asset.baseColorFactor[component] = source->baseColorFactor[component];
        }
        for (int component = 0; component < 3; ++component)
        {
            asset.emissiveFactor[component] = source->emissiveFactor[component];
        }
        asset.metallicFactor = source->metallicFactor;
        asset.roughnessFactor = source->roughnessFactor;
        asset.normalScale = source->normalScale;
        asset.occlusionStrength = source->occlusionStrength;
        fillAssetTextures(asset, *source);

        const std::string materialKey = "material/" + std::to_string(materialIndex);
        LogStage(CookStage::Serialize, recipeName, assetUri + "#" + materialKey, "baking material");
        if (const int publishCode = publishMaterial(materialKey, asset, "material-" + std::to_string(materialIndex));
            publishCode != 0)
        {
            return publishCode;
        }
    }

    if (needDefaultMaterial)
    {
        const MiniEngine::Assets::MaterialAsset defaultMaterial;
        LogStage(CookStage::Serialize, recipeName, assetUri + "#material/default", "baking default material");
        if (const int publishCode = publishMaterial("material/default", defaultMaterial, "material-default");
            publishCode != 0)
        {
            return publishCode;
        }
    }
    return 0;
}

// CookEnvironment（M4-04）：recipe.environment 段的 .hdr → .metex HdrEnvironment。
// 契约（04 篇「HDR source contract」+ 审查确认项）：
//   - stb memory API 解码（Radiance 格式前置检查），desired_channels=4；
//   - 解码值是线性光照值——不做任何 sRGB 处理，colorSpace 恒 Linear（RGBA8 与
//     .hdr 的本质区别：HDR 源再加 gamma 会把环境光整体变亮）；
//   - 拒绝 NaN/Inf/负值与超预算尺寸（maxTextureDimension/maxFileBytes 先行）；
//   - mipCount=1：EquirectToCube 转换只采 LOD0，panorama 无 mip 消费方——
//     不走 TextureMipBuilder（它对 HdrEnvironment 的角色化滤波未定义），
//     直接构造单级 BuiltTextureMips；
//   - 像素 = RGBA16F 半精度字节流（8B/像素），解码端 AssetManager 透传字节。
int CookSession::CookEnvironment(const RecipeCookScope& scope)
{
    const std::string& recipeName = scope.recipeName;
    const Recipe& recipe = *scope.recipe;
    if (!recipe.environment.has_value())
    {
        return 0; // 无 environment 段：no-op（向后兼容旧 recipe）
    }
    const RecipeEnvironment& environment = *recipe.environment;

    const std::string recordAssetUri = environment.assetUri;
    const auto resolvedEnvironment = ResolveSourceWithinRoot(m_commandLine.sourceRoot, environment.source);
    if (!resolvedEnvironment.has_value())
    {
        LogStage(CookStage::Import, recipeName, recordAssetUri,
                 "environment source escapes source-root: " + environment.source);
        return 2;
    }

    // 文件预算（与 glTF 内嵌 image 同一 maxFileBytes/maxTextureDimension 纪律）。
    const auto fileBytes = std::filesystem::file_size(*resolvedEnvironment);
    if (fileBytes > recipe.maxFileBytes)
    {
        LogStage(CookStage::Import, recipeName, recordAssetUri, "environment file exceeds maxFileBytes budget");
        return 4;
    }

    std::vector<std::byte> sourceBytes(static_cast<std::size_t>(fileBytes));
    {
        std::ifstream stream{*resolvedEnvironment, std::ios::binary};
        if (!stream.read(reinterpret_cast<char*>(sourceBytes.data()), static_cast<std::streamsize>(fileBytes)))
        {
            LogStage(CookStage::Import, recipeName, recordAssetUri, "environment source read failed");
            return 4;
        }
    }

    // 解码 + 数值校验（Radiance 前置检查 / NaN/Inf/负拒绝在 DecodeHdrBytes 内）。
    DecodedHdrImage decoded;
    std::string decodeError;
    if (!DecodeHdrBytes(sourceBytes.data(), sourceBytes.size(), decoded, decodeError))
    {
        LogStage(CookStage::Import, recipeName, recordAssetUri, "HDR decode failed: " + decodeError);
        return 4;
    }
    if (decoded.width > recipe.maxTextureDimension || decoded.height > recipe.maxTextureDimension)
    {
        LogStage(CookStage::Import, recipeName, recordAssetUri,
                 "environment dimension exceeds maxTextureDimension budget");
        return 4;
    }
    LogStage(CookStage::Import, recipeName, recordAssetUri,
             "environment " + std::to_string(decoded.width) + "x" + std::to_string(decoded.height) +
                 " decoded (linear RGBA float)");

    // 线性 float → RGBA16F 半精度字节流；单 mip（rowPitch = width*8）。
    const std::vector<std::uint8_t> halfPixels = LinearFloatToRgba16f(decoded.rgba);
    BuiltTextureMips mips;
    mips.pixels.resize(halfPixels.size());
    std::memcpy(mips.pixels.data(), halfPixels.data(), halfPixels.size());
    const TextureMipLevel mip0{0U, decoded.width * 8U, static_cast<std::uint32_t>(halfPixels.size())};
    mips.levels.push_back(mip0);

    // BuildKey：与 (image,usage) 纹理同款——canonical recipe（含 environment 段）+
    // 依赖列表（其中 (f) 已含 .hdr 内容 hash）。身份 = environment.assetUri。
    std::vector<DependencyRecord> environmentDependencies = *scope.dependencies;
    DependencyRecord identityDep;
    identityDep.kind = DependencyKind::Recipe;
    identityDep.normalizedPathOrName = "environment";
    Sha256Digest identityHash{};
    if (!ComputeSha256(std::as_bytes(std::span{environment.assetUri.data(), environment.assetUri.size()}),
                       identityHash))
    {
        LogStage(CookStage::Serialize, recipeName, recordAssetUri, "failed to hash environment identity");
        return 7;
    }
    identityDep.contentHash = identityHash;
    environmentDependencies.push_back(std::move(identityDep));

    BuildKeyInputs inputs;
    inputs.profile = recipe.profile;
    inputs.canonicalAssetUri = recordAssetUri;
    inputs.canonicalRecipeBytes = *scope.canonicalJson;
    inputs.dependencies = environmentDependencies;

    Sha256Digest buildKey{};
    std::string keyError;
    if (!ComputeBuildKey(inputs, buildKey, keyError))
    {
        LogStage(CookStage::Serialize, recipeName, recordAssetUri, "build key failed: " + keyError);
        return 7;
    }
    LogStage(CookStage::Serialize, recipeName, recordAssetUri,
             "environment " + std::to_string(decoded.width) + "x" + std::to_string(decoded.height) +
                 " build key=" + ToHexDigest(buildKey).substr(0, 12) + "...");

    // .metex v2：Rgba16Float + Linear + HdrEnvironment + 单 mip。
    std::vector<std::byte> textureBytes;
    std::string textureError;
    if (!BuildTextureArtifact(buildKey, decoded.width, decoded.height,
                              MiniEngine::Assets::TexturePixelFormat::Rgba16Float,
                              MiniEngine::Assets::TextureColorSpace::Linear,
                              MiniEngine::Assets::TextureUsage::HdrEnvironment, mips, textureBytes, textureError))
    {
        LogStage(CookStage::Serialize, recipeName, recordAssetUri, ".metex build failed: " + textureError);
        return 7;
    }
    const int publishCode = PublishArtifact(scope, recordAssetUri, "texture",
                                            MiniEngine::Assets::BakedAssetKind::Texture, "metex", buildKey,
                                            textureBytes, "environment");
    if (publishCode != 0)
    {
        return publishCode;
    }
    return 0;
}

int CookSession::BakeWorld(const RecipeCookScope& scope)
{
    const std::string& recipeName = scope.recipeName;
    const std::string& assetUri = scope.assetUri;
    const Recipe& recipe = *scope.recipe;
    const std::string& canonical = *scope.canonicalJson;
    const std::vector<DependencyRecord>& dependencies = *scope.dependencies;
    const ImportResult& importResult = *scope.import;

    // ---- World bake：场景图 → .meworld（02 篇 Node 映射、06 篇 deferral）----
    // 每个 glTF node 一个实体（primitive 展开规则稳定）；资源引用写 128-bit
    // AssetId（与 .memesh/.metex 清单条目的 assetUri 派生一致），不写 runtime Handle。
    if (importResult.nodes.empty())
    {
        return 0;
    }

    std::unordered_map<std::size_t, std::size_t> parentOf;
    for (const ImportedNode& node : importResult.nodes)
    {
        for (const std::size_t child : node.childIndices)
        {
            parentOf[child] = node.gltfNodeIndex;
        }
    }
    std::unordered_map<std::size_t, std::vector<std::size_t>> primitivesByNode;
    for (std::size_t index = 0; index < importResult.primitives.size(); ++index)
    {
        primitivesByNode[importResult.primitives[index].nodeIndex].push_back(index);
    }

    // 与 mesh 清单条目同规则：单 primitive 用裸 assetUri，多 primitive 用 #mesh/…。
    const auto meshUriFor = [&](const ImportedPrimitive& primitive) -> std::string
    {
        return importResult.primitives.size() == 1 ? assetUri : assetUri + "#" + primitive.stableKey;
    };
    // 材质 URI 与 CookMaterials 的发布身份一致：有材质 → #material/<index>；
    // 无材质 → 合成的 #material/default（ Cooker 已烘焙，Handle 恒可解析）。
    const auto materialUriFor = [&](const ImportedPrimitive& primitive) -> std::string
    {
        return primitive.gltfMaterialIndex.has_value() ? assetUri + "#material/" +
                                                             std::to_string(*primitive.gltfMaterialIndex)
                                                       : assetUri + "#material/default";
    };

    std::vector<MeworldEntity> worldEntities;
    std::unordered_map<std::size_t, std::size_t> nodeEntitySlot;
    for (const ImportedNode& node : importResult.nodes)
    {
        const auto found = primitivesByNode.find(node.gltfNodeIndex);
        const std::vector<std::size_t>* nodePrimitives =
            found == primitivesByNode.end() ? nullptr : &found->second;
        const bool hasRenderers = nodePrimitives != nullptr && !nodePrimitives->empty();

        std::int32_t parentEntity = -1;
        if (const auto parentIt = parentOf.find(node.gltfNodeIndex); parentIt != parentOf.end())
        {
            const auto parentSlotIt = nodeEntitySlot.find(parentIt->second);
            if (parentSlotIt != nodeEntitySlot.end())
            {
                parentEntity = static_cast<std::int32_t>(parentSlotIt->second);
            }
        }

        const auto fillMeshRenderer = [&](MeworldEntity& record, const ImportedPrimitive& primitive)
        {
            record.hasMesh = true;
            const MiniEngine::Assets::AssetId meshId = MiniEngine::Assets::DeriveAssetId(meshUriFor(primitive));
            record.meshAssetId = meshId.bytes;
            // v2：MeshRenderer 引用 `.memat` 的 AssetId（材质语义全部由 .memat 承载）；
            // 无材质 primitive 由 CookMaterials 烘焙的 default 材质兜底，Handle 恒可解析。
            record.hasMaterial = true;
            const MiniEngine::Assets::AssetId materialId =
                MiniEngine::Assets::DeriveAssetId(materialUriFor(primitive));
            record.materialAssetId = materialId.bytes;
        };

        MeworldEntity primary;
        primary.name = node.name;
        primary.parentIndex = parentEntity;
        if (node.meshIndex.has_value())
        {
            ConvertNodeTrsToRowMajor(node.translation, node.rotation, node.scale, primary.localRowMajor);
        }
        const std::size_t primarySlot = worldEntities.size();
        if (hasRenderers)
        {
            fillMeshRenderer(primary, importResult.primitives[(*nodePrimitives)[0]]);
        }
        worldEntities.push_back(std::move(primary));
        nodeEntitySlot[node.gltfNodeIndex] = primarySlot;

        // 多 primitive：额外 primitive 作为主实体的 child entity（本地恒等）。
        for (std::size_t primitiveIndex = 1;
             hasRenderers && primitiveIndex < nodePrimitives->size(); ++primitiveIndex)
        {
            MeworldEntity extra;
            extra.parentIndex = static_cast<std::int32_t>(primarySlot);
            fillMeshRenderer(extra, importResult.primitives[(*nodePrimitives)[primitiveIndex]]);
            worldEntities.push_back(std::move(extra));
        }
    }

    // M4：无贴图兜底改由渲染端 fallback SRV 承担，M3 的合成白贴图不再烘焙。

    const std::string worldKey = "world/default";
    const std::string worldUri = assetUri + "#" + worldKey;

    std::vector<DependencyRecord> worldDependencies = dependencies;
    DependencyRecord identityDep;
    identityDep.kind = DependencyKind::Recipe;
    identityDep.normalizedPathOrName = worldKey;
    Sha256Digest identityHash{};
    if (!ComputeSha256(std::as_bytes(std::span{worldKey.data(), worldKey.size()}), identityHash))
    {
        LogStage(CookStage::Serialize, recipeName, worldUri, "failed to hash world identity");
        return 7;
    }
    identityDep.contentHash = identityHash;
    worldDependencies.push_back(std::move(identityDep));

    BuildKeyInputs inputs;
    inputs.profile = recipe.profile;
    inputs.canonicalAssetUri = worldUri;
    inputs.canonicalRecipeBytes = canonical;
    inputs.dependencies = worldDependencies; // ComputeBuildKey 内部会排序+校验

    Sha256Digest buildKey{};
    std::string keyError;
    if (!ComputeBuildKey(inputs, buildKey, keyError))
    {
        LogStage(CookStage::Serialize, recipeName, worldUri, "build key failed: " + keyError);
        return 7;
    }

    std::vector<std::byte> worldBytes;
    std::string worldError;
    if (!BuildMeworldArtifact(buildKey, worldEntities, worldBytes, worldError))
    {
        LogStage(CookStage::Serialize, recipeName, worldUri, ".meworld build failed: " + worldError);
        return 7;
    }
    LogStage(CookStage::Serialize, recipeName, worldUri,
             "world default " + std::to_string(worldEntities.size()) + " entities build key=" +
                 ToHexDigest(buildKey).substr(0, 12) + "...");

    const int publishCode = PublishArtifact(scope, worldUri, "world", MiniEngine::Assets::BakedAssetKind::World,
                                            "meworld", buildKey, worldBytes, "world-0");
    if (publishCode != 0)
    {
        return publishCode;
    }
    return 0;
}

int CookSession::PublishArtifact(const RecipeCookScope& scope, const std::string& publishUri,
                                 const std::string& kindText, const MiniEngine::Assets::BakedAssetKind bakedKind,
                                 const std::string& extension, const Sha256Digest& artifactBuildKey,
                                 const std::vector<std::byte>& bytes, const std::string& fileNamePrefix)
{
    const std::string& recipeName = scope.recipeName;
    std::vector<CookedArtifactRecord>& collectedArtifacts = *scope.collectedArtifacts;
    const std::filesystem::path cacheDir = BuildCacheDirectory(m_commandLine.outputRoot, artifactBuildKey);

    // artifactHash 由完整字节决定，文件名（=hash）在写入前即可确定，
    // cache 命中判定无需先落盘。
    Sha256Digest artifactHash{};
    if (!ComputeSha256(bytes, artifactHash))
    {
        LogStage(CookStage::Serialize, recipeName, publishUri, "failed to hash artifact bytes");
        return 7;
    }

    const std::filesystem::path destination =
        BuildArtifactPath(cacheDir, artifactHash, fileNamePrefix, extension);
    CacheHitInput hitInput{.artifactPath = destination,
                           .expectedBuildKey = artifactBuildKey,
                           .expectedArtifactHash = artifactHash,
                           .kind = bakedKind};

    // ---- PublishArtifacts 阶段：先判 cache hit，miss 才走 staging → 发布 ----
    std::error_code dirError;
    std::filesystem::create_directories(cacheDir, dirError);
    if (dirError)
    {
        LogStage(CookStage::PublishArtifacts, recipeName, publishUri,
                 "failed to create cache directory: " + dirError.message());
        return 6;
    }

    const CacheHitResult hit = EvaluateCacheHit(hitInput);
    if (hit.hit)
    {
        // 命中：EvaluateCacheHit 已用 runtime Reader 完成重开验证（含 BuildKey
        // 比对与 artifactHash 比对），不可变文件不重写——这是版本目标的核心行为。
        LogStage(CookStage::ReopenVerify, recipeName, publishUri, "cache hit; reader verified in place");
        LogStage(CookStage::PublishArtifacts, recipeName, publishUri, "cache hit; artifact reused");
    }
    else
    {
        std::error_code existsError;
        if (std::filesystem::exists(destination, existsError))
        {
            LogStage(CookStage::PublishArtifacts, recipeName, publishUri,
                     "cache corruption (" + hit.reason + "); remove the exact BuildKey directory");
            return 6;
        }

        std::filesystem::path staged = destination;
        staged += ".tmp";
        if (!WriteBytesToFile(staged, bytes))
        {
            LogStage(CookStage::PublishArtifacts, recipeName, publishUri, "failed to write staging file");
            return 6;
        }

        // ReopenVerify：发布前用 runtime Reader 重开 staging（hash + BuildKey + 结构）。
        CacheHitInput stagedInput = hitInput;
        stagedInput.artifactPath = staged;
        const CacheHitResult stagedCheck = EvaluateCacheHit(stagedInput);
        if (!stagedCheck.hit)
        {
            std::error_code removeError;
            std::filesystem::remove(staged, removeError); // 失败的 staging 不应残留
            LogStage(CookStage::ReopenVerify, recipeName, publishUri,
                     "staging failed verification: " + stagedCheck.reason);
            return 5;
        }
        LogStage(CookStage::ReopenVerify, recipeName, publishUri, "staging reopened and verified");

        std::string publishError;
        if (!PublishImmutableFile(staged, destination, publishError))
        {
            LogStage(CookStage::PublishArtifacts, recipeName, publishUri, "publish failed: " + publishError);
            return 6;
        }
        LogStage(CookStage::PublishArtifacts, recipeName, publishUri,
                 destination.filename().string() + " published");
    }

    // 记录成功发布的 artifact，供 RunCook 最后统一发布 Manifest。
    CookedArtifactRecord record;
    record.assetUri = publishUri;
    record.kind = kindText;
    record.buildKey = artifactBuildKey;
    record.artifactHash = artifactHash;
    record.fileSize = bytes.size();
    record.artifactPath = destination.lexically_relative(m_commandLine.outputRoot).generic_string();
    collectedArtifacts.push_back(std::move(record));
    return 0;
}

int CookSession::PublishManifest(const std::vector<CookedArtifactRecord>& records) const
{
    if (records.empty())
    {
        // 没有任何成功 artifact 时保持旧 Manifest 不动（不发布空 snapshot）。
        LogStage(CookStage::PublishManifest, "", "", "no artifacts; manifest left unchanged");
        return 0;
    }

    // D2（M4-09 审查）：重复 assetUri 是发布期就必须暴露的冲突——运行期
    // AssetRegistry 对重复 AssetId fail-closed（整份 manifest 被拒），而此前
    // Cooker 会静默写出无法加载的 manifest（两个 recipe 发布同一 environment URI
    // 时即触发）。冲突直接报错，不做静默去重（审查裁决：冲突该暴露）。
    const std::vector<std::string> duplicates = FindDuplicateAssetUris(records);
    if (!duplicates.empty())
    {
        for (const std::string& uri : duplicates)
        {
            LogStage(CookStage::PublishManifest, "", uri,
                     "duplicate assetUri produced by multiple recipes/artifacts; manifest rejected");
        }
        return 6;
    }

    const std::string manifestJson = BuildManifestJson(records, m_commandLine.profile);
    const std::span<const char> jsonChars{manifestJson.data(), manifestJson.size()};

    std::error_code dirError;
    std::filesystem::create_directories(m_commandLine.outputRoot, dirError);
    if (dirError)
    {
        LogStage(CookStage::PublishManifest, "", "", "failed to create output root: " + dirError.message());
        return 6;
    }

    const std::filesystem::path destination = m_commandLine.outputRoot / "manifest.json";
    std::filesystem::path staged = destination;
    staged += ".tmp";
    if (!WriteBytesToFile(staged, std::as_bytes(jsonChars)))
    {
        LogStage(CookStage::PublishManifest, "", "", "failed to write manifest staging file");
        return 6;
    }

    std::string publishError;
    if (!ReplaceSnapshotFile(staged, destination, publishError))
    {
        LogStage(CookStage::PublishManifest, "", "", "manifest publish failed: " + publishError);
        return 6;
    }

    LogStage(CookStage::PublishManifest, "", destination.string(), "manifest published");
    return 0;
}
} // namespace MiniEngine::Tools
