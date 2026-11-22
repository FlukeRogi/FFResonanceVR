# Flat UE4SS game: loads each map, records the player start and captures a top-down view plus a small cube map there.
#   tools\survey_maps.ps1 -Maps /Game/Map/Town/01Gra/01Gra_01/Town_01Gra_01_PL,... -Out work\env\survey
param([Parameter(Mandatory = $true)][string[]]$Maps, [Parameter(Mandatory = $true)][string]$Out, [int]$Ortho = 16000, [int]$LoadWait = 25)
$t = $PSScriptRoot
$shot = Get-Content "$t\capture_shot.lua" -Raw; $cube = Get-Content "$t\capture_env.lua" -Raw
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($m in $Maps) {
    $n = ($m -split '/')[-1]
    & "$t\ue4sslua.ps1" "StaticFindObject('/Script/Engine.Default__KismetSystemLibrary'):ExecuteConsoleCommand(FindFirstOf('PlayerController'), 'open $m', nil)" | Out-Null
    Start-Sleep $LoadWait
    $pos = @(& "$t\ue4sslua.ps1" "local pc = FindFirstOf('PlayerController'); local l = pc.Pawn:K2_GetActorLocation(); print(string.format('%.0f %.0f %.0f', l.X, l.Y, l.Z))")[0]
    "$n : $pos"
    $x, $y, $z = ($pos -split ' ') | ForEach-Object { [double]$_ }
    & "$t\ue4sslua.ps1" ("SHOT = { dir = [[$Out]], name = '${n}_top.png', x = $x, y = $y, z = $($z + 8000), pitch = -90, yaw = 0, ortho = $Ortho, size = 1024 }`n" + $shot) -TimeoutMs 30000 | Out-Null
    New-Item -ItemType Directory -Force "$Out\$n" | Out-Null
    & "$t\ue4sslua.ps1" ("CAP = { dir = [[$Out\$n]], x = $x, y = $y, z = $($z + 70), size = 512, yaw = 0 }`n" + $cube) -TimeoutMs 60000 | Out-Null
    Add-Type -AssemblyName System.Drawing
    $s = 256; $bmp = New-Object Drawing.Bitmap ($s * 4), ($s * 3); $g = [Drawing.Graphics]::FromImage($bmp); $g.Clear([Drawing.Color]::Magenta)
    $layout = @{ u = @(1, 0); l = @(0, 1); f = @(1, 1); r = @(2, 1); b = @(3, 1); d = @(1, 2) }
    foreach ($k in $layout.Keys) { if (Test-Path "$Out\$n\$k.png") { $im = [Drawing.Image]::FromFile("$Out\$n\$k.png"); $g.DrawImage($im, $layout[$k][0] * $s, $layout[$k][1] * $s, $s, $s); $im.Dispose() } }
    $bmp.Save("$Out\${n}_cube.png"); $g.Dispose(); $bmp.Dispose()
}
