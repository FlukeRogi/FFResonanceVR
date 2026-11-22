# Depth-of-field check: requests a vHMD dump and prints the sharpness (mean absolute Laplacian of luminance) of
# each eye quad in three horizontal bands. Tilt-shift DoF blurs the top and bottom bands, so a missing effect in one
# eye shows as a much higher top/bottom sharpness in that eye.   tools\eye_sharpness.ps1 -Out <vhmd output dir>
param([Parameter(Mandatory = $true)][string]$Out)
Add-Type -AssemblyName System.Drawing
if (-not ("Sharp1" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class Sharp1 {
  public static double[] Get(string path) {
    using (var b = new Bitmap(path)) {
      int w = b.Width, h = b.Height;
      var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
      byte[] px = new byte[d.Stride * h]; Marshal.Copy(d.Scan0, px, 0, px.Length); b.UnlockBits(d);
      Func<int, int, double> L = (x, y) => { int i = y * d.Stride + x * 3; return 0.11 * px[i] + 0.59 * px[i + 1] + 0.3 * px[i + 2]; };
      double[] s = new double[3]; long[] n = new long[3];
      for (int y = 1; y < h - 1; y += 2) { int band = Math.Min(2, 3 * y / h);
        for (int x = 1; x < w - 1; x += 2) { s[band] += Math.Abs(4 * L(x, y) - L(x - 1, y) - L(x + 1, y) - L(x, y - 1) - L(x, y + 1)); n[band]++; } }
      for (int i = 0; i < 3; i++) s[i] /= n[i];
      return s;
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
$l = [Sharp1]::Get($new[0].FullName); $r = [Sharp1]::Get($new[1].FullName)
"sharpness top/mid/bottom  L {0:N2} {1:N2} {2:N2} | R {3:N2} {4:N2} {5:N2}" -f $l[0], $l[1], $l[2], $r[0], $r[1], $r[2]
$new | Remove-Item
