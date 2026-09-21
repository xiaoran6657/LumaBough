# ============================================================================
# M7 组合复测驱动（E-M7-COMBINED-001 / M7-COMBINED-RETEST）
#
# 目的：把 M7 的三项已保留优化放到**同一个流式场景**里做端到端帧时间复测：
#   A streaming-sync-serial    同步帧内读 + 串行 packet（基线）
#   B streaming-async-serial   异步流水线 + 上传预算（异步单变量：B vs A）
#   C streaming-async-pw-w8    异步 + 并行 packet（per-worker w8）（并行单变量：C vs B；
#                              全家桶 vs 基线：C vs A）
#
# 为什么新增驱动而不是往 run_m7_packet_sweep.ps1 塞 cell：packet sweep 的 cell 表刻意
# 只带 packet 维度（scene 固定 m7-cpu-scale、loader/upload 不入表），而组合 cell 需要
# scene + loader + upload 三组控制变量一起进 raw；分开驱动能让两边的控制面都保持单一。
#
# 用法：
#   pwsh tools/performance/run_m7_combined.ps1 `
#     -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
#     -OutputDirectory out/combined-001 -Repeats 3 -Interleave
#
# 随后按 M7-09 协议分别比较（治疗字段必须显式；`-MatchOn sceneName,rhi,chunkSize` 会把 workers
# 从配对键里放宽，比较器契约要求**被放宽的字段也一并声明**，上传四元组同理——否则判 INVALID）：
#   B vs A : -TreatmentField 'variant,loaderMode,workers,uploadBudgetCpuMs,uploadBudgetRequests,uploadBudgetReloadPercent,uploadAgingThresholdMs'
#   C vs B : -TreatmentField 'packetBuildMode,schedulerMode,workers,variant'
#   C vs A : -TreatmentField 'packetBuildMode,schedulerMode,workers,variant,loaderMode,uploadBudgetCpuMs,uploadBudgetRequests,uploadBudgetReloadPercent,uploadAgingThresholdMs'
#   并统一 -MatchOn 'sceneName,rhi,chunkSize'
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
    [int]$Repeats = 1,
    [string]$MachineManifest = 'out/performance/environment.json',
    [ValidateSet('on', 'off')][string]$ForegroundGate = 'off',
    [int]$Retries = 3,
    [string]$Only = '',
    [switch]$Interleave,
    [int]$ShuffleSeed = 20260919,
    [string]$VariantOverride = '',
    [string]$ExperimentOverride = '',
    # 上传四元组（默认值 = M7-08 决策的默认：4 MiB / 0.5 ms / 16 请求 / 50% 热重载 / aging 50 ms）
    [double]$UploadMiBPerFrame = 4,
    [double]$UploadBudgetCpuMs = 0.5,
    [int]$UploadBudgetRequests = 16,
    [int]$UploadBudgetReloadPercent = 50,
    [double]$UploadAgingMs = 50,
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

# 组合 cell：async/packet 是仅有的两个治疗维度，其余（scene/rhi/chunk/seed/帧数/manifest）全同。
$plan = @(
    [pscustomobject]@{ id = 'streaming-sync-serial'; async = $false; mode = 'serial';   scheduler = 'none';        workers = 1; experiment = 'E-M7-COMBINED-001'; variant = 'baseline' }
    [pscustomobject]@{ id = 'streaming-async-serial'; async = $true;  mode = 'serial';   scheduler = 'none';        workers = 1; experiment = 'E-M7-COMBINED-001'; variant = 'candidate-async' }
    [pscustomobject]@{ id = 'streaming-async-pw-w8'; async = $true;  mode = 'parallel'; scheduler = 'per-worker'; workers = 8; experiment = 'E-M7-COMBINED-001'; variant = 'candidate-combined' }
)

# -Only 前缀匹配；'!' 结尾 = 精确匹配（与其它 M7 驱动一致）。
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
        "--workers=$($Cell.workers)"
        "--chunk-size=$ChunkSize"
        "--packet-build=$($Cell.mode)"
        '--layout=none'
        "--metrics=$metricsPath"
        "--machine-manifest=$manifest"
        "--experiment-id=$(if ($ExperimentOverride) { $ExperimentOverride } else { $Cell.experiment })"
        "--variant=$(if ($VariantOverride) { $VariantOverride } else { $Cell.variant })"
        "--run-index=$RunIndex"
        "--stream-script=$scriptPath"
        "--upload-mib-per-frame=$UploadMiBPerFrame"
        "--async-assets=$(if ($Cell.async) { 'on' } else { 'off' })"
        "--foreground-gate=$ForegroundGate"
    )
    if ($Cell.mode -eq 'parallel') {
        $arguments += "--scheduler=$($Cell.scheduler)"
        $arguments += '--chunk-reserve=on'
    }
    # 预算三约束只在异步模式下生效（CLI 契约：串行基线不读它们）。
    if ($Cell.async) {
        $arguments += "--upload-budget-cpu-ms=$UploadBudgetCpuMs"
        $arguments += "--upload-budget-requests=$UploadBudgetRequests"
        $arguments += "--upload-budget-reload-percent=$UploadBudgetReloadPercent"
        $arguments += "--upload-aging-ms=$UploadAgingMs"
    }
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
    # 正确性/上传证据：直接从 raw 里取（避免二次解析时口径漂移）。
    $status = ''; $packetHash = ''; $graphHash = ''; $validationMessages = -1; $requests = -1; $ready = -1; $failed = -1
    if (Test-Path -LiteralPath $metricsPath) {
        $raw = Get-Content -LiteralPath $metricsPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $status = $raw.correctness.status
        $packetHash = $raw.correctness.packetSequenceHash
        $graphHash = $raw.correctness.graphHash
        $validationMessages = $raw.correctness.validationMessages
        if ($raw.assetRequests) {
            $requests = @($raw.assetRequests).Count
            $ready = @($raw.assetRequests | Where-Object { $_.result -eq 'ready' }).Count
            $failed = @($raw.assetRequests | Where-Object { $_.result -ne 'ready' }).Count
        }
    }
    $entry = [pscustomobject]@{
        cell               = $Cell.id
        asyncAssets        = $Cell.async
        packetBuildMode    = $Cell.mode
        schedulerMode      = if ($Cell.mode -eq 'parallel') { $Cell.scheduler } else { 'none' }
        workers            = $Cell.workers
        runIndex           = $RunIndex
        sequence           = $Sequence
        experiment         = if ($ExperimentOverride) { $ExperimentOverride } else { $Cell.experiment }
        variant            = if ($VariantOverride) { $VariantOverride } else { $Cell.variant }
        attempts           = $attempts
        exitCode           = $exitCode
        metricsPath        = $metricsPath
        metricsSha256      = $hash
        correctnessStatus  = $status
        packetSequenceHash = $packetHash
        graphHash          = $graphHash
        validationMessages = $validationMessages
        assetRequests      = $requests
        assetRequestsReady = $ready
        assetRequestsFailed = $failed
        startedUtc         = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        durationSeconds    = [math]::Round(($finished - $started).TotalSeconds, 3)
        status             = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $script:summary.Add($entry)
    Write-Host ("{0} exit={1} {2}s status={3} req={4}/{5} sha={6}" -f $name, $exitCode, $entry.durationSeconds, $status, $ready, $requests, $hash)
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
        chunkSize             = $ChunkSize
        uploadMiBPerFrame     = $UploadMiBPerFrame
        uploadBudgetCpuMs     = $UploadBudgetCpuMs
        uploadBudgetRequests  = $UploadBudgetRequests
        uploadBudgetReloadPercent = $UploadBudgetReloadPercent
        uploadAgingMs         = $UploadAgingMs
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
