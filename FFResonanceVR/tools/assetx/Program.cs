// assetx - extracts assets from FINAL FANTASY RESONANCE's IoStore paks (CUE4Parse).
//   assetx list <regex>                    asset paths matching the regex (case-insensitive)
//   assetx export <regex> <outdir> [max]   Texture2D -> PNG, FontFace -> font file, others -> JSON
//   options: --usmap <file> (mappings for unversioned properties)
using System.Text.RegularExpressions;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider.Usmap;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using CUE4Parse_Conversion.Textures;
using SkiaSharp;
using Newtonsoft.Json;

var argl = args.ToList();
string? Opt(string name) { int i = argl.IndexOf(name); if (i < 0 || i + 1 >= argl.Count) return null; var v = argl[i + 1]; argl.RemoveRange(i, 2); return v; }
string? usmap = Opt("--usmap");
// --game <install folder>: the game's Steam folder (FFResonanceVR.exe finds it through Steam's libraries)
string game = Opt("--game") ?? @"C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY RESONANCE";
string paks = Path.Combine(game, "FFRS", "Content", "Paks");
if (argl.Count < 2) { Console.WriteLine("usage: assetx list <regex> | export <regex> <outdir> [max] [--usmap file] [--game folder]"); return 1; }
if (!Directory.Exists(paks)) { Console.WriteLine($"ERROR game paks not found: {paks}"); return 2; }

// Oodle (only needed for Oodle-compressed packages) is used if its DLL is next to the tool; it is never downloaded
string oodle = Path.Combine(AppContext.BaseDirectory, OodleHelper.OodleFileName);
if (File.Exists(oodle)) OodleHelper.Initialize(oodle);

var provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE5_6));
if (usmap != null) provider.MappingsContainer = new FileUsmapTypeMappingsProvider(usmap);
provider.Initialize();
provider.SubmitKey(new FGuid(), new FAesKey("0x0000000000000000000000000000000000000000000000000000000000000000"));
provider.PostMount();
Console.Error.WriteLine($"{provider.Files.Count} files mounted");

var re = new Regex(argl[1], RegexOptions.IgnoreCase);
var matches = provider.Files.Keys.Where(k => re.IsMatch(k)).OrderBy(k => k).ToList();
if (argl[0] == "list") { foreach (var m in matches) Console.WriteLine(m); Console.Error.WriteLine($"{matches.Count} matches"); return 0; }

string outDir = argl[2];
int max = argl.Count > 3 ? int.Parse(argl[3]) : 200;
Directory.CreateDirectory(outDir);
int n = 0;
foreach (var path in matches) {
    if (path.EndsWith(".ufont")) {   // font file data, stored as-is (TTF/OTF)
        if (provider.TryCreateReader(path, out var fr)) {
            var bytes = fr.ReadBytes((int)fr.Length);
            // cooked .ufont = int32 byte count + the font file (+ padding)
            int len = BitConverter.ToInt32(bytes, 0);
            if (len > 0 && len <= bytes.Length - 4) bytes = bytes.Skip(4).Take(len).ToArray();
            string ext = bytes.Length > 4 && bytes[0] == 'O' && bytes[1] == 'T' && bytes[2] == 'T' && bytes[3] == 'O' ? ".otf" : ".ttf";
            File.WriteAllBytes(Path.Combine(outDir, Path.GetFileNameWithoutExtension(path) + ext), bytes);
            Console.WriteLine($"FONT {path} {bytes.Length} bytes");
        }
        continue;
    }
    if (!path.EndsWith(".uasset") && !path.EndsWith(".umap")) continue;
    if (n++ >= max) break;
    string baseName = Path.Combine(outDir, Path.GetFileNameWithoutExtension(path));
    try {
        var exports = provider.LoadPackage(path).GetExports().ToList();
        bool done = false;
        foreach (var e in exports) {
            if (e is UTexture2D tex) {
                var ct = TextureDecoder.Decode(tex, ETexturePlatform.DesktopMobile);
                if (ct != null) { using var sk = ct.ToSkBitmap(); using var data = sk.Encode(SKEncodedImageFormat.Png, 100); File.WriteAllBytes(baseName + ".png", data.ToArray()); Console.WriteLine($"PNG  {path} {ct.Width}x{ct.Height}"); done = true; }
            } else if (e.ExportType == "FontFace") {
                foreach (var ext in new[] { ".ufont" }) {
                    var fp = path.Replace(".uasset", ext);
                    if (provider.TryCreateReader(fp, out var rd)) { File.WriteAllBytes(baseName + ".ttf", rd.ReadBytes((int)rd.Length)); Console.WriteLine($"FONT {fp}"); done = true; }
                }
            }
        }
        if (!done) { File.WriteAllText(baseName + ".json", JsonConvert.SerializeObject(exports, Formatting.Indented)); Console.WriteLine($"JSON {path}"); }
    } catch (Exception ex) { Console.WriteLine($"FAIL {path}: {ex.Message}"); }
}
return 0;
