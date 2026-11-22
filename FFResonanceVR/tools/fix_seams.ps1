# Matches the up and down cube faces to the four side faces along their shared edges. The game's screen-space
# lighting (Lumen screen traces, AO) depends on what is on screen, so a face looking straight down or up is shaded a
# little differently from the side faces where they meet. Per edge, the colour ratio (linear light, per channel)
# between the neighbour just across the edge and the face just inside it is measured; a smooth gain field that
# reaches those ratios at the edges is applied to the face. Works in place on f r b l u d .png (see capture_env.lua
# for the face orientations). The sky (up face) gets a gentler correction that fades out towards its centre.   tools\fix_seams.ps1 -Dir work\env\castle
param([Parameter(Mandatory = $true)][string]$Dir, [double]$Extent = 1.0)   # Extent: the faces cover -Extent..+Extent (tangent units)
if (-not ("FfvrSeams9" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class FfvrSeams9 {
    static float[] lut = new float[256];
    static FfvrSeams9() { for (int i = 0; i < 256; ++i) { double c = i / 255.0; lut[i] = (float)(c <= 0.04045 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4)); } }
    static byte Enc(float v) { v = Math.Max(0, Math.Min(1, v)); double c = v <= 0.0031308 ? v * 12.92 : 1.055 * Math.Pow(v, 1 / 2.4) - 0.055; return (byte)Math.Round(c * 255); }
    // face bases (UE coordinates, yaw 0; the yaw is the same for all faces so it cancels out)
    static double[][] Basis(int k) {
        double[,] py = { { 0, 0 }, { 0, 90 }, { 0, 180 }, { 0, -90 }, { 90, 0 }, { -90, 0 } };
        double p = py[k, 0] * Math.PI / 180, y = py[k, 1] * Math.PI / 180, cp = Math.Cos(p), sp = Math.Sin(p), cy = Math.Cos(y), sy = Math.Sin(y);
        return new[] { new[] { cp * cy, cp * sy, sp }, new[] { -sy, cy, 0.0 }, new[] { -sp * cy, -sp * sy, cp } };
    }
    static double Dot(double[] a, double[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    class Face { public int n; public float[] px; }
    static double E = 1;   // face extent: pixel 0..n covers -E..+E
    static Face Load(string path) {
        using (var bmp = new Bitmap(path)) {
            int n = bmp.Width; var f = new Face { n = n, px = new float[n * n * 3] };
            var d = bmp.LockBits(new Rectangle(0, 0, n, n), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            var row = new byte[d.Stride];
            for (int y = 0; y < n; ++y) { Marshal.Copy(d.Scan0 + y * d.Stride, row, 0, d.Stride);
                for (int x = 0; x < n; ++x) for (int c = 0; c < 3; ++c) f.px[(y * n + x) * 3 + c] = lut[row[x * 3 + 2 - c]]; }
            bmp.UnlockBits(d); return f;
        }
    }
    static void Save(Face f, string path) {
        int n = f.n;
        using (var bmp = new Bitmap(n, n, PixelFormat.Format24bppRgb)) {
            var d = bmp.LockBits(new Rectangle(0, 0, n, n), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            var row = new byte[d.Stride];
            for (int y = 0; y < n; ++y) { for (int x = 0; x < n; ++x) for (int c = 0; c < 3; ++c) row[x * 3 + 2 - c] = Enc(f.px[(y * n + x) * 3 + c]);
                Marshal.Copy(row, 0, d.Scan0 + y * d.Stride, d.Stride); }
            bmp.UnlockBits(d); bmp.Save(path, ImageFormat.Png);
        }
    }
    // mean colour of a (2r+1)^2 window around the pixel that shows direction dir in face k
    static bool Sample(Face[] F, int k, double[] dir, int r, double[] outc) {
        var B = Basis(k); double fw = Dot(dir, B[0]); if (fw <= 0) return false;
        double u = Dot(dir, B[1]) / fw, v = Dot(dir, B[2]) / fw; int n = F[k].n;
        int px = (int)((u / E + 1) * 0.5 * n), py = (int)((1 - v / E) * 0.5 * n);
        if (px < 0 || py < 0 || px >= n || py >= n) return false;
        outc[0] = outc[1] = outc[2] = 0; int cnt = 0;
        for (int y = Math.Max(0, py - r); y <= Math.Min(n - 1, py + r); ++y) for (int x = Math.Max(0, px - r); x <= Math.Min(n - 1, px + r); ++x) {
            for (int c = 0; c < 3; ++c) outc[c] += F[k].px[(y * n + x) * 3 + c]; ++cnt; }
        for (int c = 0; c < 3; ++c) outc[c] /= cnt;
        return true;
    }
    static double[] Dir(int k, double u, double v) { var B = Basis(k); var d = new double[3]; for (int i = 0; i < 3; ++i) d[i] = B[0][i] + u * B[1][i] + v * B[2][i]; return d; }
    static int Owner(double[] dir) { int best = 0; double bv = -1e9; for (int k = 0; k < 6; ++k) { double v = Dot(dir, Basis(k)[0]); if (v > bv) { bv = v; best = k; } } return best; }

    public static string Fix(string dir, string names, double extent) {
        E = extent;
        var nm = names.Split(','); var F = new Face[6];
        for (int k = 0; k < 6; ++k) {
            string p = System.IO.Path.Combine(dir, nm[k] + ".png"), o = System.IO.Path.Combine(dir, nm[k] + ".orig.png");
            if (!System.IO.File.Exists(o)) System.IO.File.Copy(p, o);   // first run: keep the capture as it came
            F[k] = Load(o);
        }
        var log = new System.Text.StringBuilder();
        foreach (int k in new[] { 4, 5 }) {
            int n = F[k].n; const int S = 64; bool up = k == 4; double lo = up ? 0.25 : 0.12, hi = up ? 4.0 : 4.0; int inset = Math.Max(4, n / 80), win = Math.Max(4, n / 64);
            double d = 2.0 * E * inset / n;
            // ratio[edge][sample][channel]; edges: 0 top (v=+1), 1 right (u=+1), 2 bottom (v=-1), 3 left (u=-1)
            var ratio = new double[4, S, 3];
            for (int e = 0; e < 4; ++e) {
                for (int s = 0; s < S; ++s) {
                    double t = -1 + 2 * (s + 0.5) / S, u0, v0, u1, v1;
                    if (e == 0) { u0 = t; v0 = 1 - d; u1 = t; v1 = 1 + d; } else if (e == 1) { u0 = 1 - d; v0 = -t; u1 = 1 + d; v1 = -t; }
                    else if (e == 2) { u0 = t; v0 = -1 + d; u1 = t; v1 = -1 - d; } else { u0 = -1 + d; v0 = -t; u1 = -1 - d; v1 = -t; }
                    var a = new double[3]; var b = new double[3];
                    var dirIn = Dir(k, u0, v0); var dirOut = Dir(k, u1, v1);
                    bool ok = Sample(F, k, dirIn, win, a) && Sample(F, Owner(dirOut), dirOut, win, b);
                    for (int c = 0; c < 3; ++c) ratio[e, s, c] = ok && a[c] > 1e-4 ? Math.Max(lo, Math.Min(hi, b[c] / a[c])) : 1;
                }
                // smooth along the edge (log domain)
                for (int c = 0; c < 3; ++c) { var tmp = new double[S];
                    for (int s = 0; s < S; ++s) { double sum = 0; int cnt = 0; for (int q = Math.Max(0, s - 10); q <= Math.Min(S - 1, s + 10); ++q) { sum += Math.Log(ratio[e, q, c]); ++cnt; } tmp[s] = Math.Exp(sum / cnt); }
                    for (int s = 0; s < S; ++s) ratio[e, s, c] = tmp[s]; }
                double m = 0; for (int s = 0; s < S; ++s) m += (ratio[e, s, 0] + ratio[e, s, 1] + ratio[e, s, 2]) / 3; log.AppendFormat("{0} edge {1}: mean gain {2:F3}\n", nm[k], e, m / S);
            }
            // gain field: a smooth membrane (harmonic interpolation, log domain) that takes the measured gains on the
            // four edges and has no creases inside (an inverse-distance blend left visible ridges along the diagonals)
            const int G = 65;
            var mem = new double[3, G, G];
            Func<int, double, int> sIdx = (e, p) => Math.Max(0, Math.Min(S - 1, (int)((p + 1) * 0.5 * S)));
            for (int c = 0; c < 3; ++c) {
                double mean = 0; for (int e = 0; e < 4; ++e) for (int q = 0; q < S; ++q) mean += Math.Log(ratio[e, q, c]); mean /= 4 * S;
                for (int j = 0; j < G; ++j) for (int i = 0; i < G; ++i) {
                    double u = -1 + 2.0 * i / (G - 1), v = 1 - 2.0 * j / (G - 1);
                    mem[c, j, i] = mean;
                    if (j == 0) mem[c, j, i] = Math.Log(ratio[0, sIdx(0, u), c]);
                    else if (j == G - 1) mem[c, j, i] = Math.Log(ratio[2, sIdx(2, u), c]);
                    else if (i == G - 1) mem[c, j, i] = Math.Log(ratio[1, sIdx(1, -v), c]);
                    else if (i == 0) mem[c, j, i] = Math.Log(ratio[3, sIdx(3, -v), c]);
                }
                for (int it = 0; it < 6000; ++it)
                    for (int j = 1; j < G - 1; ++j) for (int i = 1; i < G - 1; ++i)
                        mem[c, j, i] = 0.25 * (mem[c, j - 1, i] + mem[c, j + 1, i] + mem[c, j, i - 1] + mem[c, j, i + 1]);
            }
            for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
                double u = E * (-1 + 2 * (x + 0.5) / n), v = E * (1 - 2 * (y + 0.5) / n);
                double gx = (Math.Max(-1, Math.Min(1, u)) + 1) * 0.5 * (G - 1), gy = (1 - Math.Max(-1, Math.Min(1, v))) * 0.5 * (G - 1);
                int x0 = Math.Min(G - 2, (int)gx), y0 = Math.Min(G - 2, (int)gy); double fx = gx - x0, fy = gy - y0;
                // the sky keeps its own colour towards the zenith (radial fade: a deepening blue overhead looks natural;
                // matching the whole face to the paler sky at 45 degrees left a darker ring between them)
                double fade = 1;
                if (up) { double r = Math.Sqrt(Math.Min(1, u * u) + Math.Min(1, v * v)), t2 = Math.Max(0, Math.Min(1, (r - 0.25) / 0.75)); fade = t2 * t2 * (3 - 2 * t2); }
                for (int c = 0; c < 3; ++c) {
                    double lg = fade * ((1 - fy) * ((1 - fx) * mem[c, y0, x0] + fx * mem[c, y0, x0 + 1]) + fy * ((1 - fx) * mem[c, y0 + 1, x0] + fx * mem[c, y0 + 1, x0 + 1]));
                    F[k].px[(y * n + x) * 3 + c] *= (float)Math.Exp(lg);
                }
            }
            Save(F[k], System.IO.Path.Combine(dir, nm[k] + ".png"));
        }
        return log.ToString();
    }
}
'@
}
[FfvrSeams9]::Fix((Resolve-Path $Dir).Path, "f,r,b,l,u,d", $Extent)



