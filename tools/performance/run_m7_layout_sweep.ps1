<#
.SYNOPSIS
  M7-08 数据布局实验驱动：E-M7-LAYOUT-001/002 的固定 cell 矩阵。

.DESCRIPTION
  唯一变量 = `--layout`（aos / hot-cold / soa）。生产 packet 构建路径不随布局变化，
  因此跨布局比较是单变量对照；布局内核在同帧内并排运行并计时（`layoutCullMs`），
  与生产 culling 逐帧/采样等价校验（raw JSON 的 layoutCheck* 与 run-notes 记录）。

  数据集矩阵（文档的完整笛卡尔积过大，按"主筛选 + 边界确认"取三组）：
    scale-50k-v50-u100     m7-cpu-scale：50k 代理 / 100% 更新（与 M7-01/05 同场景）
    layout-10k-v10-u0      m7-layout：10k 代理 / 0% 更新（静态）
    layout-100k-v90-u10    m7-layout：100k 代理 / 10% 更新（大规模）
  注意：cell id 里的 v50/v10/v90 是**请求**的可见率（`--visible-ratio` 只是场景输入目标）；
  实际可见率由固定分布与视锥决定（三个数据集实测都约 52%），以 raw JSON 的
  `correctness.visibleCount` 与汇总报告里的 `visible` 列为准。可见率作为独立维度的
  端到端 sweep 见 BACKLOG `M7-LAYOUT-VISIBILITY`（单元/微基准层已覆盖 0.1/0.5/0.9）。

.EXAMPLE
  pwsh tools/performance/run_m7_layout_sweep.ps1 `
    -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
    -OutputDirectory out/m7-08/sweep -Repeats 2
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Rhi = 'd3d12',
    [int]$WarmupFrames = 120,
    [int]$MeasureFrames = 600,
    [int]$Seed = 6657,
    [int]$Workers = 1,
    [int]$ChunkSize = 256,
    [int]$Repeats = 1,
    [string]$MachineManifest = 'out/performance/environment.json',
    [ValidateSet('on', 'off')][string]$ForegroundGate = 'off',
    [int]$Retries = 3,
    [string]$Only = '',
    # M7-09：A/B 交错采集（固定 seed 打乱 (cell, run) 执行顺序，并记录 sequence 与
    # runOrderNote；布局 A/B 天然是多臂交错）。
    [switch]$Interleave,
    [int]$ShuffleSeed = 20260918,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$manifest = (Resolve-Path -LiteralPath (Join-Path $repoRoot $MachineManifest)).Path
[System.IO.Directory]::CreateDirectory($output) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $output 'logs')) | Out-Null

# 数据集 cell（layout 由外层循环展开成三个 run 单元）。
$datasets = @(
    [pscustomobject]@{ id = 'scale-50k-v50-u100'; scene = 'm7-cpu-scale'; entities = 50000;  visible = 0.5; update = 1.0 }
    [pscustomobject]@{ id = 'layout-10k-v10-u0';  scene = 'm7-layout';    entities = 10000;  visible = 0.1; update = 0.0 }
    [pscustomobject]@{ id = 'layout-100k-v90-u10'; scene = 'm7-layout';   entities = 100000; visible = 0.9; update = 0.1 }
)
$layouts = @('aos', 'hot-cold', 'soa')

$plan = New-Object System.Collections.Generic.List[object]
foreach ($dataset in $datasets) {
    foreach ($layout in $layouts) {
        $plan.Add([pscustomobject]@{
            id       = ('{0}-{1}' -f $dataset.id, $layout)
            dataset  = $dataset.id
            scene    = $dataset.scene
            entities = $dataset.entities
            visible  = $dataset.visible
            update   = $dataset.update
            layout   = $layout
            experiment = 'E-M7-LAYOUT-001'
            variant    = if ($layout -eq 'aos') { 'baseline' } else { 'candidate' }
        })
    }
}

$onlyList = @($Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$selected = New-Object System.Collections.Generic.List[object]
foreach ($cell in $plan) {
    $matched = ($onlyList.Count -eq 0)
    foreach ($entry in $onlyList) {
        if ($entry.EndsWith('!')) {
            if ($cell.id -eq $entry.TrimEnd('!')) { $matched = $true }
        }
        elseif ($cell.id.StartsWith($entry)) { $matched = $true }
    }
    if ($matched) { $selected.Add($cell) }
}
if ($selected.Count -eq 0) { throw "no cells selected (Only=$Only)" }

$script:summary = [System.Collections.Generic.List[object]]::new()

function Invoke-Cell {
    param($Cell, [int]$RunIndex, [int]$Sequence = 0, [int]$SequenceTotal = 0)
    $name = '{0}-r{1}' -f $Cell.id, $RunIndex
    $runDirectory = Join-Path $output $name
    [System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
    $metricsPath = Join-Path $runDirectory 'run.json'
    $logPath = Join-Path (Join-Path $output 'logs') ($name + '.log')

    $arguments = @(
        "--rhi=$Rhi"
        "--scene=$($Cell.scene)"
        '--benchmark'
        "--warmup-frames=$WarmupFrames"
        "--measure-frames=$MeasureFrames"
        '--vsync=off'
        "--seed=$Seed"
        "--workers=$Workers"
        "--chunk-size=$ChunkSize"
        '--packet-build=serial'
        "--entity-count=$($Cell.entities)"
        "--visible-ratio=$($Cell.visible.ToString([System.Globalization.CultureInfo]::InvariantCulture))"
        "--update-ratio=$($Cell.update.ToString([System.Globalization.CultureInfo]::InvariantCulture))"
        "--layout=$($Cell.layout)"
        "--metrics=$metricsPath"
        "--machine-manifest=$manifest"
        "--experiment-id=$($Cell.experiment)"
        "--variant=$($Cell.variant)"
        "--run-index=$RunIndex"
        "--foreground-gate=$ForegroundGate"
    )
    if ($Sequence -gt 0) {
        $arguments += "--run-order-note=seq=$Sequence/$SequenceTotal cell=$($Cell.id) variant=$($Cell.variant) run=$RunIndex"
    }

    if ($DryRun) {
        Write-Host ("DRY-RUN {0} {1}" -f $exe, ($arguments -join ' '))
        return
    }

    $started = Get-Date
    $attempts = 0
    $exitCode = -1
    $stdout = ''
    $stderr = ''
    while ($attempts -lt $Retries) {
        ++$attempts
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $exe
        $startInfo.WorkingDirectory = $repoRoot
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.Arguments = (($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        if ($exitCode -eq 0) { break }
        Write-Warning ("attempt {0} failed (exit {1}): {2}" -f $attempts, $exitCode, $name)
        Start-Sleep -Seconds 3
    }
    [System.IO.File]::WriteAllText($logPath, $stdout, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText(($logPath + '.err'), $stderr, (New-Object System.Text.UTF8Encoding($false)))
    $finished = Get-Date
    $hash = if (Test-Path -LiteralPath $metricsPath) {
        (Get-FileHash -LiteralPath $metricsPath -Algorithm SHA256).Hash
    } else { '' }
    $entry = [pscustomobject]@{
        cell            = $Cell.id
        dataset         = $Cell.dataset
        scene           = $Cell.scene
        entities        = $Cell.entities
        visibleRatio    = $Cell.visible
        updateRatio     = $Cell.update
        layout          = $Cell.layout
        runIndex        = $RunIndex
        sequence        = $Sequence
        experiment      = $Cell.experiment
        variant         = $Cell.variant
        attempts        = $attempts
        exitCode        = $exitCode
        metricsPath     = $metricsPath
        metricsSha256   = $hash
        startedUtc      = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        durationSeconds = [math]::Round(($finished - $started).TotalSeconds, 3)
        status          = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $script:summary.Add($entry)
    Write-Host ("{0} exit={1} {2}s sha={3}" -f $name, $exitCode, $entry.durationSeconds, $hash)
    if ($exitCode -ne 0) {
        throw "benchmark failed ($exitCode): $name (log: $logPath)"
    }
}

try {
    # PS 5.1 下 @(<List[object]>) 会抛 "Argument types do not match"：计划用普通数组累积。
    $planItems = @()
    foreach ($run in 1..$Repeats) {
        foreach ($cell in $selected) {
            $planItems += [pscustomobject]@{ cell = $cell; run = $run }
        }
    }
    if ($Interleave) {
        $rng = [System.Random]::new($ShuffleSeed)
        $shuffled = @($planItems)
        for ($i = $shuffled.Count - 1; $i -gt 0; --$i) {
            $j = $rng.Next($i + 1)
            $tmp = $shuffled[$i]; $shuffled[$i] = $shuffled[$j]; $shuffled[$j] = $tmp
        }
        $planItems = @()
        foreach ($item in $shuffled) { $planItems += $item }
    }
    $sequence = 0
    foreach ($item in $planItems) {
        ++$sequence
        Invoke-Cell -Cell $item.cell -RunIndex $item.run -Sequence $sequence -SequenceTotal $planItems.Count
    }
}
finally {
    $summaryPath = Join-Path $output 'sweep-driver-summary.json'
    $payload = [ordered]@{
        schemaVersion         = 1
        generatedUtc          = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        rhi                   = $Rhi
        warmupFrames          = $WarmupFrames
        measuredFrames        = $MeasureFrames
        seed                  = $Seed
        workers               = $Workers
        chunkSize             = $ChunkSize
        executable            = $exe
        executableSha256      = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        machineManifest       = $manifest
        machineManifestSha256 = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
        dryRun                = [bool]$DryRun
        interleave            = [bool]$Interleave
        shuffleSeed           = $ShuffleSeed
        cells                 = @($script:summary | Sort-Object sequence)
        executionOrder        = @($script:summary | Sort-Object sequence | ForEach-Object { '{0}#{1}' -f $_.cell, $_.runIndex })
    }
    [System.IO.File]::WriteAllText($summaryPath, (($payload | ConvertTo-Json -Depth 6) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("driver summary: {0}" -f $summaryPath)
}
