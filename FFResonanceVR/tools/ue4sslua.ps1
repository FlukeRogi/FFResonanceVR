# Runs Lua in a flat game started with UE4SS only (ffvr_launch --dll work\ue4ss\UE4SS.dll, mod FFVRExec) and prints
# its print() output.   tools\ue4sslua.ps1 'print(1+1)'   or   tools\ue4sslua.ps1 -File script.lua
# -WaitFor <text>: wait for that output line instead of the end of the snippet (for scripts that finish asynchronously)
param([string]$Code, [string]$File, [int]$TimeoutMs = 30000, [string]$WaitFor = "<done>")
$d = Join-Path $PSScriptRoot "..\work\ue4ss"
$o = Join-Path $d "out.txt"
if (Test-Path $o) { Remove-Item $o }
if ($File) { $Code = (Get-Content $File -Raw) + "`n" + $Code }
Set-Content (Join-Path $d "cmd.lua") -Encoding ascii -Value $Code
$t = 0
while ($t -lt $TimeoutMs) {
    Start-Sleep -Milliseconds 250; $t += 250
    if ((Test-Path $o) -and ((Get-Content $o -Raw).Contains($WaitFor) -or (Get-Content $o -Raw) -match "(?m)^error: ")) { break }
}
if (Test-Path $o) { Get-Content $o | Where-Object { $_ -ne "<done>" } } else { Write-Host "(no output after $TimeoutMs ms)" }
