# ============================================================================
# M7 端到端热重载采集驱动（BACKLOG M7-LOAD-RELOAD-E2E / E-M7-RELOAD-001）
#
# 问题（M7-07 记录 + M7-11 审计）：reload 的提交/退休由长跑压力覆盖过，但**没有专门的采集场景**
# ——没有"每 N 帧改产物 revision"的端到端延迟证据（request→commit 的分位数、拒绝原因、延迟退休）。
#
# 本驱动采集 N 组（reload 频率 × 重复）benchmark run：每次运行 `--loader-reload-every=N`
# （端到端热重载：对已提交请求新 revision），产出的 raw 带 `assetRequests`（含 revision 与
# requestToReadyMs），由 tools/performance/summarize_m7_reload_sweep.py 汇总热重载延迟分位数与提交/拒绝计数。
#
# 用法：
#   pwsh tools/performance/run_m7_reload_sweep.ps1 -Executable <exe> -OutputDirectory out/m7-reload -Repeats 2
# ============================================================================
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Scene = 'm7-streaming',
    [string]$Rhi = 'd3d12',
    [string]$StreamScript = 'assets/tests/m7/streaming-burst.bin',
    [int]$WarmupFrames = 120,
    [int]$MeasureFrames = 600,
    [int]$Seed = 6657,
    [int]$ChunkSize = 256,
    [int]$Workers = 8,
    [int]$Repeats = 1,
    [string]$MachineManifest = 'out/performance/environment.json',
    [ValidateSet('on', 'off')][string]$ForegroundGate = 'off',
    [int]$Retries = 3,
    [string]$Only = '',
    [switch]$Interleave,
    [int]$ShuffleSeed = 20260919,
    [string]$ExperimentOverride = 'E-M7-RELOAD-001',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$manifest = (Resolve-Path -LiteralPath (Join-Path $repoRoot $MachineManifest)).Path
$scriptPath = (Resolve-Path -LiteralPath (Join-Path $repoRoot $StreamScript)).Path
[System.IO.Directory]::CreateDirectory($output) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $output 'logs')) | Out-Null

# cell：唯一变量是热重载频率（每 N 帧对已提交请求新 revision）。30 帧 = 高频（每 ~1 s 一次），
# 480 帧 = 低频（长跑里偶发）；两者都远小于测量窗口，保证分位数有足够样本。
$plan = @(
    [pscustomobject]@{ id = 'reload-every30';  reloadEvery = 30;  experiment = $ExperimentOverride; variant = 'reload-every30' }
    [pscustomobject]@{ id = 'reload-every120'; reloadEvery = 120; experiment = $ExperimentOverride; variant = 'reload-every120' }
)

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
        "--scene=$Scene"
        '--benchmark'
        "--warmup-frames=$WarmupFrames"
        "--measure-frames=$MeasureFrames"
        '--vsync=off'
        "--seed=$Seed"
        "--workers=$Workers"
        "--chunk-size=$ChunkSize"
        '--packet-build=parallel'
        '--scheduler=per-worker'
        '--layout=none'
        '--async-assets=on'
        "--stream-script=$scriptPath"
        '--upload-mib-per-frame=4'
        '--upload-budget-cpu-ms=0.5'
        '--upload-budget-requests=16'
        '--upload-budget-reload-percent=50'
        '--upload-aging-ms=50'
        "--loader-reload-every=$($Cell.reloadEvery)"
        "--metrics=$metricsPath"
        "--machine-manifest=$manifest"
        "--experiment-id=$($Cell.experiment)"
        "--variant=$($Cell.variant)"
        "--run-index=$RunIndex"
        "--foreground-gate=$ForegroundGate"
    )
    if ($Sequence -gt 0) {
        $arguments += "--run-order-note=seq=$Sequence/$SequenceTotal cell=$($Cell.id) run=$RunIndex"
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
        reloadEvery     = $Cell.reloadEvery
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
        scene                 = $Scene
        rhi                   = $Rhi
        streamScript          = $scriptPath
        streamScriptSha256    = (Get-FileHash -LiteralPath $scriptPath -Algorithm SHA256).Hash
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
