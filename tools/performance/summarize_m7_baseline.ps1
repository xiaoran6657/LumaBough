# ============================================================================
# summarize_m7_baseline.ps1 — 从 raw JSON 汇总串行基线（M7-01 第 6 步噪声报告）
#
# 只读 raw run JSON（out/ 下），产出：
#   * <OutputDirectory>/serial-baseline-summary.json  机读摘要（含 raw 文件 SHA-256）
#   * <OutputDirectory>/noise-report.md               人读噪声报告
#
# 统计口径与 compare_m7_results.ps1 一致：每 run 内 median/p95/p99/hitch；
# run 间取 median-of-medians，噪声下限 = max(3%, 2 × relativeMAD)。
# 同时校验同一 cell 内身份哈希一致（executable/scene/camera/stream/packet/graph/
# command/screenshot），不一致即报错——哈希不一致说明控制变量已漂移。
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$RawDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { throw 'empty sample set' }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-Mad {
    param([double[]]$Values, [double]$Median)
    [double[]]$deviations = @($Values | ForEach-Object { [Math]::Abs($_ - $Median) })
    return Get-Median $deviations
}

function Get-Quantile {
    param([double[]]$Values, [double]$Q)
    $sorted = @($Values | Sort-Object)
    $index = $Q * ($sorted.Count - 1)
    $lower = [int][Math]::Floor($index)
    $upper = [int][Math]::Ceiling($index)
    $fraction = $index - $lower
    return [double]$sorted[$lower] * (1.0 - $fraction) + [double]$sorted[$upper] * $fraction
}

$raw = (Resolve-Path -LiteralPath $RawDirectory).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($output) | Out-Null

# 只接受 run 命名（<scene>-<rhi>-w<workers>-c<chunk>-r<run>.json），避免把
# run-notes.json 一类伴随文件当成 run。
$files = Get-ChildItem -LiteralPath $raw -Filter '*.json' -File |
    Where-Object { $_.Name -match '-w\d+-c\d+-r\d+\.json$' }
if ($files.Count -eq 0) { throw "no raw run JSON under $raw" }

$cells = @{}
foreach ($file in $files) {
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    $data = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if ($data.correctness.status -ne 'PASS') { throw "correctness not PASS: $($file.Name)" }
    if (@($data.samples).Count -ne [int]$data.measuredFrames) { throw "sample count mismatch: $($file.Name)" }
    $key = '{0}|{1}' -f $data.sceneName, $data.rhi
    if (-not $cells.ContainsKey($key)) { $cells[$key] = [System.Collections.Generic.List[object]]::new() }
    $cells[$key].Add([pscustomobject]@{ File = $file.Name; Sha256 = $hash; Data = $data })
}

$identityFields = @('executableSha256', 'sceneManifestSha256', 'cameraPathSha256', 'streamScriptSha256', 'loaderMode',
                    'renderDrawLimit', 'shadowDrawLimit', 'warmupFrames', 'measuredFrames', 'seed')
$rows = [System.Collections.Generic.List[object]]::new()

foreach ($key in @($cells.Keys | Sort-Object)) {
    $runs = @($cells[$key])
    $first = $runs[0].Data
    foreach ($run in $runs) {
        foreach ($field in $identityFields) {
            if ("$($run.Data.$field)" -ne "$($first.$field)") {
                throw "identity drift in $key : $field ('$($first.$field)' vs '$($run.Data.$field)')"
            }
        }
        if ($run.Data.correctness.packetSequenceHash -ne $first.correctness.packetSequenceHash) {
            throw "packetSequenceHash drift in $key ($($run.File))"
        }
        if ($run.Data.correctness.screenshotHash -ne $first.correctness.screenshotHash) {
            throw "screenshotHash drift in $key ($($run.File))"
        }
        if ($run.Data.correctness.commandHash -ne $first.correctness.commandHash) {
            throw "commandHash drift in $key ($($run.File))"
        }
        if ($run.Data.correctness.graphHash -ne $first.correctness.graphHash) {
            throw "graphHash drift in $key ($($run.File))"
        }
    }

    [double[]]$medians = @(); [double[]]$p95s = @(); [double[]]$p99s = @()
    [double[]]$cullMedians = @(); [double[]]$sortMedians = @(); [double[]]$packetMedians = @()
    [int]$hitch16 = 0; [int]$hitch33 = 0; [int]$hitch50 = 0
    foreach ($run in $runs) {
        [double[]]$frames = @($run.Data.samples | ForEach-Object { [double]$_.cpuFrameMs })
        $medians += (Get-Median $frames)
        $p95s += (Get-Quantile -Values $frames -Q 0.95)
        $p99s += (Get-Quantile -Values $frames -Q 0.99)
        $cullMedians += [double]$run.Data.statistics.cullMs.median
        $sortMedians += [double]$run.Data.statistics.packetSortMs.median
        $packetMedians += [double]$run.Data.statistics.packetBuildMs.median
        $hitch16 += [int]$run.Data.statistics.hitches.hitch16_67
        $hitch33 += [int]$run.Data.statistics.hitches.hitch33_33
        $hitch50 += [int]$run.Data.statistics.hitches.hitch50
    }
    $medianOfMedians = Get-Median $medians
    $mad = Get-Mad -Values $medians -Median $medianOfMedians
    $relativeMad = $mad / $medianOfMedians
    $rows.Add([pscustomobject]@{
        cell              = $key
        scene             = $first.sceneName
        rhi               = $first.rhi
        runs              = $runs.Count
        warmupFrames      = $first.warmupFrames
        measuredFrames    = $first.measuredFrames
        sourceCommit      = $first.sourceCommit
        medianMs          = [math]::Round($medianOfMedians, 4)
        runToRunMadMs     = [math]::Round($mad, 4)
        relativeMad       = [math]::Round($relativeMad, 5)
        minimumUsefulChangePercent = [math]::Round(100.0 * [Math]::Max(0.03, 2.0 * $relativeMad), 3)
        p95Ms             = [math]::Round((Get-Median $p95s), 4)
        p99Ms             = [math]::Round((Get-Median $p99s), 4)
        cullMedianMs      = [math]::Round((Get-Median $cullMedians), 4)
        sortMedianMs      = [math]::Round((Get-Median $sortMedians), 4)
        packetMedianMs    = [math]::Round((Get-Median $packetMedians), 4)
        hitch16_67        = $hitch16
        hitch33_33        = $hitch33
        hitch50           = $hitch50
        visibleCount      = $first.correctness.visibleCount
        drawCount         = $first.correctness.drawCount
        packetSequenceHash = $first.correctness.packetSequenceHash
        graphHash         = $first.correctness.graphHash
        commandHash       = $first.correctness.commandHash
        screenshotHash    = $first.correctness.screenshotHash
        executableSha256  = $first.executableSha256
        sceneManifestSha256 = $first.sceneManifestSha256
        cameraPathSha256  = $first.cameraPathSha256
        streamScriptSha256 = $first.streamScriptSha256
        rawFiles          = @($runs | ForEach-Object { [pscustomobject]@{ name = $_.File; sha256 = $_.Sha256 } })
    })
}

$summaryPath = Join-Path $output 'serial-baseline-summary.json'
$payload = [ordered]@{
    schemaVersion   = 1
    generatedUtc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    rawDirectory    = $raw
    sourceCommit    = $rows[0].sourceCommit
    cells           = @($rows)
}
[System.IO.File]::WriteAllText($summaryPath, (($payload | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# M7-01 串行基线噪声报告')
$lines.Add('')
$lines.Add(('- 生成时间（UTC）：{0}' -f $payload.generatedUtc))
$lines.Add(('- raw 目录：{0}' -f $raw))
$lines.Add(('- 每 cell {0} 次独立进程运行，warmup/measured = {1}/{2}，VSync off，workers=1，chunkSize=256' -f `
    $rows[0].runs, $rows[0].warmupFrames, $rows[0].measuredFrames))
$lines.Add('')
$lines.Add('| cell | CPU frame median (ms) | run-to-run MAD | relative MAD | minimumUsefulChange | p95 | p99 | cull median | sort median | packet median | hitch>16.67/33.33/50 |')
$lines.Add('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|')
foreach ($row in $rows) {
    $lines.Add(('| {0} | {1} | {2} | {3} | {4}% | {5} | {6} | {7} | {8} | {9} | {10}/{11}/{12} |' -f `
        $row.cell, $row.medianMs, $row.runToRunMadMs, $row.relativeMad, $row.minimumUsefulChangePercent,
        $row.p95Ms, $row.p99Ms, $row.cullMedianMs, $row.sortMedianMs, $row.packetMedianMs,
        $row.hitch16_67, $row.hitch33_33, $row.hitch50))
}
$lines.Add('')
$lines.Add('## 身份哈希（同一 cell 内 5 次运行完全一致，否则脚本已报错）')
$lines.Add('')
$lines.Add('| cell | visible | drawn | packetSequence | graph | command | screenshot |')
$lines.Add('|---|---:|---:|---|---|---|---|')
foreach ($row in $rows) {
    $lines.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f `
        $row.cell, $row.visibleCount, $row.drawCount, $row.packetSequenceHash, $row.graphHash,
        ($row.commandHash.Substring(0, 16) + '…'), ($row.screenshotHash.Substring(0, 16) + '…')))
}
$lines.Add('')
$lines.Add('## raw 文件与 SHA-256')
$lines.Add('')
foreach ($row in $rows) {
    foreach ($file in $row.rawFiles) {
        $lines.Add(('- {0}: `{1}`' -f $file.name, $file.sha256))
    }
}
$lines.Add('')
$lines.Add('> 大 artifact 不入 Git；raw 文件位于本地 `out/`，外部存储位置见 docs/evidence/ARTIFACT-INDEX.md。')
[System.IO.File]::WriteAllLines((Join-Path $output 'noise-report.md'), $lines,
    (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("summary: {0}" -f $summaryPath)
$rows | Format-Table cell, medianMs, runToRunMadMs, relativeMad, minimumUsefulChangePercent, p95Ms, p99Ms -AutoSize
