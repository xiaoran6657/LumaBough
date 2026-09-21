# ============================================================================
# run_m7_profiler_overhead.ps1 — M7-02 第 6 步：profiler 自身开销四组实验
#
# 四组（同一 commit、同一场景、同一二进制配置，只有 profiler 状态不同）：
#   1. off                  Tracy 完全编译关闭（profile 构建）
#   2. on-unconnected       Tracy 编译开启、profiler 未连接（on-demand 不产生数据）
#   3. on-connected-coarse  连接 + 仅阶段 zone（--profile-detail=coarse）
#   4. on-connected-full    连接 + 完整 zone/counter/memory/lock（--profile-detail=full）
#
# 连接由 tools/performance/m7_tracy_sink.py 扮演 profiler 角色完成握手（官方 tracy-capture 需要
# CPM 联网拉取依赖，离线不可用；sink 只收不发，测量的是客户端侧开销）。
#
# 用法：
#   powershell -File tools/performance/run_m7_profiler_overhead.ps1 `
#     -OffExecutable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
#     -TracyExecutable out/build/windows-msvc-profile-tracy/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
#     -OutputDirectory out/m7-02/overhead
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$OffExecutable,
    [Parameter(Mandatory = $true)][string]$TracyExecutable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$MachineManifest = 'out/performance/environment.json',
    [string]$Scene = 'm7-cpu-scale',
    [string]$Rhi = 'd3d12',
    [int]$WarmupFrames = 100,
    [int]$MeasureFrames = 300,
    [int]$Seed = 6657,
    [int]$EntityCount = 50000
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($output) | Out-Null
$offExe = (Resolve-Path -LiteralPath $OffExecutable).Path
$tracyExe = (Resolve-Path -LiteralPath $TracyExecutable).Path
$manifest = (Resolve-Path -LiteralPath (Join-Path $repoRoot $MachineManifest)).Path

function Invoke-Group {
    param([string]$Name, [string]$Executable, [bool]$Connect, [bool]$Detail)
    $metrics = Join-Path $output ("overhead-{0}.json" -f $Name)
    $log = Join-Path $output ("overhead-{0}.log" -f $Name)
    $sinkReport = Join-Path $output ("sink-{0}.json" -f $Name)
    $arguments = @(
        "--rhi=$Rhi"
        "--scene=$Scene"
        '--benchmark'
        "--warmup-frames=$WarmupFrames"
        "--measure-frames=$MeasureFrames"
        '--vsync=off'
        "--seed=$Seed"
        '--workers=1'
        '--chunk-size=256'
        "--metrics=$metrics"
        "--machine-manifest=$manifest"
        "--experiment-id=E-M7-PROF-001"
        "--variant=$Name"
    )
    if ($Scene -eq 'm7-layout') { $arguments += "--entity-count=$EntityCount" }
    if ($Connect) {
        $arguments += '--profile-detail=' + ($(if ($Detail) { 'full' } else { 'coarse' }))
        $arguments += '--tracy-wait-connect=60000'
        $arguments += "--tracy-capture-frames=$MeasureFrames"
    }
    elseif ($Executable -eq $tracyExe) {
        $arguments += '--profile-detail=full'
    }

    $sink = $null
    if ($Connect) {
        # sink 的超时必须覆盖测量窗口（帧数 × 帧时），但也不能太长：客户端在退出时会等待
        # profiler 侧的终止确认，sink 保持连接会让进程多等一个 timeout（曾观察到 32s → 604s）。
        $sinkTimeout = [int][math]::Max(120, ($MeasureFrames * 0.12))
        $sink = Start-Process -FilePath 'python' -ArgumentList @(
            (Join-Path $repoRoot 'tools/performance/m7_tracy_sink.py'),
            '--port', '8086',
            '--timeout', "$sinkTimeout",
            '--connect-timeout', '60',
            '--report', $sinkReport
        ) -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 500
    }

    $started = Get-Date
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 3)

    if ($sink) {
        if (-not $sink.WaitForExit(120000)) { $sink.Kill() }
    }

    $summary = $null
    if (Test-Path -LiteralPath $metrics) {
        $run = Get-Content -LiteralPath $metrics -Raw | ConvertFrom-Json
        $notes = $null
        $notesPath = Join-Path (Split-Path -Parent $metrics) 'run-notes.json'
        if (Test-Path -LiteralPath $notesPath) { $notes = Get-Content -LiteralPath $notesPath -Raw | ConvertFrom-Json }
        $summary = [pscustomobject]@{
            group          = $Name
            exitCode       = $process.ExitCode
            seconds        = $seconds
            cpuMedianMs    = [math]::Round([double]$run.statistics.cpuFrameMs.median, 4)
            cpuP95Ms       = [math]::Round([double]$run.statistics.cpuFrameMs.p95, 4)
            cpuP99Ms       = [math]::Round([double]$run.statistics.cpuFrameMs.p99, 4)
            gpuMedianMs    = [math]::Round([double]$run.statistics.gpuFrameMs.median, 4)
            residentBytes  = [long]$run.statistics.residentBytes.median
            measuredFrames = [int]$run.measuredFrames
            tracyConnected = if ($notes) { [bool]$notes.tracyConnected } else { $false }
            zones          = if ($notes) { [long]$notes.profileCounters.zones } else { 0 }
            frames         = if ($notes) { [long]$notes.profileCounters.frames } else { 0 }
            plots          = if ($notes) { [long]$notes.profileCounters.plots } else { 0 }
            messages       = if ($notes) { [long]$notes.profileCounters.messages } else { 0 }
            allocations    = if ($notes) { [long]$notes.profileCounters.allocations } else { 0 }
            frees          = if ($notes) { [long]$notes.profileCounters.frees } else { 0 }
            lockAcquires   = if ($notes) { [long]$notes.profileCounters.lockAcquires } else { 0 }
            sinkBytes      = if (Test-Path -LiteralPath $sinkReport) {
                [long]((Get-Content -LiteralPath $sinkReport -Raw | ConvertFrom-Json).bytes)
            } else { 0 }
            metricsPath    = $metrics
            metricsSha256  = (Get-FileHash -LiteralPath $metrics -Algorithm SHA256).Hash
        }
    }
    else {
        $summary = [pscustomobject]@{ group = $Name; exitCode = $process.ExitCode; seconds = $seconds; metricsPath = '' }
    }
    Write-Host ("{0}: exit={1} cpuMedian={2}ms zones={3} sinkBytes={4}" -f `
        $Name, $summary.exitCode, $summary.cpuMedianMs, $summary.zones, $summary.sinkBytes)
    return $summary
}

$groups = @(
    (Invoke-Group -Name 'off' -Executable $offExe -Connect $false -Detail $false),
    (Invoke-Group -Name 'on-unconnected' -Executable $tracyExe -Connect $false -Detail $true),
    (Invoke-Group -Name 'on-connected-coarse' -Executable $tracyExe -Connect $true -Detail $false),
    (Invoke-Group -Name 'on-connected-full' -Executable $tracyExe -Connect $true -Detail $true)
)

$off = $groups | Where-Object { $_.group -eq 'off' }
$payload = [ordered]@{
    schemaVersion = 1
    generatedUtc  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    scene         = $Scene
    rhi           = $Rhi
    warmupFrames  = $WarmupFrames
    measuredFrames = $MeasureFrames
    offExecutable = $offExe
    tracyExecutable = $tracyExe
    groups        = $groups
    note          = '连接组由 tools/performance/m7_tracy_sink.py 扮演 profiler 角色；官方 tracy-capture/GUI 需在线拉取依赖，离线不可用。'
}
[System.IO.File]::WriteAllText((Join-Path $output 'profiler-overhead.json'),
    (($payload | ConvertTo-Json -Depth 6) + "`n"), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("overhead summary: {0}" -f (Join-Path $output 'profiler-overhead.json'))
