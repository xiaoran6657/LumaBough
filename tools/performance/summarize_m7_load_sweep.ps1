<#
.SYNOPSIS
  M7-07 上传预算 sweep 汇总：把 out 下的 raw JSON 收敛成基线摘要与报告（E-M7-LOAD-002/003）。

.DESCRIPTION
  对每个 cell 的多次 run 取中位数（run 间离散度用 relMAD 记录），输出：
    * <SummaryPath>：机读摘要（cells + 每请求时序分位 + A/B 判定）
    * <ReportPath> ：人读报告（表格 + 判定 + 计算口径）
  判定阈值沿用 M7-05 的噪声规则：max(3%, 2×baseline 的 run 间 relMAD)。

.EXAMPLE
  pwsh tools/performance/summarize_m7_load_sweep.ps1 `
    -InputDirectory out/m7-07/load-sweep `
    -SummaryPath out/performance/load-sweep-summary.json `
    -ReportPath out/performance/load-sweep-report.md
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputDirectory,
    [Parameter(Mandatory = $true)][string]$SummaryPath,
    [Parameter(Mandatory = $true)][string]$ReportPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$input = [System.IO.Path]::GetFullPath($InputDirectory)
$summaryFile = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $SummaryPath))
$reportFile = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $ReportPath))

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $middle = [int][math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-RelMad([double[]]$Values) {
    if ($Values.Count -le 1) { return 0.0 }
    $median = Get-Median $Values
    if ($median -le 0.0) { return 0.0 }
    $deviations = @($Values | ForEach-Object { [math]::Abs($_ - $median) })
    return (Get-Median $deviations) / $median
}

function Get-Percentile([double[]]$Values, [double]$Fraction) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $index = [int][math]::Floor($sorted.Count * $Fraction)
    if ($index -ge $sorted.Count) { $index = $sorted.Count - 1 }
    return [double]$sorted[$index]
}

$runs = @(Get-ChildItem -LiteralPath $input -Recurse -Filter 'run.json' | Sort-Object FullName)
if ($runs.Count -eq 0) { throw "no run.json under $input" }

$cells = [ordered]@{}
foreach ($file in $runs) {
    $cell = ($file.Directory.Name -replace '-r\d+$', '')
    $data = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $worstFrame = 0.0
    foreach ($sample in $data.samples) {
        if ([double]$sample.cpuFrameMs -gt $worstFrame) { $worstFrame = [double]$sample.cpuFrameMs }
    }
    $readyMs = @($data.assetRequests | ForEach-Object { [double]$_.requestToReadyMs })
    $entry = [pscustomobject]@{
        run            = $file.Directory.Name
        status         = $data.correctness.status
        loaderMode     = $data.loaderMode
        budgetMiB      = $data.uploadMiBPerFrame
        budgetCpuMs    = $data.uploadBudgetCpuMs
        budgetRequests = $data.uploadBudgetRequests
        frameMedian    = [double]$data.statistics.cpuFrameMs.median
        frameP95       = [double]$data.statistics.cpuFrameMs.p95
        frameP99       = [double]$data.statistics.cpuFrameMs.p99
        frameWorst     = $worstFrame
        hitch50        = [int]$data.statistics.hitches.over50
        rtrP50         = Get-Percentile $readyMs 0.50
        rtrP95         = Get-Percentile $readyMs 0.95
        rtrWorst       = if ($readyMs.Count -gt 0) { ($readyMs | Measure-Object -Maximum).Maximum } else { 0.0 }
        committed      = [int]$data.statistics.totalUploadsCommitted
        failed         = [int]$data.statistics.totalUploadsFailed
        pendingHW      = [int]$data.statistics.uploadPendingHighWater
        inFlightHW     = [int]$data.statistics.uploadInFlightHighWater
        fairness       = [int]$data.statistics.totalFairnessHolds
        aging          = [int]$data.statistics.totalAgingPromotions
        residentBytes  = [int64]$data.statistics.residentUploadBytes
        cancelWaste    = [int64]$data.statistics.cancelWasteBytes
        metricsSha256  = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    if (-not $cells.Contains($cell)) { $cells[$cell] = [System.Collections.Generic.List[object]]::new() }
    $cells[$cell].Add($entry)
}

$aggregated = [ordered]@{}
foreach ($cell in $cells.Keys) {
    $group = @($cells[$cell])
    $aggregated[$cell] = [pscustomobject]@{
        runs          = $group.Count
        status        = if (($group | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) { 'PASS' } else { 'FAIL' }
        loaderMode    = $group[0].loaderMode
        budgetMiB     = $group[0].budgetMiB
        budgetCpuMs   = $group[0].budgetCpuMs
        budgetRequests = $group[0].budgetRequests
        frameMedian   = Get-Median @($group | ForEach-Object { $_.frameMedian })
        frameP95      = Get-Median @($group | ForEach-Object { $_.frameP95 })
        frameP99      = Get-Median @($group | ForEach-Object { $_.frameP99 })
        frameWorst    = Get-Median @($group | ForEach-Object { $_.frameWorst })
        frameWorstRelMad = Get-RelMad @($group | ForEach-Object { $_.frameWorst })
        rtrP50        = Get-Median @($group | ForEach-Object { $_.rtrP50 })
        rtrP95        = Get-Median @($group | ForEach-Object { $_.rtrP95 })
        rtrWorst      = Get-Median @($group | ForEach-Object { $_.rtrWorst })
        committed     = Get-Median @($group | ForEach-Object { [double]$_.committed })
        failed        = Get-Median @($group | ForEach-Object { [double]$_.failed })
        pendingHW     = Get-Median @($group | ForEach-Object { [double]$_.pendingHW })
        inFlightHW    = Get-Median @($group | ForEach-Object { [double]$_.inFlightHW })
        fairness      = Get-Median @($group | ForEach-Object { [double]$_.fairness })
        aging         = Get-Median @($group | ForEach-Object { [double]$_.aging })
        residentBytes = Get-Median @($group | ForEach-Object { [double]$_.residentBytes })
        cancelWaste   = Get-Median @($group | ForEach-Object { [double]$_.cancelWaste })
    }
}

# A/B 判定：唯一变化是预算/加载模式；阈值 = max(3%, 2×baseline 的 run 间 relMAD)。
$baseline = $aggregated['serial-burst']
$comparisons = @()
foreach ($cell in $aggregated.Keys) {
    if ($cell -eq 'serial-burst') { continue }
    $candidate = $aggregated[$cell]
    $relMad = [double]$baseline.frameWorstRelMad
    $threshold = [math]::Max(0.03, 2.0 * $relMad)
    $delta = if ($baseline.frameWorst -gt 0) { ($candidate.frameWorst - $baseline.frameWorst) / $baseline.frameWorst } else { 0.0 }
    $verdict = if ([math]::Abs($delta) -le $threshold) { 'noise' } elseif ($delta -lt 0) { 'accept (worst frame lower)' } else { 'reject (worst frame higher)' }
    $comparisons += [pscustomobject]@{
        cell            = $cell
        budgetMiB       = $candidate.budgetMiB
        budgetRequests  = $candidate.budgetRequests
        worstFrameMs    = [math]::Round($candidate.frameWorst, 3)
        worstFrameDelta = [math]::Round($delta, 4)
        rtrP95Ms        = [math]::Round($candidate.rtrP95, 1)
        inFlightHW      = [int]$candidate.inFlightHW
        threshold       = [math]::Round($threshold, 4)
        verdict         = $verdict
    }
}

$summary = [ordered]@{
    schema          = 'm7-load-sweep/1'
    generatedUtc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    inputDirectory  = $input
    runs            = $runs.Count
    thresholds      = [ordered]@{ baseline = 'serial-burst'; rule = 'max(3%, 2x baseline run-to-run relMAD)'; frameWorstRelMad = [math]::Round([double]$baseline.frameWorstRelMad, 4) }
    cells           = $aggregated
    comparisons     = $comparisons
    perRun          = @($cells.Keys | ForEach-Object { $cells[$_] })
}
[System.IO.File]::WriteAllText($summaryFile, (($summary | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# M7-07 上传预算 sweep 报告（E-M7-LOAD-002）')
$lines.Add('')
$lines.Add(('- 输入：{0}（{1} 个 run）' -f $input, $runs.Count))
$lines.Add(('- 场景：m7-streaming + assets/tests/m7/streaming-burst.bin（66 请求压到 5 个突发帧）；唯一变量 = 加载模式与预算三约束'))
$lines.Add(('- 判定规则：{0}（baseline=serial-burst，frameWorstRelMad={1}）' -f $summary.thresholds.rule, $summary.thresholds.frameWorstRelMad))
$lines.Add('')
$lines.Add('| cell | 预算 | 帧中位 (ms) | 帧 p99 (ms) | **最坏帧 (ms)** | rtrP95 (ms) | 在飞高水位 | committed | 驻留 (MB) | aging |')
$lines.Add('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
foreach ($cell in $aggregated.Keys) {
    $row = $aggregated[$cell]
    $budget = if ($row.loaderMode -eq 'async-budget-pipeline') { ('{0} MiB / {1} ms / {2} req' -f $row.budgetMiB, $row.budgetCpuMs, $row.budgetRequests) } else { '串行（无预算）' }
    $lines.Add(('| {0} | {1} | {2:N2} | {3:N2} | **{4:N2}** | {5:N1} | {6:N0} | {7:N0} | {8:N2} | {9:N0} |' -f `
        $cell, $budget, $row.frameMedian, $row.frameP99, $row.frameWorst, $row.rtrP95, $row.inFlightHW, $row.committed, ($row.residentBytes / 1MB), $row.aging))
}
$lines.Add('')
$lines.Add('## A/B 判定')
$lines.Add('')
$lines.Add('| cell | 最坏帧变化 | 阈值 | 判定 |')
$lines.Add('|---|---:|---:|---|')
foreach ($comparison in $comparisons) {
    $lines.Add(('| {0} | {1:P1} | {2:P1} | {3} |' -f $comparison.cell, $comparison.worstFrameDelta, $comparison.threshold, $comparison.verdict))
}
$lines.Add('')
$lines.Add('## 口径与边界')
$lines.Add('')
$lines.Add('- 最坏帧 = measured 段内单帧 cpuFrameMs 的最大值：串行基线把突发读+校验全部塞进请求帧，')
$lines.Add('  异步模式把读取/decode/上传摊到多帧，因此该指标直接对应"上传预算是否真的避免了卡帧"。')
$lines.Add('- rtr（request-to-ready）= 每请求 enqueue→Ready；串行≈0 是因为它在请求帧内同步完成（代价就是最坏帧）。')
$lines.Add('- 驻留 = 已提交上传资源字节（全部 66 个资产常驻时 ≈6.6 MB，与预算无关）；瞬时并发见"在飞高水位"。')
$lines.Add('- 前台 gate 关闭（自动化会话），逐 run 的 foregroundRatio 在 run-notes.json；帧时间类指标带 DWM 节流风险。')
$lines.Add('- 本 sweep 不含热重载（脚本每个资产只请求一次）：公平性份额与 reload 提交由 A20 的测试覆盖，')
$lines.Add('  端到端 reload 实验（E-M7-LOAD-003）见 BACKLOG。')
[System.IO.File]::WriteAllText($reportFile, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("summary: {0}" -f $summaryFile)
Write-Host ("report : {0}" -f $reportFile)
foreach ($comparison in $comparisons) {
    Write-Host ("{0,-16} worst={1,7:N2} ms delta={2,8:P1} -> {3}" -f $comparison.cell, $comparison.worstFrameMs, $comparison.worstFrameDelta, $comparison.verdict)
}
