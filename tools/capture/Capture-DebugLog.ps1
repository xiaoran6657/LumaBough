# 退役说明（2026-09-16，LEGACY-SAMPLES-RETIRE）：本脚本定位的 samples\sandbox 旧 concrete
# 入口已删除并移出构建。它是里程碑验收的历史工具，保留供追溯；如需重跑，先从 git
# 历史（9fb65c1 之前）恢复目录，或迁移到统一 sandbox（samples/rhi_sandbox/MiniEngineSandbox.exe）。
<#
.SYNOPSIS
    通用 OutputDebugString 采集：启动/等待一个场景并用 DbgViewCli 有界记录日志，可选断言。

.DESCRIPTION
    任意“启动进程 → 让它跑 N 秒 → 关掉 → 拿日志分析”的任务都可复用：
      - 不指定 -Application：只做有界采集（场景/进程由调用方自己起，agent 并行跑）；
      - 指定 -Application：脚本负责启动（可带参数/环境变量），到时关闭；
      - -RequireLine / -ForbidLine：可选断言（marker），命中判断成败。

.PARAMETER Application
    要启动并监视的可执行文件（绝对路径）。不指定则只采集当前场景。
.PARAMETER ApplicationArgument
    传给 Application 的参数数组。
.PARAMETER Environment
    传给 Application 的环境变量哈希表（如 @{ MINIENGINE_MANIFEST = '...' }）。
.PARAMETER ProcessFilter
    dbgviewcli 的进程过滤；缺省取 Application 文件名（去掉 .exe）。仅采集全系统时置空。
.PARAMETER Seconds
    采集时长（秒）。默认 15。
.PARAMETER DbgViewCli
    dbgviewcli.exe 路径。默认从 PATH 查找 dbgviewcli64.exe（可显式指定路径，
    目录内还有 dbgviewcli64/dbgviewcli64a 变体，均以 Authenticode 校验为准）。
.PARAMETER LogFile
    日志输出文件；默认 <仓库>\out\logs\debugview-<时间戳>.log。
.PARAMETER RequireLine
    必须出现的子串（数组，任一命中即满足）。
.PARAMETER ForbidLine
    禁止出现的子串（数组，任一命中即失败）。

.EXAMPLE
    # 通用：启动任意程序并采集，断言出现 SUCCESS、不出现 FATAL
    tools\capture\Capture-DebugLog.ps1 -Application "C:\tools\demo.exe" -Seconds 10 `
        -RequireLine "SUCCESS" -ForbidLine "FATAL"

.EXAMPLE
    # Sandbox HR 用例（带 manifest 环境变量）
    tools\capture\Capture-DebugLog.ps1 `
        -Application "out/build/windows-msvc-debug/samples/rhi_sandbox/Debug/MiniEngineSandbox.exe" `
        -Environment @{ MINIENGINE_MANIFEST = "out/demo/scene/manifest.json" } `
        -Seconds 8 -RequireLine "baked scene loaded" -ForbidLine "failed to load baked scene"
#>
[CmdletBinding()]
param(
    [string]$Application = '',
    [string[]]$ApplicationArgument = @(),
    [hashtable]$Environment = @{},
    [string]$ProcessFilter = '',
    [int]$Seconds = 15,
    # DBWIN 采集有位数限制：x86 dbgviewcli 收不到 x64 进程输出。
    # 64 位变体可采 32/64 位，故默认 dbgviewcli64；采集纯 32 位进程时可显式传 dbgviewcli.exe。
    [string]$DbgViewCli = 'dbgviewcli64.exe',
    [string]$LogFile = '',
    [string[]]$RequireLine = @(),
    [string[]]$ForbidLine = @()
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command $DbgViewCli -ErrorAction SilentlyContinue)) {
    Write-Warning "dbgviewcli.exe not found at $DbgViewCli. 把 DebugView CLI 加入 PATH 或显式传入 -DbgViewCli 后重试（官方 Sysinternals 渠道获取）。"
    exit 3
}

$DbgViewCli = (Get-Command $DbgViewCli -ErrorAction Stop).Source

# 安全守则：只接受 Microsoft 签名的 dbgviewcli。
$signature = Get-AuthenticodeSignature $DbgViewCli
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft') {
    throw "DbgViewCli 签名校验未通过（Status=$($signature.Status)），拒绝执行。"
}

# DBWIN 是单占用者：先清理遗留的 dbgviewcli 实例（不误杀 DebugView GUI），
# 避免新实例报 "Could not initialize Win32 debug capture"。
Get-Process -Name 'dbgviewcli', 'dbgviewcli64', 'dbgviewcli64a' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

if ([string]::IsNullOrWhiteSpace($LogFile)) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    New-Item -ItemType Directory -Force (Join-Path $repo 'out\logs') | Out-Null
    $LogFile = Join-Path $repo "out\logs\debugview-$stamp.log"
}

if ([string]::IsNullOrWhiteSpace($ProcessFilter) -and -not [string]::IsNullOrWhiteSpace($Application)) {
    $ProcessFilter = [System.IO.Path]::GetFileNameWithoutExtension($Application)
}

$cliArgs = @('--no-banner')
if (-not [string]::IsNullOrWhiteSpace($ProcessFilter)) {
    $cliArgs += @('--process-filter', $ProcessFilter)
}
$cliArgs += @('--duration', "$Seconds", '--log', $LogFile)

$procCli = Start-Process -FilePath $DbgViewCli -ArgumentList $cliArgs -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500

$procApp = $null
$savedEnv = @{}
if (-not [string]::IsNullOrWhiteSpace($Application)) {
    foreach ($key in $Environment.Keys) {
        $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key)
        [Environment]::SetEnvironmentVariable($key, [string]$Environment[$key])
    }
    if ($ApplicationArgument.Count -gt 0) {
        $procApp = Start-Process -FilePath $Application -ArgumentList $ApplicationArgument -PassThru
    }
    else {
        $procApp = Start-Process -FilePath $Application -PassThru
    }
}

Start-Sleep -Seconds $Seconds

if ($null -ne $procApp -and -not $procApp.HasExited) {
    Stop-Process -Id $procApp.Id -Force
}
foreach ($key in $savedEnv.Keys) {
    [Environment]::SetEnvironmentVariable($key, $savedEnv[$key])
}
Start-Sleep -Seconds 2
if (-not $procCli.HasExited) {
    Stop-Process -Id $procCli.Id -Force
}

if (-not (Test-Path $LogFile)) {
    throw "capture log not produced: $LogFile"
}
$content = [string](Get-Content $LogFile -Raw)
Write-Host "log: $LogFile"
Write-Host "bytes: $((Get-Item $LogFile).Length)"

$missingRequired = @()
foreach ($pattern in $RequireLine) {
    if ($content -notlike "*$pattern*") { $missingRequired += $pattern }
}
$forbiddenHit = @()
foreach ($pattern in $ForbidLine) {
    if ($content -like "*$pattern*") { $forbiddenHit += $pattern }
}

Write-Host "require-missing: $($missingRequired -join ', ')"
Write-Host "forbidden-hit:   $($forbiddenHit -join ', ')"

if ($missingRequired.Count -gt 0 -or $forbiddenHit.Count -gt 0) {
    Write-Host '--- 日志尾部 ---'
    Get-Content $LogFile -Tail 20
    exit 1
}
Write-Host '采集完成。'
exit 0
