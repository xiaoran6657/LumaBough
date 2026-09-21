<#
.SYNOPSIS
  M7-05 并行 RenderPacket sweep 汇总：读 raw run JSON，做正确性核对、A/B 判定与瓶颈归因。

.DESCRIPTION
  判据（与 M7-09 口径一致）：
    * "有意义的改善"阈值 = max(3%, 2 × relativeMAD)（baseline cell 的 run 间相对 MAD，
      单 run 时退化为帧内 relativeMAD，报告中标注）；
    * 正确性：所有 cell 的 packetSequenceHash 必须一致（同 scene/rhi/协议），并且
      graphHash/commandHash/screenshotHash 在同一 RHI 内一致——任一不一致即 FAIL；
    * A/B 只比较数据，不宣布未测的因果；每个 cell 记录 cull/merge/wait/sort 分解，
      用于判断"瓶颈是否移动"。

.EXAMPLE
  pwsh tools/performance/summarize_m7_packet_sweep.ps1 -InputDirectory out/m7-05/sweep `
    -SummaryPath out/performance/packet-sweep-summary.json `
    -ReportPath out/performance/packet-sweep-report.md
#>
[CmdletBinding()]
param(
    [string]$InputDirectory = 'out/m7-05/sweep',
    [string]$SummaryPath = 'out/performance/packet-sweep-summary.json',
    [string]$ReportPath = 'out/performance/packet-sweep-report.md',
    [double]$NoiseFloor = 0.03
)

$ErrorActionPreference = 'Stop'

function Get-Median ([double[]]$Values) {
    if ($Values.Count -eq 0) { throw 'median of empty set' }
    $sorted = $Values | Sort-Object
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$mid] }
    return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}
function Get-Mad ([double[]]$Values, [double]$Median) {
    return Get-Median ([double[]]($Values | ForEach-Object { [Math]::Abs($_ - $Median) }))
}
function Get-Quantile ([double[]]$Values, [double]$Q) {
    $sorted = $Values | Sort-Object
    if ($sorted.Count -eq 1) { return [double]$sorted[0] }
    $rank = $Q * ($sorted.Count - 1)
    $lower = [int][Math]::Floor($rank)
    $upper = [int][Math]::Ceiling($rank)
    if ($lower -eq $upper) { return [double]$sorted[$lower] }
    $weight = $rank - $lower
    return [double]$sorted[$lower] * (1.0 - $weight) + [double]$sorted[$upper] * $weight
}

$runFiles = Get-ChildItem -LiteralPath $InputDirectory -Recurse -Filter 'run.json' -File
if (-not $runFiles) { throw "no raw runs (run.json) under $InputDirectory" }

$runs = foreach ($file in $runFiles) {
    $raw = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($raw.schemaVersion -ne 2) { throw "unexpected schemaVersion in $($file.FullName)" }
    if ($raw.correctness.status -ne 'PASS') { throw "correctness not PASS in $($file.FullName)" }
    if (@($raw.samples).Count -ne [int]$raw.measuredFrames) { throw "sample count mismatch in $($file.FullName)" }

    $notesPath = Join-Path $file.DirectoryName 'run-notes.json'
    $notes = if (Test-Path -LiteralPath $notesPath) {
        Get-Content -LiteralPath $notesPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } else { $null }

    $packet = [double[]]@($raw.samples | ForEach-Object { [double]$_.packetBuildMs })
    $frame = [double[]]@($raw.samples | ForEach-Object { [double]$_.cpuFrameMs })
    $cull = [double[]]@($raw.samples | ForEach-Object { [double]$_.cullMs })
    $merge = [double[]]@($raw.samples | ForEach-Object { [double]$_.packetMergeMs })
    $wait = [double[]]@($raw.samples | ForEach-Object { [double]$_.packetWaitMs })
    $sort = [double[]]@($raw.samples | ForEach-Object { [double]$_.packetSortMs })
    $opaque = [double[]]@($raw.samples | ForEach-Object { [double]$_.drawCount })
    $packetMedian = Get-Median $packet

    [pscustomobject]@{
        directory       = $file.DirectoryName
        cell            = '{0}|{1}|w{2}|c{3}|r{4}' -f $raw.packetBuildMode, $raw.schedulerMode, $raw.workers, $raw.chunkSize, $raw.chunkReserve
        mode            = [string]$raw.packetBuildMode
        scheduler       = [string]$raw.schedulerMode
        workers         = [int]$raw.workers
        chunk           = [int]$raw.chunkSize
        reserve         = [bool]$raw.chunkReserve
        runIndex        = [int]$raw.runIndex
        rhi             = [string]$raw.rhi
        scene           = [string]$raw.sceneName
        packetMedian    = $packetMedian
        packetMad       = Get-Mad $packet $packetMedian
        packetRelativeMad = (Get-Mad $packet $packetMedian) / $packetMedian
        packetP95       = Get-Quantile $packet 0.95
        packetP99       = Get-Quantile $packet 0.99
        frameMedian     = Get-Median $frame
        cullMedian      = Get-Median $cull
        mergeMedian     = Get-Median $merge
        waitMedian      = Get-Median $wait
        sortMedian      = Get-Median $sort
        drawMedian      = Get-Median $opaque
        packetSequenceHash = [string]$raw.correctness.packetSequenceHash
        graphHash       = [string]$raw.correctness.graphHash
        commandHash     = [string]$raw.correctness.commandHash
        screenshotHash  = [string]$raw.correctness.screenshotHash
        packetChunks    = if ($notes) { [int64]$notes.packetChunks } else { 0 }
        syncFallbacks   = if ($notes) { [int64]$notes.packetSyncFallbacks } else { 0 }
        foregroundGate  = if ($notes) { [bool]$notes.foregroundGate } else { $true }
        foregroundRatio = if ($notes) { [double]$notes.foregroundRatio } else { 1.0 }
        totalStealAttempts = [int64]$raw.statistics.totalStealAttempts
        totalStealSuccesses = [int64]$raw.statistics.totalStealSuccesses
        activeWorkers   = [int]$raw.statistics.activeWorkers
        experiments     = [string]$raw.experimentId
    }
}

# ---- 正确性核对（先于任何性能结论） -----------------------------------------
foreach ($field in @('packetSequenceHash', 'graphHash', 'commandHash')) {
    $values = $runs | Select-Object -ExpandProperty $field | Sort-Object -Unique
    if ($values.Count -ne 1 -or [string]::IsNullOrEmpty($values[0])) {
        throw "correctness divergence in $field : $($values -join ', ')"
    }
}
$screenshotValues = $runs | Select-Object -ExpandProperty screenshotHash | Sort-Object -Unique
if ($screenshotValues.Count -ne 1 -or [string]::IsNullOrEmpty($screenshotValues[0])) {
    throw "correctness divergence in screenshotHash : $($screenshotValues -join ', ')"
}

# ---- cell 归并（run 间取 median） -------------------------------------------
function Get-CellId ($First) {
    $prefix = if ($First.mode -eq 'serial') { 'serial' } elseif ($First.scheduler -eq 'global') { 'par-global' } else { 'par-pw' }
    $suffix = if ($First.reserve) { '' } else { '-noreserve' }
    return '{0}-w{1}-c{2}{3}' -f $prefix, $First.workers, $First.chunk, $suffix
}

$cellGroups = $runs | Group-Object cell
$cells = foreach ($group in $cellGroups) {
    $items = @($group.Group)
    $first = $items[0]
    [double[]]$packetMedians = @($items | ForEach-Object { $_.packetMedian })
    [double[]]$frameMedians = @($items | ForEach-Object { $_.frameMedian })
    $cellPacketMedian = Get-Median $packetMedians
    $cellPacketMad = Get-Mad $packetMedians $cellPacketMedian
    $relativeMad = if ($items.Count -ge 2) {
        $cellPacketMad / $cellPacketMedian
    } else {
        $first.packetRelativeMad   # 单 run：用帧内 relMAD 作为噪声代用，报告标注
    }
    [pscustomobject]@{
        cell            = $group.Name
        id              = Get-CellId $first
        mode            = $first.mode
        scheduler       = $first.scheduler
        workers         = $first.workers
        chunk           = $first.chunk
        reserve         = $first.reserve
        runs            = $items.Count
        packetMedian    = $cellPacketMedian
        packetP95       = Get-Median ([double[]]@($items | ForEach-Object { $_.packetP95 }))
        packetRelMad    = $relativeMad
        packetNoiseSource = if ($items.Count -ge 2) { 'run-to-run' } else { 'within-run (single run)' }
        frameMedian     = Get-Median $frameMedians
        cullMedian      = Get-Median ([double[]]@($items | ForEach-Object { $_.cullMedian }))
        mergeMedian     = Get-Median ([double[]]@($items | ForEach-Object { $_.mergeMedian }))
        waitMedian      = Get-Median ([double[]]@($items | ForEach-Object { $_.waitMedian }))
        sortMedian      = Get-Median ([double[]]@($items | ForEach-Object { $_.sortMedian }))
        drawMedian      = Get-Median ([double[]]@($items | ForEach-Object { $_.drawMedian }))
        packetChunksPerRun = ($items | Select-Object -ExpandProperty packetChunks | Sort-Object -Unique) -join ','
        syncFallbacksPerRun = ($items | Select-Object -ExpandProperty syncFallbacks | Sort-Object -Unique) -join ','
        totalStealAttempts = [int64](($items | Measure-Object -Property totalStealAttempts -Sum).Sum)
        totalStealSuccesses = [int64](($items | Measure-Object -Property totalStealSuccesses -Sum).Sum)
        activeWorkers   = $first.activeWorkers
        foregroundGate  = -not ($items | Where-Object { -not $_.foregroundGate })
        foregroundRatioMin = ($items | Measure-Object -Property foregroundRatio -Minimum).Minimum
    }
}

function Get-Cell ([string]$Id) { return $cells | Where-Object { $_.id -eq $Id } | Select-Object -First 1 }
function Compare-Cells ([string]$BaselineId, [string]$CandidateId, [string]$Metric) {
    $a = Get-Cell $BaselineId
    $b = Get-Cell $CandidateId
    if (-not $a -or -not $b) { return $null }
    $av = [double]$a.$Metric
    $bv = [double]$b.$Metric
    $threshold = [Math]::Max($NoiseFloor, 2.0 * [double]$a.packetRelMad)
    $deltaPct = if ($av -gt 0) { 100.0 * ($bv - $av) / $av } else { 0.0 }
    $verdict = if ([Math]::Abs($deltaPct) -le 100.0 * $threshold) { 'insufficient (within noise)' }
        elseif ($deltaPct -lt 0) { 'candidate faster' } else { 'candidate slower' }
    return [pscustomobject]@{
        metric = $Metric; baseline = $av; candidate = $bv
        deltaPercent = [Math]::Round($deltaPct, 2)
        requiredPercent = [Math]::Round(100.0 * $threshold, 2)
        verdict = $verdict
        speedup = if ($bv -gt 0) { [Math]::Round($av / $bv, 3) } else { 0.0 }
    }
}

$comparisons = [ordered]@{}
foreach ($metric in @('packetMedian', 'frameMedian', 'cullMedian', 'mergeMedian', 'waitMedian', 'sortMedian')) {
    $comparisons["RP001-serial-to-global:$metric"] = Compare-Cells 'serial-w1-c256' 'par-global-w8-c256' $metric
    $comparisons["RP002-global-to-perworker:$metric"] = Compare-Cells 'par-global-w8-c256' 'par-pw-w8-c256' $metric
    $comparisons["RP004-reserve-on-to-off:$metric"] = Compare-Cells 'par-pw-w8-c256' 'par-pw-w8-c256-noreserve' $metric
}
foreach ($chunk in 32, 64, 128, 512, 1024) {
    $comparisons["RP003-c256-to-c$chunk:packetMedian"] = Compare-Cells 'par-pw-w8-c256' "par-pw-w8-c$chunk" 'packetMedian'
}

$scaleCells = $cells | Where-Object { $_.mode -eq 'parallel' -and $_.scheduler -eq 'per-worker' -and $_.chunk -eq 256 -and $_.reserve } |
    Sort-Object workers

$reference = @($runs)[0]
$summary = [ordered]@{
    schema          = 'miniengine.packet-sweep-summary.v1'
    generatedUtc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    runs            = @($runs).Count
    cells           = @($cells).Count
    scene           = $reference.scene
    rhi             = $reference.rhi
    thresholds      = [ordered]@{ noiseFloor = $NoiseFloor; rule = 'max(3%, 2 x relativeMAD of the baseline cell)' }
    correctness     = [ordered]@{
        packetSequenceHash = $reference.packetSequenceHash
        graphHash          = $reference.graphHash
        commandHash        = $reference.commandHash
        screenshotHash     = $reference.screenshotHash
        note               = 'all runs agree on packet/graph/command/screenshot hashes'
    }
    matrix          = @($cells)
    scaling         = @($scaleCells)
    comparisons     = $comparisons
}
Set-Content -LiteralPath $SummaryPath -Value ($summary | ConvertTo-Json -Depth 8) -Encoding UTF8

# ---- 报告（简体中文，随数字给出瓶颈解释） -----------------------------------
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# M7-05 并行 RenderPacket sweep 报告（自动生成）')
$lines.Add('')
$lines.Add("- 输入：``$InputDirectory``，$($runs.Count) 次 run / $($cells.Count) 个 cell（scene=$($runs[0].scene)，rhi=$($runs[0].rhi)）")
$lines.Add("- 阈值：max(3%, 2 x relativeMAD)；单 run cell 的噪声用帧内 relativeMAD 代用（见矩阵表 NoiseSrc）")
$lines.Add("- 正确性：全部 run 的 packetSequenceHash/graphHash/commandHash/screenshotHash 一致（$($runs[0].packetSequenceHash)）")
$lines.Add('')
$lines.Add('## 单元矩阵（ms，median）')
$lines.Add('')
$lines.Add('| cell | runs | packet | p95 | frame | cull(sum) | merge | wait | sort | chunks | syncFallback | steal S/A | fgRatio | NoiseSrc |')
$lines.Add('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|')
foreach ($cell in ($cells | Sort-Object mode, scheduler, workers, chunk, reserve)) {
    # 先构造再 Add：PS 5.1 在方法调用内对"-f + 反引号续行"的解析会丢参数
    # （FormatError: index out of range），拆成两步可避免。
    $row = '| {0} | {1} | {2:N3} | {3:N3} | {4:N2} | {5:N3} | {6:N3} | {7:N3} | {8:N3} | {9} | {10} | {11}/{12} | {13:N2} | {14} |' -f `
        $cell.id, $cell.runs, $cell.packetMedian, $cell.packetP95, $cell.frameMedian, $cell.cullMedian, `
        $cell.mergeMedian, $cell.waitMedian, $cell.sortMedian, $cell.packetChunksPerRun, $cell.syncFallbacksPerRun, `
        $cell.totalStealSuccesses, $cell.totalStealAttempts, $cell.foregroundRatioMin, $cell.packetNoiseSource
    $lines.Add($row)
}
$automatedRuns = @($cells | Where-Object { -not $_.foregroundGate })
if ($automatedRuns.Count -gt 0) {
    $lines.Add('')
    $lines.Add('> 采集前提：部分 cell 在自动化会话中运行（``--foreground-gate=off``，fgRatio 见上表）。packet/merge/sort/wait/task 指标是 CPU 侧测量，不受 DWM 前台节流影响；frame/present 类指标带节流风险，只作参考。')
}
$lines.Add('')
$lines.Add('## A/B 判定')
$lines.Add('')
$lines.Add('| 比较 | 指标 | baseline | candidate | Δ% | 阈值% | 判定 |')
$lines.Add('|---|---|---:|---:|---:|---:|---|')
foreach ($key in $comparisons.Keys) {
    $data = $comparisons[$key]
    if ($null -eq $data) { continue }
    $tableRow = '| {0} | {1} | {2:N3} | {3:N3} | {4:N2} | {5:N2} | {6} |' -f `
        $key.Split(':')[0], $data.metric, $data.baseline, $data.candidate, $data.deltaPercent, $data.requiredPercent, $data.verdict
    $lines.Add($tableRow)
}
$lines.Add('')
if ($scaleCells) {
    $lines.Add('## worker scaling（per-worker，c256）')
    $lines.Add('')
    $lines.Add('| workers | packet | frame | cull(sum) | merge | wait | sort |')
    $lines.Add('|---:|---:|---:|---:|---:|---:|---:|')
    foreach ($cell in $scaleCells) {
        $scaleRow = '| {0} | {1:N3} | {2:N2} | {3:N3} | {4:N3} | {5:N3} | {6:N3} |' -f `
            $cell.workers, $cell.packetMedian, $cell.frameMedian, $cell.cullMedian, $cell.mergeMedian, $cell.waitMedian, $cell.sortMedian
        $lines.Add($scaleRow)
    }
    $lines.Add('')
}
Set-Content -LiteralPath $ReportPath -Value ($lines -join "`r`n") -Encoding UTF8

Write-Host ("summary: {0}" -f $SummaryPath)
Write-Host ("report : {0}" -f $ReportPath)
Write-Host ("runs={0} cells={1}" -f $runs.Count, $cells.Count)
