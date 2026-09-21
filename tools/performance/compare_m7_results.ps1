# ============================================================================
# compare_m7_results.ps1 — M7 raw 结果比较（M7-01 第 6 步 / M7-09 A/B）
#
# 先校验再比较，任何输入不满足契约立即以退出码 2 失败（INVALID），不产出"看起来
# 通过"的结论：
#   * schemaVersion ∈ 1..4（缺失的新字段按更早版本语义回填）；
#   * correctness.status == PASS；样本数 == measuredFrames（raw 与 statistics 双查）；
#   * 目标指标在 statistics 里有分布，且值有限、中位数 > 0；
#   * 控制变量逐字段一致（scene/rhi/workers/chunkSize/seed/warmup/measured/
#     sceneManifestSha256/renderDrawLimit/shadowDrawLimit/camera/stream/前台 gate/
#     分辨率/VSync/上传预算/loaderMode/chunkReserve…）；
#   * 身份字段（executableSha256/sourceCommit）默认也要一致，源码改动的 A/B 需显式
#     -AllowExecutableChange 放行并记录；
#   * **唯一变量**：两侧允许不同的字段必须显式声明（-TreatmentField），未声明的差异
#     直接 INVALID（防止"顺手多改了一处"）。
#
# 统计口径（与 docs/architecture/DECISIONS.md 一致）：
#   * 每个 run 内：median / MAD / p95 / p99 / hitch 计数（取自 raw 的 statistics 分布）；
#   * run 间：对每 run 的 median 再取 median，噪声下限 = max(3%, 2 × run 间 relativeMAD)；
#   * 目标指标改善必须超过噪声下限；protected 指标退化不得超过 -ProtectedRegressionPercent；
#   * 漂移：按执行顺序（driver summary 的 startedUtc，缺失时用 runIndex）做每侧前半/后半
#     对比，漂移超过噪声下限时该 cell 降级为 INCONCLUSIVE（环境/时间漂移，不是"结果"）。
#
# 结果类别（09 文档）：
#   * ACCEPTED     ：改善 > 噪声下限、protected 全通过、无漂移；
#   * REJECTED     ：明显退化、protected 违规，或测量足够紧（噪声下限 ≤ 5%）但没有足够改善；
#   * INCONCLUSIVE ：噪声下限 > 5%（分辨不出有意义的效应）、漂移超限或数据缺失；
#   * INVALID      ：控制变量/构建/场景/hash/采集程序错误（throw + 退出码 2，必须重测）。
#
# 用法：
#   pwsh tools/performance/compare_m7_results.ps1 -Baseline out/m7-05/serial -Candidate out/m7-05/parallel `
#        -TreatmentField packetBuildMode -OutputJson out/m7-09/compare.json
#   pwsh tools/performance/compare_m7_results.ps1 -Baseline out/m7-08/sweep -BaselineFilter '*-aos' `
#        -Candidate out/m7-08/sweep -CandidateFilter '*-hot-cold' -TargetMetric layoutCullMs
#   pwsh tools/performance/compare_m7_results.ps1 -Baseline <runs> -Candidate <runs> -SelfCheck
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Baseline,
    [Parameter(Mandatory = $true)][string]$Candidate,
    [double]$ProtectedRegressionPercent = 5.0,
    [string]$TargetMetric = 'cpuFrameMs',
    [string[]]$TreatmentField = @(),
    [switch]$AllowExecutableChange,
    [switch]$SelfCheck,
    [double]$NoiseFloorPercentOverride = 0.0,
    [string]$BaselineFilter = '*',
    [string]$CandidateFilter = '*',
    # cell 配对键（默认 scene+rhi+workers+chunkSize）。当治疗本身包含某个控制变量
    # （例如 serial 基线按契约只能在 w1 采集 ⇒ workers 必须从配对键里去掉）时，用本参数
    # 显式放宽；被放宽的字段必须同时列进 -TreatmentField，否则 INVALID。
    [string[]]$MatchOn = @(),
    [string]$OutputJson = ''
)

$ErrorActionPreference = 'Stop'
# Python/其他 PowerShell 宿主可能传入不同版本的 PSModulePath；从本进程 PSHOME 加载。
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop


# 任何契约违规（throw）都以退出码 2 结束并打印 INVALID 前缀：PowerShell 的 -File
# 默认把未捕获的终止错误映射成退出码 1，会与"结果被 REJECTED"混淆。
trap {
    Write-Host ("INVALID: " + $_.Exception.Message)
    exit 2
}

# -File 调用时数组参数只能写成逗号分隔单串，这里统一拆开（与其它 M7 脚本的 -Only 一致）。
$TreatmentField = @($TreatmentField | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } |
                    Where-Object { $_ })

# 配对键：默认 scene+rhi+workers+chunkSize；-MatchOn 放宽时，被去掉的字段必须已在
# -TreatmentField 里声明（否则是把"顺手多改的变量"藏进配对键里）。
$defaultMatchOn = @('sceneName', 'rhi', 'workers', 'chunkSize')
# 注意：$matchOn 与 [string[]]$MatchOn 是同一个变量（大小写不敏感），所以先把调用方
# 传入的值读进 $matchOnRequested，再决定最终配对键；绝不能在读完之前写 $matchOn。
$matchOnRequested = @($MatchOn | ForEach-Object { $_ -split '[,\s]+' } | ForEach-Object { $_.Trim() } |
                      Where-Object { $_ })
$matchOn = if ($matchOnRequested.Count -gt 0) { $matchOnRequested } else { $defaultMatchOn }
if ($matchOnRequested.Count -gt 0) {
    foreach ($field in $matchOn) {
        if ($defaultMatchOn -notcontains $field) {
            throw "unknown -MatchOn field '$field' (expected subset of: $($defaultMatchOn -join ', '))"
        }
    }
    foreach ($removed in ($defaultMatchOn | Where-Object { $matchOn -notcontains $_ })) {
        if ($TreatmentField -notcontains $removed) {
            throw "relaxed pairing field '$removed' must also be declared in -TreatmentField"
        }
    }
}

# protected 指标（09 文档）：GPU frame、内存、allocation；hitch 与目标指标 p95 另算。
$protectedMetrics = @('gpuFrameMs', 'residentBytes', 'allocationCount')

# 控制变量：两侧必须逐字段一致（字符串全等）。
$controlFields = @('sceneName', 'rhi', 'workers', 'chunkSize', 'seed', 'warmupFrames', 'measuredFrames',
                   'sceneManifestSha256', 'renderDrawLimit', 'shadowDrawLimit', 'uploadMiBPerFrame', 'loaderMode',
                   'uploadBudgetCpuMs', 'uploadBudgetRequests', 'uploadBudgetReloadPercent', 'uploadAgingThresholdMs',
                   'foregroundGate', 'chunkReserve', 'vsync', 'cameraPathSha256', 'streamScriptSha256')

# 身份字段：默认也要求一致（源码改动的 A/B 需 -AllowExecutableChange）。
$identityFields = @('executableSha256', 'sourceCommit')

# 可能成为"唯一变量"的运行期开关（-TreatmentField 的合法取值）。
$treatableFields = @('packetBuildMode', 'schedulerMode', 'chunkReserve', 'foregroundGate', 'layoutVariant',
                     'loaderMode', 'uploadMiBPerFrame', 'uploadBudgetCpuMs', 'uploadBudgetRequests',
                     'uploadBudgetReloadPercent', 'uploadAgingThresholdMs', 'workers', 'chunkSize',
                     # M7-RP-ORDER-001 起：实现变体 A/B（同 commit 的两次构建，只改一处实现）
                     # 用 variant 作治疗字段；配合 -AllowExecutableChange 记录两侧 exe SHA。
                     'variant')

function Read-Run {
    param([string]$Path)
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $hash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
    $json = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    [pscustomobject]@{ Path = $resolved; Hash = $hash; Data = $json }
}

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

function Get-Hitches {
    param([double[]]$Values)
    # M7-HITCH-METRIC：绝对阈值（16.67/33.33/50 ms）在本项目的**所有** M7 场景都饱和——m7-cpu-scale
    # 帧中位 ~65 ms、m7-streaming / load-sweep ~33–35 ms，都 >16.67，于是计数 = 总帧数，判据失去分辨力。
    # 补一组**尺度无关**的相对阈值：帧 > 中位 + k × MAD（k = 3/5/10），与项目噪声下限同一套词汇
    # （`max(3%, 2×MAD)`）；实测这些场景下 +3MAD 计数 14–109、+5MAD 5–59，全部非零且有区分度。
    # 另保留比例口径（>1.5/2/3 × 中位）作为参照：实测在本项目的hitch形态下过于粗糙（多为 0）。
    # 饱和判定与切换规则在 Get-CellStats / Compare-Cell（数据驱动，不按指标名硬编码）。
    $median = if ($Values.Count -gt 0) { Get-Median $Values } else { 0.0 }
    $mad = if ($Values.Count -gt 0) { Get-Mad -Values $Values -Median $median } else { 0.0 }
    $threshold = { param([double]$k) $median + $k * $mad }
    [pscustomobject]@{
        over16_67 = @($Values | Where-Object { $_ -gt 16.67 }).Count
        over33_33 = @($Values | Where-Object { $_ -gt 33.33 }).Count
        over50    = @($Values | Where-Object { $_ -gt 50.0 }).Count
        overMad3  = if ($mad -gt 0.0) { @($Values | Where-Object { $_ -gt (& $threshold 3) }).Count } else { 0 }
        overMad5  = if ($mad -gt 0.0) { @($Values | Where-Object { $_ -gt (& $threshold 5) }).Count } else { 0 }
        overMad10 = if ($mad -gt 0.0) { @($Values | Where-Object { $_ -gt (& $threshold 10) }).Count } else { 0 }
        overMedian1_5 = if ($median -gt 0.0) { @($Values | Where-Object { $_ -gt 1.5 * $median }).Count } else { 0 }
        overMedian2x  = if ($median -gt 0.0) { @($Values | Where-Object { $_ -gt 2.0 * $median }).Count } else { 0 }
        overMedian3x  = if ($median -gt 0.0) { @($Values | Where-Object { $_ -gt 3.0 * $median }).Count } else { 0 }
        medianMs  = $median
        madMs     = $mad
    }
}

function Get-Distribution {
    param($Data, [string]$Metric)
    $box = $Data.statistics
    if ($null -eq $box -or $null -eq $box.$Metric) { return $null }
    return $box.$Metric
}

function Invoke-Validate {
    param($Run, [string]$Label)
    $data = $Run.Data
    # v1 = M7-01 串行基线；v2 = M7-05 起（并行构建控制变量与等待段）；
    # v3 = M7-07 起（上传预算控制变量与上传证据）；v4 = M7-08 起（布局实验）。
    # 四者都可读：缺失字段按更早版本语义回填，并把"该版本不存在此字段"记进
    # AbsentFields——跨版本比较时这些字段不作相等要求（否则会拿不存在的概念误判）。
    if ($data.schemaVersion -notin 1, 2, 3, 4) {
        throw "[$Label] unsupported schemaVersion in $($Run.Path)"
    }
    $absent = @()
    if ($null -eq $data.packetBuildMode) { $absent += 'packetBuildMode'; $data | Add-Member -NotePropertyName packetBuildMode -NotePropertyValue 'serial' -Force }
    if ($null -eq $data.schedulerMode) { $absent += 'schedulerMode'; $data | Add-Member -NotePropertyName schedulerMode -NotePropertyValue 'none' -Force }
    foreach ($field in 'uploadBudgetCpuMs', 'uploadBudgetRequests', 'uploadBudgetReloadPercent', 'uploadAgingThresholdMs') {
        if ($null -eq $data.$field) { $absent += $field; $data | Add-Member -NotePropertyName $field -NotePropertyValue 0 -Force }
    }
    if (-not $data.layoutVariant) { $absent += 'layoutVariant'; $data | Add-Member -NotePropertyName layoutVariant -NotePropertyValue 'aos' -Force }
    if (-not $data.loaderMode) { $absent += 'loaderMode'; $data | Add-Member -NotePropertyName loaderMode -NotePropertyValue 'serial-sync-read-validate' -Force }
    foreach ($field in 'foregroundGate', 'chunkReserve', 'vsync') {
        if ($null -eq $data.$field) { $absent += $field; $data | Add-Member -NotePropertyName $field -NotePropertyValue $false -Force }
    }
    foreach ($field in 'cameraPathSha256', 'streamScriptSha256', 'executableSha256', 'sourceCommit', 'runOrderNote') {
        if ($null -eq $data.$field) { $absent += $field; $data | Add-Member -NotePropertyName $field -NotePropertyValue '' -Force }
    }
    if (-not $data.correctness -or $data.correctness.status -ne 'PASS') {
        throw "[$Label] correctness is not PASS in $($Run.Path)"
    }
    if (@($data.samples).Count -ne [int]$data.measuredFrames) {
        throw "[$Label] sample count != measuredFrames in $($Run.Path)"
    }
    if ($null -ne $data.statistics -and [int]$data.statistics.sampleCount -ne [int]$data.measuredFrames) {
        throw "[$Label] statistics.sampleCount != measuredFrames in $($Run.Path)"
    }
    [double[]]$frames = @($data.samples | ForEach-Object { [double]$_.cpuFrameMs })
    foreach ($value in $frames) {
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0.0) {
            throw "[$Label] frame samples must be finite and positive; got $value"
        }
    }
    $dist = Get-Distribution -Data $data -Metric $TargetMetric
    if ($null -eq $dist) {
        throw "[$Label] target metric '$TargetMetric' missing from statistics in $($Run.Path)"
    }
    foreach ($field in 'median', 'mad', 'p95', 'p99', 'maximum') {
        $value = [double]$dist.$field
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0.0) {
            throw "[$Label] target metric '$TargetMetric'.$field must be finite and non-negative in $($Run.Path)"
        }
    }
    if ([double]$dist.median -le 0.0) {
        throw "[$Label] target metric '$TargetMetric'.median must be positive in $($Run.Path)"
    }
    return $absent
}

function Get-CellKey ($Data) {
    # 配对键默认 scene+rhi+workers+chunkSize；-MatchOn 可显式放宽（见参数说明）。
    # 治疗字段（packetBuildMode/schedulerMode/layoutVariant/…）不进配对键，
    # 否则两侧无法配对；它们由 Assert-OnlyDeclaredDifferences 管。
    $parts = @()
    foreach ($field in $matchOn) {
        $parts += ('{0}={1}' -f $field, $Data.$field)
    }
    return ($parts -join '|')
}

function Resolve-RunFiles {
    param([string]$Path, [string]$Filter)
    if (Test-Path -LiteralPath $Path -PathType Container) {
        # 两种采集布局都支持：
        #   a) run_m7_matrix.ps1：<scene>-<rhi>-w…-c…-r<N>.json；
        #   b) sweep 驱动：<cell>-r<N>/run.json（一层子目录）。
        $rootFull = (Resolve-Path -LiteralPath $Path).Path
        $flatFiles = @(Get-ChildItem -LiteralPath $Path -Filter '*.json' -File |
            Where-Object { $_.Name -match '-w\d+-c\d+-r\d+\.json$' -or $_.Name -eq 'run.json' })
        $nestedFiles = @(Get-ChildItem -LiteralPath $Path -Recurse -Filter 'run.json' -File |
            Where-Object { $_.DirectoryName -ne $rootFull })
        $files = @($flatFiles) + @($nestedFiles)
        if ($Filter -ne '*') {
            $matching = @($files | Where-Object { $_.Directory.Name -like $Filter })
            if ($matching.Count -eq 0) { $matching = @($files | Where-Object { $_.Name -like $Filter }) }
            $files = $matching
        }
        if ($files.Count -eq 0) { throw "no raw run JSON found under $Path (filter '$Filter')" }
        return @($files)
    }
    return @(Get-Item -LiteralPath $Path)
}

# driver summary（可选）：提供执行顺序（startedUtc）与逐 run 的 metricsSha256。
function Get-DriverOrder {
    param([string]$Root, [string]$Filter)
    $map = @{}
    $summaryPath = Join-Path $Root 'sweep-driver-summary.json'
    if (-not (Test-Path -LiteralPath $summaryPath)) { return $map }
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    $sequence = 0
    foreach ($entry in @($summary.cells | Sort-Object startedUtc)) {
        ++$sequence
        $key = '{0}|{1}' -f $entry.cell, $entry.runIndex
        $map[$key] = [pscustomobject]@{ sequence = $sequence; startedUtc = $entry.startedUtc; metricsSha256 = $entry.metricsSha256; attempts = $entry.attempts }
    }
    return $map
}

# M7-ORDER-HISTORY：执行顺序的**可见性**。漂移分析要把每侧按执行顺序分半；顺序来自
# 驱动摘要（`sweep-driver-summary.json`，M7-09 起由驱动写 `executionOrder`/`startedUtc`）。
# 早于 M7-09 的批次（M7-01…08）没有这份记录，此时 `Get-Drift` 只能按 `runIndex` 回退——
# 那是**不可信**的分半顺序（交错采集时 runIndex ≠ 执行顺序），必须显式标注而不是静默使用。
function Get-DriverOrderInfo {
    param([string]$Root)
    $summaryPath = Join-Path $Root 'sweep-driver-summary.json'
    $available = Test-Path -LiteralPath $summaryPath
    $count = 0
    $hasExplicitOrder = $false
    if ($available) {
        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        $count = @($summary.cells).Count
        $hasExplicitOrder = $null -ne $summary.executionOrder -and @($summary.executionOrder).Count -gt 0
        $available = $count -gt 0
    }
    [pscustomobject]@{
        available          = $available
        entries            = $count
        explicitOrderField = $hasExplicitOrder
        summaryPath        = $summaryPath
        source             = if ($available -and $hasExplicitOrder) { 'driver-execution-order' }
                             elseif ($available) { 'driver-startedUtc' }
                             else { 'runIndex-fallback' }
    }
}

function Load-Collection {
    param([string]$Path, [string]$Label, [string]$Filter)
    $files = Resolve-RunFiles -Path $Path -Filter $Filter
    $runs = @($files | ForEach-Object { Read-Run $_.FullName })
    foreach ($run in $runs) {
        $absent = @(Invoke-Validate -Run $run -Label $Label)
        $run | Add-Member -NotePropertyName AbsentFields -NotePropertyValue $absent -Force
    }
    $cells = @{}
    foreach ($run in $runs) {
        $key = Get-CellKey $run.Data
        if (-not $cells.ContainsKey($key)) { $cells[$key] = [System.Collections.Generic.List[object]]::new() }
        $cells[$key].Add($run)
    }
    return [pscustomobject]@{ Label = $Label; Runs = $runs; Cells = $cells; Root = $Path; Filter = $Filter }
}

function Test-Controls {
    param($BaselineRun, $CandidateRun, [string]$Key)
    $fields = @($controlFields)
    foreach ($field in $identityFields) {
        if ($AllowExecutableChange) { continue }
        $fields += $field
    }
    $skipped = @()
    foreach ($field in $fields) {
        if ($TreatmentField -contains $field) { continue }
        # 跨 schema 版本：某一侧不存在该字段（更早版本的 schema 没有这个概念）时
        # 不做相等要求，记入 notComparable 供报告说明。
        if ((@($BaselineRun.AbsentFields) -contains $field) -or (@($CandidateRun.AbsentFields) -contains $field)) {
            $skipped += $field
            continue
        }
        $a = $BaselineRun.Data.$field
        $b = $CandidateRun.Data.$field
        if ("$a" -ne "$b") { throw "control mismatch for $Key : $field ('$a' vs '$b')" }
    }
    return $skipped
}

# 唯一变量强制：未声明为治疗的差异一律 INVALID。
function Assert-OnlyDeclaredDifferences {
    param($BaselineRun, $CandidateRun, [string]$Key)
    $differences = @()
    foreach ($field in @($treatableFields) + @($identityFields)) {
        if ((@($BaselineRun.AbsentFields) -contains $field) -or (@($CandidateRun.AbsentFields) -contains $field)) {
            continue # 更早的 schema 没有这个字段：不存在"改动了它"的可能
        }
        $a = $BaselineRun.Data.$field
        $b = $CandidateRun.Data.$field
        if ("$a" -ne "$b") { $differences += $field }
    }
    foreach ($field in $differences) {
        if (($field -eq 'executableSha256' -or $field -eq 'sourceCommit') -and $AllowExecutableChange) { continue }
        if (@($TreatmentField) -notcontains $field) {
            throw "single-variable violation for $Key : '$field' differs but is not in -TreatmentField"
        }
    }
    foreach ($field in $TreatmentField) {
        if (@($treatableFields) -notcontains $field -and @($identityFields) -notcontains $field) {
            throw "unknown -TreatmentField '$field'"
        }
    }
    return $differences
}

function Get-CellStats {
    param($Runs)
    $medians = [System.Collections.Generic.List[double]]::new()
    $p95s = [System.Collections.Generic.List[double]]::new()
    $p99s = [System.Collections.Generic.List[double]]::new()
    $mads = [System.Collections.Generic.List[double]]::new()
    $hitch16 = 0; $hitch33 = 0; $hitch50 = 0
    # M7-HITCH-METRIC：相对 hitch（帧 > 中位 + k×MAD）与总帧数一起累计，用于饱和判定与切换；
    # madZeroRuns 记录退化 run（整段恒定帧长 ⇒ MAD=0 ⇒ 相对判据同样无分辨力）。
    $hitchMad3 = 0; $hitchMad5 = 0; $hitchMad10 = 0
    $hitchMedian2x = 0; $hitchMedian3x = 0; $frameTotal = 0; $madZeroRuns = 0
    $protected = @{}
    foreach ($name in $protectedMetrics) { $protected[$name] = [System.Collections.Generic.List[double]]::new() }
    foreach ($run in $Runs) {
        $dist = Get-Distribution -Data $run.Data -Metric $TargetMetric
        $medians.Add([double]$dist.median)
        $mads.Add([double]$dist.mad)
        $p95s.Add([double]$dist.p95)
        $p99s.Add([double]$dist.p99)
        $hitches = Get-Hitches @(@($run.Data.samples | ForEach-Object { [double]$_.cpuFrameMs }))
        $hitch16 += $hitches.over16_67; $hitch33 += $hitches.over33_33; $hitch50 += $hitches.over50
        $hitchMad3 += $hitches.overMad3; $hitchMad5 += $hitches.overMad5; $hitchMad10 += $hitches.overMad10
        $hitchMedian2x += $hitches.overMedian2x; $hitchMedian3x += $hitches.overMedian3x
        $frameTotal += [int]$run.Data.measuredFrames
        if ($hitches.madMs -le 0.0) { $madZeroRuns += 1 }
        foreach ($name in $protectedMetrics) {
            $box = Get-Distribution -Data $run.Data -Metric $name
            if ($null -ne $box) { $protected[$name].Add([double]$box.median) }
        }
    }
    $medianList = @($medians); $madList = @($mads); $p95List = @($p95s); $p99List = @($p99s)
    $runToRunMedian = Get-Median $medianList
    $runToRunMad = Get-Mad -Values $medianList -Median $runToRunMedian
    $relativeMad = if ($runToRunMedian -gt 0) { $runToRunMad / $runToRunMedian } else { 0.0 }
    $required = if ($NoiseFloorPercentOverride -gt 0.0) { $NoiseFloorPercentOverride }
                else { [Math]::Max(3.0, 200.0 * $relativeMad) }
    $protectedMedians = @{}
    foreach ($name in $protectedMetrics) {
        $protectedMedians[$name] = if ($protected[$name].Count) { Get-Median @($protected[$name]) } else { 0.0 }
    }
    # M7-GPUTIME-RESOLUTION：受保护指标需要**逐 run 的原始值**才能判断它自身的量测精度
    # （见 Get-PrecisionFloor：若指标在基线侧自己的 run 间极差就已 ≥ 判定阈值，它无法分辨
    # 阈值量级的效应）。这里把逐 run 值一并导出，判定逻辑在 Compare-Cell 里。
    $protectedRunMedians = @{}
    foreach ($name in $protectedMetrics) { $protectedRunMedians[$name] = @($protected[$name]) }
    [pscustomobject]@{
        runs                = @($Runs).Count
        medianMs            = $runToRunMedian
        runToRunMadMs       = $runToRunMad
        withinRunMadMs      = (Get-Median $madList)
        p95Ms               = (Get-Median $p95List)
        p99Ms               = (Get-Median $p99List)
        hitch16_67          = $hitch16
        hitch33_33          = $hitch33
        hitch50             = $hitch50
        hitchMad3           = $hitchMad3
        hitchMad5           = $hitchMad5
        hitchMad10          = $hitchMad10
        hitchMedian2x       = $hitchMedian2x
        hitchMedian3x       = $hitchMedian3x
        frameTotal          = $frameTotal
        madZeroRuns         = $madZeroRuns
        protectedMedians    = $protectedMedians
        protectedRunMedians = $protectedRunMedians
        p95RunValues        = $p95List
        requiredPercent     = $required
    }
}

# 精度下限（M7-GPUTIME-RESOLUTION 的裁定落地）：一个受保护指标只有在**它自己能被分辨**时
# 才能否决一次改善。判定条件（两者同时成立才算"仅报不判"）：
#   1) 基线侧逐 run 值的极差 / 中位数 ≥ 判定阈值——即指标自身噪声已经吃掉整个判定窗口；
#   2) 候选侧的值仍落在基线侧观测到的 [min, max] 内——即变化没有超出"该指标自己会到的范围"。
# 条件 2 是安全阀：真正的系统性退化（候选跑出基线历史范围）仍然照常判 REJECTED。
# 数据驱动、无按指标名硬编码，因此对稳定指标（例如 residentBytes）保护完全不变。
function Get-PrecisionFloor {
    param([double[]]$Values, [double]$BaselineValue, [double]$CandidateValue, [double]$LimitPercent)
    if ($Values.Count -lt 3 -or $BaselineValue -le 0.0) {
        return [pscustomobject]@{ limited = $false }
    }
    $min = ($Values | Measure-Object -Minimum).Minimum
    $max = ($Values | Measure-Object -Maximum).Maximum
    $spreadPercent = 100.0 * ($max - $min) / $BaselineValue
    $inside = ($CandidateValue -ge $min) -and ($CandidateValue -le $max)
    return [pscustomobject]@{
        limited               = (($spreadPercent -ge $LimitPercent) -and $inside)
        spreadPercent         = [math]::Round($spreadPercent, 3)
        baselineMin           = [math]::Round($min, 4)
        baselineMax           = [math]::Round($max, 4)
        candidateInsideRange  = [bool]$inside
    }
}

# 漂移：每侧按执行顺序排序后，前半 / 后半的 median 变化。顺序优先用 driver summary
# 的 startedUtc（sequence），缺失时退回 runIndex。
function Get-Drift {
    param($BaselineCellRuns, $CandidateCellRuns, [double]$RequiredPercent, $BaselineOrder, $CandidateOrder,
          $BaselineOrderInfo, $CandidateOrderInfo)
    function Sort-ByOrder($runs, $orderMap) {
        return @($runs | Sort-Object {
                $key = '{0}|{1}' -f ($_.Path | Split-Path -Parent | Split-Path -Leaf), $_.Data.runIndex
                if ($orderMap.ContainsKey($key)) { [int]$orderMap[$key].sequence } else { [int]$_.Data.runIndex }
            })
    }
    $a = Sort-ByOrder $BaselineCellRuns $BaselineOrder
    $b = Sort-ByOrder $CandidateCellRuns $CandidateOrder
    $orderSources = [ordered]@{
        baseline  = if ($null -ne $BaselineOrderInfo) { $BaselineOrderInfo.source } else { 'unknown' }
        candidate = if ($null -ne $CandidateOrderInfo) { $CandidateOrderInfo.source } else { 'unknown' }
    }
    $orderWarning = if ($orderSources.Values -contains 'runIndex-fallback') {
        '分半顺序来自 runIndex 回退：该批次未记录执行顺序（早于 M7-09 的采集），漂移分半不可信；重评旧结论应按交错协议重采'
    }
    else { '' }
    if ($a.Count -lt 2 -or $b.Count -lt 2) {
        return [pscustomobject]@{ available = $false; reason = 'insufficient runs per side (<2)'; flagged = $false;
                                  orderSources = $orderSources; orderWarning = $orderWarning }
    }
    $cutA = [int][Math]::Floor($a.Count / 2)
    $cutB = [int][Math]::Floor($b.Count / 2)
    $aFirst = Get-Median @(@($a | Select-Object -First $cutA) | ForEach-Object { [double](Get-Distribution -Data $_.Data -Metric $TargetMetric).median })
    $aSecond = Get-Median @(@($a | Select-Object -Last ($a.Count - $cutA)) | ForEach-Object { [double](Get-Distribution -Data $_.Data -Metric $TargetMetric).median })
    $bFirst = Get-Median @(@($b | Select-Object -First $cutB) | ForEach-Object { [double](Get-Distribution -Data $_.Data -Metric $TargetMetric).median })
    $bSecond = Get-Median @(@($b | Select-Object -Last ($b.Count - $cutB)) | ForEach-Object { [double](Get-Distribution -Data $_.Data -Metric $TargetMetric).median })
    $baselineDrift = if ($aFirst -gt 0) { 100.0 * ($aSecond - $aFirst) / $aFirst } else { 0.0 }
    $candidateDrift = if ($bFirst -gt 0) { 100.0 * ($bSecond - $bFirst) / $bFirst } else { 0.0 }
    $worst = [Math]::Max([Math]::Abs($baselineDrift), [Math]::Abs($candidateDrift))
    return [pscustomobject]@{
        available             = $true
        orderSources          = $orderSources
        orderWarning          = $orderWarning
        baselineDriftPercent  = [math]::Round($baselineDrift, 3)
        candidateDriftPercent = [math]::Round($candidateDrift, 3)
        thresholdPercent      = [math]::Round($RequiredPercent, 3)
        flagged               = ($worst -gt $RequiredPercent)
    }
}

# 注意：PowerShell 变量名大小写不敏感，且 [string] 参数会把赋值强制转型——
# 所以局部集合变量用 $baselineSide/$candidateSide，绝不能用 $baseline/$candidate
# （它们与 [string]$Baseline/$Candidate 是同一个变量，赋值时对象会被转成字符串）。
$baselineSide = Load-Collection -Path $Baseline -Label 'baseline' -Filter $BaselineFilter
$candidateSide = Load-Collection -Path $Candidate -Label 'candidate' -Filter $CandidateFilter
$baselineOrder = Get-DriverOrder -Root $baselineSide.Root -Filter $BaselineFilter
$candidateOrder = Get-DriverOrder -Root $candidateSide.Root -Filter $CandidateFilter
# M7-ORDER-HISTORY：顺序来源必须可见（缺失即 runIndex 回退，漂移分半不可信）。
$baselineOrderInfo = Get-DriverOrderInfo -Root $baselineSide.Root
$candidateOrderInfo = Get-DriverOrderInfo -Root $candidateSide.Root

$isSelfCheck = [bool]$SelfCheck
if (-not $isSelfCheck) {
    $aHashes = (@($baselineSide.Runs | ForEach-Object { $_.Hash }) | Sort-Object) -join ';'
    $bHashes = (@($candidateSide.Runs | ForEach-Object { $_.Hash }) | Sort-Object) -join ';'
    $isSelfCheck = ($aHashes -eq $bHashes)
}

$baselineCells = @($baselineSide.Cells.Keys | Sort-Object)
$candidateCells = @($candidateSide.Cells.Keys | Sort-Object)
if (($baselineCells -join ';') -ne ($candidateCells -join ';')) {
    throw "cell sets differ: baseline=[$($baselineCells -join ', ')] candidate=[$($candidateCells -join ', ')]"
}

if (@($TreatmentField).Count -eq 0 -and -not $isSelfCheck) {
    Write-Warning 'no -TreatmentField declared: any treatment-field difference will be INVALID'
}

$report = [System.Collections.Generic.List[object]]::new()
$anyRejected = $false
$anyAccepted = $false
$anyInconclusive = $false
foreach ($key in $baselineCells) {
    $aRuns = @($baselineSide.Cells[$key]); $bRuns = @($candidateSide.Cells[$key])
    if ($aRuns.Count -ne $bRuns.Count) {
        throw "run count mismatch for $key : $($aRuns.Count) vs $($bRuns.Count)"
    }
    $differences = @()
    $notComparable = @()
    for ($i = 0; $i -lt $aRuns.Count; ++$i) {
        $notComparable += Test-Controls -BaselineRun $aRuns[$i] -CandidateRun $bRuns[$i] -Key $key
        $differences += Assert-OnlyDeclaredDifferences -BaselineRun $aRuns[$i] -CandidateRun $bRuns[$i] -Key $key
    }
    $differences = @($differences | Select-Object -Unique)
    $notComparable = @($notComparable | Select-Object -Unique)
    $a = Get-CellStats $aRuns
    $b = Get-CellStats $bRuns
    $changePercent = 100.0 * ($b.medianMs - $a.medianMs) / $a.medianMs
    $p95ChangePercent = 100.0 * ($b.p95Ms - $a.p95Ms) / $a.p95Ms

    $protectedDetail = [ordered]@{}
    $precisionLimited = [System.Collections.Generic.List[string]]::new()
    $p95Ok = ($p95ChangePercent -le $ProtectedRegressionPercent)
    $p95Precision = Get-PrecisionFloor -Values @($a.p95RunValues) -BaselineValue $a.p95Ms -CandidateValue $b.p95Ms -LimitPercent $ProtectedRegressionPercent
    if ($p95Precision.limited) { $precisionLimited.Add('p95'); $p95Ok = $true }
    $protectedDetail['p95'] = [ordered]@{ baseline = [math]::Round($a.p95Ms, 4); candidate = [math]::Round($b.p95Ms, 4); changePercent = [math]::Round($p95ChangePercent, 3); ok = $p95Ok; precision = $p95Precision }
    $protectedOk = $p95Ok
    foreach ($name in $protectedMetrics) {
        $av = [double]$a.protectedMedians[$name]
        $bv = [double]$b.protectedMedians[$name]
        if ($av -le 0.0) { continue }
        $delta = 100.0 * ($bv - $av) / $av
        $ok = ($delta -le $ProtectedRegressionPercent)
        $precision = Get-PrecisionFloor -Values @($a.protectedRunMedians[$name]) -BaselineValue $av -CandidateValue $bv -LimitPercent $ProtectedRegressionPercent
        if ($precision.limited) { $precisionLimited.Add($name); $ok = $true }
        $protectedOk = $protectedOk -and $ok
        $protectedDetail[$name] = [ordered]@{ baseline = [math]::Round($av, 4); candidate = [math]::Round($bv, 4); changePercent = [math]::Round($delta, 3); ok = $ok; precision = $precision }
    }
    # M7-HITCH-METRIC：绝对阈值饱和（基线侧每一帧都 >16.67 ms ⇒ 计数 = 总帧数）时该判据没有分辨力。
    # 切换阶梯（数据驱动、按分辨力从高到低）：中位+k×MAD（优先）→ 比例中位（MAD 退化为 0 时）→ 绝对。
    $absoluteSaturated = ($a.frameTotal -gt 0 -and $a.hitch16_67 -ge $a.frameTotal)
    $hitchMetricUsed = 'hitch16_67'
    if ($absoluteSaturated) {
        if ($a.madZeroRuns -eq 0 -and ($a.hitchMad5 + $b.hitchMad5) -gt 0) { $hitchMetricUsed = 'overMad5' }
        elseif ($a.madZeroRuns -eq 0) { $hitchMetricUsed = 'overMad3' }
        else { $hitchMetricUsed = 'overMedian3x' }
    }
    $hitchValues = @{
        hitch16_67   = @($a.hitch16_67, $b.hitch16_67)
        overMad3     = @($a.hitchMad3, $b.hitchMad3)
        overMad5     = @($a.hitchMad5, $b.hitchMad5)
        overMedian3x = @($a.hitchMedian3x, $b.hitchMedian3x)
    }
    $hitchBaseline = [int]$hitchValues[$hitchMetricUsed][0]
    $hitchCandidate = [int]$hitchValues[$hitchMetricUsed][1]
    if ($hitchBaseline -gt 0) {
        $hitchDeltaPercent = 100.0 * ($hitchCandidate - $hitchBaseline) / $hitchBaseline
        $hitchOk = ($hitchDeltaPercent -le $ProtectedRegressionPercent)
        $protectedOk = $protectedOk -and $hitchOk
    }
    else { $hitchDeltaPercent = 0.0; $hitchOk = $true }
    $protectedDetail['hitch'] = [ordered]@{
        metric = $hitchMetricUsed
        baseline = $hitchBaseline
        candidate = $hitchCandidate
        changePercent = [math]::Round($hitchDeltaPercent, 3)
        ok = $hitchOk
        absoluteSaturated = $absoluteSaturated
        spectrum = [ordered]@{
            baselineMad3 = $a.hitchMad3; baselineMad5 = $a.hitchMad5; baselineMad10 = $a.hitchMad10
            candidateMad3 = $b.hitchMad3; candidateMad5 = $b.hitchMad5; candidateMad10 = $b.hitchMad10
            baselineMedian3x = $a.hitchMedian3x; candidateMedian3x = $b.hitchMedian3x
        }
        frames = $a.frameTotal
    }

    $drift = Get-Drift -BaselineCellRuns $aRuns -CandidateCellRuns $bRuns -RequiredPercent $a.requiredPercent -BaselineOrder $baselineOrder -CandidateOrder $candidateOrder -BaselineOrderInfo $baselineOrderInfo -CandidateOrderInfo $candidateOrderInfo
    $improved = ($changePercent -lt 0) -and ([Math]::Abs($changePercent) -gt $a.requiredPercent)
    $regressed = ($changePercent -gt $a.requiredPercent)

    if ($isSelfCheck) {
        if ([Math]::Abs($changePercent) -gt 1e-9 -or -not $protectedOk) {
            throw "self-check failed for $key : change=$changePercent% protectedOk=$protectedOk"
        }
        $verdict = 'SELF-CHECK-OK'
        $result = 'ACCEPTED'
    }
    elseif (-not $protectedOk) { $verdict = 'PROTECTED-REGRESSION'; $result = 'REJECTED' }
    elseif ($regressed) { $verdict = 'REGRESSION'; $result = 'REJECTED' }
    elseif ($improved) {
        if ($drift.available -and $drift.flagged) { $verdict = 'IMPROVED-BUT-DRIFT'; $result = 'INCONCLUSIVE' }
        else { $verdict = 'IMPROVED'; $result = 'ACCEPTED' }
    }
    else {
        # 噪声带内：测量足够紧（噪声下限 ≤ 5%）说明确实没有足够改善 → REJECTED；
        # 噪声下限 > 5% 说明本次测量分辨不出有意义的效应 → INCONCLUSIVE。
        $verdict = if ($a.requiredPercent -le 5.0) { 'WITHIN-NOISE-TIGHT' } else { 'WITHIN-NOISE-LOOSE' }
        $result = if ($a.requiredPercent -le 5.0) { 'REJECTED' } else { 'INCONCLUSIVE' }
    }

    switch ($result) {
        'ACCEPTED' { $anyAccepted = $true }
        'REJECTED' { $anyRejected = $true }
        'INCONCLUSIVE' { $anyInconclusive = $true }
    }

    $entry = [pscustomobject]@{
        cell                   = $key
        result                 = $result
        verdict                = $verdict
        targetMetric           = $TargetMetric
        treatment              = ((@($differences) | ForEach-Object { '{0}: {1} -> {2}' -f $_, $aRuns[0].Data.$_, $bRuns[0].Data.$_ }) -join '; ')
        baselineRuns           = $a.runs
        candidateRuns          = $b.runs
        baselineMedianMs       = [math]::Round($a.medianMs, 4)
        baselineRunToRunMadMs  = [math]::Round($a.runToRunMadMs, 4)
        baselineWithinRunMadMs = [math]::Round($a.withinRunMadMs, 4)
        baselineP95Ms          = [math]::Round($a.p95Ms, 4)
        baselineP99Ms          = [math]::Round($a.p99Ms, 4)
        candidateMedianMs      = [math]::Round($b.medianMs, 4)
        candidateP95Ms         = [math]::Round($b.p95Ms, 4)
        candidateP99Ms         = [math]::Round($b.p99Ms, 4)
        changePercent          = [math]::Round($changePercent, 3)
        p95ChangePercent       = [math]::Round($p95ChangePercent, 3)
        requiredPercent        = [math]::Round($a.requiredPercent, 3)
        protectedOk            = $protectedOk
        protected              = $protectedDetail
        # 精度受限（仅报不判）的受保护指标：判定仍会打印它们的变化，但不进 protectedOk。
        precisionLimited       = @($precisionLimited)
        hitches                = "$($a.hitch16_67)/$($a.hitch33_33)/$($a.hitch50) -> $($b.hitch16_67)/$($b.hitch33_33)/$($b.hitch50)（判据 $hitchMetricUsed）"
        drift                  = $drift
        notComparableFields    = @($notComparable)
        sourceHashes           = ((@($aRuns) | ForEach-Object { $_.Hash }) -join ',')
    }
    $report.Add($entry)
}

$overall = if ($anyRejected) { 'REJECTED' }
           elseif ($anyAccepted -and -not $anyInconclusive) { 'ACCEPTED' }
           elseif ($anyAccepted) { 'ACCEPTED (partial: some cells inconclusive)' }
           else { 'INCONCLUSIVE' }

$report | Format-Table cell, result, verdict, baselineMedianMs, candidateMedianMs, changePercent, requiredPercent, protectedOk -AutoSize
$allPrecisionLimited = @($report | ForEach-Object { @($_.precisionLimited) } | Sort-Object -Unique)
if ($allPrecisionLimited.Count -gt 0) {
    Write-Host ("precision-limited protected metrics (report-only, M7-GPUTIME-RESOLUTION): {0}" -f ($allPrecisionLimited -join ', '))
}
$orderWarnings = @($report | ForEach-Object { $_.drift.orderWarning } | Where-Object { $_ } | Sort-Object -Unique)
if ($orderWarnings.Count -gt 0) {
    Write-Warning ($orderWarnings -join ' | ')
}
$orderSourcesUsed = @($report | ForEach-Object { $_.drift.orderSources.Values } | Sort-Object -Unique)
Write-Host ("drift order source: {0}" -f ($orderSourcesUsed -join ', '))
Write-Host ("overall: {0}" -f $overall)

if ($OutputJson) {
    $payload = [ordered]@{
        schemaVersion              = 2
        generatedUtc               = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        baselineRoot               = $Baseline
        candidateRoot              = $Candidate
        baselineFilter             = $BaselineFilter
        candidateFilter            = $CandidateFilter
        targetMetric               = $TargetMetric
        treatmentFields            = @($TreatmentField)
        selfCheck                  = [bool]$isSelfCheck
        protectedRegressionPercent = $ProtectedRegressionPercent
        noiseFloorOverridePercent  = $NoiseFloorPercentOverride
        overallResult              = $overall
        rows                       = @($report)
    }
    $jsonPath = [System.IO.Path]::GetFullPath($OutputJson)
    $jsonDirectory = Split-Path -Parent $jsonPath
    if ($jsonDirectory -and -not (Test-Path -LiteralPath $jsonDirectory)) {
        [System.IO.Directory]::CreateDirectory($jsonDirectory) | Out-Null
    }
    [System.IO.File]::WriteAllText($jsonPath,
        (($payload | ConvertTo-Json -Depth 8) + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("comparison: {0}" -f $OutputJson)
}

# 退出码：0 = 无 REJECTED（ACCEPTED / INCONCLUSIVE）；1 = 存在 REJECTED；2 = 输入不满足契约（throw）。
if ($anyRejected) { exit 1 }
exit 0
