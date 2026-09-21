// ============================================================================
// SourceUriTests.cpp — glTF URI 沙箱的七步校验契约
// 里程碑：M3（02 篇 G1）
// 职责：验证允许/拒绝清单（相对 URI、percent 编码、data: URI、scheme/绝对路径/
//       反斜杠/逃逸/兄弟前缀/reparse point/目录），以及拒绝原因名的稳定性。
// 关联：tools/asset_cooker/src/SourceUri.cpp（被测实现）
//       docs/architecture/README.md 第 5 节
// ============================================================================

#include "SourceUri.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using MiniEngine::Tools::ResolvedSourceUri;
using MiniEngine::Tools::UriRejectReason;

std::uint32_t g_dirCounter = 0;

// 进程级基址 + 计数器：gtest_discover_tests 每用例独立进程，避免 temp 撞名。
std::filesystem::path MakeSourceRoot()
{
    static const std::uint64_t processBase =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    ++g_dirCounter;
    const auto root = std::filesystem::temp_directory_path() /
                      ("MiniEngineSourceUriTests-" + std::to_string(processBase) + "-" + std::to_string(g_dirCounter));
    std::filesystem::create_directories(root / "demo");
    return root;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

struct ResolveResult final
{
    bool ok{};
    UriRejectReason reason{};
    std::string error;
    ResolvedSourceUri resolved{};
};

// sourceDirectory 是 glTF 所在目录（root/demo），sourceRoot 是 root。
ResolveResult Resolve(const std::filesystem::path& root, const std::string& uri)
{
    ResolveResult result;
    result.ok =
        MiniEngine::Tools::ResolveSourceUri(uri, root / "demo", root, result.resolved, result.reason, result.error);
    return result;
}
} // namespace

// 沙箱内相对 URI：解析成功并输出规范相对路径。
TEST(SourceUriTests, AcceptsRelativeUriInsideSourceRoot)
{
    const auto root = MakeSourceRoot();
    ASSERT_TRUE(WriteFile(root / "demo" / "scene.bin", "data"));

    const auto result = Resolve(root, "scene.bin");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.resolved.normalizedRelativePath, "demo/scene.bin");
    EXPECT_FALSE(result.resolved.isDataUri);
}

// ".." 逃出 source-root 即拒绝。
TEST(SourceUriTests, RejectsTraversalOutsideRoot)
{
    const auto root = MakeSourceRoot();
    ASSERT_TRUE(WriteFile(root / ".." / "MiniEngineSourceUriTests-outside.bin", "data"));

    const auto result = Resolve(root, "../../MiniEngineSourceUriTests-outside.bin");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.reason, UriRejectReason::EscapesSourceRoot);
    EXPECT_FALSE(result.error.empty());
}

// 兄弟前缀目录（root2 vs root）不得因字符串前缀相同而误判为沙箱内。
TEST(SourceUriTests, RejectsSiblingPrefixDirectoryOutsideRoot)
{
    const auto root = MakeSourceRoot();
    // 兄弟目录 <root>2：按路径组件比较，不能把它的内容误判为 root 内。
    const auto sibling = std::filesystem::path{root.string() + "2"};
    ASSERT_TRUE(WriteFile(sibling / "demo" / "scene.bin", "data"));

    const auto result = Resolve(root, "../../" + sibling.filename().string() + "/demo/scene.bin");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.reason, UriRejectReason::Ok);
}

// scheme / 绝对路径 / 盘符 / '\' 分隔符一律拒绝（glTF 契约要求 '/'）。
TEST(SourceUriTests, RejectsSchemesAbsolutePathsAndBackslashes)
{
    const auto root = MakeSourceRoot();
    ASSERT_TRUE(WriteFile(root / "demo" / "scene.bin", "data"));

    EXPECT_FALSE(Resolve(root, "https://example.com/scene.bin").ok);
    EXPECT_FALSE(Resolve(root, "file:///demo/scene.bin").ok);
    EXPECT_FALSE(Resolve(root, "/demo/scene.bin").ok);
    EXPECT_FALSE(Resolve(root, "C:/demo/scene.bin").ok);
    EXPECT_FALSE(Resolve(root, "\\\\server\\share\\scene.bin").ok);
    EXPECT_FALSE(Resolve(root, "demo\\scene.bin").ok);
    EXPECT_FALSE(Resolve(root, "").ok);
}

// 非法 percent 编码与解码出的控制字符拒绝，防双重编码绕过。
TEST(SourceUriTests, RejectsInvalidPercentEncoding)
{
    const auto root = MakeSourceRoot();
    EXPECT_FALSE(Resolve(root, "scene%ZZ.bin").ok);
    EXPECT_FALSE(Resolve(root, "scene%4.bin").ok);
    EXPECT_FALSE(Resolve(root, "scene%00.bin").ok);
}

// 合法 percent 编码（如 %2f）解码后仍在沙箱内的路径被接受。
TEST(SourceUriTests, AcceptsPercentEncodedSeparatorStillInsideRoot)
{
    const auto root = MakeSourceRoot();
    ASSERT_TRUE(WriteFile(root / "demo" / "nested" / "scene.bin", "data"));

    // %2F 解码为 '/'：组合后仍在 root 内 → 允许（包含性在解码之后校验）。
    const auto result = Resolve(root, "nested%2Fscene.bin");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.resolved.normalizedRelativePath, "demo/nested/scene.bin");
}

// data: URI 仅识别不访问文件系统（内容由 source glTF hash 覆盖）。
TEST(SourceUriTests, DataUriIsRecognizedWithoutFilesystemAccess)
{
    const auto root = MakeSourceRoot();
    const auto result = Resolve(root, "data:application/octet-stream;base64,AAAA");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.resolved.isDataUri);
    EXPECT_TRUE(result.resolved.absolutePath.empty());
}

// 目标不存在拒绝（MissingFile），且发生在全部沙箱校验之后。
TEST(SourceUriTests, MissingFileIsRejected)
{
    const auto root = MakeSourceRoot();
    const auto result = Resolve(root, "missing.bin");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.reason, UriRejectReason::MissingFile);
}

// 目标是目录而非常规文件即拒绝。
TEST(SourceUriTests, DirectoryTargetIsRejected)
{
    const auto root = MakeSourceRoot();
    std::error_code createError;
    std::filesystem::create_directories(root / "demo" / "adir", createError);

    const auto result = Resolve(root, "adir");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.reason, UriRejectReason::NotARegularFile);
}

// 拒绝原因短名稳定可读（日志与 exit code 诊断依赖它）。
TEST(SourceUriTests, UriRejectReasonNamesAreStable)
{
    EXPECT_STREQ(MiniEngine::Tools::UriRejectReasonName(UriRejectReason::Ok), "Ok");
    EXPECT_STREQ(MiniEngine::Tools::UriRejectReasonName(UriRejectReason::EscapesSourceRoot), "EscapesSourceRoot");
    EXPECT_STREQ(MiniEngine::Tools::UriRejectReasonName(UriRejectReason::ReparsePoint), "ReparsePoint");
}
