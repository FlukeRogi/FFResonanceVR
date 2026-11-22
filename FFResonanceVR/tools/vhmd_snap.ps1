# Asks the running virtual HMD for a frame dump and makes a preview PNG: left eye | right eye (top row) and
# every other quad layer (bottom row).   tools\vhmd_snap.ps1 -Out <vhmd output dir> [-Name snap] [-Width 800]
param([Parameter(Mandatory = $true)][string]$Out, [string]$Name = "snap", [int]$Width = 800)
Add-Type -AssemblyName System.Drawing
$before = @(Get-ChildItem $Out -Filter content_*.bmp | ForEach-Object Name)
New-Item -ItemType File -Force (Join-Path $Out "dump.req") | Out-Null
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 200
    if (-not (Test-Path (Join-Path $Out "dump.req"))) { Start-Sleep -Milliseconds 1500; break }
}
$new = @(Get-ChildItem $Out -Filter content_*.bmp | Where-Object { $before -notcontains $_.Name } | Sort-Object Name)
if (-not $new) { Write-Host "no new dump"; exit 1 }
$frame = ($new[0].Name -split '_')[1]
$files = @($new | Where-Object { $_.Name -like "content_${frame}_*" })
$imgs = $files | ForEach-Object { [Drawing.Image]::FromFile($_.FullName) }
$h0 = [int]($imgs[0].Height * $Width / $imgs[0].Width)
$rows = [Math]::Ceiling(($imgs.Count - 2) / 2) + 1
$bmp = New-Object Drawing.Bitmap ($Width * 2 + 10), ($h0 * $rows + 10 * $rows)
$g = [Drawing.Graphics]::FromImage($bmp); $g.Clear([Drawing.Color]::White)
for ($i = 0; $i -lt $imgs.Count; $i++) {
    $x = ($i % 2) * ($Width + 10); $y = [Math]::Floor($i / 2) * ($h0 + 10)
    $h = [int]($imgs[$i].Height * $Width / $imgs[$i].Width)
    $g.DrawImage($imgs[$i], $x, $y, $Width, [Math]::Min($h, $h0))
}
$imgs | ForEach-Object { $_.Dispose() }
$png = Join-Path $Out "$Name.png"
$bmp.Save($png); $g.Dispose(); $bmp.Dispose()
Write-Host $png
