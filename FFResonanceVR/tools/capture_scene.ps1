# Captures one environment scene in the flat UE4SS game and packs it into assets\env\<Name> for the VR layer:
# f r b l u d .jpg colour faces and .f32 depth faces (see src\layer\ffvr_env.cpp).
#   tools\capture_scene.ps1 -Name deck -Map /Game/Map/Town/00Com/00Com_03/Town_00Com_03_PL -X -2500 -Y 0 -Z 160 -Yaw 0
# (-Map is optional: without it the map that is loaded now is used.)
param([Parameter(Mandatory = $true)][string]$Name, [string]$Map, [double]$X, [double]$Y, [double]$Z, [double]$Yaw = 0,
      [int]$Size = 2048, [int]$DepthSize = 2048, [int]$Quality = 92, [int]$LoadWait = 25, [double]$Extent = 1.1)
# (DepthSize 2048 = the colour resolution: tools\pack_depth.ps1 then stores it compactly as <f>.zd, for the edge fixes in
# ffvr_env.cpp, and puts a 512 version in <f>.f32 for the mesh)
$t = $PSScriptRoot
$root = Split-Path $t
$raw = Join-Path $root "work\env\$Name"
$dst = Join-Path $root "assets\env\$Name"
New-Item -ItemType Directory -Force $raw, $dst | Out-Null
$cube = Get-Content "$t\capture_env.lua" -Raw
if ($Map) {
    & "$t\ue4sslua.ps1" "StaticFindObject('/Script/Engine.Default__KismetSystemLibrary'):ExecuteConsoleCommand(FindFirstOf('PlayerController'), 'open $Map', nil)" | Out-Null
    Start-Sleep $LoadWait
}
# colour from the game's main view (Lumen, exposure, post effects), depth from a SceneCapture
& "$t\capture_view.ps1" -Out $raw -X $X -Y $Y -Z $Z -Yaw $Yaw -Size $Size -Extent $Extent
if ($LASTEXITCODE) { exit 1 }
& "$t\fix_seams.ps1" -Dir $raw -Extent $Extent
& "$t\ue4sslua.ps1" ("CAP = { dir = [[$raw]], x = $X, y = $Y, z = $Z, size = $DepthSize, yaw = $Yaw, depth = true }`n" + $cube) -TimeoutMs 120000 -WaitFor captured
Add-Type -AssemblyName System.Drawing
$codec = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$ep = New-Object Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality), ([long]$Quality)
foreach ($f in 'f', 'r', 'b', 'l', 'u', 'd') {
    $im = [Drawing.Image]::FromFile("$raw\$f.png"); $im.Save("$dst\$f.jpg", $codec, $ep); $im.Dispose()
    $info = & "$t\exr_depth.ps1" -In "$raw\${f}_depth" -Out "$dst\$f.f32"
    "$f : $info"
}
& "$t\pack_depth.ps1" -Dir $dst
# env.ini: face extent for the renderer (brightness kept if set)
$ini = "$dst\env.ini"; $bp = 85
if (Test-Path $ini) { $m = Select-String $ini -Pattern "BrightnessPercent=(\d+)"; if ($m) { $bp = $m.Matches[0].Groups[1].Value } }
Set-Content $ini -Encoding ascii -Value "[Env]`r`nBrightnessPercent=$bp`r`nExtent=$Extent"
Set-Content "$dst\capture.txt" -Encoding ascii -Value "map=$Map`r`nx=$X y=$Y z=$Z yaw=$Yaw size=$Size depth=$DepthSize extent=$Extent"
