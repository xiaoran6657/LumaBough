// ============================================================================
// main.cpp — MiniEngineAssetCooker 的命令行入口
// 里程碑：M3-04
// 职责：解析四个命令（validate / cook / inspect / watch）的参数并做逐命令的
//       必填/禁用项校验，失败打印用法并返回退出码 2；成功则把参数原样交给
//       CookSession::Run，其余退出码由 CookSession 定义。
// 关联：tools/asset_cooker/src/CookSession.h（命令语义与退出码）
// ============================================================================

#include "CookSession.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
struct ParsedArguments final
{
    std::string_view command;
    // 以下选项每个只允许出现一次；重复出现或出现该命令不允许的选项即解析失败。
    std::filesystem::path sourceRoot;
    std::filesystem::path recipe;
    std::filesystem::path recipeRoot;
    std::filesystem::path report;
    std::filesystem::path outputRoot;
    std::filesystem::path artifact;
    std::filesystem::path validator;
    std::string profile;
    bool deterministic{};
};

// 打印四个命令的用法（stderr）；伴随退出码 2 输出。
void PrintUsage()
{
    std::cerr
        << "MiniEngineAssetCooker validate --source-root <path> --recipe <file> --report <file> "
           "[--validator <exe>]\n"
        << "MiniEngineAssetCooker cook --source-root <path> --recipe-root <path> "
           "--output <path> --profile <name> [--deterministic] [--validator <exe>]\n"
        << "MiniEngineAssetCooker inspect --artifact <file>\n"
        << "MiniEngineAssetCooker watch --source-root <path> --recipe-root <path> "
           "--output <path> --profile <name> [--validator <exe>]\n";
}

// 解析命令行。除逐选项的"只允许一次"检查外，还按命令校验必填/禁用项：
//   validate → 必填 source-root/recipe/report；
//   inspect  → 必填 artifact；
//   cook     → 必填 source-root/recipe-root/output/profile（--deterministic 可选）；
//   watch    → 同 cook，但不接受 --deterministic。
//   各命令同时禁用不属于自己的选项（例：inspect 不接受 --validator）。
// 返回：命令与选项组合合法为 true；未知选项、重复选项或必填缺失为 false。
bool ParseArguments(const int argc, char** argv, ParsedArguments& result)
{
    if (argc < 2)
    {
        return false;
    }
    result.command = argv[1];

    for (int index = 2; index < argc; ++index)
    {
        const std::string_view option = argv[index];
        if (option == "--deterministic")
        {
            if (result.deterministic)
            {
                return false;
            }
            result.deterministic = true;
            continue;
        }

        if (index + 1 >= argc)
        {
            return false;
        }
        const std::string_view value = argv[++index];

        if (option == "--source-root")
        {
            if (!result.sourceRoot.empty())
            {
                return false;
            }
            result.sourceRoot = value;
        }
        else if (option == "--recipe")
        {
            if (!result.recipe.empty())
            {
                return false;
            }
            result.recipe = value;
        }
        else if (option == "--recipe-root")
        {
            if (!result.recipeRoot.empty())
            {
                return false;
            }
            result.recipeRoot = value;
        }
        else if (option == "--report")
        {
            if (!result.report.empty())
            {
                return false;
            }
            result.report = value;
        }
        else if (option == "--output")
        {
            if (!result.outputRoot.empty())
            {
                return false;
            }
            result.outputRoot = value;
        }
        else if (option == "--artifact")
        {
            if (!result.artifact.empty())
            {
                return false;
            }
            result.artifact = value;
        }
        else if (option == "--validator")
        {
            if (!result.validator.empty())
            {
                return false;
            }
            result.validator = value;
        }
        else if (option == "--profile")
        {
            if (!result.profile.empty())
            {
                return false;
            }
            result.profile = value;
        }
        else
        {
            return false;
        }
    }

    if (result.command == "validate")
    {
        return !result.sourceRoot.empty() && !result.recipe.empty() && !result.report.empty() &&
               result.recipeRoot.empty() && result.outputRoot.empty() && result.artifact.empty() &&
               result.profile.empty() && !result.deterministic;
    }

    if (result.command == "inspect")
    {
        return !result.artifact.empty() && result.sourceRoot.empty() && result.recipe.empty() &&
               result.recipeRoot.empty() && result.report.empty() && result.outputRoot.empty() &&
               result.profile.empty() && !result.deterministic && result.validator.empty();
    }

    if (result.command == "cook" || result.command == "watch")
    {
        return !result.sourceRoot.empty() && !result.recipeRoot.empty() && !result.outputRoot.empty() &&
               !result.profile.empty() && result.recipe.empty() && result.report.empty() &&
               result.artifact.empty() && (result.command == "cook" || !result.deterministic);
    }

    return false;
}
} // namespace

// 入口：解析参数 → 组装 CookCommandLine → 交给 CookSession::Run。
// 退出码：2 = 用法/参数错误（本文件）；其余（0/3/4/6/7…）由 CookSession 定义。
int main(const int argc, char** argv)
{
    ParsedArguments parsed{};
    if (!ParseArguments(argc, argv, parsed))
    {
        PrintUsage();
        return 2;
    }

    MiniEngine::Tools::CookCommandLine commandLine;
    commandLine.command = std::string{parsed.command};
    commandLine.sourceRoot = parsed.sourceRoot;
    commandLine.recipe = parsed.recipe;
    commandLine.recipeRoot = parsed.recipeRoot;
    commandLine.report = parsed.report;
    commandLine.outputRoot = parsed.outputRoot;
    commandLine.artifact = parsed.artifact;
    commandLine.validator = parsed.validator;
    commandLine.profile = parsed.profile;
    commandLine.deterministic = parsed.deterministic;

    return MiniEngine::Tools::CookSession{std::move(commandLine)}.Run();
}
