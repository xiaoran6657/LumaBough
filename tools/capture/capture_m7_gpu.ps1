# ============================================================================
# capture_m7_gpu.ps1 — M7 场景的 GPU 捕获（D3D11 → RenderDoc，D3D12 → PIX）
# 里程碑：M7-12（A25/A26）。M6-10 的 M6 场景捕获流程在 M7 场景上的等价物。
#
# 为什么需要这个脚本：两条捕获路径都被工具链咬过，且都是"必须在同一条命令里复现"
# 的知识（写进脚本而不是文档，避免下次重踩）：
#
# 1) RenderDoc：必须用 `renderdoccmd capture` 启动进程（注入 renderdoc.dll），
#    sandbox 的 `--renderdoc-capture` 会做末帧 programmatic capture。
#    **不能**加 `--opt-api-validation`：RenderDoc 会因此用 debug layer 创建 D3D11
#    设备，与 `--debug` 缺省时的能力校验冲突（实测报 "reported D3D11 capabilities
#    do not match the selected device"）。
#
# 2) PIX（pixtool 2603.25）的坑（逐条实测）：
#    a. `--command-line` 的值不能裸含空格（会被拆成多个未知选项），要带**字面引号**；
#    b. 该版本还**不接受** `--working-directory`；应用参数必须以 `@响应文件` 传递
#       （sandbox 支持 `@path` 展开），因为值里连 `=` 都会被解析器拒绝；
#    c. `PIXIsAttachedForGpuCapture()` 只在 pixtool **launch** 流程为真——attach 流程
#       不能做 GPU capture（PIXTOOL17）；
#    d. 捕获由 PIX 侧 `--captureFromStart take-capture --open save-capture` 完成。
#    本脚本把 2a—2d 全部固化；应用侧只负责正常跑场景（不传 --pix-capture）。
#
# 用法：
#   powershell -File tools/capture/capture_m7_gpu.ps1 -Rhi d3d11 -Scene m7-cpu-scale -Frames 300
#   powershell -File tools/capture/capture_m7_gpu.ps1 -Rhi d3d12 -Scene m7-streaming -Frames 600
# 产物：<OutputDirectory>/<scene>-<rhi>.rdc|.wpix（+ PIX 的 events.csv 可读性证据）
# ============================================================================
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('d3d11', 'd3d12')][string]$Rhi,
    [Parameter(Mandatory = $true)][string]$Executable,
    [string]$Scene = 'm7-cpu-scale',
    [int]$Frames = 300,
    [string]$OutputDirectory = 'out/m7-12/capture',
    [switch]$SkipReadabilityCheck
)

# 原生工具（renderdoccmd/pixtool）会把应用的 stderr 转发到控制台；PS 5.1 在
# $ErrorActionPreference='Stop' 下会把任何 stderr 行当成终止错误（实测：
# sandbox 的 "[M7] stage: ..." 让脚本半路中断）。诊断输出一律写文件后再判定。
$ErrorActionPreference = 'Continue'
$renderDocCmd = 'C:\Program Files\RenderDoc\renderdoccmd.exe'
$pixTool = (Get-ChildItem 'C:\Program Files\Microsoft PIX' -Recurse -Filter 'pixtool.exe' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notlike '*ARM64*' } | Select-Object -First 1).FullName
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$root = (Resolve-Path $OutputDirectory).Path
$exe = (Resolve-Path $Executable).Path
$runDirectory = Join-Path $root $Scene
[System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null

# 场景参数：绝对路径（pixtool 不接受 --working-directory，进程 cwd 不可控）。
$sceneArguments = @(
    "--rhi=$Rhi"
    "--scene=$Scene"
    "--frames=$Frames"
    '--vsync=off'
    '--packet-build=serial'
    '--layout=none'
    '--foreground-gate=off'
    "--output=$runDirectory"
)

if ($Rhi -eq 'd3d11')
{
    if (-not (Test-Path -LiteralPath $renderDocCmd)) { throw "RenderDoc not installed: $renderDocCmd" }
    $capture = Join-Path $root "$Scene-d3d11.rdc"
    if (Test-Path -LiteralPath $capture) { throw "capture already exists: $capture" }
    $template = Join-Path $root 'rdc-template'
    $log = Join-Path $runDirectory 'renderdoc.log'
    Write-Host "[renderdoc] $exe $($sceneArguments -join ' ')"
    # --opt-disallow-vsync 保证捕获期间不打开 vsync；不加 --opt-api-validation（见文件头）。
    & $renderDocCmd capture -c $template -w --opt-hook-children --opt-disallow-vsync $exe `
        ($sceneArguments + "--renderdoc-capture=$capture") *> $log
    Select-String -Path $log -Pattern 'status|failed|error|capabilities' | Select-Object -Last 3 |
        ForEach-Object { Write-Host $_.Line }
    if (-not (Test-Path -LiteralPath $capture)) { throw "RenderDoc capture missing: $capture (log: $log)" }
    Write-Host ("CAPTURE OK: {0} bytes={1}" -f $capture, (Get-Item -LiteralPath $capture).Length)
    return
}

if (-not $pixTool) { throw 'PIX pixtool.exe not found' }
$capture = Join-Path $root "$Scene-d3d12.wpix"
if (Test-Path -LiteralPath $capture) { throw "capture already exists: $capture" }
$runDirectory = Join-Path $root $Scene
[System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null

# 应用参数经响应文件传递（理由见文件头 2b）。
$argumentsFile = Join-Path $runDirectory 'pix-arguments.txt'
[System.IO.File]::WriteAllText($argumentsFile, (($sceneArguments -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
$commandLine = "--command-line=@$argumentsFile"
$log = Join-Path $runDirectory 'pixtool.log'
Write-Host "[pix] $pixTool launch $exe $commandLine"
& $pixTool launch $exe $commandLine --captureFromStart take-capture --open save-capture $capture *> $log
Get-Content -LiteralPath $log | Select-Object -Last 6 | ForEach-Object { Write-Host $_ }
if (-not (Test-Path -LiteralPath $capture)) { throw "PIX capture missing: $capture" }
Write-Host ("CAPTURE OK: {0} bytes={1}" -f $capture, (Get-Item -LiteralPath $capture).Length)

if (-not $SkipReadabilityCheck)
{
    # 可读性证据：回放一次捕获并导出事件表（PIX 的 UI 检查不可自动化，事件表可）。
    $events = Join-Path $runDirectory 'events.csv'
    & $pixTool open-capture $capture perform-single-playback save-event-list $events 2>&1 |
        Select-Object -Last 4 | ForEach-Object { Write-Host $_ }
    if (-not (Test-Path -LiteralPath $events)) { throw 'PIX readability check failed: no event list' }
    $rows = (Get-Content -LiteralPath $events | Measure-Object).Count
    Write-Host ("READABLE: {0} rows={1} bytes={2}" -f $events, $rows, (Get-Item -LiteralPath $events).Length)
}
