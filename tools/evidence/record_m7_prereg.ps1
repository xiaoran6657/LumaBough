# ============================================================================
# M7 预注册记录器（M7-PREREG-EVIDENCE）
#
# 问题：账本里的"预注册"段落只是自述——读者无法验证阈值/方向文本写于采集之前
# （M7-11 审计：E-M7-METHOD-001 的协议可证先于数据，但阈值文本首次提交晚于采集）。
#
# 做法：把预注册**载荷**（作者手写的 JSON：假设/目标/方向/阈值/protected/计划）
# 与**盖章**（时间戳 + HEAD + 工作区脏文件 + 可执行文件 SHA-256 + 载荷 SHA-256）
# 落成两个文件，放进实验的 raw 目录随 topic 一起发布：
#   <dir>/prereg-payload.json   作者原文（逐字节哈希，之后任何修改都会被校验器抓到）
#   <dir>/prereg.json           盖章（含 payloadSha256 / sourceCommit / recordedUtc）
#
# 之后由 tools/evidence/check_m7_prereg.py 验证"预注册早于数据"（载荷 mtime 早于全部数据文件）
# 且载荷未被改动。**本工具必须在采集之前调用**——它是把纪律变成证据的那一步。
#
# 用法：
#   pwsh tools/evidence/record_m7_prereg.ps1 `
#     -ExperimentId E-M7-STRESS-001 `
#     -PayloadPath out/stress-001/prereg-payload.json `
#     -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe
# ============================================================================
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExperimentId,
    # 作者手写的预注册载荷（JSON）。必填字段见下方 $requiredFields。
    [Parameter(Mandatory = $true)][string]$PayloadPath,
    # 盖章与载荷的落盘目录；默认取载荷所在目录。
    [string]$OutputDirectory = '',
    # 可选：把当次实验使用的可执行文件 SHA-256 盖进预注册（实现变体 A/B 时需要）。
    [string]$Executable = '',
    [string]$Repository = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Repository) { $Repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$payloadFull = [System.IO.Path]::GetFullPath($PayloadPath)
if (-not (Test-Path -LiteralPath $payloadFull)) { throw "payload not found: $payloadFull" }
$output = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Split-Path -Parent $payloadFull }
[System.IO.Directory]::CreateDirectory($output) | Out-Null

# 载荷schema：字段缺失 = 预注册不完整，拒绝盖章（宁可在采集前失败）。
$payload = Get-Content -LiteralPath $payloadFull -Raw -Encoding UTF8 | ConvertFrom-Json
$requiredFields = @('experimentId', 'hypothesis', 'targetMetric', 'expectedDirection', 'threshold',
    'protectedMetrics', 'plan', 'plannedRuns', 'decisionRule')
$missing = @()
foreach ($field in $requiredFields) {
    if ($null -eq $payload.$field -or ($payload.$field -is [string] -and [string]::IsNullOrWhiteSpace($payload.$field))) {
        $missing += $field
    }
}
if ($missing.Count -gt 0) {
    throw ("pre-registration payload is incomplete, missing: {0}" -f ($missing -join ', '))
}
if ($payload.experimentId -ne $ExperimentId) {
    throw ("payload experimentId '{0}' does not match -ExperimentId '{1}'" -f $payload.experimentId, $ExperimentId)
}
# 允许 'post-hoc' 显式标注：那是"承认不满足纪律"，不是"满足纪律"。
if ($payload.status -and $payload.status -ne 'PRE-REGISTERED' -and $payload.status -ne 'post-hoc') {
    throw "payload.status must be PRE-REGISTERED or post-hoc"
}

$payloadBytes = (Get-Item -LiteralPath $payloadFull).Length
$payloadSha = (Get-FileHash -LiteralPath $payloadFull -Algorithm SHA256).Hash

$commit = (& git -C $Repository rev-parse HEAD 2>$null)
if (-not $commit) { throw "cannot resolve HEAD in $Repository" }
$dirty = @(& git -C $Repository status --porcelain 2>$null | ForEach-Object { $_.ToString() })
$executableSha = ''
$executablePath = ''
if ($Executable) {
    $executablePath = (Resolve-Path -LiteralPath $Executable).Path
    $executableSha = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
}

$recordedUtc = (Get-Date).ToUniversalTime()
$prereg = [ordered]@{
    schema           = 1
    experimentId     = $ExperimentId
    status           = if ($payload.status) { $payload.status } else { 'PRE-REGISTERED' }
    recordedUtc      = $recordedUtc.ToString('yyyy-MM-ddTHH:mm:ssZ')
    recordedLocal    = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz')
    payloadFile      = [System.IO.Path]::GetFileName($payloadFull)
    payloadBytes     = $payloadBytes
    payloadSha256    = $payloadSha
    sourceCommit     = $commit.ToString().Trim()
    sourceDirtyFiles = $dirty
    executable       = $executablePath
    executableSha256 = $executableSha
    host             = $env:COMPUTERNAME
    tool             = 'tools/evidence/record_m7_prereg.ps1'
}
$preregPath = Join-Path $output 'prereg.json'
[System.IO.File]::WriteAllText($preregPath, (($prereg | ConvertTo-Json -Depth 5) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# 账本里可直接粘贴的一行（含载荷哈希，读者可核对即将发布的那份载荷）。
Write-Host ("prereg recorded: {0}" -f $preregPath)
Write-Host ("  payload            : {0} ({1} bytes)" -f $payloadFull, $payloadBytes)
Write-Host ("  payloadSha256      : {0}" -f $payloadSha)
Write-Host ("  recordedUtc        : {0}" -f $prereg.recordedUtc)
Write-Host ("  sourceCommit       : {0}" -f $prereg.sourceCommit)
Write-Host ("  sourceDirtyFiles   : {0}" -f $dirty.Count)
if ($executableSha) { Write-Host ("  executableSha256   : {0}" -f $executableSha) }
Write-Host "  下一步：现在采集数据（顺序由 tools/evidence/check_m7_prereg.py 在发布前后验证）。"
