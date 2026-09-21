<#
.SYNOPSIS
  M7-10 集成矩阵驱动：parity（双 RHI 自一致 + 跨后端等价）与 validation（Debug Layer / GBV）。

.DESCRIPTION
  两类 suite 共用一个驱动（每个 run 独立进程，独立目录，含 raw/摘要 + 截图）：

    -Suite parity      两个 RHI 跑同一批 M7 场景（m7-cpu-scale w1/w8、m7-streaming async），
                       每 cell N 次重复：用于"同侧自一致（hash 逐位相同）"与"跨后端等价
                       （packet/graph hash、可见数、draw 数、资源 revision manifest、像素阈值）"。
                       采集的是 benchmark raw（profile 构建，warmup+measure 帧）。
    -Suite validation  Debug 构建 + 非 benchmark 冒烟（--frames），覆盖 d3d11/d3d12 × {--debug}
                       与 d3d12 --gbv：断言 validationMessages == 0、liveResources/alive/retiring 全 0。
                       冒烟不写 raw JSON，驱动把 stdout 的摘要 JSON 落成 run.json（统一消费形态）。

  driver summary 记录 sequence/executionOrder/metricsSha256（与其它 M7 驱动一致）。

.EXAMPLE
  pwsh tools/performance/run_m7_parity_matrix.ps1 -Suite parity `
    -Executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe `
    -OutputDirectory out/m7-10/parity -Repeats 3
  pwsh tools/performance/run_m7_parity_matrix.ps1 -Suite validation `
    -Executable out/build/windows-msvc-debug/samples/rhi_sandbox/Debug/MiniEngineSandbox.exe `
    -OutputDirectory out/m7-10/validation -Frames 300
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('parity', 'validation')][string]$Suite,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$WarmupFrames = 30,
    [int]$MeasureFrames = 120,
    [int]$Frames = 300,
    [int]$Seed = 6657,
    [int]$Repeats = 3,
    [string]$MachineManifest = 'out/performance/environment.json',
    [string]$StreamScript = 'assets/tests/m7/streaming-burst.bin',
    [ValidateSet('on', 'off')][string]$ForegroundGate = 'off',
    [int]$Retries = 3,
    [string]$Only = '',
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

# parity cells：唯一变量是 rhi（跨后端比较）；同 cell 内重复用于自一致。
$parityCells = @(
    [pscustomobject]@{ id = 'cpu-scale-w1-d3d11'; scene = 'm7-cpu-scale'; rhi = 'd3d11'; workers = 1; extra = @('--packet-build=serial'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'cpu-scale-w1-d3d12'; scene = 'm7-cpu-scale'; rhi = 'd3d12'; workers = 1; extra = @('--packet-build=serial'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'cpu-scale-w8-d3d11'; scene = 'm7-cpu-scale'; rhi = 'd3d11'; workers = 8; extra = @('--packet-build=parallel', '--scheduler=per-worker', '--chunk-reserve=on'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'cpu-scale-w8-d3d12'; scene = 'm7-cpu-scale'; rhi = 'd3d12'; workers = 8; extra = @('--packet-build=parallel', '--scheduler=per-worker', '--chunk-reserve=on'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'streaming-async-d3d11'; scene = 'm7-streaming'; rhi = 'd3d11'; workers = 1; extra = @('--packet-build=serial', '--async-assets=on', '--upload-mib-per-frame=4', '--upload-budget-cpu-ms=0.5', '--upload-budget-requests=16', '--upload-budget-reload-percent=50', '--upload-aging-ms=50'); loaderMode = 'async-budget-pipeline' }
    [pscustomobject]@{ id = 'streaming-async-d3d12'; scene = 'm7-streaming'; rhi = 'd3d12'; workers = 1; extra = @('--packet-build=serial', '--async-assets=on', '--upload-mib-per-frame=4', '--upload-budget-cpu-ms=0.5', '--upload-budget-requests=16', '--upload-budget-reload-percent=50', '--upload-aging-ms=50'); loaderMode = 'async-budget-pipeline' }
)

# validation cells：Debug 构建的非 benchmark 冒烟（--debug / --gbv），断言零 validation 消息。
$validationCells = @(
    [pscustomobject]@{ id = 'cpu-scale-d3d11-debuglayer'; scene = 'm7-cpu-scale'; rhi = 'd3d11'; workers = 1; extra = @('--packet-build=serial', '--debug'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'cpu-scale-d3d12-debuglayer'; scene = 'm7-cpu-scale'; rhi = 'd3d12'; workers = 1; extra = @('--packet-build=serial', '--debug'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'cpu-scale-d3d12-gbv'; scene = 'm7-cpu-scale'; rhi = 'd3d12'; workers = 1; extra = @('--packet-build=serial', '--gbv'); loaderMode = 'serial-sync-read-validate' }
    [pscustomobject]@{ id = 'streaming-d3d12-debuglayer'; scene = 'm7-streaming'; rhi = 'd3d12'; workers = 1; extra = @('--packet-build=serial', '--debug', '--async-assets=on', '--upload-mib-per-frame=4', '--upload-budget-cpu-ms=0.5', '--upload-budget-requests=16', '--upload-budget-reload-percent=50', '--upload-aging-ms=50'); loaderMode = 'async-budget-pipeline' }
)

$allCells = if ($Suite -eq 'parity') { $parityCells } else { $validationCells }
$repeats = if ($Suite -eq 'parity') { $Repeats } else { 1 }

$onlyList = @($Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$selected = New-Object System.Collections.Generic.List[object]
foreach ($cell in $allCells) {
    $matched = ($onlyList.Count -eq 0)
    foreach ($entry in $onlyList) {
        if ($entry.EndsWith('!')) {
            if ($cell.id -eq $entry.TrimEnd('!')) { $matched = $true }
        }
        elseif ($cell.id.StartsWith($entry)) { $matched = $true }
    }
    if ($matched) { $selected.Add($cell) }
}
if ($selected.Count -eq 0) { throw "no cells selected (Suite=$Suite Only=$Only)" }

$script:summary = [System.Collections.Generic.List[object]]::new()

function Invoke-Cell {
    param($Cell, [int]$RunIndex, [int]$Sequence = 0, [int]$SequenceTotal = 0)
    $name = '{0}-r{1}' -f $Cell.id, $RunIndex
    $runDirectory = Join-Path $output $name
    [System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
    $runPath = Join-Path $runDirectory 'run.json'
    $logPath = Join-Path (Join-Path $output 'logs') ($name + '.log')

    if ($Suite -eq 'parity') {
        $arguments = @(
            "--rhi=$($Cell.rhi)"
            "--scene=$($Cell.scene)"
            '--benchmark'
            "--warmup-frames=$WarmupFrames"
            "--measure-frames=$MeasureFrames"
            '--vsync=off'
            "--seed=$Seed"
            "--workers=$($Cell.workers)"
            '--chunk-size=256'
            '--layout=none'
            "--metrics=$runPath"
            "--machine-manifest=$manifest"
            "--experiment-id=E-M7-PARITY"
            "--variant=$($Cell.rhi)"
            "--run-index=$RunIndex"
            "--foreground-gate=$ForegroundGate"
        )
        $arguments += $Cell.extra
        if ($Cell.scene -eq 'm7-streaming') {
            $arguments += "--stream-script=$scriptPath"
        }
    }
    else {
        $arguments = @(
            "--rhi=$($Cell.rhi)"
            "--scene=$($Cell.scene)"
            "--frames=$Frames"
            '--vsync=off'
            "--seed=$Seed"
            "--workers=$($Cell.workers)"
            '--chunk-size=256'
            '--layout=none'
            '--headless'
            "--output=$runDirectory"
        )
        $arguments += $Cell.extra
        if ($Cell.scene -eq 'm7-streaming') {
            $arguments += "--stream-script=$scriptPath"
        }
    }
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

    if ($Suite -eq 'validation') {
        # 冒烟不写 raw：把 stdout 里最后一行 JSON 摘要落成 run.json（统一比较形态）。
        $summaryLine = @($stdout -split "`n" | Where-Object { $_.Trim().StartsWith('{"status"') } | Select-Object -Last 1)
        if ($summaryLine.Count -eq 0) { throw "no summary JSON in stdout for $name (see $logPath)" }
        [System.IO.File]::WriteAllText($runPath, $summaryLine[0].Trim() + "`n", (New-Object System.Text.UTF8Encoding($false)))
    }

    $finished = Get-Date
    $hash = if (Test-Path -LiteralPath $runPath) { (Get-FileHash -LiteralPath $runPath -Algorithm SHA256).Hash } else { '' }
    $entry = [pscustomobject]@{
        cell            = $Cell.id
        suite           = $Suite
        scene           = $Cell.scene
        rhi             = $Cell.rhi
        workers         = $Cell.workers
        runIndex        = $RunIndex
        sequence        = $Sequence
        variant         = $Cell.rhi
        attempts        = $attempts
        exitCode        = $exitCode
        metricsPath     = $runPath
        metricsSha256   = $hash
        startedUtc      = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        durationSeconds = [math]::Round(($finished - $started).TotalSeconds, 3)
        status          = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $script:summary.Add($entry)
    Write-Host ("{0} exit={1} {2}s sha={3}" -f $name, $exitCode, $entry.durationSeconds, $hash)
    if ($exitCode -ne 0) {
        throw "run failed ($exitCode): $name (log: $logPath)"
    }
}

try {
    $planItems = @()
    foreach ($run in 1..$repeats) {
        foreach ($cell in $selected) {
            $planItems += [pscustomobject]@{ cell = $cell; run = $run }
        }
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
        suite                 = $Suite
        warmupFrames          = $WarmupFrames
        measuredFrames        = $MeasureFrames
        frames                = $Frames
        seed                  = $Seed
        executable            = $exe
        executableSha256      = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        machineManifest       = $manifest
        machineManifestSha256 = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
        streamScript          = $scriptPath
        streamScriptSha256    = (Get-FileHash -LiteralPath $scriptPath -Algorithm SHA256).Hash
        dryRun                = [bool]$DryRun
        cells                 = @($script:summary | Sort-Object sequence)
        executionOrder        = @($script:summary | Sort-Object sequence | ForEach-Object { '{0}#{1}' -f $_.cell, $_.runIndex })
    }
    [System.IO.File]::WriteAllText($summaryPath, (($payload | ConvertTo-Json -Depth 6) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("driver summary: {0}" -f $summaryPath)
}
