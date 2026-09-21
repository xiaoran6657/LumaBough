<#
.SYNOPSIS
  M7-A12 scaling sweep 汇总：读 raw cell JSON，给出曲线拐点与默认 worker/chunk 建议。

.DESCRIPTION
  判据（与 M7-09 口径一致）：
    * "有意义的改善"阈值 = max(3%, 2 × relativeMAD)（tools/benchmark 的 MinimumUsefulChange 同义）；
    * 默认 worker：在选定 mode+chunk 上，取**达到最佳中位耗时噪声带内的最小 worker 数**（曲线拐点）；
    * 默认 chunk：在选定 mode+workers 上，取**仍在噪声带内的最大 chunk**（任务更少、开销更低）。
  同时校验每个 cell 的 checksumMatchesSerial（跨配置确定性证据）；任一 cell 不匹配即 FAIL。

.EXAMPLE
  pwsh tools/performance/summarize_m7_task_sweep.ps1 -InputDirectory out/m7-04/sweep `
    -SummaryPath out/performance/task-sweep-summary.json -ReportPath out/performance/task-sweep-report.md
#>
[CmdletBinding()]
param(
    [string]$InputDirectory = 'out/m7-04/sweep',
    [string]$SummaryPath = 'out/performance/task-sweep-summary.json',
    [string]$ReportPath = 'out/performance/task-sweep-report.md',
    [double]$NoiseFloor = 0.03
)

$ErrorActionPreference = 'Stop'

$files = Get-ChildItem -LiteralPath $InputDirectory -Filter '*.json' -File |
    Where-Object { $_.Name -notlike 'task-sweep-summary*' }
if (-not $files) { throw "no raw cells in $InputDirectory" }

# M7-TASK-NOISE：按 (mode, workers, chunk) **分组**聚合重复 run。
# 阈值口径与 M7-09 一致（max(3%, 2 × run 间 relative MAD)）；重复数 < 2 时只能退回"帧内 MAD"，
# 此时摘要里显式标注 basis = within-run(single-run fallback)，不假装是 run 间口径。
$runs = foreach ($file in $files) {
    $raw = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($raw.schema -ne 'miniengine.task-sweep.v1') { throw "unexpected schema in $($file.Name): $($raw.schema)" }
    [pscustomobject]@{
        file           = $file.Name
        mode           = $raw.cell.mode
        workers        = [int]$raw.cell.workers
        chunk          = [int]$raw.cell.chunk
        entities       = [int]$raw.cell.entities
        medianMs       = [double]$raw.buildMs.median
        p95Ms          = [double]$raw.buildMs.p95
        relativeMad    = [double]$raw.buildMs.relativeMad
        serialMedianMs = [double]$raw.serialMedianMs
        speedup        = [double]$raw.speedupVsSerial
        tasksPerFrame  = [int]$raw.workload.tasksPerFrame
        visible        = [int64]$raw.workload.visibleEntities
        checksumOk     = [bool]$raw.workload.checksumMatchesSerial
        checksum       = [string]$raw.workload.checksum
        stealSuccesses = [int64]$raw.counters.stealSuccesses
        stealAttempts  = [int64]$raw.counters.stealAttempts
        localPushes    = [int64]$raw.counters.localPushes
        injectedPushes = [int64]$raw.counters.injectedPushes
        wakeups        = [int64]$raw.counters.wakeups
        sleeps         = [int64]$raw.counters.sleeps
        queueLatencyUs = [double]$raw.counters.queueLatencyMeanMicroseconds
    }
}

function Get-MedianValue([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

$groups = @{}
foreach ($run in $runs) {
    $key = '{0}|{1}|{2}' -f $run.mode, $run.workers, $run.chunk
    if (-not $groups.ContainsKey($key)) { $groups[$key] = [System.Collections.Generic.List[object]]::new() }
    $groups[$key].Add($run)
}

$cells = foreach ($key in ($groups.Keys | Sort-Object)) {
    $group = @($groups[$key])
    $medians = @($group | ForEach-Object { $_.medianMs })
    $median = Get-MedianValue $medians
    $runToRunMad = if ($medians.Count -ge 2) {
        Get-MedianValue @($medians | ForEach-Object { [Math]::Abs($_ - $median) })
    }
    else { 0.0 }
    $relativeMadRunToRun = if ($median -gt 0) { $runToRunMad / $median } else { 0.0 }
    $withinRun = Get-MedianValue @($group | ForEach-Object { $_.relativeMad })
    $basis = if ($medians.Count -ge 2) { 'run-to-run' } else { 'within-run(single-run fallback)' }
    $maximum = ($medians | Measure-Object -Maximum).Maximum
    $minimum = ($medians | Measure-Object -Minimum).Minimum
    [pscustomobject]@{
        file             = ($group | Sort-Object file | Select-Object -First 1).file
        repeats          = $medians.Count
        mode             = $group[0].mode
        workers          = $group[0].workers
        chunk            = $group[0].chunk
        entities         = $group[0].entities
        medianMs         = $median
        medianSpreadMs   = if ($medians.Count -ge 2) { $maximum - $minimum } else { 0.0 }
        p95Ms            = Get-MedianValue @($group | ForEach-Object { $_.p95Ms })
        relativeMad      = if ($basis -eq 'run-to-run') { $relativeMadRunToRun } else { $withinRun }
        withinRunMad     = $withinRun
        thresholdBasis   = $basis
        serialMedianMs   = $group[0].serialMedianMs
        speedup          = Get-MedianValue @($group | ForEach-Object { $_.speedup })
        tasksPerFrame    = $group[0].tasksPerFrame
        visible          = $group[0].visible
        checksumOk       = -not ($group | Where-Object { -not $_.checksumOk })
        checksum         = $group[0].checksum
        stealSuccesses   = $group[0].stealSuccesses
        stealAttempts    = $group[0].stealAttempts
        localPushes      = $group[0].localPushes
        injectedPushes   = $group[0].injectedPushes
        wakeups          = $group[0].wakeups
        sleeps           = $group[0].sleeps
        queueLatencyUs   = Get-MedianValue @($group | ForEach-Object { $_.queueLatencyUs })
    }
}

$badChecksum = $cells | Where-Object { -not $_.checksumOk }
if ($badChecksum) {
    ($badChecksum | ForEach-Object { "checksum mismatch: $($_.file)" }) | Write-Error
    throw "determinism check failed for $($badChecksum.Count) cell(s)"
}
# 确定性口径：校验和按叶子切分生成，因此**同 chunk 下跨 mode/workers 必须一致**（输出与
# 调度无关）；跨 chunk 只比较可见实体数（与切分无关的量）。
foreach ($chunk in ($cells | Select-Object -ExpandProperty chunk | Sort-Object -Unique)) {
    $variants = ($cells | Where-Object { $_.chunk -eq $chunk } | Select-Object -ExpandProperty checksum | Sort-Object -Unique)
    if ($variants.Count -ne 1) { throw "cells disagree on checksum at chunk=${chunk}: $($variants -join ', ')" }
}
$visibleCounts = ($cells | Select-Object -ExpandProperty visible | Sort-Object -Unique)
if ($visibleCounts.Count -ne 1) { throw "cells disagree on visible entities: $($visibleCounts -join ', ')" }
$checksums = ($cells | Select-Object -ExpandProperty checksum | Sort-Object -Unique)

# 阈值：与 M7-09 口径一致（3% 底线；relativeMAD 更大时用 2×MAD）。
$threshold = { param($cell) [Math]::Max($NoiseFloor, 2.0 * [double]$cell.relativeMad) }

$taskCells = $cells | Where-Object { $_.mode -ne 'serial' }
$bestCell = $taskCells | Sort-Object medianMs | Select-Object -First 1
$defaultMode = $bestCell.mode

# 默认 worker：选定 mode 与最佳 chunk 上，达到噪声带内的最小 worker 数。
$atBestChunk = $taskCells | Where-Object { $_.mode -eq $defaultMode -and $_.chunk -eq $bestCell.chunk } | Sort-Object workers
$bestAtChunk = $atBestChunk | Sort-Object medianMs | Select-Object -First 1
$band = & $threshold $bestAtChunk
$defaultWorkers = ($atBestChunk | Where-Object { $_.medianMs -le $bestAtChunk.medianMs * (1.0 + $band) } |
    Sort-Object workers | Select-Object -First 1).workers

# 默认 chunk：选定 mode 与默认 workers 上，仍在噪声带内的最大 chunk。
$atDefaultWorkers = $taskCells | Where-Object { $_.mode -eq $defaultMode -and $_.workers -eq $defaultWorkers } | Sort-Object chunk
$bestAtWorkers = $atDefaultWorkers | Sort-Object medianMs | Select-Object -First 1
$bandChunk = & $threshold $bestAtWorkers
$defaultChunk = ($atDefaultWorkers | Where-Object { $_.medianMs -le $bestAtWorkers.medianMs * (1.0 + $bandChunk) } |
    Sort-Object chunk -Descending | Select-Object -First 1).chunk
$chosen = $atDefaultWorkers | Where-Object { $_.chunk -eq $defaultChunk } | Select-Object -First 1
$serialAtChunk = $cells | Where-Object { $_.mode -eq 'serial' -and $_.chunk -eq $defaultChunk } | Select-Object -First 1

$summary = [ordered]@{
    schema         = 'miniengine.task-sweep-summary.v1'
    generatedUtc   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    entities       = $bestCell.entities
    cells          = $cells.Count
    visibleEntities = $visibleCounts[0]
    checksumNote   = 'checksum is per-chunk (leaf partition); identical across mode/workers within the same chunk'
    thresholds     = [ordered]@{ noiseFloor = $NoiseFloor; rule = 'max(3%, 2 x relativeMAD)'; basis = @($cells | Select-Object -ExpandProperty thresholdBasis | Sort-Object -Unique) }
    recommendation = [ordered]@{
        mode           = $defaultMode
        workers        = $defaultWorkers
        chunk          = $defaultChunk
        medianMs       = $chosen.medianMs
        p95Ms          = $chosen.p95Ms
        speedupVsSerial = $chosen.speedup
        serialMedianMs = $serialAtChunk.serialMedianMs
        tasksPerFrame  = $chosen.tasksPerFrame
        rationale      = "curve knee: smallest worker count within noise band at chunk=$defaultChunk; largest chunk within noise band at workers=$defaultWorkers"
    }
    best           = [ordered]@{
        mode = $bestCell.mode; workers = $bestCell.workers; chunk = $bestCell.chunk
        medianMs = $bestCell.medianMs; speedupVsSerial = $bestCell.speedup
    }
    matrix         = $cells
}
Set-Content -LiteralPath $SummaryPath -Value ($summary | ConvertTo-Json -Depth 6) -Encoding UTF8

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# M7-A12 scaling sweep 报告（自动生成）')
$lines.Add('')
$lines.Add("- 输入：``$InputDirectory`` 的 $($cells.Count) 个 cell（entities=$($bestCell.entities)，visible=$($visibleCounts[0])）")
$basisLabel = (@($cells | Select-Object -ExpandProperty thresholdBasis | Sort-Object -Unique) -join ", ")
$lines.Add(("- 阈值：max(3%, 2 x relativeMAD)（口径：{0}）；默认值取自曲线拐点，不来自逻辑核数猜测" -f $basisLabel))
$lines.Add("- 确定性：所有 cell 的 checksum 与 serial 参考一致")
$lines.Add('')
$lines.Add('## 每帧 build 中位耗时（ms，越小越好）')
$lines.Add('')
foreach ($mode in ($cells | Select-Object -ExpandProperty mode | Sort-Object -Unique)) {
    $modeCells = $cells | Where-Object { $_.mode -eq $mode }
    $chunks = ($modeCells | Select-Object -ExpandProperty chunk | Sort-Object -Unique)
    $workerSet = ($modeCells | Select-Object -ExpandProperty workers | Sort-Object -Unique)
    $lines.Add("### $mode")
    $lines.Add('')
    $lines.Add('| workers \ chunk | ' + (($chunks | ForEach-Object { $_ }) -join ' | ') + ' |')
    $lines.Add('|' + ('---|' * ($chunks.Count + 1)))
    foreach ($worker in $workerSet) {
        $row = foreach ($chunk in $chunks) {
            $cell = $modeCells | Where-Object { $_.workers -eq $worker -and $_.chunk -eq $chunk } | Select-Object -First 1
            if ($cell) { '{0:N3} ({1:N2}x)' -f $cell.medianMs, $cell.speedup } else { '-' }
        }
        $lines.Add("| $worker | " + ($row -join ' | ') + ' |')
    }
    $lines.Add('')
}
$lines.Add('## 推荐默认值')
$lines.Add('')
$lines.Add("- mode = **$defaultMode**，workers = **$defaultWorkers**，chunk = **$defaultChunk**")
$lines.Add("- 该配置：median $('{0:N3}' -f $chosen.medianMs) ms / p95 $('{0:N3}' -f $chosen.p95Ms) ms / serial $('{0:N3}' -f $serialAtChunk.serialMedianMs) ms → **$('{0:N2}' -f $chosen.speedup)x**")
$lines.Add("- 全局最佳 cell：mode=$($bestCell.mode) workers=$($bestCell.workers) chunk=$($bestCell.chunk) median $('{0:N3}' -f $bestCell.medianMs) ms（$('{0:N2}' -f $bestCell.speedup)x）")
$lines.Add("- 任务数/帧（推荐配置）：$($chosen.tasksPerFrame)；steal 成功/尝试：$($chosen.stealSuccesses)/$($chosen.stealAttempts)（仅 per-worker 模式会窃取）")
$lines.Add('')
Set-Content -LiteralPath $ReportPath -Value ($lines -join "`r`n") -Encoding UTF8

Write-Host ("summary: {0}" -f $SummaryPath)
Write-Host ("report : {0}" -f $ReportPath)
Write-Host ("recommendation: mode={0} workers={1} chunk={2} median={3:N3}ms speedup={4:N2}x" -f `
        $defaultMode, $defaultWorkers, $defaultChunk, $chosen.medianMs, $chosen.speedup)
