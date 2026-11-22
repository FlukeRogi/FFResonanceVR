# Runs console commands in the game (through the mod's UEVR Lua script).  tools\gamecmd.ps1 "r.SSR.Quality 0" "..."
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Commands)
$f = Join-Path $env:APPDATA "UnrealVRMod\FFRS-Win64-Shipping\data\ffvr_console.txt"
Set-Content $f -Encoding ascii -Value $Commands
for ($i = 0; $i -lt 30 -and (Get-Item $f).Length -gt 0; $i++) { Start-Sleep -Milliseconds 200 }
if ((Get-Item $f).Length -gt 0) { Write-Host "not picked up (is the game running with the mod?)"; exit 1 }

