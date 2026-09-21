<#
.SYNOPSIS
  把本地证据目录发布到外部证据库：逐文件校验 SHA-256、写 artifact-manifest.json、维护顶层 index.json。

.DESCRIPTION
  外部库布局（<Root>\<Milestone>\<Topic>\）：
    <EvidenceStore>\
      README.md                 结构、命名、校验与保留策略
      index.json                顶层索引（miniengine.evidence-store.v1）
      M6\2026-09-16-m6-handover\{*.tar.gz, bundle-index.json, artifact-manifest.json}
      M7\2026-09-17-m7-01-serial-baseline\{..., artifact-manifest.json}

  设计约束：
  - **复制后回读校验**：源与目标的 SHA-256 必须一致，否则失败退出（不留下"看起来成功"的目录）。
  - **幂等**：同一 Milestone/Topic 重复发布会覆盖同名文件并更新 index.json 条目，不产生副本目录。
  - **可移交**：index.json 里每条都带目录、文件数、字节数、manifest 名与 manifest 自身的 SHA-256，
    远端拿到库后可以离线逐文件复核。

.EXAMPLE
  pwsh tools/evidence/publish_evidence.ps1 -DestinationRoot <evidence-store> -Source out/m7-01 -Milestone M7 -Topic 2026-09-17-m7-01-serial-baseline
.EXAMPLE
  pwsh tools/evidence/publish_evidence.ps1 -DestinationRoot <evidence-store> -Source out/artifact-bundles -Milestone M6 -Topic 2026-09-16-m6-handover -DryRun
.EXAMPLE
  # M7 实验 topic：先验预注册证据链（缺失/被改/事后补写都会在写盘前中止）
  pwsh tools/evidence/publish_evidence.ps1 -DestinationRoot <evidence-store> -Source out/submit-004 -Milestone M7 -Topic 2026-09-19-m7-submit-004 -RequirePrereg
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Milestone,
    [Parameter(Mandatory = $true)][string]$Topic,
    [Parameter(Mandatory = $true)][string]$DestinationRoot,
    [string[]]$Include = @('*'),
    [string]$Repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
    [string]$Note = '',
    [switch]$DryRun,
    # M7-PREREG-EVIDENCE（M7 纪律）：发布前先验证源目录的预注册证据链（完整性 / 未篡改 /
    # 载荷 mtime 早于目录内全部其他文件）。失败**在写任何目标文件之前**中止。
    # 对没有实验预注册的 topic（例如 M6 移交包）保持可选，默认不启用。
    [switch]$RequirePrereg,
    # 允许测试/CI 覆盖校验器路径；默认 <Repository>\tools\evidence\check_m7_prereg.py。
    [string]$PreregChecker = ''
)

$ErrorActionPreference = 'Stop'
# Python/其他 PowerShell 宿主可能传入不同版本的 PSModulePath；从本进程 PSHOME 加载。
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop

Set-StrictMode -Version Latest

function Get-ManifestEntry {
    param([string]$Path, [string]$RelativePath)
    return [ordered]@{
        path   = $RelativePath.Replace('\', '/')
        bytes  = (Get-Item -LiteralPath $Path).Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

$sourceFull = (Resolve-Path -LiteralPath $Source).Path
$destination = Join-Path (Join-Path $DestinationRoot $Milestone) $Topic
$manifestPath = Join-Path $destination 'artifact-manifest.json'
$indexPath = Join-Path $DestinationRoot 'index.json'

# 0) 预注册门（可选）：在收集/复制任何文件之前验证证据链，失败即中止。
#    "忘了预注册 / 事后补写"在这里被挡住，而不是靠发布者自觉。
if ($RequirePrereg) {
    $checker = if ($PreregChecker) { $PreregChecker } else { Join-Path (Join-Path $Repository 'tools/evidence') 'check_m7_prereg.py' }
    if (-not (Test-Path -LiteralPath $checker)) {
        throw "-RequirePrereg: checker not found at $checker"
    }
    $interpreter = Get-Command python -ErrorAction SilentlyContinue
    $interpreterArgs = @()
    if (-not $interpreter) {
        $interpreter = Get-Command py -ErrorAction SilentlyContinue
        $interpreterArgs = @('-3')
    }
    if (-not $interpreter) {
        throw "-RequirePrereg: no python interpreter on PATH (tried python, py)"
    }
    $checkOutput = & $interpreter.Source @interpreterArgs $checker --dir $sourceFull 2>&1
    $checkCode = $LASTEXITCODE
    if ($checkCode -ne 0) {
        throw ("pre-registration gate FAILED (exit {0}); publish aborted before writing anything:`n{1}" -f
            $checkCode, ($checkOutput -join "`n"))
    }
    Write-Host ("pre-registration gate OK: {0}" -f (($checkOutput | Select-Object -Last 1) -join ''))
}

# 1) 收集源文件：按相对路径匹配 Include glob，跳过明显的临时文件。
$files = Get-ChildItem -LiteralPath $sourceFull -Recurse -File |
    Where-Object { $_.Name -notlike '*.tmp' -and $_.Name -notlike '*.partial' -and $_.Name -ne 'artifact-manifest.json' }
$selected = foreach ($file in $files) {
    $relative = $file.FullName.Substring($sourceFull.Length).TrimStart('\')
    foreach ($pattern in $Include) {
        if ($relative -like $pattern -or $file.Name -like $pattern) { $relative; break }
    }
}
if (-not $selected) { throw "no files selected under $sourceFull for include patterns: $($Include -join ', ')" }

Write-Host ("publish {0} file(s): {1} -> {2}" -f @($selected).Count, $sourceFull, $destination)
if ($DryRun) {
    $selected | Select-Object -First 8 | ForEach-Object { "  (dry-run) $_" }
    return
}

New-Item -ItemType Directory -Force -Path $destination | Out-Null

# 2) 复制并回读校验（源哈希 → 目标哈希必须一致）。
$entries = New-Object System.Collections.Generic.List[object]
$totalBytes = 0L
foreach ($relative in $selected) {
    $sourcePath = Join-Path $sourceFull $relative
    $targetPath = Join-Path $destination $relative
    $targetDirectory = Split-Path -Parent $targetPath
    if ($targetDirectory) { New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null }
    Copy-Item -LiteralPath $sourcePath -Destination $targetPath -Force

    $entry = Get-ManifestEntry -Path $sourcePath -RelativePath $relative
    $copied = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
    if ($copied -ne $entry.sha256) {
        throw "copy verification failed for $relative (source $($entry.sha256) vs destination $copied)"
    }
    $entries.Add($entry)
    $totalBytes += [int64]$entry.bytes
}

# 3) 写 artifact-manifest.json（含来源仓库 commit / dirty 状态）。
#    注意：ConvertTo-Json 会把单元素数组折叠成对象，因此数组一律手工拼接，
#    保证 files/entries 永远是 JSON 数组（schema 稳定，与元素个数无关）。
$commit = (& git -C $Repository rev-parse --verify --quiet HEAD)
$dirtyCount = @(& git -C $Repository status --porcelain=v1 2>$null).Count
$commitValue = $null
if ($commit) { $commitValue = $commit.Trim() }
$publishedUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$quote = { param([string]$value) return ($value | ConvertTo-Json -Compress) }

$fileLines = foreach ($entry in $entries) { '    ' + ($entry | ConvertTo-Json -Compress -Depth 4) }
$manifestText = @(
    '{'
    '  "schema": "miniengine.evidence-artifact-manifest.v1",'
    '  "milestone": ' + (& $quote $Milestone) + ','
    '  "topic": ' + (& $quote $Topic) + ','
    '  "publishedUtc": ' + (& $quote $publishedUtc) + ','
    '  "source": ' + (& $quote $sourceFull) + ','
    '  "repository": ' + (& $quote $Repository) + ','
    '  "sourceCommit": ' + (& $quote $commitValue) + ','
    '  "sourceDirtyFiles": ' + $dirtyCount + ','
    '  "fileCount": ' + $entries.Count + ','
    '  "totalBytes": ' + $totalBytes + ','
    '  "note": ' + (& $quote $Note) + ','
    '  "files": ['
    ($fileLines -join ",`r`n")
    '  ]'
    '}'
) -join "`r`n"
Set-Content -LiteralPath $manifestPath -Value $manifestText -Encoding UTF8
$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash

# 4) 更新顶层 index.json（按 <Milestone>/<Topic> 键去重，稳定排序）。
$store = $null
if (Test-Path -LiteralPath $indexPath) {
    $store = Get-Content -LiteralPath $indexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($store.schema -ne 'miniengine.evidence-store.v1') { throw "unexpected index schema: $($store.schema)" }
}
$key = "$Milestone/$Topic"
$kept = @()
if ($store) { $kept = @($store.entries | Where-Object { "$($_.milestone)/$($_.topic)" -ne $key }) }
$entry = [ordered]@{
    key          = $key
    milestone    = $Milestone
    topic        = $Topic
    directory    = "$Milestone/$Topic"
    publishedUtc = $publishedUtc
    fileCount    = $entries.Count
    totalBytes   = $totalBytes
    manifest     = "$Milestone/$Topic/artifact-manifest.json"
    manifestSha256 = $manifestHash
    sourceCommit = $commitValue
    sourceDirtyFiles = $dirtyCount
    note         = $Note
}
$ordered = @((@($kept) + $entry) | Sort-Object key)
$entryLines = foreach ($item in $ordered) { '    ' + ($item | ConvertTo-Json -Compress -Depth 5) }
$indexText = @(
    '{'
    '  "schema": "miniengine.evidence-store.v1",'
    '  "root": ' + (& $quote $DestinationRoot) + ','
    '  "updatedUtc": ' + (& $quote $publishedUtc) + ','
    '  "entries": ['
    ($entryLines -join ",`r`n")
    '  ]'
    '}'
) -join "`r`n"
Set-Content -LiteralPath $indexPath -Value $indexText -Encoding UTF8

Write-Host ("published {0} files, {1:N2} MB; manifest SHA-256 {2}" -f $entries.Count, ($totalBytes / 1MB), $manifestHash)
Write-Host ("index: {0} ({1} entries)" -f $indexPath, $ordered.Count)
