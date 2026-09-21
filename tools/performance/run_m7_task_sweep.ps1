<#
.SYNOPSIS
  M7-A12 scaling sweep 驱动：workers × chunk × 版本（serial / global / per-worker）矩阵。

.DESCRIPTION
  每个 cell 是独立进程（隔离调度器与内存状态），raw JSON 落 -OutputDirectory。
  serial 与 worker 数无关，只跑一次每 chunk。
  之后用 tools/performance/summarize_m7_task_sweep.ps1 汇总曲线并给出默认值。

.EXAMPLE
  pwsh tools/performance/run_m7_task_sweep.ps1
  pwsh tools/performance/run_m7_task_sweep.ps1 -Workers 1,2,4,8 -ChunkSizes 64,256,1024
#>
[CmdletBinding()]
param(
    [string]$Executable = 'out/build/windows-msvc-profile/tools/tasks_bench/Release/MiniEngineTasksBench.exe',
    [string]$OutputDirectory = 'out/m7-04/sweep',
    # 数组参数用逗号分隔的字符串（便于 Start-Process/CI 传参）。
    [string]$Workers = '1,2,4,6,8',
    [string]$ChunkSizes = '32,64,128,256,512,1024',
    [string]$Modes = 'serial,global,per-worker',
    [int]$Entities = 50000,
    [int]$TopLevelTasks = 8,
    [int]$WarmupFrames = 20,
    [int]$MeasureFrames = 60,
    [int]$Seed = 6657,
    # M7-TASK-NOISE：单 run 的 cell 只能算"帧内 MAD"，而 M7-09 的阈值算式是"run 间 relative MAD"。
    # Repeats >= 2 才让噪声下限口径成立（汇总器会在 reports 里标注实际使用的口径）。
    [int]$Repeats = 1,
    [switch]$Interleave,
    [int]$ShuffleSeed = 20260919
)

$ErrorActionPreference = 'Stop'

$workerList = @($Workers -split ',' | ForEach-Object { [int]$_.Trim() })
$chunkList = @($ChunkSizes -split ',' | ForEach-Object { [int]$_.Trim() })
$modeList = @($Modes -split ',' | ForEach-Object { $_.Trim() })

if (-not (Test-Path -LiteralPath $Executable)) { throw "missing bench executable: $Executable" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$plan = New-Object System.Collections.Generic.List[object]
foreach ($mode in $modeList) {
    $roundWorkers = if ($mode -eq 'serial') { @(1) } else { $workerList }
    foreach ($chunk in $chunkList) {
        foreach ($worker in $roundWorkers) {
            foreach ($run in 1..$Repeats) {
                $plan.Add([pscustomobject]@{ mode = $mode; workers = $worker; chunk = $chunk; runIndex = $run })
            }
        }
    }
}
if ($Interleave) {
    $rng = [System.Random]::new($ShuffleSeed)
    $shuffled = @($plan)
    for ($i = $shuffled.Count - 1; $i -gt 0; --$i) {
        $j = $rng.Next($i + 1)
        $tmp = $shuffled[$i]; $shuffled[$i] = $shuffled[$j]; $shuffled[$j] = $tmp
    }
    $plan = New-Object System.Collections.Generic.List[object]
    foreach ($item in $shuffled) { $plan.Add($item) }
}

$cells = 0
$sequence = 0
$records = New-Object System.Collections.Generic.List[object]
foreach ($item in $plan) {
    ++$sequence
    $suffix = if ($Repeats -gt 1) { "-r$($item.runIndex)" } else { '' }
    $name = "$($item.mode)-w$($item.workers)-c$($item.chunk)$suffix.json"
    $path = Join-Path $OutputDirectory $name
    $started = Get-Date
    & $Executable "--mode=$($item.mode)" "--workers=$($item.workers)" "--chunk=$($item.chunk)" "--entities=$Entities" `
        "--top-level-tasks=$TopLevelTasks" "--warmup-frames=$WarmupFrames" `
        "--measure-frames=$MeasureFrames" "--seed=$Seed" "--output=$path" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cell failed: $name (exit $LASTEXITCODE)" }
    $cells++
    $sha = if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { '' }
    $records.Add([pscustomobject]@{
            cell          = "$($item.mode)-w$($item.workers)-c$($item.chunk)"
            mode          = $item.mode
            workers       = [int]$item.workers
            chunk         = [int]$item.chunk
            runIndex      = [int]$item.runIndex
            sequence      = [int]$sequence
            file          = $name
            startedUtc    = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
            metricsSha256 = $sha
        })
    Write-Host ("cell {0} [{1}/{2}]" -f $name, $sequence, $plan.Count)
}
# 驱动摘要：执行顺序是漂移分析的前提（比较器读 executionOrder / startedUtc）。
$ordered = @($records | Sort-Object -Property sequence)
$order = @()
foreach ($record in $ordered) { $order += ('{0}#{1}' -f $record.cell, $record.runIndex) }
$exePath = (Resolve-Path -LiteralPath $Executable).Path
$summary = [ordered]@{
    schemaVersion    = 1
    generatedUtc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    repeats          = [int]$Repeats
    interleave       = [bool]$Interleave
    shuffleSeed      = [int]$ShuffleSeed
    executable       = $exePath
    executableSha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    cells            = $ordered
    executionOrder   = $order
}
$summaryPath = Join-Path $OutputDirectory 'task-sweep-driver-summary.json'
[System.IO.File]::WriteAllText($summaryPath, (($summary | ConvertTo-Json -Depth 6) + "`n"), [System.Text.Encoding]::UTF8)
Write-Host ("driver summary: {0}" -f $summaryPath)
