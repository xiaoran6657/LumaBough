<#
.SYNOPSIS
  M7-08 布局 sweep 汇总：E-M7-LAYOUT-001/002 的机读摘要与报告。

.DESCRIPTION
  对每个 (数据集, 布局) 的多次 run 取中位数（run 间离散度用 relMAD 记录），输出：
    * <SummaryPath>：机读摘要（逐 cell 指标 + 等价性证据 + A/B 判定）
    * <ReportPath> ：人读报告（表格 + 判定 + 口径）
  判定阈值沿用 M7-05 的噪声规则：max(3%, 2×baseline 的 run 间 relMAD)；
  A/B 只在同一数据集内进行（布局是唯一变量）。

.EXAMPLE
  pwsh tools/performance/summarize_m7_layout_sweep.ps1 `
    -InputDirectory out/m7-08/sweep `
    -SummaryPath out/performance/layout-sweep-summary.json `
    -ReportPath out/performance/layout-sweep-report.md
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

$runs = @(Get-ChildItem -LiteralPath $input -Recurse -Filter 'run.json' | Sort-Object FullName)
if ($runs.Count -eq 0) { throw "no run.json under $input" }

$cells = [ordered]@{}
foreach ($file in $runs) {
    $cell = ($file.Directory.Name -replace '-r\d+$', '')
    $data = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $layoutWorst = 0.0
    foreach ($sample in $data.samples) {
        if ([double]$sample.layoutCullMs -gt $layoutWorst) { $layoutWorst = [double]$sample.layoutCullMs }
    }
    # 代理数（= 场景实体数）来自 run-notes：它是控制变量而非逐帧指标，不进 schema。
    $proxyCount = 0
    $notesPath = Join-Path $file.Directory.FullName 'run-notes.json'
    if (Test-Path -LiteralPath $notesPath) {
        $notes = Get-Content -LiteralPath $notesPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($null -ne $notes.layoutProxies) { $proxyCount = [int]$notes.layoutProxies }
    }
    $entry = [pscustomobject]@{
        run                  = $file.Directory.Name
        status               = $data.correctness.status
        dataset              = ($cell -replace '-aos$|-hot-cold$|-soa$', '')
        layout               = $data.layoutVariant
        scene                = $data.sceneName
        entityCount          = $proxyCount
        visible              = $data.correctness.visibleCount
        layoutCullP50        = [double]$data.statistics.layoutCullMs.median
        layoutCullP95        = [double]$data.statistics.layoutCullMs.p95
        layoutCullP99        = [double]$data.statistics.layoutCullMs.p99
        layoutCullWorst      = $layoutWorst
        productionCullP50    = [double]$data.statistics.cullMs.median
        frameP50             = [double]$data.statistics.cpuFrameMs.median
        frameP95             = [double]$data.statistics.cpuFrameMs.p95
        hitch16              = [int]$data.statistics.hitches.over16_67
        hitch33              = [int]$data.statistics.hitches.over33_33
        residentBytes        = [double]$data.statistics.residentBytes.median
        layoutCheckFrames    = [int]$data.statistics.layoutCheckFrames
        layoutCheckMismatch  = [int]$data.statistics.layoutCheckMismatches
        layoutSemanticHash   = $data.correctness.layoutSemanticHash
        packetSequenceHash   = $data.correctness.packetSequenceHash
        metricsSha256        = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    if (-not $cells.Contains($cell)) { $cells[$cell] = [System.Collections.Generic.List[object]]::new() }
    $cells[$cell].Add($entry)
}

$aggregated = [ordered]@{}
foreach ($cell in $cells.Keys) {
    $group = @($cells[$cell])
    $aggregated[$cell] = [pscustomobject]@{
        runs              = $group.Count
        status            = if (($group | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) { 'PASS' } else { 'FAIL' }
        dataset           = $group[0].dataset
        layout            = $group[0].layout
        scene             = $group[0].scene
        entityCount       = Get-Median @($group | ForEach-Object { [double]$_.entityCount })
        visible           = Get-Median @($group | ForEach-Object { [double]$_.visible })
        layoutCullP50     = Get-Median @($group | ForEach-Object { $_.layoutCullP50 })
        layoutCullP95     = Get-Median @($group | ForEach-Object { $_.layoutCullP95 })
        layoutCullP99     = Get-Median @($group | ForEach-Object { $_.layoutCullP99 })
        layoutCullWorst   = Get-Median @($group | ForEach-Object { $_.layoutCullWorst })
        layoutCullRelMad  = Get-RelMad @($group | ForEach-Object { $_.layoutCullP95 })
        productionCullP50 = Get-Median @($group | ForEach-Object { $_.productionCullP50 })
        frameP50          = Get-Median @($group | ForEach-Object { $_.frameP50 })
        frameP95          = Get-Median @($group | ForEach-Object { $_.frameP95 })
        hitch16           = Get-Median @($group | ForEach-Object { [double]$_.hitch16 })
        hitch33           = Get-Median @($group | ForEach-Object { [double]$_.hitch33 })
        residentBytes     = Get-Median @($group | ForEach-Object { [double]$_.residentBytes })
        layoutCheckFrames = Get-Median @($group | ForEach-Object { [double]$_.layoutCheckFrames })
        layoutCheckMismatch = Get-Median @($group | ForEach-Object { [double]$_.layoutCheckMismatch })
        layoutSemanticHash = $group[0].layoutSemanticHash
        packetSequenceHash = $group[0].packetSequenceHash
    }
}

# A/B：同一数据集内，每个布局对照 aos（唯一变量是布局）。
$comparisons = @()
$datasets = $aggregated.Keys | ForEach-Object { $aggregated[$_].dataset } | Select-Object -Unique
foreach ($dataset in $datasets) {
    $baselineCell = "$dataset-aos"
    if (-not $aggregated.Contains($baselineCell)) { continue }
    $baseline = $aggregated[$baselineCell]
    $threshold = [math]::Max(0.03, 2.0 * [double]$baseline.layoutCullRelMad)
    foreach ($cell in $aggregated.Keys) {
        if ($aggregated[$cell].dataset -ne $dataset -or $cell -eq $baselineCell) { continue }
        $candidate = $aggregated[$cell]
        $delta = if ($baseline.layoutCullP95 -gt 0) {
            ($candidate.layoutCullP95 - $baseline.layoutCullP95) / $baseline.layoutCullP95
        } else { 0.0 }
        $verdict = if ([math]::Abs($delta) -le $threshold) { 'noise' }
                   elseif ($delta -lt 0) { 'accept (layout kernel faster)' }
                   else { 'reject (layout kernel slower)' }
        $equivalent = ($candidate.layoutSemanticHash -eq $baseline.layoutSemanticHash) -and
                      ($candidate.packetSequenceHash -eq $baseline.packetSequenceHash)
        $comparisons += [pscustomobject]@{
            dataset              = $dataset
            layout               = $candidate.layout
            baselineLayout       = 'aos'
            layoutCullP95Ms      = [math]::Round($candidate.layoutCullP95, 3)
            baselineCullP95Ms    = [math]::Round($baseline.layoutCullP95, 3)
            delta                = [math]::Round($delta, 4)
            threshold            = [math]::Round($threshold, 4)
            outputEquivalent     = $equivalent
            verdict              = $verdict
        }
    }
}

$summary = [ordered]@{
    schema        = 'm7-layout-sweep/1'
    generatedUtc  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    inputDirectory = $input
    runs          = $runs.Count
    thresholds    = [ordered]@{ baseline = 'aos（同一数据集内）'; rule = 'max(3%, 2x baseline run-to-run relMAD)' }
    cells         = $aggregated
    comparisons   = $comparisons
}
[System.IO.File]::WriteAllText($summaryFile, (($summary | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# M7-08 数据布局 sweep 报告（E-M7-LAYOUT-001）')
$lines.Add('')
$lines.Add(('- 输入：{0}（{1} 个 run）' -f $input, $runs.Count))
$lines.Add('- 唯一变量：`--layout`（aos / hot-cold / soa）；生产 packet 构建路径不变，跨布局只改"每帧并排运行的布局内核"')
$lines.Add(('- 判定规则：{0}' -f $summary.thresholds.rule))
$lines.Add('')
$lines.Add('| 数据集 | 布局 | 实体 | 可见 | 布局内核 p50 (ms) | 布局内核 p95 (ms) | 生产 cull p50 (ms) | 帧 p50 (ms) | 校验帧 | 语义 hash |')
$lines.Add('|---|---|---:|---:|---:|---:|---:|---:|---:|---|')
foreach ($cell in $aggregated.Keys) {
    $row = $aggregated[$cell]
    $lines.Add(('| {0} | {1} | {2:N0} | {3:N0} | {4:N3} | {5:N3} | {6:N3} | {7:N2} | {8:N0} | `{9}` |' -f `
        $row.dataset, $row.layout, $row.entityCount, $row.visible, $row.layoutCullP50, $row.layoutCullP95,
        $row.productionCullP50, $row.frameP50, $row.layoutCheckFrames, $row.layoutSemanticHash))
}
$lines.Add('')
$lines.Add('## A/B 判定（同一数据集内以 aos 为基线）')
$lines.Add('')
$lines.Add('| 数据集 | 布局 | p95 (ms) | 基线 p95 (ms) | 变化 | 阈值 | 输出等价 | 判定 |')
$lines.Add('|---|---|---:|---:|---:|---:|---|---|')
foreach ($comparison in $comparisons) {
    $lines.Add(('| {0} | {1} | {2:N3} | {3:N3} | {4:P1} | {5:P1} | {6} | {7} |' -f `
        $comparison.dataset, $comparison.layout, $comparison.layoutCullP95Ms, $comparison.baselineCullP95Ms,
        $comparison.delta, $comparison.threshold, $(if ($comparison.outputEquivalent) { '是' } else { '否' }), $comparison.verdict))
}
$lines.Add('')
$lines.Add('## 口径与边界')
$lines.Add('')
$lines.Add('- **布局内核** = 提取 + 保守包围球变换 + 视锥分类 + 收集（`layoutCullMs`），在计时区外只做逐帧字段刷新。')
$lines.Add('- **生产 cull** = 生产 `BuildRenderPacket` 的 culling 段（不随布局变化）——它是"单变量成立"的内部对照。')
$lines.Add('- **帧时间**包含布局内核（同帧并排运行），因此布局 run 的帧时间**不可**与不含布局的基线 run 直接比较。')
$lines.Add('- **输出等价**：每个 run 的 `layoutSemanticHash`/`packetSequenceHash` 跨布局一致，且 `layoutCheckMismatches=0`')
$lines.Add('  （benchmark 模式每 256 帧采样校验，非 benchmark 每帧校验；不一致会直接让 run 失败）。')
$lines.Add('- 前台 gate 关闭（自动化会话），逐 run `foregroundRatio` 在 run-notes.json；帧时间类指标带 DWM 节流风险。')
$lines.Add('- 数据集里的 `visible` 是**实测**可见数：`--visible-ratio` 只是场景输入目标，实际可见率由固定分布')
$lines.Add('  与视锥决定（三个数据集都约 52%）；可见率作为独立维度的端到端 sweep 记在 BACKLOG `M7-LAYOUT-VISIBILITY`。')
$lines.Add('- **hot-cold 是"同代码对照"**：它的扫描数组与 AoS 逐字节同构、走同一个内核函数，因此它的差值')
$lines.Add('  直接反映本次配置的经验噪声下限（不同分配地址/缓存着色），用于判读 SoA 的差值是否真实。')
[System.IO.File]::WriteAllText($reportFile, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("summary: {0}" -f $summaryFile)
Write-Host ("report : {0}" -f $reportFile)
foreach ($comparison in $comparisons) {
    Write-Host ("{0,-24} {1,-9} p95={2,7:N3} ms delta={3,8:P1} -> {4}" -f `
        $comparison.dataset, $comparison.layout, $comparison.layoutCullP95Ms, $comparison.delta, $comparison.verdict)
}
