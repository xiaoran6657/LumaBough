<#
.SYNOPSIS
    P 批次：由所有者在独立本地终端启动实验采集（保持前台可见控制台）。

.DESCRIPTION
    为什么必须这样启动：采样窗口只有在**驱动进程本身来自前台可见控制台**时才拿得到前台。
    隐藏窗口 / DETACHED_PROCESS / 由后台 agent 以隐藏方式启动的链路上，M7 的前台就绪会
    等满 60 秒并 TIMEOUT（003 第 9 次、004 两次准备中断、005 前的诊断都实测过），
    详见 docs/evidence/NEW-PERFORMANCE.md。

    本脚本故意 **不** 使用 -WindowStyle Hidden，也不做任何窗口/焦点强夺：
    它只是把采集器放在一个普通可见控制台里运行，进度同时写日志。

.PARAMETER Workspace
    工作区相对路径，例如 out/performance/lb-current-005（须已由准备工具生成并有预注册）。

.PARAMETER Action
    pilot（十次预演）/ register（冻结并注册）/ run（25 次正式运行）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/performance/start_portfolio_experiment.ps1 -Workspace out/performance/lb-current-005 -Action run

.NOTES
    采样期间请勿切换窗口、勿锁屏、勿运行其它负载；任何一次正式运行失败都会使整组 INVALID，
    按预注册不重试、不替换、不拼接。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Workspace,
    [Parameter(Mandatory = $true)][ValidateSet('pilot', 'register', 'run')][string]$Action
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$driver = Join-Path $repo 'tools/performance/run_portfolio_experiment.py'
$log = Join-Path $repo "$Workspace/$Action-driver.out.log"
if (-not (Test-Path $driver)) { throw "driver missing: $driver" }
if (-not (Test-Path (Join-Path $repo $Workspace))) { throw "workspace missing: $Workspace" }
if ($Action -eq 'run' -and -not (Test-Path (Join-Path $repo "$Workspace/PREREGISTRATION.json"))) {
    throw "run requires PREREGISTRATION.json in $Workspace"
}
Write-Host "LumaBough: $Action -> $Workspace"
Write-Host '采样期间请保持本窗口在前台可见：不要切换窗口、不要锁屏、不要运行其它负载。'
Set-Location $repo
& python -B $driver $Action --workspace $Workspace 2>&1 | Tee-Object -FilePath $log
exit $LASTEXITCODE
