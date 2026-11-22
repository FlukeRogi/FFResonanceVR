# Packs full-resolution depth faces for shipping. For each face <f>.f32 in -Dir that is larger than 512 x 512:
#  * <f>.zd   the full-resolution depth, compact: 16-bit log2(depth cm) * 4096 (0.02% steps), each row stored as
#             differences to the previous pixel, deflate-compressed. Header: "FFZD", uint32 size. Read by ffvr_env.cpp
#             (fine depth for the edge texture fixes).
#  * <f>.f32  replaced by a 512 x 512 version (the pixel nearest each 512-grid pixel centre), used for the mesh.
#   tools\pack_depth.ps1 -Dir assets\env\castle
param([Parameter(Mandatory = $true)][string]$Dir, [int]$MeshSize = 512)
Add-Type -TypeDefinition @"
using System; using System.IO; using System.IO.Compression;
public static class FfzdPack {
    public static string Pack(string f32, int meshSize) {
        byte[] raw = File.ReadAllBytes(f32);
        int n = (int)Math.Round(Math.Sqrt(raw.Length / 4.0));
        if ((long)n * n * 4 != raw.Length || n <= meshSize) return Path.GetFileName(f32) + ": skipped (" + n + ")";
        float[] d = new float[n * n]; Buffer.BlockCopy(raw, 0, d, 0, raw.Length);
        var ms = new MemoryStream();
        using (var z = new DeflateStream(ms, CompressionLevel.Optimal, true)) {
            byte[] row = new byte[n * 2];
            for (int y = 0; y < n; ++y) {
                int prev = 0;
                for (int x = 0; x < n; ++x) {
                    double v = d[y * n + x]; if (!(v > 1)) v = 1;
                    int q = (int)Math.Round(Math.Log(v, 2) * 4096); if (q > 65535) q = 65535;
                    int delta = (q - prev) & 0xFFFF; prev = q;
                    row[2 * x] = (byte)delta; row[2 * x + 1] = (byte)(delta >> 8);
                }
                z.Write(row, 0, row.Length);
            }
        }
        string zd = Path.ChangeExtension(f32, ".zd");
        using (var o = new BinaryWriter(File.Create(zd))) { o.Write(new byte[] { (byte)'F', (byte)'F', (byte)'Z', (byte)'D' }); o.Write(n); o.Write(ms.ToArray()); }
        // the mesh depth: nearest full-resolution pixel to each mesh-grid pixel centre
        float[] m = new float[meshSize * meshSize];
        for (int y = 0; y < meshSize; ++y) for (int x = 0; x < meshSize; ++x) {
            int sx = (int)((x + 0.5) * n / meshSize), sy = (int)((y + 0.5) * n / meshSize);
            m[y * meshSize + x] = d[sy * n + sx];
        }
        byte[] mb = new byte[m.Length * 4]; Buffer.BlockCopy(m, 0, mb, 0, mb.Length);
        File.WriteAllBytes(f32, mb);
        return Path.GetFileName(f32) + ": " + n + " -> .zd " + (new FileInfo(zd).Length / 1024) + " KB, mesh " + meshSize;
    }
}
"@
foreach ($f in Get-ChildItem $Dir -Filter *.f32) { [FfzdPack]::Pack($f.FullName, $MeshSize) }
