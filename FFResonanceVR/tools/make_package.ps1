# Builds the player package: release\FFResonanceVR\ (only what the mod needs to run) and release\<Name>.zip.
# Not included: sources, tools, tests, the virtual HMD, logs, the game assets extracted on this PC (FFResonanceVR.exe
# extracts them on the player's first start), UEVR (downloaded by FFResonanceVR.exe on first start), and the development
# console script. Licenses\ (tools\make_licenses.ps1) is included.
param([string]$Name = "FFResonanceVR")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $root "release"
$pkg = Join-Path $rel "FFResonanceVR"
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force $pkg | Out-Null

function Put([string]$from, [string]$to) {
    $dst = Join-Path $pkg $to
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item (Join-Path $root $from) $dst
}
Put "FFResonanceVR.exe" "FFResonanceVR.exe"
Put "README.md" "README.txt"
foreach ($f in "ffvr_launch.exe", "ffvr_boot.dll", "ffvr_layer.dll", "XR_APILAYER_FFVR_window.json", "assetx\assetx.exe", "assetx\FFRS.usmap") {
    Put "build\$f" "build\$f"
}
# default settings (the same values as the Settings defaults in src\layer\ffvr_menu.h)
@"
; FFResonanceVR settings (also changed from the in-game VR menu)
[Screen]
Distance=2.500
Width=3.200
Aspect=1.7778
Height=0.000
[Stereo]
Strength=1.000
HeadMotion=1.000
[Render]
EyeWidth=2048
Margin=0.000
ResolutionScale=1.00
[Environment]
Scene=2
Floor=0
[Game]
DepthOfField=100
BattleIntroLift=6.0
BattleMarkers3D=2
BattleZoom=1
[Debug]
Passthrough=0
"@ | Set-Content -Encoding ascii (Join-Path $pkg "build\ffvr_settings.ini")
# (UEVR is not included: FFResonanceVR.exe downloads it from praydog's GitHub releases on first start)
& (Join-Path $PSScriptRoot "make_licenses.ps1") | Out-Null
Get-ChildItem (Join-Path $root "Licenses") -File | ForEach-Object { Put "Licenses\$($_.Name)" "Licenses\$($_.Name)" }
foreach ($f in "config_overrides.txt", "user_script.txt", "plugins\ffvr_plugin.dll", "scripts\ffvr_camera.lua", "scripts\ffvr_dof.lua", "scripts\ffvr_worldui.lua") {
    Put "uevr_profile\$f" "uevr_profile\$f"
}
Get-ChildItem (Join-Path $root "assets\env") -Recurse -File | Where-Object { $_.Name -notlike "*.orig.*" } | ForEach-Object {
    Put $_.FullName.Substring($root.Length + 1) $_.FullName.Substring($root.Length + 1)
}

$zip = Join-Path $rel "$Name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
$files = Get-ChildItem $pkg -Recurse -File
"{0} files, {1:N1} MB unpacked, zip {2:N1} MB: {3}" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), ((Get-Item $zip).Length / 1MB), $zip
