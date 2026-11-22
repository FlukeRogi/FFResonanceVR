// FFResonanceVR launcher: starts FINAL FANTASY RESONANCE through Steam (or attaches to it) and injects
// ffvr_boot.dll (environment), openxr_loader.dll and UEVRBackend.dll.
//
//   ffvr_launch.exe [--attach] [--vhmd] [--no-layer] [--delay <s>] [--timeout <s>] [--wait]
//                   [--env NAME=VALUE]... [-- <extra game args>]
//
//   --vhmd       render to the virtual HMD (build\vhmd\ffvr_vhmd64.json) instead of the system OpenXR runtime
//   --steamvr    use SteamVR's OpenXR runtime (default: the system's active runtime, e.g. Virtual Desktop VDXR)
//   --runtime    use the OpenXR runtime manifest given
//   --no-layer   plain UEVR (no window layer), for comparisons
//   --env        extra environment for the game process (e.g. ODE_VHMD_OUT=..., FFVR_SETTINGS=...)
//   --wait       wait for the game to exit
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static const wchar_t* kExeName = L"FFRS-Win64-Shipping.exe";
static const wchar_t* kAppId = L"3259780";

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
struct WinFind { DWORD pid; HWND hwnd; };
static BOOL CALLBACK EnumWin(HWND h, LPARAM lp)
{
    auto* f = (WinFind*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
        RECT r; GetWindowRect(h, &r);
        if (r.right - r.left > 320) { f->hwnd = h; return FALSE; }
    }
    return TRUE;
}
static HWND MainWindow(DWORD pid) { WinFind f{ pid, nullptr }; EnumWindows(EnumWin, (LPARAM)&f); return f.hwnd; }
static bool ModuleLoaded(DWORD pid, const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (!_wcsicmp(me.szModule, name)) { found = true; break; }
    CloseHandle(snap);
    return found;
}
static bool Inject(HANDLE proc, const std::wstring& dll)
{
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { wprintf(L"missing: %s\n", dll.c_str()); return false; }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return false;
    WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr);
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, ll, mem, 0, nullptr);
    if (!th) { VirtualFreeEx(proc, mem, 0, MEM_RELEASE); return false; }
    WaitForSingleObject(th, 30000);
    DWORD code = 0; GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    wprintf(L"injected %s -> %s\n", dll.c_str(), code ? L"ok" : L"FAILED");
    return code != 0;
}
static std::string Utf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// Apply key=value overrides (uevr_profile\config_overrides.txt) to UEVR's per-game config.txt
static void ApplyUevrProfile(const std::wstring& root)
{
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return;
    std::wstring dir = std::wstring(appdata) + L"\\UnrealVRMod\\FFRS-Win64-Shipping";
    CreateDirectoryW((std::wstring(appdata) + L"\\UnrealVRMod").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring cfg = dir + L"\\config.txt";
    std::vector<std::pair<std::string, std::string>> lines;
    std::map<std::string, size_t> idx;
    { std::ifstream in(cfg); std::string l;
      while (std::getline(in, l)) { auto eq = l.find('='); if (eq == std::string::npos) continue; idx[l.substr(0, eq)] = lines.size(); lines.push_back({ l.substr(0, eq), l.substr(eq + 1) }); } }
    std::ifstream ov(root + L"\\uevr_profile\\config_overrides.txt");
    std::string l; int n = 0;
    while (std::getline(ov, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        auto eq = l.find('='); if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (idx.count(k)) lines[idx[k]].second = v; else { idx[k] = lines.size(); lines.push_back({ k, v }); }
        ++n;
    }
    // the VR menu's Resolution Scale (build\ffvr_settings.ini) becomes UEVR's render scale for this run
    {
        wchar_t v[32] = L"";
        GetPrivateProfileStringW(L"Render", L"ResolutionScale", L"1.0", v, 32, (root + L"\\build\\ffvr_settings.ini").c_str());
        float s = (float)_wtof(v);
        if (s < 0.5f || s > 2.0f) s = 1.0f;
        char b[32]; snprintf(b, sizeof(b), "%.6f", s);
        const std::string k = "OpenXR_ResolutionScale";
        if (idx.count(k)) lines[idx[k]].second = b; else { idx[k] = lines.size(); lines.push_back({ k, b }); }
        wprintf(L"resolution scale %.2f\n", s);
    }
    std::ofstream out(cfg, std::ios::trunc);
    for (auto& kv : lines) out << kv.first << "=" << kv.second << "\n";
    // console commands UEVR runs at startup (cvars that fix stereo rendering problems)
    std::wstring us = root + L"\\uevr_profile\\user_script.txt";
    if (GetFileAttributesW(us.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW(us.c_str(), (dir + L"\\user_script.txt").c_str(), FALSE);
    // Lua scripts and plugins of the mod
    for (const wchar_t* sub : { L"scripts", L"plugins" }) {
        CreateDirectoryW((dir + L"\\" + sub).c_str(), nullptr);
        WIN32_FIND_DATAW fd;
        std::wstring src = root + L"\\uevr_profile\\" + sub;
        {   // remove the mod's files that this version no longer has (ffvr_*), so an older copy cannot stay active
            HANDLE o = FindFirstFileW((dir + L"\\" + sub + L"\\ffvr_*.*").c_str(), &fd);
            if (o != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW((src + L"\\" + fd.cFileName).c_str()) == INVALID_FILE_ATTRIBUTES)
                        DeleteFileW((dir + L"\\" + sub + L"\\" + fd.cFileName).c_str());
                } while (FindNextFileW(o, &fd));
                FindClose(o);
            }
        }
        HANDLE h = FindFirstFileW((src + L"\\*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring from = src + L"\\" + fd.cFileName, to = dir + L"\\" + sub + L"\\" + fd.cFileName;
            std::wstring name = fd.cFileName;
            if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".lua")) {
                // fill in the profile folder (UEVR's Lua has no os.getenv)
                std::ifstream in(from, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
                std::string text = ss.str(), prof = Utf8(dir);
                for (auto& c : prof) if (c == '\\') c = '/';
                for (size_t pos; (pos = text.find("@@PROFILE@@")) != std::string::npos;) text.replace(pos, 11, prof);
                std::ofstream out2(to, std::ios::binary | std::ios::trunc); out2 << text;
            } else {
                CopyFileW(from.c_str(), to.c_str(), FALSE);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wprintf(L"UEVR profile: %d override(s) applied to %s\n", n, cfg.c_str());
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: FFResonanceVR.exe shows each line as progress
    bool attach = false, vhmd = false, layer = true, wait = false;
    int delay = 10, timeout = 240;
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring build = self; build = build.substr(0, build.find_last_of(L'\\'));
    std::wstring root = build.substr(0, build.find_last_of(L'\\'));
    std::wstring uevr = root + L"\\uevr";
    std::vector<std::wstring> env, dlls;   // dlls: --dll, injected instead of the mod (tools such as UE4SS)
    std::wstring extra, runtime;   // runtime: an OpenXR runtime manifest; empty = the system's active runtime
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--attach") attach = true;
        else if (a == L"--vhmd") vhmd = true;
        else if (a == L"--steamvr") runtime = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win64.json";
        else if (a == L"--runtime" && i + 1 < argc) runtime = argv[++i];
        else if (a == L"--no-layer") layer = false;
        else if (a == L"--dll" && i + 1 < argc) dlls.push_back(argv[++i]);
        else if (a == L"--wait") wait = true;
        else if (a == L"--delay" && i + 1 < argc) delay = _wtoi(argv[++i]);
        else if (a == L"--timeout" && i + 1 < argc) timeout = _wtoi(argv[++i]);
        else if (a == L"--env" && i + 1 < argc) env.push_back(argv[++i]);
        else if (a == L"--") { for (++i; i < argc; ++i) { extra += L" "; extra += argv[i]; } }
        else { wprintf(L"unknown argument %s\n", a.c_str()); return 1; }
    }

    // environment for the game process, applied by ffvr_boot.dll before the OpenXR loader loads
    {
        std::ofstream f(build + L"\\ffvr_boot.env", std::ios::trunc);
        f << "FFVR_ACTIVE=1\n";
        if (layer) {
            f << "XR_API_LAYER_PATH=" << Utf8(build) << "\n";
            f << "XR_ENABLE_API_LAYERS=XR_APILAYER_FFVR_window\n";
        } else {
            f << "XR_ENABLE_API_LAYERS=\n";
        }
        f << "XR_RUNTIME_JSON=" << (vhmd ? Utf8(build + L"\\vhmd\\ffvr_vhmd64.json") : Utf8(runtime)) << "\n";
        for (auto& e : env) f << Utf8(e) << "\n";
    }
    ApplyUevrProfile(root);

    DWORD pid = FindProcess(kExeName);
    if (pid && !attach) { wprintf(L"game already running (pid %lu); use --attach\n", pid); return 2; }
    if (!pid) {
        if (attach) { wprintf(L"game not running\n"); return 2; }
        wchar_t steam[MAX_PATH] = L"C:\\Program Files (x86)\\Steam\\steam.exe";
        DWORD sz = sizeof(steam);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", RRF_RT_REG_SZ, nullptr, steam, &sz);
        std::wstring cmd = L"\"" + std::wstring(steam) + L"\" -applaunch " + kAppId + extra;
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { wprintf(L"Steam launch failed %lu\n", GetLastError()); return 1; }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        wprintf(L"asked Steam to start app %s\n", kAppId);
    }
    for (int t = 0; t < timeout * 10; ++t) {
        pid = FindProcess(kExeName);
        if (pid && MainWindow(pid)) break;
        Sleep(100);
    }
    if (!pid || !MainWindow(pid)) { wprintf(L"no game window after %d s\n", timeout); return 3; }
    wprintf(L"game pid %lu, window up; injecting in %d s\n", pid, delay);
    Sleep(delay * 1000);
    if (ModuleLoaded(pid, L"UEVRBackend.dll")) { wprintf(L"UEVR already loaded\n"); return 0; }
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed %lu\n", GetLastError()); return 4; }
    bool ok = true;
    if (!dlls.empty()) { for (auto& d : dlls) ok = ok && Inject(proc, d); }
    else ok = Inject(proc, build + L"\\ffvr_boot.dll") && Inject(proc, uevr + L"\\openxr_loader.dll") && Inject(proc, uevr + L"\\UEVRBackend.dll");
    if (ok && wait) { wprintf(L"waiting for the game to exit\n"); WaitForSingleObject(proc, INFINITE); }
    CloseHandle(proc);
    return ok ? 0 : 5;
}
