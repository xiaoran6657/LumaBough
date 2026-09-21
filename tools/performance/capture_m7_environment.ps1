# ============================================================================
# capture_m7_environment.ps1 — 采集机器级环境 manifest（M7-01 第 3 步）
#
# 为什么独立成文件：raw JSON 每次运行都要引用它（--machine-manifest=PATH 并写入
# SHA-256），但机器信息在一次采集批次内不变，不该在每份 raw JSON 里重复且互相漂移。
#
# 用法：
#   pwsh tools/performance/capture_m7_environment.ps1 -Output out/performance/environment.json
#
# 采集不到的项目写空串并在 notes 中说明，不用占位值冒充已测量。
# ============================================================================
param(
    [Parameter(Mandatory = $true)]
    [string]$Output,

    [string]$Note = ''
)

$ErrorActionPreference = 'Stop'

function Get-Safe {
    param([scriptblock]$Body, [string]$Fallback = '')
    try { return (& $Body) } catch { return $Fallback }
}

$cpu = Get-Safe { Get-CimInstance Win32_Processor | Select-Object -First 1 } $null
$system = Get-Safe { Get-CimInstance Win32_ComputerSystem } $null
$os = Get-Safe { Get-CimInstance Win32_OperatingSystem } $null
$videos = @(Get-Safe { Get-CimInstance Win32_VideoController } @())
# 本机存在虚拟显示适配器（远程/串流）：性能证据必须绑定真实 GPU。优先选择名字里
# 不含 Virtual/Basic/Render 的适配器，并把全部适配器一起记录以便复核。
$video = $videos | Where-Object { $_.Name -notmatch 'Virtual|Basic|Render|Remote|GameViewer' } |
    Select-Object -First 1
if (-not $video) { $video = $videos | Select-Object -First 1 }
$powerPlan = Get-Safe { (powercfg /getactivescheme) -replace '.*\(', '' -replace '\).*', '' } ''
$cmakeCommand = Get-Safe { Get-Command cmake -ErrorAction Stop } $null
$cmakeVersion = Get-Safe { (cmake --version | Select-Object -First 1) -replace '^cmake version ', '' } ''
$cmakePath = Get-Safe { $cmakeCommand.Source } ''
$clVersion = Get-Safe { ((cl 2>&1 | Select-Object -First 1) -replace '.*Version ', '').Trim() } ''

$displayMode = Get-Safe {
    Add-Type -AssemblyName System.Windows.Forms
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    '{0}x{1}' -f $bounds.Width, $bounds.Height
} ''

$manifest = [ordered]@{
    schemaVersion      = 1
    capturedUtc        = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    hostName           = (Get-Safe { $env:COMPUTERNAME } '')
    osVersion          = (Get-Safe { '{0} {1}' -f $os.Caption, $os.Version } '')
    cpuModel           = (Get-Safe { $cpu.Name } '')
    physicalCores      = [int](Get-Safe { $cpu.NumberOfCores } 0)
    logicalCores       = [int](Get-Safe { $cpu.NumberOfLogicalProcessors } 0)
    physicalMemoryBytes = [long](Get-Safe { $system.TotalPhysicalMemory } 0)
    gpuAdapter         = (Get-Safe { $video.Name } '')
    gpuDriver          = (Get-Safe { $video.DriverVersion } '')
    gpuDriverDate      = (Get-Safe { if ($video.DriverDate) { $video.DriverDate.ToString('yyyy-MM-dd') } else { '' } } '')
    gpuAdapters        = @($videos | ForEach-Object { $_.Name })
    displayMode        = $displayMode
    powerPlan          = $powerPlan
    cmakeVersion       = $cmakeVersion
    cmakePath          = $cmakePath
    msvcCompilerVersion = $clVersion
    windowsSdkVersion  = (Get-Safe { (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory |
        Sort-Object Name -Descending | Select-Object -First 1).Name } '')
    temperatureTool    = ''
    notes              = $Note
}

$directory = Split-Path -Parent ([System.IO.Path]::GetFullPath($Output))
if ($directory) { [System.IO.Directory]::CreateDirectory($directory) | Out-Null }
$json = ($manifest | ConvertTo-Json -Depth 4) + "`n"
[System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($Output), $json, (New-Object System.Text.UTF8Encoding($false)))

$hash = (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash
Write-Host ("environment manifest: {0} sha256={1}" -f $Output, $hash)
