# Frame-to-frame image motion of a vHMD run with consecutive dumps: for each consecutive pair of one quad layer's
# dumps (content_NNNNN_quad<Q>.bmp), the best 2-D shift (pixels, block matching at 1/4 scale, so steps of 4 px)
# and the mean absolute difference after aligning. A still head and still game camera should give 0,0 everywhere;
# a "jump and recentre" shows up as a pair of opposite shifts.
#   tools\frame_jumps.ps1 -Dir work\runs\<run> [-Quad 0]
param([Parameter(Mandatory = $true)][string]$Dir, [int]$Quad = 0)
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class FJ {
  public static float[,] Gray(string path) {
    using (var b = new Bitmap(path)) {
      int w = b.Width / 4, h = b.Height / 4; var g = new float[h, w];
      var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
      var buf = new byte[d.Stride * b.Height]; Marshal.Copy(d.Scan0, buf, 0, buf.Length); b.UnlockBits(d);
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float s = 0; for (int yy = 0; yy < 4; yy++) for (int xx = 0; xx < 4; xx++) { int o = (y * 4 + yy) * d.Stride + (x * 4 + xx) * 3; s += buf[o] + buf[o + 1] + buf[o + 2]; }
        g[y, x] = s / 48f;
      }
      return g;
    }
  }
  public static string Compare(float[,] A, float[,] B) {
    int h = A.GetLength(0), w = A.GetLength(1); int R = 12;
    double best = double.MaxValue; int bx = 0, by = 0; double raw = 0; int n0 = 0;
    for (int y = R; y < h - R; y++) for (int x = R; x < w - R; x++) { raw += Math.Abs(A[y, x] - B[y, x]); n0++; }
    for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
      double e = 0; int n = 0;
      for (int y = R; y < h - R; y += 2) for (int x = R; x < w - R; x += 2) { e += Math.Abs(A[y, x] - B[y + dy, x + dx]); n++; }
      e /= n; if (e < best) { best = e; bx = dx; by = dy; }
    }
    return String.Format("shift {0,3},{1,3} px  diff {2,5:F2} (aligned {3,5:F2})", bx * 4, by * 4, raw / n0, best);
  }
}
'@
$files = Get-ChildItem $Dir -Filter "content_*_quad$Quad.bmp" | Sort-Object Name
$prev = $null; $pn = $null
foreach ($f in $files) {
    $g = [FJ]::Gray($f.FullName)
    $n = [int](($f.Name -split '_')[1])
    if ($prev -ne $null -and $n -eq $pn + 1) { "{0} -> {1}: {2}" -f $pn, $n, [FJ]::Compare($prev, $g) }
    $prev = $g; $pn = $n
}
