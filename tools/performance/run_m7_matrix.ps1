# ============================================================================
# run_m7_matrix.ps1 — M7 串行基线采集矩阵（M7-01 第 5 步）
#
# 约束（与 docs/architecture/README.md 一致）：
#   * 控制变量全部显式传给 sandbox；缺省即让 sandbox 报错，不在这里补默认值；
#   * 每次运行独立进程，固定 warmup/measure/seed/resolution/VSync off；
#   * 每个 raw JSON 记录 SHA-256，任一 run 非 0 退出即抛错（不挑选数据）；
#   * recipe（assets/recipes/m7-performance-scenes.json）是冻结记录，本脚本在运行前
#     核对实际参数与 recipe 一致；不一致即拒绝采集。
#
# 用法示例：
#   pwsh tools/performance/run_m7_matrix.ps1 `
#     -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
#     -OutputDirectory out/m7-01/baseline `
#     -MachineManifest out/performance/environment.json
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Variant = 'baseline',
    [string]$ExperimentId = 'E-M7-BASELINE',
    [string[]]$Scenes = @('m7-cpu-scale', 'm7-layout', 'm7-streaming'),
    [string[]]$Rhis = @('d3d12', 'd3d11'),
    [int[]]$Workers = @(1),
    [int[]]$ChunkSizes = @(256),
    [int]$Runs = 5,
    [int]$RunStart = 1,
    [int]$WarmupFrames = 300,
    [int]$MeasureFrames = 1800,
    [int]$Seed = 6657,
    [int]$EntityCountOverride = 0,
    [string]$MachineManifest = 'out/performance/environment.json',
    [string]$Recipe = 'assets/recipes/m7-performance-scenes.json',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$manifest = (Resolve-Path -LiteralPath (Join-Path $repoRoot $MachineManifest)).Path
$recipePath = (Resolve-Path -LiteralPath (Join-Path $repoRoot $Recipe)).Path
[System.IO.Directory]::CreateDirectory($output) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $output 'logs')) | Out-Null

$recipeJson = Get-Content -LiteralPath $recipePath -Raw | ConvertFrom-Json
$script:summary = [System.Collections.Generic.List[object]]::new()

function Get-Scene ([string]$name) {
    $scene = $recipeJson.scenes | Where-Object { $_.name -eq $name }
    if (-not $scene) { throw "recipe 中没有场景 $name" }
    return $scene
}

# ---- recipe 一致性核对（采集前，而不是采集后） -----------------------------
$cpuScale = Get-Scene 'm7-cpu-scale'
if ($cpuScale.renderProxyCount -ne 50000) { throw 'recipe: m7-cpu-scale 必须固定 50000 代理' }
$layout = Get-Scene 'm7-layout'
$streaming = Get-Scene 'm7-streaming'
$common = $recipeJson.common
if ($Seed -ne $recipeJson.seed) { throw "seed 与 recipe 不一致：$Seed != $($recipeJson.seed)" }
if ($WarmupFrames -ne $common.warmupFrames) { throw "warmupFrames 与 recipe 不一致" }
if ($MeasureFrames -ne $common.measuredFrames) { throw "measuredFrames 与 recipe 不一致" }
# M7-05：benchmark 模式要求显式 packet-build；本脚本是 M7-01 串行基线采集器。
if ($common.packetBuild -ne 'serial') { throw "recipe common.packetBuild 必须是 serial（本脚本是串行基线矩阵）" }
if (@($Workers) -ne @(1)) { throw "serial baseline matrix requires workers=1" }
$streamingScript = (Resolve-Path -LiteralPath (Join-Path $repoRoot $streaming.requestScript)).Path
$scriptHash = (Get-FileHash -LiteralPath $streamingScript -Algorithm SHA256).Hash
if ($scriptHash -ne $streaming.expectedSha256 -and $scriptHash -ne $streaming.requestScriptSha256) {
    throw "streaming-script.bin 的 SHA-256 与 recipe 不一致：$scriptHash"
}
Write-Host ("recipe ok: seed={0} warmup={1} measure={2} streamScript={3}" -f $Seed, $WarmupFrames, $MeasureFrames, $scriptHash)

function Invoke-Run {
    param([string]$Scene, [string]$Rhi, [int]$WorkerCount, [int]$ChunkSize, [int]$RunIndex)
    $name = '{0}-{1}-w{2}-c{3}-r{4}.json' -f $Scene, $Rhi, $WorkerCount, $ChunkSize, $RunIndex
    $metricsPath = Join-Path $output $name
    $logPath = Join-Path (Join-Path $output 'logs') ($name -replace '\.json$', '.log')

    $arguments = @(
        "--rhi=$Rhi"
        "--scene=$Scene"
        '--benchmark'
        "--warmup-frames=$WarmupFrames"
        "--measure-frames=$MeasureFrames"
        '--vsync=off'
        "--seed=$Seed"
        "--workers=$WorkerCount"
        "--chunk-size=$ChunkSize"
        "--packet-build=serial"
        "--metrics=$metricsPath"
        "--machine-manifest=$manifest"
        "--experiment-id=$ExperimentId"
        "--variant=$Variant"
        "--run-index=$RunIndex"
    )
    if ($Scene -eq 'm7-layout') {
        $cell = $layout.baselineCell
        $entityCount = if ($EntityCountOverride -gt 0) { $EntityCountOverride } else { $cell.entityCount }
        $arguments += "--entity-count=$entityCount"
        $arguments += "--visible-ratio=$($cell.visibleRatio)"
        $arguments += "--update-ratio=$($cell.updateRatio)"
    }
    if ($Scene -eq 'm7-streaming') {
        $arguments += "--stream-script=$streamingScript"
    }

    if ($DryRun) {
        Write-Host ("DRY-RUN {0} {1}" -f $exe, ($arguments -join ' '))
        return
    }

    $started = Get-Date
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $logPath -RedirectStandardError ($logPath + '.err')
    $finished = Get-Date
    $hash = if (Test-Path -LiteralPath $metricsPath) {
        (Get-FileHash -LiteralPath $metricsPath -Algorithm SHA256).Hash
    } else { '' }
    $entry = [pscustomobject]@{
        scene           = $Scene
        rhi             = $Rhi
        workers         = $WorkerCount
        chunkSize       = $ChunkSize
        runIndex        = $RunIndex
        variant         = $Variant
        exitCode        = $process.ExitCode
        metricsPath     = $metricsPath
        metricsSha256   = $hash
        startedUtc      = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        durationSeconds = [math]::Round(($finished - $started).TotalSeconds, 3)
        status          = if ($process.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $script:summary.Add($entry)
    Write-Host ("{0} exit={1} {2}s sha={3}" -f $name, $process.ExitCode, $entry.durationSeconds, $hash)
    if ($process.ExitCode -ne 0) {
        throw "benchmark failed ($($process.ExitCode)): $name (log: $logPath)"
    }
}

try {
    for ($run = $RunStart; $run -le $Runs; ++$run) {
        foreach ($scene in $Scenes) {
            foreach ($rhi in $Rhis) {
                foreach ($workerCount in $Workers) {
                    foreach ($chunkSize in $ChunkSizes) {
                        Invoke-Run -Scene $scene -Rhi $rhi -WorkerCount $workerCount -ChunkSize $chunkSize -RunIndex $run
                    }
                }
            }
        }
    }
}
finally {
    $summaryPath = Join-Path $output 'matrix-summary.json'
    # 分多次调用（例如 RunStart 分批）时合并已有条目：同一 (scene,rhi,workers,chunk,runIndex)
    # 以本次结果为准，其余保留；避免后一批覆盖前一批的哈希。
    $merged = [System.Collections.Generic.List[object]]::new()
    if (Test-Path -LiteralPath $summaryPath) {
        $existing = (Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json).runs
        foreach ($entry in @($existing)) {
            $replaced = $script:summary | Where-Object {
                $_.scene -eq $entry.scene -and $_.rhi -eq $entry.rhi -and $_.workers -eq $entry.workers -and
                $_.chunkSize -eq $entry.chunkSize -and $_.runIndex -eq $entry.runIndex
            }
            if (-not $replaced) { $merged.Add($entry) }
        }
    }
    foreach ($entry in $script:summary) { $merged.Add($entry) }
    $payload = [ordered]@{
        schemaVersion = 1
        generatedUtc  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        executable    = $exe
        executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        machineManifest = $manifest
        machineManifestSha256 = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
        recipe        = $recipePath
        recipeSha256  = (Get-FileHash -LiteralPath $recipePath -Algorithm SHA256).Hash
        variant       = $Variant
        dryRun        = [bool]$DryRun
        runs          = @($merged | Sort-Object scene, rhi, workers, chunkSize, runIndex)
    }
    [System.IO.File]::WriteAllText($summaryPath, (($payload | ConvertTo-Json -Depth 6) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("matrix summary: {0}" -f $summaryPath)
}

# 说明：A/B 交错顺序（A1 B1 B2 A2 …）由 M7-09 的采集流程在高一层编排；
# 本脚本只保证单 variant 的独立进程重复与显式控制变量。
