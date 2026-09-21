<#
.SYNOPSIS
    格式化并校验 MiniEngine 自有 C++ 源文件。

.DESCRIPTION
    在已加载的 MiniEngine 开发者环境中运行：用 git ls-files 列出 engine/samples/tests
    下的 .h/.hpp/.cpp（排除 out/ 等被忽略目录），先用 clang-format -i 就地格式化，
    再用 --dry-run --Werror 校验。不处理 out/ 中的 FetchContent 或生成文件。

    优先使用 ripgrep（rg），若未安装则回退到 git ls-files + 后缀过滤。
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Get-SourceFiles
{
    $extensions = @('.h', '.hpp', '.cpp')

    $rg = Get-Command rg -ErrorAction SilentlyContinue
    if ($null -ne $rg)
    {
        return (& $rg --files engine samples tests -g '*.h' -g '*.hpp' -g '*.cpp') | ForEach-Object { $_.Trim() }
    }

    # 回退：git ls-files 已遵守 .gitignore，天然排除 out/ 等构建目录
    $tracked = git -C (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) ls-files engine samples tests
    $all = git -C (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) ls-files --others --exclude-standard engine samples tests
    $candidates = $tracked + $all
    return $candidates | ForEach-Object { $_.Trim() } | Where-Object {
        $_.Length -gt 0 -and
        ($extensions -contains [System.IO.Path]::GetExtension($_).ToLowerInvariant())
    }
}

$sourceFiles = @(Get-SourceFiles)
if ($sourceFiles.Count -eq 0)
{
    Write-Host '未找到需要处理 C++ 源文件。' -ForegroundColor Yellow
    return
}
$sourceFiles

Write-Host '== FORMAT ==' -ForegroundColor Cyan
clang-format -i @sourceFiles

Write-Host '== VERIFY ==' -ForegroundColor Cyan
clang-format --dry-run --Werror @sourceFiles
if ($LASTEXITCODE -ne 0)
{
    throw "clang-format 校验失败（退出码 $LASTEXITCODE）。"
}

Write-Host 'OK: 所有源文件格式一致。' -ForegroundColor Green
