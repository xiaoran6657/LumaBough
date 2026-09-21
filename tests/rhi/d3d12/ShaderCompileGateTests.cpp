// ============================================================================
// ShaderCompileGateTests.cpp — D3D12 shader manifest 门禁
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 1/4 条）
// 职责：校验 tools/shader_compiler 产出的 manifest 与产物本身：
//   - 每个登记入口都有 manifest 与 .dxil；
//   - manifest 字段完整（08 篇清单：source/entry/target/compilerVersion/
//     arguments/sourceSha256/dxilSha256/pdbName），并标记 full PDB；
//   - **sourceSha256 与源文件当前内容一致、dxilSha256 与 .dxil 内容一致**
//     ——这是"编译可复现"的可执行定义：产物过期或源文件被改没重编译都会失败；
//   - PDB 以 manifest 记录的名字存在（PIX 自动定位的前提）。
// 为什么不在测试里调 DXC：编译入口的唯一实现在 tools/shader_compiler；测试若自己
//   再编译一次，就会把"产物没生成/没更新"这类问题掩盖掉（08 篇验收：帧内不编译）。
// 关联：tools/shader_compiler/src/DxcCompiler.h（产物与 manifest 的生产者）
//       docs/architecture/README.md（manifest 字段 / 验收项）
// ============================================================================
#include <MiniEngine/Assets/Sha256.h>

#include <Windows.h>

#include <wrl/client.h>

#include <dxcapi.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ShaderCompileRegistry.h"

#ifndef MINIENGINE_D3D12_SHADER_DIR
#error "MINIENGINE_D3D12_SHADER_DIR must be defined by the test target"
#endif

namespace
{
// 产物目录（build artifact）：.dxil / .pdb / manifest 在这里。
std::filesystem::path g_shaderDirectory{MINIENGINE_D3D12_SHADER_DIR};
// 源码目录（仓库内）：`shaders/d3d12/*.hlsl` 在这里；两者同名但不同物，
// 用错一个会把"磁盘上有 shader"误判成"已编译"。
std::filesystem::path g_shaderSourceDirectory{MINIENGINE_SHADER_DIR};
std::filesystem::path g_repositoryRoot{MINIENGINE_PROJECT_ROOT};

std::vector<std::byte> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return {};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        return {};
    }
    return bytes;
}

std::string Sha256Hex(const std::vector<std::byte>& bytes)
{
    const MiniEngine::Assets::Sha256Digest digest = MiniEngine::Assets::Sha256(bytes);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::byte byte : digest)
    {
        stream << std::setw(2) << static_cast<unsigned>(std::to_integer<std::uint8_t>(byte));
    }
    return stream.str();
}

// 极简字段取值（manifest 是本工具产出的固定形态：每行一个 "key": value）。
std::string FieldValue(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\": ";
    const std::size_t begin = text.find(needle);
    if (begin == std::string::npos)
    {
        return {};
    }
    std::size_t first = text.find('"', begin + needle.size());
    if (first == std::string::npos)
    {
        return {};
    }
    const std::size_t end = text.find('"', first + 1U);
    if (end == std::string::npos)
    {
        return {};
    }
    return text.substr(first + 1U, end - first - 1U);
}

std::string OutputBaseName(const MiniEngine::ShaderCompiler::ShaderEntry& entry)
{
    const std::string target{entry.target};
    const std::string stage = target.substr(0U, target.find('_'));
    return std::filesystem::path{entry.source}.stem().string() + "." + entry.entry + "." + stage;
}

// manifest 里的字符串数组字段（"arguments": ["a", "b"]）。
std::vector<std::string> StringArrayField(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\": [";
    const std::size_t begin = text.find(needle);
    if (begin == std::string::npos)
    {
        return {};
    }
    const std::size_t end = text.find(']', begin + needle.size());
    if (end == std::string::npos)
    {
        return {};
    }

    std::vector<std::string> values;
    std::size_t cursor = begin + needle.size();
    while (cursor < end)
    {
        const std::size_t open = text.find('"', cursor);
        if (open == std::string::npos || open >= end)
        {
            break;
        }
        const std::size_t close = text.find('"', open + 1U);
        if (close == std::string::npos || close >= end)
        {
            break;
        }
        values.push_back(text.substr(open + 1U, close - open - 1U));
        cursor = close + 1U;
    }
    return values;
}

// 源文件里 `#include "X"` 的文件名（PIX 侧"PDB 是否带源"这一项要逐个核对）。
std::vector<std::string> IncludedHeaders(const std::string& text)
{
    std::vector<std::string> headers;
    std::size_t cursor = 0U;
    for (;;)
    {
        const std::size_t at = text.find("#include", cursor);
        if (at == std::string::npos)
        {
            break;
        }
        const std::size_t open = text.find('"', at);
        const std::size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1U);
        if (close == std::string::npos)
        {
            break;
        }
        headers.push_back(text.substr(open + 1U, close - open - 1U));
        cursor = close + 1U;
    }
    return headers;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    const std::vector<std::byte> bytes = ReadBytes(path);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
} // namespace

// manifest 字段完整性与哈希可复现性（每个登记入口）。
TEST(ShaderCompileGateTests, EveryEntryHasManifestWithMatchingHashes)
{
    std::size_t checked = 0U;
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        const std::string baseName = OutputBaseName(entry);
        const std::filesystem::path manifestPath = g_shaderDirectory / (baseName + ".manifest.json");
        ASSERT_TRUE(std::filesystem::exists(manifestPath))
            << "缺少 manifest（先跑 MiniEngineShaderCompiler）：" << manifestPath.string();

        const std::vector<std::byte> manifestBytes = ReadBytes(manifestPath);
        const std::string manifestText{reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()};

        // 08 篇 manifest 字段清单。
        EXPECT_EQ(FieldValue(manifestText, "source"), entry.source) << manifestPath.string();
        EXPECT_EQ(FieldValue(manifestText, "entry"), entry.entry) << manifestPath.string();
        EXPECT_EQ(FieldValue(manifestText, "target"), entry.target) << manifestPath.string();
        EXPECT_FALSE(FieldValue(manifestText, "compilerVersion").empty()) << manifestPath.string();
        EXPECT_NE(manifestText.find("\"arguments\""), std::string::npos) << manifestPath.string();
        // warnings-as-errors 与 HLSL 2021 必须在参数里（08 篇冻结口径）。
        EXPECT_NE(manifestText.find("-WX"), std::string::npos) << "缺少 -WX：" << manifestPath.string();
        EXPECT_NE(manifestText.find("2021"), std::string::npos) << "缺少 -HV 2021：" << manifestPath.string();

        // 源文件哈希必须与当前内容一致（产物过期的判定）。
        const std::vector<std::byte> sourceBytes = ReadBytes(g_repositoryRoot / entry.source);
        ASSERT_FALSE(sourceBytes.empty()) << entry.source;
        EXPECT_EQ(FieldValue(manifestText, "sourceSha256"), Sha256Hex(sourceBytes))
            << "源已改动但未重编译：" << entry.source;

        // DXIL 哈希必须与落盘对象一致。
        const std::filesystem::path objectPath = g_shaderDirectory / (baseName + ".dxil");
        ASSERT_TRUE(std::filesystem::exists(objectPath)) << objectPath.string();
        const std::vector<std::byte> objectBytes = ReadBytes(objectPath);
        EXPECT_FALSE(objectBytes.empty()) << objectPath.string();
        EXPECT_EQ(FieldValue(manifestText, "dxilSha256"), Sha256Hex(objectBytes)) << objectPath.string();

        // PIX 自动定位依赖 manifest 记录的 PDB 名真实存在。
        const std::string pdbName = FieldValue(manifestText, "pdbName");
        ASSERT_FALSE(pdbName.empty()) << manifestPath.string();
        EXPECT_TRUE(std::filesystem::exists(g_shaderDirectory / pdbName)) << "缺少 PDB：" << pdbName;
        ++checked;
    }
    EXPECT_EQ(checked, MiniEngine::ShaderCompiler::Entries().size()) << "全部入口都应被检查";
}

// 每个 `shaders/d3d12/*.hlsl` 都必须登记进了口（与 M4 的 HLSL gate 同款护栏）：
// 新增 shader 忘了登记时，"没有任何东西会被编译"这件事会被这条断言抓住。
TEST(ShaderCompileGateTests, EveryShaderSourceFileHasRegisteredEntries)
{
    std::set<std::string> registered;
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        registered.insert(entry.source);
    }

    std::size_t fileCount = 0U;
    for (const std::filesystem::directory_entry& candidate :
         std::filesystem::directory_iterator{g_shaderSourceDirectory})
    {
        if (!candidate.is_regular_file() || candidate.path().extension() != ".hlsl")
        {
            continue;
        }
        ++fileCount;
        const std::string relative = "shaders/d3d12/" + candidate.path().filename().string();
        EXPECT_TRUE(registered.contains(relative)) << "未登记编译入口的 shader 文件：" << relative;
    }
    EXPECT_GT(fileCount, 0U) << "shaders/d3d12 下应当有 .hlsl 文件";
}

// IBL 生成入口是固定清单的一部分：四个 shader 共 9 个入口，其中 Irradiance 的
// PSDownsample 专门供 EnvironmentDownsample pass 使用，漏登记会让 baseline 无法创建。
TEST(ShaderCompileGateTests, IblBakeEntriesIncludeDownsampleEntry)
{
    std::set<std::string> entries;
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        const std::string source{entry.source};
        if (source.find("EquirectToCube.hlsl") != std::string::npos ||
            source.find("IrradianceConvolution.hlsl") != std::string::npos ||
            source.find("PrefilterEnvironment.hlsl") != std::string::npos ||
            source.find("IntegrateBrdf.hlsl") != std::string::npos)
        {
            entries.insert(source + ":" + entry.entry + ":" + entry.target);
        }
    }
    EXPECT_EQ(entries.size(), 9U) << "四个 IBL shader 必须登记 9 个入口（含 PSDownsample）";
    EXPECT_TRUE(entries.contains("shaders/d3d12/IrradianceConvolution.hlsl:PSDownsample:ps_6_0"));
}

// 参数必须是 08 篇冻结的那一份：多一个少一个都会改变"可复现"的口径，
// 因此这里逐项比较而不是"包含 -WX 就算过"。
TEST(ShaderCompileGateTests, ArgumentsAreExactlyTheFrozenContract)
{
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        const std::string baseName = OutputBaseName(entry);
        const std::filesystem::path manifestPath = g_shaderDirectory / (baseName + ".manifest.json");
        ASSERT_TRUE(std::filesystem::exists(manifestPath)) << manifestPath.string();

        const std::string manifestText = ReadTextFile(manifestPath);
        // -E/-T 有独立的 entry/target 字段，不在此重复（重复会引入两处漂移源）。
        const std::vector<std::string> expected{"-HV", "2021", "-Ges", "-WX", "-Zi", "-Zss"};
        EXPECT_EQ(StringArrayField(manifestText, "arguments"), expected) << manifestPath.string();

        // SM 6.0 是 08 篇的固定 target：`-WX` 之外的每个 call site 都不得降级。
        EXPECT_TRUE(std::string{entry.target} == "vs_6_0" || std::string{entry.target} == "ps_6_0")
            << entry.source << " target=" << entry.target;
    }
}

// PIX 自动定位 shader PDB 的机制是"DXIL 容器里写着 PDB 名"。这条连线断了，
// 现场就只剩"PIX 找不到符号"这种无法归因的现象——因此由测试直接断言。
TEST(ShaderCompileGateTests, DxilReferencesTheManifestPdbName)
{
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        const std::string baseName = OutputBaseName(entry);
        const std::string manifestText = ReadTextFile(g_shaderDirectory / (baseName + ".manifest.json"));
        const std::string pdbName = FieldValue(manifestText, "pdbName");
        ASSERT_FALSE(pdbName.empty()) << baseName;

        const std::string dxilText = ReadTextFile(g_shaderDirectory / (baseName + ".dxil"));
        EXPECT_NE(dxilText.find(pdbName), std::string::npos)
            << "DXIL 未引用 PDB 名（PIX 无法自动解析）：" << baseName << " pdb=" << pdbName;
    }
}

// PIX 要能跳到 HLSL 源码，PDB 里必须带源（`-Zss`）及其被 #include 的头文件。
TEST(ShaderCompileGateTests, PdbCarriesSourceAndIncludePathsForPix)
{
    // 同一个 .hlsl 的多个入口共用同一份源，按"文件"去重即可（取首个入口的产物）。
    std::map<std::string, std::string> firstEntryBySource;
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        firstEntryBySource.emplace(entry.source, OutputBaseName(entry));
    }

    for (const auto& [source, baseName] : firstEntryBySource)
    {
        const std::string manifestText = ReadTextFile(g_shaderDirectory / (baseName + ".manifest.json"));
        const std::string pdbName = FieldValue(manifestText, "pdbName");
        ASSERT_FALSE(pdbName.empty()) << source;

        const std::string pdbText = ReadTextFile(g_shaderDirectory / pdbName);
        EXPECT_FALSE(pdbText.empty()) << "PDB 为空：" << pdbName;
        // 源文件名（不含路径也必须出现，PIX 用它匹配本地文件）。
        const std::string fileName = std::filesystem::path{source}.filename().string();
        EXPECT_NE(pdbText.find(fileName), std::string::npos) << "PDB 缺少源文件名：" << pdbName;

        const std::string sourceText = ReadTextFile(g_repositoryRoot / source);
        for (const std::string& header : IncludedHeaders(sourceText))
        {
            EXPECT_NE(pdbText.find(header), std::string::npos)
                << "PDB 缺少被 include 的头文件：" << header << "（" << source << "）";
        }
    }
}

// 通过 DXC 自己的 PDB 解析接口检查落盘 blob，而不是仅相信 manifest 的文字标记。
// 这样 -Zi/-Zss 意外回退为 slim、或写错了 blob，都会在 shader gate 里失败。
TEST(ShaderCompileGateTests, PdbArtifactsAreFull)
{
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    ASSERT_TRUE(SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) << "无法创建 IDxcUtils";
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        const std::string baseName = OutputBaseName(entry);
        const std::filesystem::path manifestPath = g_shaderDirectory / (baseName + ".manifest.json");
        const std::string manifestText = ReadTextFile(manifestPath);
        const std::string pdbName = FieldValue(manifestText, "pdbName");
        ASSERT_FALSE(pdbName.empty()) << manifestPath.string();

        const std::vector<std::byte> pdbBytes = ReadBytes(g_shaderDirectory / pdbName);
        ASSERT_FALSE(pdbBytes.empty()) << pdbName;
        ASSERT_LE(pdbBytes.size(), static_cast<std::size_t>(UINT32_MAX)) << pdbName;

        Microsoft::WRL::ComPtr<IDxcBlobEncoding> pdbBlob;
        Microsoft::WRL::ComPtr<IDxcPdbUtils2> pdbUtils;
        ASSERT_TRUE(SUCCEEDED(DxcCreateInstance(CLSID_DxcPdbUtils, IID_PPV_ARGS(&pdbUtils))))
            << "无法创建 IDxcPdbUtils2";
        const HRESULT createBlobHr =
            utils->CreateBlob(pdbBytes.data(), static_cast<UINT32>(pdbBytes.size()), DXC_CP_ACP, &pdbBlob);
        ASSERT_TRUE(SUCCEEDED(createBlobHr)) << "CreateBlob 失败，HRESULT=" << std::showbase << std::hex
                                             << static_cast<unsigned long>(createBlobHr) << "，PDB=" << pdbName;
        ASSERT_TRUE(SUCCEEDED(pdbUtils->Load(pdbBlob.Get()))) << "IDxcPdbUtils2::Load 失败，PDB=" << pdbName;
        EXPECT_TRUE(pdbUtils->IsFullPDB()) << "PDB 仍为 slim：" << pdbName;
        EXPECT_EQ(FieldValue(manifestText, "pdbType"), "full") << manifestPath.string();
    }
}

// PDB 名必须唯一：重名会让两次编译产物互相覆盖，PIX 解析到错的源。
TEST(ShaderCompileGateTests, PdbNamesAreUniqueAcrossEntries)
{
    std::set<std::string> names;
    for (const MiniEngine::ShaderCompiler::ShaderEntry& entry : MiniEngine::ShaderCompiler::Entries())
    {
        const std::string manifestText = ReadTextFile(g_shaderDirectory / (OutputBaseName(entry) + ".manifest.json"));
        const std::string pdbName = FieldValue(manifestText, "pdbName");
        EXPECT_TRUE(names.insert(pdbName).second) << "PDB 名重复：" << pdbName << "（" << entry.source << "）";
        EXPECT_GT(std::filesystem::file_size(g_shaderDirectory / pdbName), 0U) << "PDB 为空：" << pdbName;
    }
    EXPECT_EQ(names.size(), MiniEngine::ShaderCompiler::Entries().size());
}
