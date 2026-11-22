# Finds the exposure bias that makes a SceneCapture match the game's own view (flat UE4SS game): captures the game
# camera's view with a bias, takes a HighResShot of the real frame, compares mean linear luminance, repeats.
#   tools\calibrate_exposure.ps1      prints the bias to pass to capture_scene.ps1 -Bias
param([int]$Iterations = 4, [double]$Bias = 0)
$t = $PSScriptRoot
$root = Split-Path $t
$dir = Join-Path $root "work\env\calib"
New-Item -ItemType Directory -Force $dir | Out-Null
$shots = "$env:LOCALAPPDATA\FFRS\Saved\Screenshots\Windows"
Add-Type -AssemblyName System.Drawing
function Get-MeanLum([string]$path) {
    $im = [Drawing.Image]::FromFile($path)
    $b = New-Object Drawing.Bitmap 64, 36; $g = [Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
    $g.DrawImage($im, 0, 0, 64, 36); $im.Dispose(); $g.Dispose()
    $sum = 0.0
    for ($y = 0; $y -lt 36; $y++) { for ($x = 0; $x -lt 64; $x++) {
        $c = $b.GetPixel($x, $y); $l = 0.0
        foreach ($v in @(@($c.R, 0.2126), @($c.G, 0.7152), @($c.B, 0.0722))) { $s = $v[0] / 255.0; $l += $v[1] * [math]::Pow($s, 2.2) }
        $sum += $l } }
    $b.Dispose()
    return $sum / (64 * 36)
}
# the game's frame
$before = @(Get-ChildItem $shots -Filter *.png -ErrorAction SilentlyContinue | ForEach-Object Name)
& "$t\ue4sslua.ps1" "StaticFindObject('/Script/Engine.Default__KismetSystemLibrary'):ExecuteConsoleCommand(FindFirstOf('PlayerController'), 'HighResShot 1', nil)" | Out-Null
Start-Sleep 3
$game = Get-ChildItem $shots -Filter *.png | Where-Object { $before -notcontains $_.Name } | Select-Object -First 1
$lg = Get-MeanLum $game.FullName
"game frame: mean luminance {0:F4}" -f $lg
for ($i = 0; $i -lt $Iterations; $i++) {
    $name = "cal$i.png"
    $code = @"
local pc = FindFirstOf("PlayerController"); local world = pc:GetWorld(); local cm = pc.PlayerCameraManager
local l = cm:GetCameraLocation(); local r = cm:GetCameraRotation(); local fov = cm:GetFOVAngle()
local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics"); local krl = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary")
local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = l.X, Y = l.Y, Z = l.Z }, Scale3D = { X = 1, Y = 1, Z = 1 } }
local a = gs:BeginDeferredActorSpawnFromClass(world, StaticFindObject("/Script/Engine.SceneCapture2D"), xf, 1, nil, 0); gs:FinishSpawningActor(a, xf, 0)
local sc = a.CaptureComponent2D
local rt = krl:CreateRenderTarget2D(world, 640, 360, 2, { R = 0, G = 0, B = 0, A = 1 }, false, false)
sc.FOVAngle = fov; sc.CaptureSource = 2; sc.bCaptureEveryFrame = false; sc.bCaptureOnMovement = false; sc.TextureTarget = rt
local pp = sc.PostProcessSettings; pp.bOverride_AutoExposureBias = true; pp.AutoExposureBias = $Bias
sc:K2_SetWorldLocationAndRotation({ X = l.X, Y = l.Y, Z = l.Z }, { Pitch = r.Pitch, Yaw = r.Yaw, Roll = r.Roll }, false, {}, true)
sc:CaptureScene(); krl:ExportRenderTarget(world, rt, [[$dir]], "$name"); a:K2_DestroyActor()
print("captured")
"@
    & "$t\ue4sslua.ps1" $code -TimeoutMs 30000 -WaitFor captured | Out-Null
    $lc = Get-MeanLum "$dir\$name"
    "bias {0:F2}: capture mean luminance {1:F4}" -f $Bias, $lc
    if ($lc -le 0) { break }
    $Bias += [math]::Log($lg / $lc, 2) * 0.8
}
"suggested bias: {0:F2}" -f $Bias
