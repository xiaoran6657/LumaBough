<#
.SYNOPSIS
  M7-10 稳定性 soak（A27）：长跑 + 端到端热重载 + 稳定性压力 + 遥测不变量判定。

.DESCRIPTION
  跑一次 m7-streaming 的异步流水线长跑（默认 10,000 帧）：
    * 每 `-ReloadEvery` 帧对已提交资产请求新 revision（端到端热重载：覆盖提交 + 旧资源退休）；
    * 每 `-StressEvery` 帧触发零尺寸挂起 + 恢复（最小化/还原路径，与 streaming 同帧发生）；
    * 每 `-TelemetryEvery` 帧采样句柄数 / 设备驻留对象 / loader 待办 / task 待办 / 常驻内存。
  跑完按下列不变量判定（失败即退出码 1）：
    * 运行器自身 PASS（含 validation 零消息、live 资源 0、alive/retiring 0、无流式失败）；
    * 热重载确实发生（reloadRequests > 0 且 reloadCommits > 0）；
    * 句柄数回落到基线附近、驻留对象不高于基线、常驻内存不增长（阈值见参数）。

.EXAMPLE
  pwsh tools/performance/run_m7_soak.ps1 `
    -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
    -OutputDirectory out/m7-10/soak -Frames 10000 -ReloadEvery 30
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Rhi = 'd3d12',
    [string]$Scene = 'm7-streaming',
    [int]$Frames = 10000,
    [int]$ReloadEvery = 30,
    [int]$StressEvery = 600,
    [int]$TelemetryEvery = 250,
    [string]$StreamScript = 'assets/tests/m7/streaming-burst.bin',
    [double]$UploadMiBPerFrame = 4,
    [double]$UploadBudgetCpuMs = 0.5,
    [int]$UploadBudgetRequests = 16,
    [int]$UploadBudgetReloadPercent = 50,
    [double]$UploadAgingMs = 50,
    # 不变量阈值：句柄允许的净增长 / 峰值余量、常驻内存允许的增长比例。
    [int]$HandleSlack = 8,
    [int]$HandlePeakSlack = 32,
    [double]$ResidentGrowthPercent = 2.0,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$scriptPath = (Resolve-Path -LiteralPath (Join-Path $repoRoot $StreamScript)).Path
[System.IO.Directory]::CreateDirectory($output) | Out-Null

$arguments = @(
    "--rhi=$Rhi"
    "--scene=$Scene"
    "--frames=$Frames"
    '--vsync=off'
    '--async-assets=on'
    "--stream-script=$scriptPath"
    "--upload-mib-per-frame=$UploadMiBPerFrame"
    "--upload-budget-cpu-ms=$UploadBudgetCpuMs"
    "--upload-budget-requests=$UploadBudgetRequests"
    "--upload-budget-reload-percent=$UploadBudgetReloadPercent"
    "--upload-aging-ms=$UploadAgingMs"
    "--loader-reload-every=$ReloadEvery"
    "--stability-stress-every=$StressEvery"
    "--soak-telemetry-every=$TelemetryEvery"
    "--output=$output"
    # --metrics 只用来让运行器把 artifactDirectory 指向本次输出目录（非 benchmark 不写 raw），
    # 于是 stability.csv / stability-summary.json / 末帧截图会落在这里而不是被丢掉。
    "--metrics=$output/soak-metrics.json"
    '--packet-build=serial'
    '--layout=none'
    '--foreground-gate=off'
    '--headless'
)

if ($DryRun) {
    Write-Host ("DRY-RUN {0} {1}" -f $exe, ($arguments -join ' '))
    return
}

$started = Get-Date
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
$duration = [math]::Round(((Get-Date) - $started).TotalSeconds, 3)
[System.IO.File]::WriteAllText((Join-Path $output 'soak.log'), $stdout,
    (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText((Join-Path $output 'soak.log.err'), $stderr,
    (New-Object System.Text.UTF8Encoding($false)))

$summaryLine = @($stdout -split "`n" | Where-Object { $_.Trim().StartsWith('{"status"') } | Select-Object -Last 1)
if ($summaryLine.Count -eq 0) { throw "no summary JSON in stdout (exit=$exitCode; see soak.log)" }
# 把运行器摘要同时落盘（证据）并用 Get-Content -Raw 解析：PS 5.1 下把长字符串直接管道给
# ConvertFrom-Json 会抛 "Argument types do not match"（与 M7-P40 同源的工具坑）。
$runnerSummaryPath = Join-Path $output 'runner-summary.json'
[System.IO.File]::WriteAllText($runnerSummaryPath, $summaryLine[0].Trim() + "`n",
    (New-Object System.Text.UTF8Encoding($false)))
$summary = Get-Content -LiteralPath $runnerSummaryPath -Raw -Encoding UTF8 | ConvertFrom-Json

$failures = New-Object System.Collections.Generic.List[string]
$checks = New-Object System.Collections.Generic.List[object]
function Test-Invariant {
    param([string]$Name, [bool]$Ok, [string]$Detail)
    $checks.Add([pscustomobject]@{ invariant = $Name; ok = $Ok; detail = $Detail })
    if (-not $Ok) { $failures.Add(('{0}: {1}' -f $Name, $Detail)) }
    Write-Host ("  {0,-28} {1}  {2}" -f $Name, $(if ($Ok) { 'OK  ' } else { 'FAIL' }), $Detail)
}

$stability = $summary.stability
Test-Invariant 'runner-pass' ($exitCode -eq 0 -and $summary.status -eq 'PASS') "exit=$exitCode status=$($summary.status)"
Test-Invariant 'validation-messages' ($summary.validationMessages -eq 0) "validationMessages=$($summary.validationMessages)"
Test-Invariant 'no-live-resources' ($summary.liveResources -eq 0) "liveResources=$($summary.liveResources)"
Test-Invariant 'no-live-device-objects' ($summary.aliveObjects -eq 0 -and $summary.retiringObjects -eq 0) `
    "alive=$($summary.aliveObjects) retiring=$($summary.retiringObjects)"
Test-Invariant 'hot-reload-happened' ($summary.reloadRequests -gt 0 -and $summary.reloadCommits -gt 0) `
    "requests=$($summary.reloadRequests) commits=$($summary.reloadCommits) superseded=$($summary.supersededSamples)"
$sampleCount = if ($null -ne $stability) { $stability.samples } else { 0 }
$stressCount = if ($null -ne $stability) { $stability.stressEvents } else { 0 }
Test-Invariant 'telemetry-sampled' ($sampleCount -gt 0) "samples=$sampleCount stressEvents=$stressCount"
if ($null -ne $stability) {
    Test-Invariant 'handles-return-to-baseline' ($stability.finalHandles -le ($stability.baselineHandles + $HandleSlack)) `
        "baseline=$($stability.baselineHandles) peak=$($stability.peakHandles) final=$($stability.finalHandles) slack=$HandleSlack"
    Test-Invariant 'handles-peak-bounded' ($stability.peakHandles -le ($stability.baselineHandles + $HandlePeakSlack)) `
        "peak=$($stability.peakHandles) baseline=$($stability.baselineHandles) peakSlack=$HandlePeakSlack"
    Test-Invariant 'resident-stable' ($stability.residentPeak -le ($stability.residentFirst * (1.0 + $ResidentGrowthPercent / 100.0))) `
        "first=$($stability.residentFirst) peak=$($stability.residentPeak) last=$($stability.residentLast)"
    Test-Invariant 'stress-events' ($stability.stressEvents -gt 0) "stressEvents=$($stability.stressEvents)"
}

$report = [ordered]@{
    schema          = 'miniengine.m7-soak.v1'
    generatedUtc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    executable      = $exe
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    frames          = $Frames
    reloadEvery     = $ReloadEvery
    stressEvery     = $StressEvery
    telemetryEvery  = $TelemetryEvery
    durationSeconds = $duration
    exitCode        = $exitCode
    runnerSummary   = $summary
    # 注意：PS 5.1 下 @(<List[T]>) 会抛 "Argument types do not match"（M7-P40），
    # 用 ToArray() 而不是数组子表达式展开。
    invariants      = $checks.ToArray()
    failures        = $failures.ToArray()
    status          = if ($failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
}
$reportPath = Join-Path $output 'soak-report.json'
[System.IO.File]::WriteAllText($reportPath, (($report | ConvertTo-Json -Depth 8) + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("soak report: {0} ({1}s)" -f $reportPath, $duration)
if ($failures.Count -gt 0) {
    Write-Host "soak FAILED:"
    foreach ($failure in $failures) { Write-Host ("  - {0}" -f $failure) }
    exit 1
}
Write-Host "soak PASSED"
exit 0
