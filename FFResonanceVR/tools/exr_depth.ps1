# Converts an OpenEXR file written by UE's ExportRenderTarget (RGBA16f render target, scanline, ZIP or no compression)
# into a raw little-endian float32 file of its R channel (for depth captures: scene depth in cm).
#   tools\exr_depth.ps1 -In d.exr -Out d.f32     prints width, height, min, max
param([Parameter(Mandatory = $true)][string]$In, [Parameter(Mandatory = $true)][string]$Out)
if (-not ("FfvrExr" -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO; using System.IO.Compression; using System.Collections.Generic; using System.Text;
public static class FfvrExr {
    static float Half(ushort h) {
        int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v;
        if (e == 0) v = (float)(m * Math.Pow(2, -24)); else if (e == 31) v = m == 0 ? float.PositiveInfinity : float.NaN;
        else v = (float)((1 + m / 1024.0) * Math.Pow(2, e - 15));
        return s == 1 ? -v : v;
    }
    public static float[] ReadR(string path, out int w, out int h) {
        byte[] b = File.ReadAllBytes(path); int p = 8;
        var chans = new List<KeyValuePair<string, int>>(); int comp = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        while (b[p] != 0) {
            int s = p; while (b[p] != 0) p++; string name = Encoding.ASCII.GetString(b, s, p - s); p++;
            s = p; while (b[p] != 0) p++; string type = Encoding.ASCII.GetString(b, s, p - s); p++;
            int size = BitConverter.ToInt32(b, p); p += 4;
            if (name == "channels") {
                int q = p; while (b[q] != 0) { int t = q; while (b[q] != 0) q++; string cn = Encoding.ASCII.GetString(b, t, q - t); q++;
                    chans.Add(new KeyValuePair<string, int>(cn, BitConverter.ToInt32(b, q))); q += 16; }
            } else if (name == "compression") comp = b[p];
            else if (name == "dataWindow") { x0 = BitConverter.ToInt32(b, p); y0 = BitConverter.ToInt32(b, p + 4); x1 = BitConverter.ToInt32(b, p + 8); y1 = BitConverter.ToInt32(b, p + 12); }
            p += size;
        }
        p++;
        w = x1 - x0 + 1; h = y1 - y0 + 1;
        int lines = comp == 3 ? 16 : 1;
        int chunks = (h + lines - 1) / lines; p += 8 * chunks;
        int ri = -1, off = 0, rOff = 0; foreach (var c in chans) { if (c.Key == "R") { ri = 1; rOff = off; } off += c.Value == 1 ? 2 : 4; }
        if (ri < 0) throw new Exception("no R channel");
        int rowBytes = w * off; float[] r = new float[w * h];
        bool rHalf = true; foreach (var c in chans) if (c.Key == "R") rHalf = c.Value == 1;
        for (int ch = 0; ch < chunks; ++ch) {
            int y = BitConverter.ToInt32(b, p) - y0; int ds = BitConverter.ToInt32(b, p + 4); p += 8;
            int n = Math.Min(lines, h - y); int raw = n * rowBytes; byte[] data;
            if (ds < raw && comp != 0) {
                var ms = new MemoryStream(b, p + 2, ds - 2); var outp = new MemoryStream();
                using (var z = new DeflateStream(ms, CompressionMode.Decompress)) z.CopyTo(outp);
                byte[] t = outp.ToArray();
                for (int i = 1; i < t.Length; ++i) t[i] = (byte)(t[i - 1] + t[i] - 128);
                data = new byte[t.Length]; int half = (t.Length + 1) / 2;
                for (int i = 0, k = 0; k < t.Length; ++i) { data[k++] = t[i]; if (k < t.Length) data[k++] = t[half + i]; }
            } else { data = new byte[raw]; Buffer.BlockCopy(b, p, data, 0, raw); }
            p += ds;
            for (int l = 0; l < n; ++l) for (int x = 0; x < w; ++x) {
                int o = l * rowBytes + rOff * w + x * (rHalf ? 2 : 4);
                r[(y + l) * w + x] = rHalf ? Half(BitConverter.ToUInt16(data, o)) : BitConverter.ToSingle(data, o);
            }
        }
        return r;
    }
}
'@
}
$w = 0; $h = 0
$r = [FfvrExr]::ReadR((Resolve-Path $In).Path, [ref]$w, [ref]$h)
$bytes = New-Object byte[] ($r.Length * 4)
[Buffer]::BlockCopy($r, 0, $bytes, 0, $bytes.Length)
[IO.File]::WriteAllBytes($Out, $bytes)
$m = $r | Measure-Object -Minimum -Maximum
"{0}x{1} min {2:F1} max {3:F1} centre {4:F1}" -f $w, $h, $m.Minimum, $m.Maximum, $r[($h / 2) * $w + $w / 2]
