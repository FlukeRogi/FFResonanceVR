# Runs a Lua snippet inside the game (through the mod's UEVR Lua console script) and prints what it print()s.
#   tools\gamelua.ps1 'print(1+1)'
param([Parameter(Mandatory = $true)][string]$Code, [int]$WaitMs = 1500)
$d = Join-Path $env:APPDATA "UnrealVRMod\FFRS-Win64-Shipping\data"
$o = Join-Path $d "ffvr_out.txt"
if (Test-Path $o) { Remove-Item $o }
Set-Content (Join-Path $d "ffvr_console.txt") -Encoding ascii -Value ("lua " + ($Code -replace "`r?`n", " "))
Start-Sleep -Milliseconds $WaitMs
if (Test-Path $o) { Get-Content $o } else { Write-Host "(no output)" }
