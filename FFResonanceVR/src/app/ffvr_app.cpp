// FFResonanceVR.exe: the mod's launcher window. Title logo and fonts come from the installed game (assets\, made by
// tools\extract_assets.ps1; nothing from the game is built into the exe). It checks that Steam, the game, the chosen
// VR runtime and the mod's files are there, starts SteamVR if that is chosen, then runs build\ffvr_launch.exe (hidden)
// and turns its output into status messages. The output is also written to build\ffvr_launcher.log.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

static const wchar_t* kGameExe = L"FFRS-Win64-Shipping.exe";
static const wchar_t* kGameAppId = L"3259780";
static const wchar_t* kSteamVrAppId = L"250820";
enum { WM_APP_STATUS = WM_APP + 1 };
enum { ST_INFO, ST_OK, ST_ERR, ST_DONE };

static HWND g_hwnd;
static std::wstring g_root;           // the mod folder (the one holding build\ and uevr\)
static float g_scale = 1.0f;          // DPI / 96
static bool g_busy = false, g_steamvr = false, g_setup = false;   // g_setup: first-start setup running
static std::wstring g_status;
static int g_statusKind = ST_INFO;
static std::wstring g_activeName = L"Virtual Desktop";

// ---------------------------------------------------------------- helpers
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring Widen(const std::string& s, UINT cp = CP_UTF8)
{
    int n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
static DWORD FindProcess(const wchar_t* name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}
static std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}
static std::wstring IniPath() { return g_root + L"\\build\\ffvr_launcher.ini"; }

// ---------------------------------------------------------------- Steam / runtime checks
static std::wstring SteamPath()
{
    std::wstring p = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (auto& c : p) if (c == L'/') c = L'\\';
    return (!p.empty() && Exists(p + L"\\steam.exe")) ? p : L"";
}
static std::string VdfValue(const std::string& line, const char* key)   // "key"  "value" -> value (unescaped)
{
    std::string k = std::string("\"") + key + "\"";
    auto p = line.find(k); if (p == std::string::npos) return "";
    auto q1 = line.find('"', p + k.size()); if (q1 == std::string::npos) return "";
    auto q2 = line.find('"', q1 + 1); if (q2 == std::string::npos) return "";
    std::string v = line.substr(q1 + 1, q2 - q1 - 1), out;
    for (size_t i = 0; i < v.size(); ++i) { if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i; out += v[i]; }
    return out;
}
static std::vector<std::wstring> SteamLibraries(const std::wstring& steam)
{
    std::vector<std::wstring> libs{ steam };
    std::ifstream f(steam + L"\\steamapps\\libraryfolders.vdf");
    for (std::string l; std::getline(f, l);) {
        std::string v = VdfValue(l, "path");
        if (v.empty()) continue;
        std::wstring w = Widen(v);
        bool dup = false;
        for (auto& e : libs) if (!_wcsicmp(e.c_str(), w.c_str())) dup = true;
        if (!dup) libs.push_back(w);
    }
    return libs;
}
static std::wstring FindSteamApp(const std::wstring& steam, const wchar_t* appid)   // install folder, or empty
{
    for (auto& lib : SteamLibraries(steam)) {
        std::ifstream f(lib + L"\\steamapps\\appmanifest_" + appid + L".acf");
        for (std::string l; std::getline(f, l);) {
            std::string v = VdfValue(l, "installdir");
            if (v.empty()) continue;
            std::wstring dir = lib + L"\\steamapps\\common\\" + Widen(v);
            if (Exists(dir)) return dir;
        }
    }
    return L"";
}
static std::wstring ActiveRuntime()
{
    return RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
}
static std::wstring RuntimeName(const std::wstring& json)
{
    std::wstring l = Lower(json);
    if (l.find(L"virtualdesktop") != std::wstring::npos) return L"Virtual Desktop";
    if (l.find(L"steamxr") != std::wstring::npos || l.find(L"steamvr") != std::wstring::npos) return L"SteamVR (active)";
    if (l.find(L"oculus") != std::wstring::npos) return L"Meta Quest Link";
    if (l.find(L"mixedreality") != std::wstring::npos) return L"Windows Mixed Reality";
    if (l.find(L"pimax") != std::wstring::npos) return L"Pimax";
    return L"Active VR runtime";
}

static bool UevrPresent();
struct Check { bool ok = false; std::wstring msg, runtimeJson; };
static Check Preflight(bool steamvr)
{
    Check c;
    for (const wchar_t* f : { L"build\\ffvr_launch.exe", L"build\\ffvr_boot.dll", L"build\\ffvr_layer.dll",
                              L"build\\XR_APILAYER_FFVR_window.json" })
        if (!Exists(g_root + L"\\" + f)) { c.msg = std::wstring(L"The mod's files are incomplete: ") + f + L" is missing."; return c; }
    if (!UevrPresent()) { c.msg = L"UEVR (the Unreal Engine VR mod this mod builds on) isn't downloaded yet. Press Launch to download it."; return c; }
    std::wstring steam = SteamPath();
    if (steam.empty()) { c.msg = L"Steam isn't installed. The game runs through Steam: install Steam and FINAL FANTASY RESONANCE first."; return c; }
    if (FindSteamApp(steam, kGameAppId).empty()) { c.msg = L"FINAL FANTASY RESONANCE isn't installed. Install it from your Steam library, then try again."; return c; }
    if (steamvr) {
        std::wstring svr = FindSteamApp(steam, kSteamVrAppId);
        if (svr.empty() || !Exists(svr + L"\\steamxr_win64.json")) { c.msg = L"SteamVR isn't installed. Install it from Steam, or choose " + g_activeName + L"."; return c; }
        c.runtimeJson = svr + L"\\steamxr_win64.json";
    } else {
        std::wstring rt = ActiveRuntime();
        if (rt.empty() || !Exists(rt)) { c.msg = L"No VR runtime is active. Start Virtual Desktop (or another OpenXR runtime), or choose SteamVR."; return c; }
    }
    c.ok = true;
    return c;
}

// ---------------------------------------------------------------- the game's artwork (assets\), extracted on first run
// The fonts and textures the VR menu and this window use are read from the installed game by build\assetx\assetx.exe
// (self-contained; tools\assetx). The patterns match tools\extract_assets.ps1.
enum { WM_APP_ASSETS = WM_APP + 2 };
static const wchar_t* kAssetFiles[] = {
    L"SE-VOGEN.ttf", L"SE-VOGEB___.ttf", L"SE-GARRITHR.ttf",
    L"MenuIcon_FingerCursor_R.png", L"T_mainmenu_Listlabel.png", L"T_mainmenu_Listlabel_Select.png", L"T_CommonInsideFrame.png",
    L"T_Window_Frame_Box_4_2.png", L"T_menu_Title_Line00.png", L"T_TabArrow.png", L"T_Menu_line_center.png",
    L"T_menu_Line_Light.png", L"T_CommonMenudecoIcon_star.png",
    L"KEY_WINpad_BUTTON_A.png", L"KEY_WINpad_BUTTON_B.png", L"KEY_WINpad_CROSSKEY_UPDOWN.png", L"KEY_WINpad_CROSSKEY_RIGHTLEFT.png",
    L"KEY_WINpad_STICK_L_PUSH.png", L"KEY_WINpad_STICK_R_PUSH.png",
    L"Key_Win_Enter.png", L"Key_Win_Esc.png", L"Key_Win_Up.png", L"Key_Win_Down.png", L"Key_Win_Left.png", L"Key_Win_Right.png",
    L"Key_Win_BackSpace.png", L"T_TitleLogoImageRGB.png", L"T_TitleLogoText.png",
};
static const wchar_t* kAssetPatterns[] = {
    L"UI/Font/FontFace/SE/(SE-VOGEN|SE-VOGEB___|SE-GARRITHR)\\.ufont$",
    L"UI/Textures/(menu/(MenuIcon_FingerCursor_R|T_mainmenu_Listlabel|T_mainmenu_Listlabel_Select|T_CommonInsideFrame|"
    L"T_Window_Frame_Box_4_2|T_menu_Title_Line00|T_TabArrow|T_Menu_line_center|T_menu_Line_Light|T_CommonMenudecoIcon_star)"
    L"|KeyHelp/Key_WIN/KEY_WINpad_(BUTTON_A|BUTTON_B|CROSSKEY_UPDOWN|CROSSKEY_RIGHTLEFT|STICK_L_PUSH|STICK_R_PUSH)"
    L"|KeyHelp/Key_PC/Key_Win_(Enter|Esc|Up|Down|Left|Right|BackSpace)"
    L"|Title/T_TitleLogo(ImageRGB|Text))\\.uasset$",
};
static bool AssetsPresent()
{
    for (auto* f : kAssetFiles) if (!Exists(g_root + L"\\assets\\" + f)) return false;
    return true;
}
static bool ExtractAssets(const std::wstring& game)
{
    std::wstring tool = g_root + L"\\build\\assetx\\assetx.exe", out = g_root + L"\\assets";
    CreateDirectoryW(out.c_str(), nullptr);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileW((g_root + L"\\build\\ffvr_assets.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = true;
    for (auto* pat : kAssetPatterns) {
        std::wstring cmd = L"\"" + tool + L"\" export \"" + pat + L"\" \"" + out + L"\" --usmap \"" + g_root +
                           L"\\build\\assetx\\FFRS.usmap\" --game \"" + game + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = log; si.hStdError = log;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, out.c_str(), &si, &pi)) { ok = false; break; }
        WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (code != 0) ok = false;
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return ok && AssetsPresent();
}

static void Post(int kind, const std::wstring& s) { PostMessageW(g_hwnd, WM_APP_STATUS, kind, (LPARAM)new std::wstring(s)); }

// ---------------------------------------------------------------- UEVR, downloaded on first run
// UEVR (praydog's Universal Unreal Engine VR Mod) is not ours to redistribute ("All rights reserved"), so the package
// does not include it: the launcher downloads the exact nightly this mod is built and tested against from praydog's
// official GitHub releases, checks its SHA-256, and unpacks the files the mod uses into uevr\.
static const char* kUevrCommit = "4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d";
static const wchar_t* kUevrHost = L"github.com";
static const wchar_t* kUevrPath = L"/praydog/UEVR-nightly/releases/download/nightly-01143-4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d/uevr.zip";
static const char* kUevrSha256 = "4cd6932d1d01168178aef70499d40b101ea6f19868cf76099d8e0087ef8dc6d8";
static const wchar_t* kUevrFiles[] = { L"UEVRBackend.dll", L"LuaVR.dll", L"openxr_loader.dll", L"openvr_api.dll", L"revision.txt" };

static bool UevrPresent()
{
    for (auto* f : kUevrFiles) if (!Exists(g_root + L"\\uevr\\" + f)) return false;
    std::ifstream r(g_root + L"\\uevr\\revision.txt");
    std::string rev; std::getline(r, rev);
    return rev.rfind(kUevrCommit, 0) == 0;
}

static std::string Sha256Hex(const std::vector<uint8_t>& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t out[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
        BCryptFinishHash(h, out, sizeof(out), 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (uint8_t b : out) { s += hx[b >> 4]; s += hx[b & 15]; }
    return s;
}

// Downloads over HTTPS (WinHTTP follows GitHub's redirect to its download server), reporting progress in the status.
static bool DownloadUevr(std::wstring& err)
{
    const std::wstring what = L"Downloading UEVR, the Unreal Engine VR mod by praydog that this mod builds on, from its "
                              L"official GitHub page (first start only, about 39 MB)…";
    Post(ST_INFO, what);
    std::vector<uint8_t> data;
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"FFResonanceVR-launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, kUevrHost, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", kUevrPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0; len = sizeof(total);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            int lastPct = -1;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) break;
                if (avail == 0) { ok = true; break; }
                size_t at = data.size(); data.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, data.data() + at, avail, &got)) break;
                data.resize(at + got);
                int pct = total ? (int)(100.0 * data.size() / total) : -1;
                if (pct >= 0 && pct / 5 != lastPct / 5) { lastPct = pct; Post(ST_INFO, what + L" " + std::to_wstring(pct) + L"%"); }
            }
        } else err = L"GitHub answered " + std::to_wstring(code) + L".";
    }
    if (!ok && err.empty()) err = L"Couldn't connect to GitHub. Check your internet connection.";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok) return false;
    if (Sha256Hex(data) != kUevrSha256) { err = L"The downloaded UEVR didn't match the expected version (checksum)."; return false; }
    // unpack the files the mod uses with Windows' own tar (it reads zip)
    Post(ST_INFO, L"Unpacking UEVR…");
    std::wstring dir = g_root + L"\\uevr", zip = g_root + L"\\build\\uevr_download.zip";
    CreateDirectoryW(dir.c_str(), nullptr);
    { std::ofstream f(zip, std::ios::binary); f.write((const char*)data.data(), (std::streamsize)data.size()); if (!f) { err = L"Couldn't save the download to the mod's folder."; return false; } }
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    for (auto* f : kUevrFiles) cmd += std::wstring(L" ") + f;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 2 * 60 * 1000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    DeleteFileW(zip.c_str());
    if (code != 0 || !UevrPresent()) { err = L"Couldn't unpack UEVR."; return false; }
    return true;
}

// ---------------------------------------------------------------- first-start setup: UEVR, then the game's artwork
enum { SETUP_UEVR_FAILED = 1, SETUP_ASSETS_FAILED = 2 };
static void Setup(std::wstring game)
{
    int result = 0;
    std::wstring* err = new std::wstring;
    if (!UevrPresent() && !DownloadUevr(*err)) result |= SETUP_UEVR_FAILED;
    if (!AssetsPresent() && !game.empty() && Exists(g_root + L"\\build\\assetx\\assetx.exe")) {
        Post(ST_INFO, L"Getting the game's artwork ready for the VR menu (first start only, just a moment)…");
        if (!ExtractAssets(game)) result |= SETUP_ASSETS_FAILED;
    }
    PostMessageW(g_hwnd, WM_APP_ASSETS, result, (LPARAM)err);
}

// ---------------------------------------------------------------- the launch worker

static void Worker(bool attach, bool steamvr)
{
    bool errored = false, running = false;
    auto fail = [&](const std::wstring& m) { Post(ST_ERR, m); errored = true; };
    auto done = [&]() { Post(ST_DONE, L""); };
    Check c = Preflight(steamvr);
    if (!c.ok) { fail(c.msg); return done(); }
    DWORD game = FindProcess(kGameExe);
    if (!attach && game) { fail(L"The game is already running. Use \"Add VR to running game\", or close the game and press Launch."); return done(); }
    if (attach && !game) { fail(L"The game isn't running. Start it from Steam first, or just press Launch."); return done(); }
    if (steamvr && !FindProcess(L"vrserver.exe")) {
        Post(ST_INFO, L"Starting SteamVR…");
        ShellExecuteW(nullptr, L"open", (std::wstring(L"steam://rungameid/") + kSteamVrAppId).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        int t = 0;
        for (; t < 900 && !FindProcess(L"vrserver.exe"); ++t) Sleep(100);
        if (t >= 900) { fail(L"SteamVR didn't start within 90 seconds. Start it yourself, then try again."); return done(); }
        Post(ST_INFO, L"SteamVR is starting up…");
        Sleep(6000);
    }

    std::wstring cmd = L"\"" + g_root + L"\\build\\ffvr_launch.exe\" --wait";
    if (attach) cmd += L" --attach --delay 3";
    if (steamvr) cmd += L" --runtime \"" + c.runtimeJson + L"\"";
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  (g_root + L"\\build").c_str(), &si, &pi);
    CloseHandle(wr);
    if (!started) { CloseHandle(rd); fail(L"Couldn't start the mod's launcher (build\\ffvr_launch.exe)."); return done(); }
    Post(ST_INFO, attach ? L"Adding VR to the game…" : L"Starting the game through Steam…");

    std::ofstream log(g_root + L"\\build\\ffvr_launcher.log", std::ios::trunc);
    std::string pending;
    auto line = [&](const std::string& l) {
        log << l << "\n"; log.flush();
        if (l.find("asked Steam") == 0) Post(ST_INFO, L"Starting the game through Steam, waiting for its window…");
        else if (l.find("game pid") == 0) Post(ST_INFO, attach ? L"Adding VR to the game…" : L"The game window is up. Adding VR in about 10 seconds…");
        else if (l.find("injected") == 0 && l.find("FAILED") != std::string::npos)
            fail(L"Couldn't add VR to the game (a mod file failed to load). Details are in build\\ffvr_launcher.log.");
        else if (l.find("injected") == 0 && l.find("UEVRBackend") != std::string::npos && !errored) {
            running = true;
            Post(ST_OK, L"VR is on. Put your headset on and enjoy the game!");
        }
        else if (l.find("UEVR already loaded") == 0) { running = true; Post(ST_OK, L"VR is already active in the game."); }
        else if (l.find("no game window") == 0) fail(L"The game window didn't appear within 4 minutes. Check Steam for an update or a message, then try again.");
        else if (l.find("Steam launch failed") == 0) fail(L"Steam couldn't be started.");
        else if (l.find("missing:") == 0) fail(L"A mod file is missing: " + Widen(l.substr(9), CP_ACP));
        else if (l.find("OpenProcess failed") == 0) fail(L"Windows didn't allow adding VR to the game. If the game runs as administrator, run this launcher as administrator too.");
        else if (l.find("game already running") == 0) fail(L"The game is already running. Use \"Add VR to running game\".");
        else if (l.find("game not running") == 0) fail(L"The game isn't running. Start it from Steam first, or just press Launch.");
    };
    char buf[1024]; DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
        pending.append(buf, n);
        for (size_t p; (p = pending.find('\n')) != std::string::npos;) {
            std::string l = pending.substr(0, p);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            pending.erase(0, p + 1);
            line(l);
        }
    }
    if (!pending.empty()) line(pending);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (!errored) {
        if (code != 0) fail(L"Something went wrong (code " + std::to_wstring(code) + L"). Details are in build\\ffvr_launcher.log.");
        else if (running && !FindProcess(kGameExe)) Post(ST_INFO, L"The game has closed. Ready to launch again.");
    }
    done();
}

// ---------------------------------------------------------------- images (premultiplied BGRA, cropped to their alpha)
struct Img { int w = 0, h = 0; std::vector<uint32_t> px; };
static Img LoadPng(const std::wstring& path)
{
    Img im;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return im;
    int w, h, n;
    unsigned char* d = stbi_load_from_file(f, &w, &h, &n, 4);
    fclose(f);
    if (!d) return im;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        if (d[(y * w + x) * 4 + 3] > 8) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        im.w = x1 - x0 + 1; im.h = y1 - y0 + 1; im.px.resize((size_t)im.w * im.h);
        for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x) {
            const unsigned char* s = d + ((y + y0) * w + x + x0) * 4;
            uint32_t a = s[3];
            im.px[(size_t)y * im.w + x] = ((s[2] * a + 127) / 255) | (((s[1] * a + 127) / 255) << 8) | (((s[0] * a + 127) / 255) << 16) | (a << 24);
        }
    }
    stbi_image_free(d);
    return im;
}
static Img Scale(const Img& s, int dw, int dh)   // area average
{
    Img d; d.w = std::max(1, dw); d.h = std::max(1, dh); d.px.resize((size_t)d.w * d.h);
    double sx = (double)s.w / d.w, sy = (double)s.h / d.h;
    for (int y = 0; y < d.h; ++y) {
        int ya = std::min(s.h - 1, (int)(y * sy)), yb = std::min(s.h, std::max(ya + 1, (int)((y + 1) * sy)));
        for (int x = 0; x < d.w; ++x) {
            int xa = std::min(s.w - 1, (int)(x * sx)), xb = std::min(s.w, std::max(xa + 1, (int)((x + 1) * sx)));
            uint32_t acc[4] = {}, cnt = 0;
            for (int yy = ya; yy < yb; ++yy) for (int xx = xa; xx < xb; ++xx) {
                uint32_t p = s.px[(size_t)yy * s.w + xx];
                for (int k = 0; k < 4; ++k) acc[k] += (p >> (8 * k)) & 255;
                ++cnt;
            }
            uint32_t o = 0;
            for (int k = 0; k < 4; ++k) o |= ((acc[k] + cnt / 2) / cnt) << (8 * k);
            d.px[(size_t)y * d.w + x] = o;
        }
    }
    return d;
}
static HBITMAP ToBitmap(const Img& im)
{
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), im.w, -im.h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (b && bits) memcpy(bits, im.px.data(), im.px.size() * 4);
    return b;
}
struct Sprite { Img src; HBITMAP bmp = nullptr; int w = 0, h = 0; };
static Sprite g_emblem, g_logoText;
static void Prepare(Sprite& s, int w, int h)
{
    if (s.src.px.empty() || (s.bmp && s.w == w && s.h == h)) return;
    if (s.bmp) DeleteObject(s.bmp);
    s.bmp = ToBitmap(Scale(s.src, w, h)); s.w = w; s.h = h;
}
static void DrawSprite(HDC dc, const Sprite& s, int x, int y)
{
    if (!s.bmp) return;
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(m, s.bmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, s.w, s.h, m, 0, 0, s.w, s.h, bf);
    SelectObject(m, old); DeleteDC(m);
}
static void LoadAssets()
{
    for (const wchar_t* f : { L"SE-VOGEN.ttf", L"SE-VOGEB___.ttf", L"SE-GARRITHR.ttf" })
        AddFontResourceExW((g_root + L"\\assets\\" + f).c_str(), FR_PRIVATE, nullptr);
    for (Sprite* s : { &g_emblem, &g_logoText }) { if (s->bmp) DeleteObject(s->bmp); s->bmp = nullptr; s->w = s->h = 0; }
    g_emblem.src = LoadPng(g_root + L"\\assets\\T_TitleLogoImageRGB.png");
    g_logoText.src = LoadPng(g_root + L"\\assets\\T_TitleLogoText.png");
}
static HICON MakeIcon(const Img& src, int size)   // the emblem, fitted into a square (straight alpha for the icon)
{
    if (src.px.empty()) return nullptr;
    float k = std::min((float)size / src.w, (float)size / src.h);
    Img sm = Scale(src, std::max(1, (int)(src.w * k)), std::max(1, (int)(src.h * k)));
    Img sq; sq.w = sq.h = size; sq.px.assign((size_t)size * size, 0);
    int ox = (size - sm.w) / 2, oy = (size - sm.h) / 2;
    for (int y = 0; y < sm.h; ++y) for (int x = 0; x < sm.w; ++x) {
        uint32_t p = sm.px[(size_t)y * sm.w + x], a = p >> 24, o = a << 24;
        if (a) for (int c = 0; c < 3; ++c) o |= std::min(255u, (((p >> (8 * c)) & 255) * 255 + a / 2) / a) << (8 * c);
        sq.px[(size_t)(y + oy) * size + x + ox] = o;
    }
    HBITMAP color = ToBitmap(sq);
    std::vector<uint8_t> zero((size_t)size * size / 8, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zero.data());
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

// ---------------------------------------------------------------- layout (in 96-dpi units) and painting
static const int kW = 760, kH = 630;
static int S(float v) { return (int)lroundf(v * g_scale); }
struct Btn { float x, y, w, h; };
enum { B_ACTIVE, B_STEAMVR, B_LAUNCH, B_ATTACH, B_SETTINGS, B_README, B_COUNT };
static const Btn kBtn[B_COUNT] = {
    { 130, 392, 245, 38 }, { 385, 392, 245, 38 },
    { 230, 444, 300, 52 }, { 255, 506, 250, 32 },
    { 18, 600, 130, 22 }, { 612, 600, 130, 22 },
};
static int g_hover = -1, g_down = -1;
static RECT BtnRect(int i) { const Btn& b = kBtn[i]; return { S(b.x), S(b.y), S(b.x + b.w), S(b.y + b.h) }; }
static bool BtnEnabled(int i) { return i >= B_SETTINGS || !g_busy; }
static int HitTest(int x, int y)
{
    POINT p{ x, y };
    for (int i = 0; i < B_COUNT; ++i) { RECT r = BtnRect(i); if (PtInRect(&r, p)) return i; }
    return -1;
}

static HFONT g_fBody, g_fSmall, g_fSub, g_fBtn, g_fBtnBig, g_fTitle;
static void MakeFonts()
{
    for (HFONT f : { g_fBody, g_fSmall, g_fSub, g_fBtn, g_fBtnBig, g_fTitle }) if (f) DeleteObject(f);
    auto mk = [](float px, int weight, const wchar_t* face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_fBody = mk(16, FW_NORMAL, L"Vogel");
    g_fSmall = mk(14, FW_NORMAL, L"Vogel");
    g_fSub = mk(19, FW_BOLD, L"Vogel");
    g_fBtn = mk(16, FW_NORMAL, L"Vogel");
    g_fBtnBig = mk(23, FW_BOLD, L"Vogel");
    g_fTitle = mk(34, FW_NORMAL, L"Garrick Thin");
}

static const COLORREF kTop = RGB(14, 20, 38), kBottom = RGB(4, 6, 13);
static const COLORREF kGold = RGB(218, 194, 136), kText = RGB(226, 229, 236), kDim = RGB(146, 154, 172);
static COLORREF Mix(COLORREF a, COLORREF b, float t)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}
static void Fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void Frame(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FrameRect(dc, &r, b); DeleteObject(b); }
static void VGradient(HDC dc, RECT r, COLORREF a, COLORREF b, bool horizontal = false)
{
    TRIVERTEX v[2] = {
        { r.left, r.top, (COLOR16)(GetRValue(a) << 8), (COLOR16)(GetGValue(a) << 8), (COLOR16)(GetBValue(a) << 8), 0 },
        { r.right, r.bottom, (COLOR16)(GetRValue(b) << 8), (COLOR16)(GetGValue(b) << 8), (COLOR16)(GetBValue(b) << 8), 0 } };
    GRADIENT_RECT g{ 0, 1 };
    GradientFill(dc, v, 2, &g, 1, horizontal ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V);
}
static COLORREF BgAt(int y, int h) { return Mix(kTop, kBottom, std::max(0.0f, std::min(1.0f, (float)y / h))); }
static void GoldLine(HDC dc, int x0, int x1, int y, int H)   // fades out to both ends
{
    COLORREF bg = BgAt(y, H);
    int xm = (x0 + x1) / 2;
    VGradient(dc, { x0, y, xm, y + std::max(1, S(1)) }, bg, kGold, true);
    VGradient(dc, { xm, y, x1, y + std::max(1, S(1)) }, kGold, bg, true);
}
static int Text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt)   // returns the height used
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    if (fmt & DT_SINGLELINE) { DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX); return r.bottom - r.top; }
    RECT m = r;
    DrawTextW(dc, s.c_str(), -1, &m, fmt | DT_CALCRECT | DT_NOPREFIX);
    if (fmt & DT_CENTER) { int w = m.right - m.left; m.left = r.left + (r.right - r.left - w) / 2; m.right = m.left + w; }
    DrawTextW(dc, s.c_str(), -1, &m, fmt | DT_NOPREFIX);
    return m.bottom - m.top;
}

static void DrawButton(HDC dc, int i, const std::wstring& label, bool selected, bool primary)
{
    RECT r = BtnRect(i);
    bool en = BtnEnabled(i), hov = en && g_hover == i, down = hov && g_down == i;
    if (primary) {
        COLORREF a = en ? (hov ? RGB(196, 164, 92) : RGB(170, 140, 76)) : RGB(60, 58, 54);
        COLORREF b = en ? (hov ? RGB(120, 92, 44) : RGB(98, 74, 36)) : RGB(40, 39, 37);
        if (down) std::swap(a, b);
        VGradient(dc, r, a, b);
        Frame(dc, r, en ? RGB(246, 226, 170) : RGB(90, 88, 84));
        Text(dc, label, r, g_fBtnBig, en ? RGB(255, 250, 236) : RGB(130, 128, 122), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    COLORREF fill = selected ? RGB(62, 54, 32) : RGB(20, 28, 48);
    if (hov) fill = Mix(fill, RGB(70, 80, 110), 0.35f);
    if (!en) fill = Mix(fill, RGB(16, 18, 26), 0.6f);
    Fill(dc, r, fill);
    Frame(dc, r, selected ? kGold : (hov ? RGB(150, 160, 190) : RGB(70, 80, 104)));
    COLORREF tc = selected ? kGold : (hov ? kText : kDim);
    if (!en) tc = Mix(tc, RGB(30, 32, 40), 0.45f);
    Text(dc, label, r, g_fBtn, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void DrawLink(HDC dc, int i, const std::wstring& label, UINT align)
{
    RECT r = BtnRect(i);
    bool hov = g_hover == i;
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, hov ? kGold : kDim);
    RECT m = r;
    DrawTextW(dc, label.c_str(), -1, &m, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (hov) {
        SIZE sz; GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &sz);
        int x0 = (align & DT_RIGHT) ? r.right - sz.cx : r.left;
        Fill(dc, { x0, (r.top + r.bottom + sz.cy) / 2, x0 + sz.cx, (r.top + r.bottom + sz.cy) / 2 + std::max(1, S(1)) }, kGold);
    }
}

static void Paint(HDC dc, int W, int H)
{
    VGradient(dc, { 0, 0, W, H }, kTop, kBottom);
    SetBkMode(dc, TRANSPARENT);
    // title: the game's emblem with its lettering across it, as on the game's title screen
    if (!g_emblem.src.px.empty() && !g_logoText.src.px.empty()) {
        int eh = S(156), ew = (int)(eh * (float)g_emblem.src.w / g_emblem.src.h);
        int tw = S(540), th = (int)(tw * (float)g_logoText.src.h / g_logoText.src.w);
        Prepare(g_emblem, ew, eh); Prepare(g_logoText, tw, th);
        DrawSprite(dc, g_emblem, (W - ew) / 2, S(14));
        DrawSprite(dc, g_logoText, (W - tw) / 2, S(14) + (int)(eh * 0.68f) - th / 2);
    } else {
        Text(dc, L"FINAL FANTASY RESONANCE", { 0, S(70), W, S(130) }, g_fTitle, kText, DT_CENTER | DT_SINGLELINE);
    }
    SetTextCharacterExtra(dc, S(5));
    Text(dc, L"VR", { 0, S(176), W, S(202) }, g_fSub, kGold, DT_CENTER | DT_SINGLELINE);
    SetTextCharacterExtra(dc, 0);
    GoldLine(dc, S(110), W - S(110), S(210), H);

    // instructions
    int x = S(64), r = W - S(64), y = S(224);
    y += Text(dc, L"Welcome to the FF Resonance VR mod! Play the game on a large virtual 3D screen while surrounded by "
                  L"locations from the game.", { x, y, r, y + S(200) }, g_fBody, kText, DT_WORDBREAK | DT_CENTER) + S(16);
    const wchar_t* bullets[] = {
        L"Have your headset connected with Virtual Desktop or SteamVR.",
        L"The game starts through Steam as usual. VR appears about 10 seconds after the game window opens.",
        L"VR menu: press L3 + R3 (both sticks in) or F10. Re-centre the view with Ctrl+F12.",
    };
    for (auto* b : bullets) {
        Fill(dc, { x + S(6), y + S(8), x + S(11), y + S(13) }, kGold);
        y += Text(dc, b, { x + S(22), y, r, y + S(100) }, g_fSmall, Mix(kText, kDim, 0.35f), DT_WORDBREAK) + S(6);
    }

    // runtime choice, launch buttons
    Text(dc, L"VR RUNTIME", { 0, S(370), W, S(388) }, g_fSmall, kDim, DT_CENTER | DT_SINGLELINE);
    DrawButton(dc, B_ACTIVE, g_activeName, !g_steamvr, false);
    DrawButton(dc, B_STEAMVR, L"SteamVR", g_steamvr, false);
    DrawButton(dc, B_LAUNCH, g_setup ? L"Please wait…" : g_busy ? L"Running…" : L"Launch", false, true);
    DrawButton(dc, B_ATTACH, L"Add VR to running game", false, false);

    // status
    COLORREF sc = g_statusKind == ST_ERR ? RGB(242, 118, 104) : g_statusKind == ST_OK ? RGB(132, 214, 150) : kDim;
    Text(dc, g_status, { S(40), S(548), W - S(40), S(592) }, g_fSmall, sc, DT_WORDBREAK | DT_CENTER);

    DrawLink(dc, B_SETTINGS, L"Settings file", DT_LEFT);
    DrawLink(dc, B_README, L"Read me", DT_RIGHT);
}

// ---------------------------------------------------------------- window
static bool g_statusIsCheck = false;   // the status is the idle check (not a launch result): refreshed on activation
static void SetStatus(int kind, const std::wstring& s) { g_statusKind = kind; g_status = s; g_statusIsCheck = false; InvalidateRect(g_hwnd, nullptr, FALSE); }
static void ReadyStatus();
static void ReadyStatusImpl()
{
    Check c = Preflight(g_steamvr);
    if (!c.ok) SetStatus(ST_ERR, c.msg);
    else if (FindProcess(kGameExe)) SetStatus(ST_INFO, L"The game is running. Use \"Add VR to running game\" to switch it to VR.");
    else SetStatus(ST_INFO, g_steamvr ? L"Ready. SteamVR will be started if it isn't running." : L"Ready.");
}
static void ReadyStatus() { ReadyStatusImpl(); g_statusIsCheck = true; }
static void StartSetup()
{
    std::wstring steam = SteamPath(), game = steam.empty() ? L"" : FindSteamApp(steam, kGameAppId);
    g_busy = g_setup = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    std::thread(Setup, game).detach();
}
static void Start(bool attach)
{
    if (g_busy) return;
    if (!UevrPresent()) { StartSetup(); return; }   // (first start, or an earlier download failed: try again)
    g_busy = true;
    SetStatus(ST_INFO, L"Checking…");
    std::thread(Worker, attach, g_steamvr).detach();
}
static void OpenInNotepad(const std::wstring& file)
{
    if (!Exists(file)) { SetStatus(ST_ERR, L"Not found: " + file); return; }
    ShellExecuteW(g_hwnd, L"open", L"notepad.exe", (L"\"" + file + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
static void Click(int i)
{
    if (i < 0 || !BtnEnabled(i)) return;
    switch (i) {
    case B_ACTIVE: case B_STEAMVR:
        g_steamvr = i == B_STEAMVR;
        WritePrivateProfileStringW(L"Launcher", L"Runtime", g_steamvr ? L"steamvr" : L"active", IniPath().c_str());
        ReadyStatus();
        break;
    case B_LAUNCH: Start(false); break;
    case B_ATTACH: Start(true); break;
    case B_SETTINGS: OpenInNotepad(g_root + L"\\build\\ffvr_settings.ini"); break;
    case B_README: OpenInNotepad(g_root + (Exists(g_root + L"\\README.txt") ? L"\\README.txt" : L"\\README.md")); break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void SetIcons(HWND h)
{
    static HICON big, small_;
    if (big) DestroyIcon(big);
    if (small_) DestroyIcon(small_);
    big = MakeIcon(g_emblem.src, GetSystemMetricsForDpi(SM_CXICON, (UINT)(96 * g_scale)));
    small_ = MakeIcon(g_emblem.src, GetSystemMetricsForDpi(SM_CXSMICON, (UINT)(96 * g_scale)));
    if (big) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small_) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small_);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP b = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(m, b);
        Paint(m, cr.right, cr.bottom);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old); DeleteObject(b); DeleteDC(m);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hv = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = -1; g_down = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: g_down = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int up = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int d = g_down; g_down = -1;
        if (up == d) Click(up);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hover >= 0 && BtnEnabled(g_hover)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) Click(B_LAUNCH);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !g_busy && g_statusIsCheck) ReadyStatus();   // re-check (game started or installed meanwhile)
        break;
    case WM_APP_STATUS: {
        std::wstring* s = (std::wstring*)lp;
        if (wp == ST_DONE) { g_busy = false; InvalidateRect(h, nullptr, FALSE); }
        else SetStatus((int)wp, *s);
        delete s;
        return 0;
    }
    case WM_APP_ASSETS: {   // first-start setup finished
        std::wstring* err = (std::wstring*)lp;
        g_busy = g_setup = false;
        LoadAssets(); MakeFonts(); SetIcons(h);
        if (wp & SETUP_UEVR_FAILED)
            SetStatus(ST_ERR, L"Couldn't download UEVR: " + *err + L" Press Launch to try again.");
        else if (wp & SETUP_ASSETS_FAILED)
            SetStatus(ST_ERR, L"Couldn't read the game's artwork, so the VR menu will look plain. You can still play. "
                              L"Details are in build\\ffvr_assets.log.");
        else ReadyStatus();
        delete err;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.0f;
        MakeFonts(); SetIcons(h);
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L'\\'));
    g_root = (Lower(dir).size() > 6 && Lower(dir).substr(dir.size() - 6) == L"\\build") ? dir.substr(0, dir.size() - 6) : dir;

    LoadAssets();
    std::wstring rt = ActiveRuntime();
    if (!rt.empty()) g_activeName = RuntimeName(rt);
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"Launcher", L"Runtime", L"active", v, 32, IniPath().c_str());
    g_steamvr = !_wcsicmp(v, L"steamvr");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"FFResonanceVRLauncher";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    POINT pt{ 0, 0 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dpix = 96, dpiy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll"))
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(sh, "GetDpiForMonitor")) fn(mon, 0, &dpix, &dpiy);
    g_scale = dpix / 96.0f;
    RECT r{ 0, 0, S(kW), S(kH) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpix);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2, y = mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - wh) / 2);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"FFResonanceVR", style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    MakeFonts();
    SetIcons(g_hwnd);
    ReadyStatus();
    // first start: download UEVR, and read the game's artwork (fonts, menu pictures, title logo) from the installed game
    {
        std::wstring steam = SteamPath(), game = steam.empty() ? L"" : FindSteamApp(steam, kGameAppId);
        bool needAssets = !AssetsPresent() && !game.empty() && Exists(g_root + L"\\build\\assetx\\assetx.exe");
        if (!UevrPresent() || needAssets) StartSetup();
    }
    ShowWindow(g_hwnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
