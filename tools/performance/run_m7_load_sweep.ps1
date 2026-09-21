<#
.SYNOPSIS
  M7-07 上传预算实验驱动：E-M7-LOAD-002 的固定 cell 矩阵（m7-streaming + 突发请求脚本）。

.DESCRIPTION
  每个 cell 是独立进程（隔离线程/内存状态），raw JSON + 截图落 -OutputDirectory。
  控制变量全部显式传给 sandbox（benchmark 模式缺省即报错，不在本脚本补默认值）。

  cell 定义（唯一变量 = 加载模式/上传预算三约束）：
    serial-burst        串行基线：请求帧内阻塞读 + 内容哈希校验（M7-01 语义，before 数据）
    burst-b1            异步：1 MiB / 0.25 ms / 4 请求每帧
    burst-b2            异步：2 MiB / 0.50 ms / 8 请求每帧
    burst-b4            异步：4 MiB / 0.50 ms / 16 请求每帧（recipe 默认组合）
    burst-b8            异步：8 MiB / 2.00 ms / 32 请求每帧
    burst-b4-aging      异步：4 MiB / 0.50 ms / 16 请求每帧 + aging 1 ns（优先级老化敏感度）

  输入脚本 assets/tests/m7/streaming-burst.bin 与稀疏版同源（tools/assets/gen_m7_scene_inputs.py），
  把 66 个请求压到 5 个突发帧，使单帧上传需求达到 1.7—4.5 MiB：小预算会真实成为约束。

.EXAMPLE
  pwsh tools/performance/run_m7_load_sweep.ps1 `
    -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
    -OutputDirectory out/m7-07/load-sweep -Repeats 2
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Scene = 'm7-streaming',
    [string]$Rhi = 'd3d12',
    [string]$StreamScript = 'assets/tests/m7/streaming-burst.bin',
    [int]$WarmupFrames = 8,
    [int]$MeasureFrames = 200,
    [int]$Workers = 1,
    [int]$ChunkSize = 256,
    [int]$Seed = 6657,
    [int]$Repeats = 1,
    [string]$MachineManifest = 'out/performance/environment.json',
    [ValidateSet('on', 'off')][string]$ForegroundGate = 'off',
    [int]$Retries = 3,
    [string]$Only = '',
    # M7-09：A/B 交错采集（固定 seed 打乱 (cell, run) 执行顺序，并把 sequence 与
    # runOrderNote 写进 driver summary 与 raw JSON，供漂移检查使用）。
    [switch]$Interleave,
    [int]$ShuffleSeed = 20260918,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# 自动化会话无法稳定保持前台（IDE 周期性抢占），默认关闭 gate 并把逐 run 的
# foregroundRatio 写进 run-notes；预算类指标（upload*/hitch）是 render thread 与
# 帧时间的测量，带 DWM 节流风险的部分在报告里单独标注。
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$manifest = (Resolve-Path -LiteralPath (Join-Path $repoRoot $MachineManifest)).Path
$scriptPath = (Resolve-Path -LiteralPath (Join-Path $repoRoot $StreamScript)).Path
[System.IO.Directory]::CreateDirectory($output) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $output 'logs')) | Out-Null

$plan = @(
    [pscustomobject]@{ id = 'serial-burst';   async = $false; bytes = 4; cpuMs = 0.5; requests = 16; reload = 50; agingMs = 50;    experiment = 'E-M7-LOAD-002'; variant = 'baseline'  }
    [pscustomobject]@{ id = 'burst-b1';       async = $true;  bytes = 1; cpuMs = 0.25; requests = 4;  reload = 50; agingMs = 50;   experiment = 'E-M7-LOAD-002'; variant = 'candidate' }
    [pscustomobject]@{ id = 'burst-b2';       async = $true;  bytes = 2; cpuMs = 0.5;  requests = 8;  reload = 50; agingMs = 50;   experiment = 'E-M7-LOAD-002'; variant = 'candidate' }
    [pscustomobject]@{ id = 'burst-b4';       async = $true;  bytes = 4; cpuMs = 0.5;  requests = 16; reload = 50; agingMs = 50;   experiment = 'E-M7-LOAD-002'; variant = 'candidate' }
    [pscustomobject]@{ id = 'burst-b8';       async = $true;  bytes = 8; cpuMs = 2.0;  requests = 32; reload = 50; agingMs = 50;   experiment = 'E-M7-LOAD-002'; variant = 'candidate' }
    [pscustomobject]@{ id = 'burst-b4-aging'; async = $true;  bytes = 4; cpuMs = 0.5;  requests = 16; reload = 50; agingMs = 0.001; experiment = 'E-M7-LOAD-003'; variant = 'candidate' }
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
        '--packet-build=serial'
        "--metrics=$metricsPath"
        "--machine-manifest=$manifest"
        "--experiment-id=$($Cell.experiment)"
        "--variant=$($Cell.variant)"
        "--run-index=$RunIndex"
        "--stream-script=$scriptPath"
    )
    if ($Sequence -gt 0) {
        $arguments += "--run-order-note=seq=$Sequence/$SequenceTotal cell=$($Cell.id) variant=$($Cell.variant) run=$RunIndex"
    }
    $arguments += @(
        "--upload-mib-per-frame=$($Cell.bytes)"
        "--async-assets=$(if ($Cell.async) { 'on' } else { 'off' })"
        "--foreground-gate=$ForegroundGate"
    )
    # 预算三约束是 v3 控制变量：串行基线不读它们，但 CLI 在 benchmark 下要求
    # m7-streaming 显式给出 upload-mib-per-frame（已给）；异步模式再要求另外两条。
    if ($Cell.async) {
        $arguments += "--upload-budget-cpu-ms=$($Cell.cpuMs)"
        $arguments += "--upload-budget-requests=$($Cell.requests)"
        $arguments += "--upload-budget-reload-percent=$($Cell.reload)"
        $arguments += "--upload-aging-ms=$($Cell.agingMs)"
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
        asyncAssets     = $Cell.async
        uploadMiB       = $Cell.bytes
        uploadCpuMs     = $Cell.cpuMs
        uploadRequests  = $Cell.requests
        reloadPercent   = $Cell.reload
        agingMs         = $Cell.agingMs
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
