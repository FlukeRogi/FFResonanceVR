# Runs capture_view.lua (flat UE4SS game) and turns the six HighResShots into square cube faces f r b l u d .png.
# Exposure: the forward face is first shot with the game's auto exposure; then a manual exposure (EV bias) is found
# that gives the same mean brightness, and all six faces are shot with it, so there are no seams between faces.
#   tools\capture_view.ps1 -Out work\env\x -X 0 -Y 0 -Z 160 [-Yaw 0] [-ExposureEv <fixed bias, skips calibration>]
param([Parameter(Mandatory = $true)][string]$Out, [double]$X, [double]$Y, [double]$Z, [double]$Yaw = 0,
      [double]$Settle = 4, [double]$First = 4, [int]$Mult = 3, [int]$Size = 2048, [string]$ExposureEv = "", [double]$Overscan = 1.2, [double]$Extent = 1.1)
# Overscan: each shot covers this much more than the face (the game's screen-space lighting darkens the top and bottom
# ~2% of the screen, which is exactly where the up and down faces meet the side faces); only the centre is kept.
# Extent: each face image covers -Extent..+Extent (tangent units) instead of -1..1, so neighbouring faces overlap and
# the renderer can cross-fade them at the seams (ffvr_env.cpp reads the extent from env.ini).
$fov = 2 * [math]::Atan(16.0 / 9.0 * $Overscan) * 180 / [math]::PI
$t = $PSScriptRoot
$shots = "$env:LOCALAPPDATA\FFRS\Saved\Screenshots\Windows"
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing

function Invoke-Shots([string]$ExposureEv, [bool]$onlyF, [int]$mult, [double]$first) {
    $before = @(Get-ChildItem $shots -Filter *.png -ErrorAction SilentlyContinue | ForEach-Object Name)
    $evs = if ($ExposureEv -ne "") { "ev = $ExposureEv," } else { "" }
    $of = if ($onlyF) { "true" } else { "false" }
    $code = "VIEW = { x = $X, y = $Y, z = $Z, yaw = $Yaw, fov = $fov, settle = $Settle, first = $first, mult = $mult, $evs only_f = $of }`n" + (Get-Content "$t\capture_view.lua" -Raw)
    & "$t\ue4sslua.ps1" $code -TimeoutMs 120000 -WaitFor captured | Out-Null
    Start-Sleep 2
    return @(Get-ChildItem $shots -Filter *.png | Where-Object { $before -notcontains $_.Name } | Sort-Object Name)
}
function Get-MeanLum([string]$path) {
    # centre square (the face), linear luminance
    $im = [Drawing.Image]::FromFile($path)
    $s = [int]($im.Height / $Overscan); $x0 = [int](($im.Width - $s) / 2); $y0 = [int](($im.Height - $s) / 2)
    $b = New-Object Drawing.Bitmap 48, 48; $g = [Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
    $g.DrawImage($im, (New-Object Drawing.Rectangle 0, 0, 48, 48), (New-Object Drawing.Rectangle $x0, $y0, $s, $s), [Drawing.GraphicsUnit]::Pixel)
    $im.Dispose(); $g.Dispose()
    $sum = 0.0
    for ($yy = 0; $yy -lt 48; $yy++) { for ($xx = 0; $xx -lt 48; $xx++) {
        $c = $b.GetPixel($xx, $yy)
        $sum += 0.2126 * [math]::Pow($c.R / 255.0, 2.2) + 0.7152 * [math]::Pow($c.G / 255.0, 2.2) + 0.0722 * [math]::Pow($c.B / 255.0, 2.2) } }
    $b.Dispose()
    return $sum / (48 * 48)
}

if ($ExposureEv -eq "") {
    $ref = Invoke-Shots "" $true 1 $First
    if ($ref.Count -ne 1) { Write-Host "calibration shot failed"; exit 1 }
    $target = Get-MeanLum $ref[0].FullName; Remove-Item $ref[0].FullName
    $e = 0.0
    for ($i = 0; $i -lt 4; $i++) {
        $s = Invoke-Shots ("{0:F3}" -f $e) $true 1 1.5
        $l = Get-MeanLum $s[0].FullName; Remove-Item $s[0].FullName
        "  EV {0,6:F2}: {1:F4} (auto exposure: {2:F4})" -f $e, $l, $target
        if ($l -le 0 -or [math]::Abs([math]::Log($target / $l, 2)) -lt 0.05) { break }
        $e += [math]::Max(-4.0, [math]::Min(4.0, [math]::Log($target / $l, 2)))   # (double literals: int overload otherwise)
    }
    $ExposureEv = "{0:F3}" -f $e
}
"exposure EV bias $ExposureEv"
$new = Invoke-Shots $ExposureEv $false $Mult $First
if ($new.Count -ne 6) { Write-Host "expected 6 screenshots, got $($new.Count)"; exit 1 }
$names = 'f', 'r', 'b', 'l', 'u', 'd'
foreach ($n in $names) { Remove-Item (Join-Path $Out "$n.orig.png") -ErrorAction SilentlyContinue }   # fix_seams.ps1 keeps the uncorrected faces there
for ($i = 0; $i -lt 6; $i++) {
    $im = [Drawing.Image]::FromFile($new[$i].FullName)
    $s = [int]($im.Height * $Extent / $Overscan); $x0 = [int](($im.Width - $s) / 2); $y0 = [int](($im.Height - $s) / 2)
    $b = New-Object Drawing.Bitmap $Size, $Size
    $g = [Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($im, (New-Object Drawing.Rectangle 0, 0, $Size, $Size), (New-Object Drawing.Rectangle $x0, $y0, $s, $s), [Drawing.GraphicsUnit]::Pixel)
    $b.Save((Join-Path $Out "$($names[$i]).png"), [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $b.Dispose(); $im.Dispose()
    Remove-Item $new[$i].FullName
}
Set-Content (Join-Path $Out "exposure.txt") -Value $ExposureEv
"faces written to $Out"

