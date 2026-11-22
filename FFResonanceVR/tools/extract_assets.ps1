# Extracts the game assets the VR menu and the launcher use (fonts, menu textures, button prompts, title logo) from
# the installed game's paks into FFResonanceVR\assets. Nothing from the game is shipped with the mod; this runs on the
# player's machine. FFResonanceVR.exe does the same automatically on its first start (keep the patterns in step with
# kAssetPatterns in src\app\ffvr_app.cpp).
# Uses build\assetx\assetx.exe (tools\assetx, published self-contained by build.bat: no .NET install needed) and
# build\assetx\FFRS.usmap (the game's property mappings).
param([string]$Game = "")
$root = Split-Path -Parent $PSScriptRoot
$ax = Join-Path $root "build\assetx\assetx.exe"
$map = Join-Path $root "build\assetx\FFRS.usmap"
$out = Join-Path $root "assets"
if (-not $Game) {
    # the game's install folder, from Steam's library list
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath -replace '/', '\'
    $libs = @($steam) + (Select-String -Path "$steam\steamapps\libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    foreach ($l in $libs) {
        $m = "$l\steamapps\appmanifest_3259780.acf"
        if (Test-Path $m) { $d = (Select-String -Path $m -Pattern '"installdir"\s+"(.+)"').Matches[0].Groups[1].Value; $Game = "$l\steamapps\common\$d"; break }
    }
}
if (-not $Game -or -not (Test-Path $Game)) { Write-Error "FINAL FANTASY RESONANCE not found in Steam's libraries (pass -Game <folder>)"; exit 1 }
New-Item -ItemType Directory -Force $out | Out-Null
$fonts = "UI/Font/FontFace/SE/(SE-VOGEN|SE-VOGEB___|SE-GARRITHR)\.ufont$"
$tex = "UI/Textures/(menu/(MenuIcon_FingerCursor_R|T_mainmenu_Listlabel|T_mainmenu_Listlabel_Select|T_CommonInsideFrame|T_Window_Frame_Box_4_2|T_menu_Title_Line00|T_TabArrow|T_Menu_line_center|T_menu_Line_Light|T_CommonMenudecoIcon_star)" +
       "|KeyHelp/Key_WIN/KEY_WINpad_(BUTTON_A|BUTTON_B|CROSSKEY_UPDOWN|CROSSKEY_RIGHTLEFT|STICK_L_PUSH|STICK_R_PUSH)" +
       "|KeyHelp/Key_PC/Key_Win_(Enter|Esc|Up|Down|Left|Right|BackSpace)" +
       "|Title/T_TitleLogo(ImageRGB|Text))\.uasset$"   # the title logo, for the launcher window (FFResonanceVR.exe)
& $ax export $fonts $out --usmap $map --game $Game
& $ax export $tex $out --usmap $map --game $Game
Get-ChildItem $out -File | Select-Object Name, Length
