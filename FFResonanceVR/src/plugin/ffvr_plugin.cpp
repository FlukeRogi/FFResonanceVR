// ffvr_plugin.dll - UEVR plugin (loaded from <UEVR profile>\plugins).
//
//  1. Menu input: forwards every XInputGetState result to the window layer's VR menu (ffvr_layer.dll!FfvrMenuXInput),
//     which reads it and neutralises it for the game while the menu is open. UEVR calls plugins after its own
//     controller emulation, so VR controllers mapped to a gamepad count.
//  2. Per-frame projections: UEVR builds each eye's projection matrix once, from the first field of view the runtime
//     reports, and keeps it. The window layer reports a new off-axis frustum every frame (it follows your eyes), so
//     this plugin hooks runtimes::OpenXR::update_matrices (found through UEVRBackend.pdb) and sets VRRuntime's
//     should_recalculate_eye_projections flag before each call: the projection follows the layer every frame.
//  3. Diagnostics: reports the projection UEVR actually used to the layer (FfvrReportProjection) once per frame.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "uevr_API.h"

static const UEVR_PluginInitializeParam* g_p = nullptr;

// ---- 1. menu input ------------------------------------------------------------------------------------------
typedef bool (*MenuXInputFn)(unsigned int, unsigned int, void*);
typedef void (*ReportProjFn)(int eye, const float* m16);
static MenuXInputFn g_menuFn = nullptr;
static ReportProjFn g_reportFn = nullptr;
static bool ResolveLayer()
{
    if (g_menuFn && g_reportFn) return true;
    HMODULE m = GetModuleHandleW(L"ffvr_layer.dll");   // loads when UEVR creates its OpenXR instance
    if (!m) return false;
    g_menuFn = (MenuXInputFn)GetProcAddress(m, "FfvrMenuXInput");
    g_reportFn = (ReportProjFn)GetProcAddress(m, "FfvrReportProjection");
    return g_menuFn != nullptr;
}
static void OnXInputGetState(unsigned int* retval, unsigned int userIndex, void* state)
{
    if (ResolveLayer() && g_menuFn) g_menuFn(retval ? *retval : 1, userIndex, state);
}

// ---- 2. projection hook --------------------------------------------------------------------------------------
typedef int (*UpdateMatricesFn)(void* self, float nearz, float farz);
static UpdateMatricesFn g_origUpdate = nullptr;
static long g_flagOffset = -1;
static int HookedUpdateMatrices(void* self, float nearz, float farz)
{
    if (self && g_flagOffset >= 0) *((bool*)self + g_flagOffset) = true;
    return g_origUpdate(self, nearz, farz);
}

static long MemberOffset(HANDLE proc, DWORD64 base, const wchar_t* typeName, const wchar_t* member)
{
    SYMBOL_INFOW si{}; si.SizeOfStruct = sizeof(si);
    if (!SymGetTypeFromNameW(proc, base, typeName, &si)) return -1;
    DWORD count = 0;
    if (!SymGetTypeInfo(proc, base, si.TypeIndex, TI_GET_CHILDRENCOUNT, &count) || !count) return -1;
    std::string buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG), '\0');
    auto* kids = (TI_FINDCHILDREN_PARAMS*)buf.data(); kids->Count = count; kids->Start = 0;
    if (!SymGetTypeInfo(proc, base, si.TypeIndex, TI_FINDCHILDREN, kids)) return -1;
    for (DWORD i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        if (!SymGetTypeInfo(proc, base, kids->ChildId[i], TI_GET_SYMNAME, &name) || !name) continue;
        bool match = wcscmp(name, member) == 0;
        LocalFree(name);
        DWORD off = 0;
        if (match && SymGetTypeInfo(proc, base, kids->ChildId[i], TI_GET_OFFSET, &off)) return (long)off;
    }
    return -1;
}

static DWORD WINAPI InstallProjectionHook(void*)
{
    if (GetEnvironmentVariableW(L"FFVR_NO_PROJ_HOOK", nullptr, 0)) { g_p->functions->log_info("[FFResonanceVR] projection hook disabled (FFVR_NO_PROJ_HOOK)"); return 0; }
    HMODULE uevr = GetModuleHandleW(L"UEVRBackend.dll");
    if (!uevr) { g_p->functions->log_error("[FFResonanceVR] UEVRBackend.dll not found"); return 0; }
    wchar_t path[MAX_PATH]; GetModuleFileNameW(uevr, path, MAX_PATH);
    std::wstring dir = path; dir = dir.substr(0, dir.find_last_of(L"\\/"));
    HANDLE proc = GetCurrentProcess();
    // our own symbol session would clash with UEVR's dbghelp use only if both used the same handle; use a fake one
    HANDLE symH = (HANDLE)(uintptr_t)0xFF5E;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitializeW(symH, dir.c_str(), FALSE)) { g_p->functions->log_error("[FFResonanceVR] SymInitialize failed"); return 0; }
    DWORD64 base = SymLoadModuleExW(symH, nullptr, path, nullptr, (DWORD64)uevr, 0, nullptr, 0);
    if (!base) { g_p->functions->log_error("[FFResonanceVR] could not load UEVRBackend symbols (UEVRBackend.pdb next to the DLL?)"); SymCleanup(symH); return 0; }
    struct { SYMBOL_INFOW si; wchar_t name[256]; } s{};
    s.si.SizeOfStruct = sizeof(SYMBOL_INFOW); s.si.MaxNameLen = 255;
    bool found = SymFromNameW(symH, L"UEVRBackend!runtimes::OpenXR::update_matrices", &s.si) != FALSE;
    long off = MemberOffset(symH, base, L"VRRuntime", L"should_recalculate_eye_projections");

    SymCleanup(symH);
    (void)proc;
    if (!found || off < 0) { g_p->functions->log_error("[FFResonanceVR] update_matrices %s, flag offset %ld: projection hook NOT installed", found ? "found" : "not found", off); return 0; }
    g_flagOffset = off;
    int id = g_p->functions->register_inline_hook((void*)s.si.Address, (void*)&HookedUpdateMatrices, (void**)&g_origUpdate);
    g_p->functions->log_info("[FFResonanceVR] projection hook %s: update_matrices at %p, flag at +0x%lx", id >= 0 ? "installed" : "FAILED", (void*)s.si.Address, off);
    return 0;
}

// ---- 3. report the projection used --------------------------------------------------------------------------
static void OnPresent()
{
    if (!g_p || !ResolveLayer() || !g_reportFn) return;
    for (int eye = 0; eye < 2; ++eye) {
        UEVR_Matrix4x4f m{};
        g_p->vr->get_ue_projection_matrix((UEVR_Eye)eye, &m);
        g_reportFn(eye, (const float*)&m);
    }
}

extern "C" __declspec(dllexport) void uevr_plugin_required_version(UEVR_PluginVersion* v)
{
    v->major = UEVR_PLUGIN_VERSION_MAJOR; v->minor = UEVR_PLUGIN_VERSION_MINOR; v->patch = UEVR_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool uevr_plugin_initialize(const UEVR_PluginInitializeParam* p)
{
    g_p = p;
    p->functions->log_info("[FFResonanceVR] plugin loaded");
    p->callbacks->on_xinput_get_state(OnXInputGetState);
    p->callbacks->on_present(OnPresent);
    CloseHandle(CreateThread(nullptr, 0, InstallProjectionHook, nullptr, 0, nullptr));   // symbol loading takes a few seconds
    return true;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(m); return TRUE; }
