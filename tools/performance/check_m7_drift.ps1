# ============================================================================
# check_m7_drift.ps1 — M7-09 漂移检查（执行顺序 vs 指标趋势）
#
# 09 文档要求："每个 run 启动新进程；记录顺序，观察是否随时间单调变慢，以识别温度
# 或后台任务漂移"。本工具消费采集目录里的 sweep-driver-summary.json（执行顺序与
# startedUtc）与逐 run 的 run.json，产出：
#   * 每 cell × 每 variant 的时间序列（按执行顺序）；
#   * 前半 / 后半中位数对比（driftPercent）；
#   * 线性回归斜率与 Pearson r（趋势诊断：r 越接近 ±1 越像单调漂移）；
#   * 与噪声下限（max(3%, 2×序列 relativeMAD)）比较 → flagged。
#
# 判定：任一 cell/variant 的 |drift| > 噪声下限，或 |r| > 0.7 且 |slope| 超过噪声下限的
# 1/4 → `drift=flagged`，退出码 1（采集结果应判 INCONCLUSIVE，或重新采集）。
#
# 用法：
#   pwsh tools/performance/check_m7_drift.ps1 -InputDirectory out/m7-09/ab -TargetMetric cpuFrameMs `
#        -OutputJson out/m7-09/drift.json
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$InputDirectory,
    [string]$TargetMetric = 'cpuFrameMs',
    [double]$TrendRThreshold = 0.7,
    [string]$OutputJson = ''
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

$root = [System.IO.Path]::GetFullPath($InputDirectory)
$summaryPath = Join-Path $root 'sweep-driver-summary.json'
if (-not (Test-Path -LiteralPath $summaryPath)) {
    throw "sweep-driver-summary.json not found under $root (drift check needs the recorded execution order)"
}
$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
$entries = @($summary.cells)
if ($entries.Count -eq 0) { throw "no runs recorded in $summaryPath" }

# 逐 run 读目标指标；缺 run.json 或指标缺失即 INVALID（不静默跳过）。
$series = New-Object System.Collections.Generic.List[object]
$sequence = 0
foreach ($entry in ($entries | Sort-Object { if ($null -ne $_.sequence -and [int]$_.sequence -gt 0) { [int]$_.sequence } else { 0 } }, startedUtc)) {
    ++$sequence
    $runPath = $entry.metricsPath
    if (-not $runPath -or -not (Test-Path -LiteralPath $runPath)) {
        throw "missing run JSON for $($entry.cell)#$($entry.runIndex): $runPath"
    }
    $data = Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
    $dist = $data.statistics.$TargetMetric
    if ($null -eq $dist) {
        throw "target metric '$TargetMetric' missing in $runPath"
    }
    $series.Add([pscustomobject]@{
        sequence  = $sequence
        cell      = $entry.cell
        variant   = $entry.variant
        runIndex  = $entry.runIndex
        startedUtc = $entry.startedUtc
        value     = [double]$dist.median
        p95       = [double]$dist.p95
    })
}

function Get-Trend {
    param([object[]]$Points)
    # 线性回归（x = sequence, y = value）与 Pearson r。
    $n = $Points.Count
    if ($n -lt 3) { return [pscustomobject]@{ slope = 0.0; r = 0.0 } }
    $meanX = (Get-Median @($Points | ForEach-Object { [double]$_.sequence }))
    $meanY = (Get-Median @($Points | ForEach-Object { [double]$_.value }))
    $sxy = 0.0; $sxx = 0.0; $syy = 0.0
    foreach ($point in $Points) {
        $dx = [double]$point.sequence - $meanX
        $dy = [double]$point.value - $meanY
        $sxy += $dx * $dy; $sxx += $dx * $dx; $syy += $dy * $dy
    }
    $slope = if ($sxx -gt 0) { $sxy / $sxx } else { 0.0 }
    $r = if ($sxx -gt 0 -and $syy -gt 0) { $sxy / [Math]::Sqrt($sxx * $syy) } else { 0.0 }
    return [pscustomobject]@{ slope = $slope; r = $r }
}

$cells = [ordered]@{}
$flagged = $false
foreach ($group in ($series | Group-Object cell, variant)) {
    $points = @($group.Group | Sort-Object sequence)
    $values = @($points | ForEach-Object { $_.value })
    $median = Get-Median $values
    $mad = Get-Mad -Values $values -Median $median
    $relativeMad = if ($median -gt 0) { $mad / $median } else { 0.0 }
    $noiseFloor = [Math]::Max(3.0, 200.0 * $relativeMad)
    $cut = [int][Math]::Floor($points.Count / 2)
    $first = if ($cut -gt 0) { Get-Median @(@($points | Select-Object -First $cut) | ForEach-Object { $_.value }) } else { $median }
    $second = if ($points.Count - $cut -gt 0) { Get-Median @(@($points | Select-Object -Last ($points.Count - $cut)) | ForEach-Object { $_.value }) } else { $median }
    $driftPercent = if ($first -gt 0) { 100.0 * ($second - $first) / $first } else { 0.0 }
    $trend = Get-Trend $points
    $slopePercentPerRun = if ($median -gt 0) { 100.0 * $trend.slope / $median } else { 0.0 }
    $cellFlagged = ([Math]::Abs($driftPercent) -gt $noiseFloor) -or
                   ([Math]::Abs($trend.r) -gt $TrendRThreshold -and [Math]::Abs($slopePercentPerRun) -gt ($noiseFloor / 4.0))
    if ($cellFlagged) { $flagged = $true }
    $cells[$group.Name] = [pscustomobject]@{
        runs                 = $points.Count
        order                = @($points | ForEach-Object { $_.sequence })
        values               = @($points | ForEach-Object { [math]::Round($_.value, 4) })
        medianMs             = [math]::Round($median, 4)
        firstHalfMedianMs    = [math]::Round($first, 4)
        secondHalfMedianMs   = [math]::Round($second, 4)
        driftPercent         = [math]::Round($driftPercent, 3)
        noiseFloorPercent    = [math]::Round($noiseFloor, 3)
        slopePercentPerRun   = [math]::Round($slopePercentPerRun, 4)
        trendR               = [math]::Round($trend.r, 4)
        flagged              = $cellFlagged
    }
}

$cells.GetEnumerator() | ForEach-Object {
    [pscustomobject]@{ series = $_.Key; runs = $_.Value.runs; medianMs = $_.Value.medianMs
        driftPercent = $_.Value.driftPercent; noiseFloorPercent = $_.Value.noiseFloorPercent
        trendR = $_.Value.trendR; flagged = $_.Value.flagged }
} | Format-Table -AutoSize

$payload = [ordered]@{
    schemaVersion   = 1
    generatedUtc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    inputDirectory  = $root
    targetMetric    = $TargetMetric
    interleave      = [bool]$summary.interleave
    shuffleSeed     = $summary.shuffleSeed
    executableSha256 = $summary.executableSha256
    trendRThreshold = $TrendRThreshold
    driftFlagged    = $flagged
    series          = $cells
}
if ($OutputJson) {
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($OutputJson),
        (($payload | ConvertTo-Json -Depth 8) + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("drift report: {0}" -f $OutputJson)
}
Write-Host ("drift: {0}" -f $(if ($flagged) { 'FLAGGED' } else { 'clean' }))
if ($flagged) { exit 1 }
exit 0
