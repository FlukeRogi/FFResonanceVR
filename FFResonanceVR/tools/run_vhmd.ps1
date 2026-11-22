# Runs any program against the ODE Virtual HMD (no headset needed) and prints the runtime's summary.
#
#   tools\run_vhmd.ps1 -Exe <program.exe> [-Args "..."] [-Frames 300] [-Motion sweep|static|yaw|file]
#                      [-Paced 1] [-Out <dir>] [-TimeoutSec 120] [-Config <ode_vhmd.ini>] [-WorkDir <dir>]
#
# Only this process tree sees the virtual HMD (XR_RUNTIME_JSON is set for the child process alone);
# the system's OpenXR runtime registration is not changed. Exit code: the program's, or 124 on timeout.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Args = "",
    [int]$Frames = 300,
    [string]$Motion = "sweep",
    [int]$Paced = 1,
    [string]$Out = "",
    [int]$TimeoutSec = 120,
    [string]$Config = "",
    [string]$WorkDir = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$Exe = (Resolve-Path $Exe).Path

# pick the runtime matching the program's bitness (PE machine field)
$fs = [IO.File]::OpenRead($Exe)
try {
    $br = New-Object IO.BinaryReader($fs)
    $fs.Position = 0x3C; $pe = $br.ReadInt32(); $fs.Position = $pe + 4; $machine = $br.ReadUInt16()
} finally { $fs.Close() }
$bits = if ($machine -eq 0x8664) { 64 } else { 32 }
$manifest = Join-Path $root "build\vhmd\ode_vhmd$bits.json"
if (-not (Test-Path $manifest)) { throw "Virtual HMD not built: $manifest (run build.bat)" }

if (-not $Out) { $Out = Join-Path $root ("work\vhmd_runs\" + [IO.Path]::GetFileNameWithoutExtension($Exe) + "-" + (Get-Date -Format "yyyyMMdd-HHmmss")) }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = [IO.Path]::GetFullPath((Join-Path (Get-Location) $Out)) }   # the child runs in another folder
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = $Args
$psi.WorkingDirectory = if ($WorkDir) { $WorkDir } else { Split-Path -Parent $Exe }
$psi.UseShellExecute = $false
$psi.EnvironmentVariables["XR_RUNTIME_JSON"] = $manifest
$psi.EnvironmentVariables["ODE_VHMD_OUT"] = $Out
$psi.EnvironmentVariables["ODE_VHMD_MAXFRAMES"] = "$Frames"
$psi.EnvironmentVariables["ODE_VHMD_MOTION"] = $Motion
$psi.EnvironmentVariables["ODE_VHMD_PACED"] = "$Paced"
if ($Config) { $psi.EnvironmentVariables["ODE_VHMD_CONFIG"] = (Resolve-Path $Config).Path }

Write-Host "vHMD x$bits  ->  $Exe $Args"
Write-Host "output       ->  $Out"
$p = [Diagnostics.Process]::Start($psi)
$code = 0
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Host "timeout after $TimeoutSec s - stopping the process tree"
    & taskkill /PID $p.Id /T /F | Out-Null
    $code = 124
} else { $code = $p.ExitCode }

$sum = Join-Path $Out "summary.json"
if (Test-Path $sum) { Write-Host "---- summary.json"; Get-Content $sum | Write-Host }
else { Write-Host "no summary.json (runtime never loaded, or the process was killed before it could write one)"; Get-Content (Join-Path $Out "vhmd.log") -ErrorAction SilentlyContinue | Select-Object -Last 20 | Write-Host }
exit $code
