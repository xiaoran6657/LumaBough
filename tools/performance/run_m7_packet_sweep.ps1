<#
.SYNOPSIS
  M7-05 并行 RenderPacket 实验驱动：E-M7-RP-001..004 与 worker scaling 的固定 cell 矩阵。

.DESCRIPTION
  每个 cell 是独立进程（隔离调度器与内存状态），raw JSON + 截图落 -OutputDirectory。
  控制变量全部显式传给 sandbox（benchmark 模式缺省即报错，不在本脚本补默认值）。

  cell 定义（mode/scheduler/workers/chunk/reserve 唯一）：
    serial-w1-c256                     E-M7-RP-001 基线（M7-01 路径）
    par-global-w8-c256                 E-M7-RP-001 candidate / E-M7-RP-002 基线
    par-pw-w8-c256                     E-M7-RP-002 candidate / E-M7-RP-003 参考点
    par-pw-w8-c{32,64,128,512,1024}    E-M7-RP-003 chunk sweep
    par-pw-w8-c256-noreserve           E-M7-RP-004 candidate
    par-pw-w{1,2,4,12,16}-c256         worker scaling（A16 瓶颈移动曲线）

.EXAMPLE
  pwsh tools/performance/run_m7_packet_sweep.ps1 `
    -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
    -OutputDirectory out/m7-05/sweep -Repeats 2
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Scene = 'm7-cpu-scale',
    [string]$Rhi = 'd3d12',
    [int]$WarmupFrames = 120,
    [int]$MeasureFrames = 600,
    [int]$Seed = 6657,
    [int]$Repeats = 1,
    [string]$MachineManifest = 'out/performance/environment.json',
    # 前台 gate：on 时前台比例 <0.9 即 FAIL（交互式采集用）；off 时记录比例但不判失败
    # （自动化采集用，性能结论必须带 run notes 的 foregroundRatio 解读）。
    [ValidateSet('on', 'off')][string]$ForegroundGate = 'off',
    # 每个 run 的最大尝试次数（环境稳定性重试；所有尝试的退出码都进 driver summary）。
    [int]$Retries = 3,
    # 过滤 cell 前缀，逗号分隔（便于 Start-Process/CI 传参）；空 = 全部。
    [string]$Only = '',
    # M7-09：A/B 交错采集。开启后按固定 seed 全量随机打乱 (cell, run) 执行顺序
    # （09 文档："每个 run 启动新进程；记录顺序"），并把 sequence 与 runOrderNote
    # 写进 driver summary 与 raw JSON，供漂移检查使用。
    [switch]$Interleave,
    [int]$ShuffleSeed = 20260918,
    # 实现变体 A/B（M7-RP-ORDER-001 起）：把 cell 表里的 variant/experiment 覆写成调用方
    # 指定的值。两侧二进制不同、控制变量相同，`variant` 因此成为唯一的治疗字段
    # （compare_m7_results.ps1 -TreatmentField variant -AllowExecutableChange）。
    # 空 = 保持 cell 表原值（默认行为不变）。
    [string]$VariantOverride = '',
    [string]$ExperimentOverride = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# 焦点干预的实测教训（2026-09-17）：自动化会话里用 AttachThreadInput +
# SetForegroundWindow 反复抢前台会让 runner 命中 "M7 scene requires a stable,
# non-minimized window"（窗口激活/尺寸在抢焦过程中抖动）。因此本驱动**不操作焦点**：
# 只以 --foreground-gate=off 运行并把逐 run 的 foregroundRatio 写进 run notes，
# packet 阶段指标（packet/cull/merge/wait/sort/tasks）是 CPU 侧测量，不受 DWM 节流
# 影响；frame/present 类指标带节流风险，报告里单独标注。

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$manifest = (Resolve-Path -LiteralPath (Join-Path $repoRoot $MachineManifest)).Path
[System.IO.Directory]::CreateDirectory($output) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $output 'logs')) | Out-Null

# 前台 gate：自动化会话无法稳定保持前台（IDE 等会周期性抢占），默认关闭并记录
# foregroundRatio；runner 仍会尽力聚焦，原始比例写入每个 run 的 run-notes.json。
$gateValue = if ($ForegroundGate) { $ForegroundGate } else { 'off' }

# cell 计划：id 只用于命名与筛选；控制变量直接映射到 CLI。
$plan = @(
    [pscustomobject]@{ id = 'serial-w1-c256';          mode = 'serial';   scheduler = 'none';       workers = 1;  chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-001'; variant = 'baseline' }
    [pscustomobject]@{ id = 'par-global-w8-c256';      mode = 'parallel'; scheduler = 'global';     workers = 8;  chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-001'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c256';          mode = 'parallel'; scheduler = 'per-worker'; workers = 8;  chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-002'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c32';           mode = 'parallel'; scheduler = 'per-worker'; workers = 8;  chunk = 32;   reserve = $true;  experiment = 'E-M7-RP-003'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c64';           mode = 'parallel'; scheduler = 'per-worker'; workers = 8;  chunk = 64;   reserve = $true;  experiment = 'E-M7-RP-003'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c128';          mode = 'parallel'; scheduler = 'per-worker'; workers = 8;  chunk = 128;  reserve = $true;  experiment = 'E-M7-RP-003'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c512';          mode = 'parallel'; scheduler = 'per-worker'; workers = 8;  chunk = 512;  reserve = $true;  experiment = 'E-M7-RP-003'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c1024';         mode = 'parallel'; scheduler = 'per-worker'; workers = 8;  chunk = 1024; reserve = $true;  experiment = 'E-M7-RP-003'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w8-c256-noreserve'; mode = 'parallel'; scheduler = 'per-worker'; workers = 8; chunk = 256;  reserve = $false; experiment = 'E-M7-RP-004'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w1-c256';          mode = 'parallel'; scheduler = 'per-worker'; workers = 1;  chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-SCALE'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w2-c256';          mode = 'parallel'; scheduler = 'per-worker'; workers = 2;  chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-SCALE'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w4-c256';          mode = 'parallel'; scheduler = 'per-worker'; workers = 4;  chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-SCALE'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w12-c256';         mode = 'parallel'; scheduler = 'per-worker'; workers = 12; chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-SCALE'; variant = 'candidate' }
    [pscustomobject]@{ id = 'par-pw-w16-c256';         mode = 'parallel'; scheduler = 'per-worker'; workers = 16; chunk = 256;  reserve = $true;  experiment = 'E-M7-RP-SCALE'; variant = 'candidate' }
)

# -Only 条目：前缀匹配；以 '!' 结尾表示"完整 id 精确匹配"（必须写全 id，例如
# "par-pw-w8-c256!" 精确命中同名前缀 cell，不会命中 "-noreserve"）。用显式循环而不是
# 嵌套 Where-Object：内层 scriptblock 看不到外层 scriptblock 的局部变量
# （PowerShell 作用域），嵌套写法会静默匹配不到任何 cell。
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
    # 每个 run 独立目录：raw JSON、截图与 run-notes.json 不互相覆盖（A15 需要逐 run 截图）。
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
        "--chunk-size=$($Cell.chunk)"
        "--packet-build=$($Cell.mode)"
        # M7-08 起 m7-cpu-scale 在 benchmark 下要求显式布局控制变量；本 sweep 不做布局
        # 实验 → 显式 none（不运行布局内核，帧时间口径与 M7-05 采集一致）。
        '--layout=none'
        "--metrics=$metricsPath"
        "--machine-manifest=$manifest"
        "--experiment-id=$(if ($ExperimentOverride) { $ExperimentOverride } else { $Cell.experiment })"
        "--variant=$(if ($VariantOverride) { $VariantOverride } else { $Cell.variant })"
        "--run-index=$RunIndex"
        )
    if ($Sequence -gt 0) {
        $noteVariant = if ($VariantOverride) { $VariantOverride } else { $Cell.variant }
        $arguments += "--run-order-note=seq=$Sequence/$SequenceTotal cell=$($Cell.id) variant=$noteVariant run=$RunIndex"
    }
    if ($Cell.mode -eq 'parallel') {
        $arguments += "--scheduler=$($Cell.scheduler)"
        $arguments += "--chunk-reserve=$(if ($Cell.reserve) { 'on' } else { 'off' })"
    }
    $arguments += "--foreground-gate=$gateValue"

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
        # 不用 Start-Process -RedirectStandard*：PS 5.1 下该路径拿不到可靠的 ExitCode。
        # 直接构造 ProcessStartInfo（UseShellExecute=false）以可靠捕获输出与 ExitCode。
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
        # 失败重试是环境稳定性问题（自动化会话的窗口抖动），不是数据挑选：所有尝试的
        # 退出码都记录在 driver summary，最终仍失败即抛错。
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
        mode            = $Cell.mode
        scheduler       = $Cell.scheduler
        workers         = $Cell.workers
        chunkSize       = $Cell.chunk
        chunkReserve    = $Cell.reserve
        runIndex        = $RunIndex
        experiment      = $Cell.experiment
        variant         = $Cell.variant
        attempts        = $attempts
        exitCode        = $exitCode
        sequence        = $Sequence
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
    # 执行计划：默认按 (run, cell) 顺序；-Interleave 时按固定 seed 全量打乱
    # （固定 seed 的 Fisher-Yates，可复现；顺序本身进 summary 供漂移检查）。
    # 注意：PS 5.1 下 @(<List[object]>) 会抛 "Argument types do not match"，
    # 计划因此用普通数组累积（规模 <= 细胞数 × 重复数，代价可忽略）。
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
        schemaVersion    = 1
        generatedUtc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        scene            = $Scene
        rhi              = $Rhi
        warmupFrames     = $WarmupFrames
        measuredFrames   = $MeasureFrames
        seed             = $Seed
        interleave       = [bool]$Interleave
        shuffleSeed      = $ShuffleSeed
        executable       = $exe
        executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        machineManifest  = $manifest
        machineManifestSha256 = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
        dryRun           = [bool]$DryRun
        cells            = @($script:summary | Sort-Object sequence)
        executionOrder   = @($script:summary | Sort-Object sequence | ForEach-Object { '{0}#{1}' -f $_.cell, $_.runIndex })
    }
    [System.IO.File]::WriteAllText($summaryPath, (($payload | ConvertTo-Json -Depth 6) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("driver summary: {0}" -f $summaryPath)
}
