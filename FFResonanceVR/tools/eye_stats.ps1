# Requests a vHMD dump and prints per-eye statistics of the two eye quads: mean brightness and the fraction of
# "dead" pixels (magenta: the colour of this game's NaN blocks; nothing in the world is that colour) - NaN/Inf blocks show up as dead pixels in one eye only.
#   tools\eye_stats.ps1 -Out <vhmd output dir>
param([Parameter(Mandatory = $true)][string]$Out, [switch]$Quiet)
Add-Type -AssemblyName System.Drawing
if (-not ("EyeStat3" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class EyeStat3 {
  public static double[] Get(string path) {
    using (var b = new Bitmap(path)) {
      var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
      byte[] px = new byte[d.Stride * b.Height]; Marshal.Copy(d.Scan0, px, 0, px.Length); b.UnlockBits(d);
      double sum = 0; long dead = 0, n = 0;
      for (int y = 0; y < b.Height; y++) for (int x = 0; x < b.Width; x++) {
        int i = y * d.Stride + x * 3; int bl = px[i], g = px[i + 1], r = px[i + 2];
        sum += r + g + bl; n++;
        if (r > g + 30 && bl > g + 20 && r < 230) dead++;
      }
      return new double[] { sum / n / 3, 100.0 * dead / n };
    }
  }
}
"@
}
$before = @(Get-ChildItem $Out -Filter content_*.bmp | ForEach-Object Name)
New-Item -ItemType File -Force (Join-Path $Out "dump.req") | Out-Null
for ($i = 0; $i -lt 100 -and (Test-Path (Join-Path $Out "dump.req")); $i++) { Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds 1500
$new = @(Get-ChildItem $Out -Filter content_*_quad*.bmp | Where-Object { $before -notcontains $_.Name } | Sort-Object Name)
if ($new.Count -lt 2) { Write-Host "no dump"; exit 1 }
$l = [EyeStat3]::Get($new[0].FullName); $r = [EyeStat3]::Get($new[1].FullName)
$s = "L mean {0:N1} dead {1:N3}% | R mean {2:N1} dead {3:N3}%" -f $l[0], $l[1], $r[0], $r[1]
$new | Remove-Item
$s


