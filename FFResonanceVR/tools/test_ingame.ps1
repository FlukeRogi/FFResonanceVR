# Automated in-game test on the virtual HMD: (re)start the game with the mod, Continue (loads the latest save;
# save slots 1-8 are read-only during tests), then take snapshots.
#   tools\test_ingame.ps1 -Name <run> [-Snaps 2] [-Motion sweep] [-NoLayer] [-Keys "..."]
param([string]$Name = "run", [int]$Snaps = 2, [string]$Motion = "sweep", [switch]$NoLayer, [string[]]$Keys = @(), [int]$LoadWait = 45, [string]$VhmdConfig = "", [string[]]$ExtraEnv = @())
$root = Split-Path -Parent $PSScriptRoot
Stop-Process -Name FFRS-Win64-Shipping -Force -ErrorAction SilentlyContinue
Start-Sleep 4
$out = Join-Path $root "work\runs\$Name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
$a = @("--vhmd", "--delay", "12", "--env", "ODE_VHMD_OUT=$out", "--env", "ODE_VHMD_MOTION=$Motion")
if ($NoLayer) { $a += "--no-layer" }
foreach ($e in $ExtraEnv) { $a += @("--env", $e) }
if ($VhmdConfig) { $a += @("--env", "ODE_VHMD_CONFIG=$VhmdConfig") }
& (Join-Path $root "build\ffvr_launch.exe") @a
Start-Sleep 20
& (Join-Path $PSScriptRoot "gamekeys.ps1") -Keys "Enter"
Start-Sleep $LoadWait
if ($Keys.Count) { & (Join-Path $PSScriptRoot "gamekeys.ps1") -Keys $Keys; Start-Sleep 3 }
for ($i = 1; $i -le $Snaps; $i++) { & (Join-Path $PSScriptRoot "vhmd_snap.ps1") -Out $out -Name "snap$i"; Start-Sleep 3 }


