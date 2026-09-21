<#
.SYNOPSIS
    进入 LumaBough 的 VS 2026 x64 开发环境，优先使用 VS 附带的 CMake。
#>
[CmdletBinding()]
param([string]$VsInstallPath = '', [string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
if (-not $VsInstallPath) {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw '请安装 VS 2026 C++ 工具，或用 -VsInstallPath 指定安装位置。' }
    $instances = @(& $vswhere -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    $VsInstallPath = $instances | Where-Object { Test-Path (Join-Path $_ 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe') } | Select-Object -First 1
    if (-not $VsInstallPath) { throw 'VS 2026 实例缺少 C++ CMake tools for Windows。' }
}
$module = Join-Path $VsInstallPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
$cmakeBin = Join-Path $VsInstallPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
Import-Module $module
Enter-VsDevShell -VsInstallPath $VsInstallPath -SkipAutomaticLocation -DevCmdArguments "-arch=$Architecture -host_arch=$Architecture" | Out-Null
$env:PATH = $cmakeBin + ';' + $env:PATH
$versionLine = & (Join-Path $cmakeBin 'cmake.exe') --version
$version = [version](($versionLine[0] -replace '^cmake version ', '') -replace '-.*$', '')
if ($version -lt [version]'4.2') { throw "需要 CMake 4.2+，当前 $version" }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$tmp = Join-Path $repo 'out/tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TEMP = $tmp
$env:TMP = $tmp
$env:PYTHONDONTWRITEBYTECODE = '1'
$clang = Join-Path $VsInstallPath 'VC/Tools/Llvm/x64/bin/clang-format.exe'
if (Test-Path -LiteralPath $clang) { $env:PATH = (Split-Path -Parent $clang) + ';' + $env:PATH }
Set-Location -LiteralPath $repo
Write-Host "LumaBough: VS 2026 / $Architecture / CMake $version"
