// FFResonanceVR window layer (XR_APILAYER_FFVR_window)
//
// An OpenXR API layer between UEVR and the real runtime (SteamVR, VDXR or the virtual HMD). It turns UEVR's
// head-locked native-stereo output into a "window into the game world":
//
//  * A virtual screen (a rectangle, Distance metres in front of the recentred viewer) is fixed in the room.
//  * UEVR is told that the head never rotates (orientation = the screen's axes) and that each eye sits where the
//    real eye is, and each eye's field of view is the off-axis frustum from that eye through the screen's edges.
//    So the game renders, per eye, exactly what you would see of its world through that window.
//  * In xrEndFrame the projection layer is replaced by one world-locked quad per eye (eyeVisibility LEFT/RIGHT),
//    the same sub-rectangle of UEVR's swapchain, placed on the screen. The compositor reprojects quads, so head
//    rotation is always correct even when the game runs below the headset's refresh rate.
//
// Coordinates: "S" is the screen frame (origin = the viewer position at the last recentre, axes = yaw-only head
// orientation at that time), expressed in our own LOCAL space. UEVR sees S coordinates in place of its world space.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "ffvr_math.h"
#include "ffvr_menu.h"

// ---- loader <-> layer negotiation (openxr_loader_negotiation.h) -------------------------------------------
enum XrLoaderInterfaceStructs_ { LIS_UNINIT = 0, LIS_LOADER_INFO, LIS_API_LAYER_REQUEST, LIS_RUNTIME_REQUEST, LIS_API_LAYER_CREATE_INFO, LIS_API_LAYER_NEXT_INFO };
struct XrApiLayerCreateInfo;
typedef XrResult(XRAPI_PTR* PFN_xrCreateApiLayerInstance)(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*, XrInstance*);
struct XrNegotiateLoaderInfo_ { int structType; uint32_t structVersion; size_t structSize; uint32_t minInterfaceVersion, maxInterfaceVersion; XrVersion minApiVersion, maxApiVersion; };
struct XrNegotiateApiLayerRequest_ { int structType; uint32_t structVersion; size_t structSize; uint32_t layerInterfaceVersion; XrVersion layerApiVersion; PFN_xrGetInstanceProcAddr getInstanceProcAddr; PFN_xrCreateApiLayerInstance createApiLayerInstance; };
struct XrApiLayerNextInfo { int structType; uint32_t structVersion; size_t structSize; char layerName[XR_MAX_API_LAYER_NAME_SIZE]; PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr; PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance; XrApiLayerNextInfo* next; };
struct XrApiLayerCreateInfo { int structType; uint32_t structVersion; size_t structSize; void* loaderInstance; char settings_file_location[512]; XrApiLayerNextInfo* nextInfo; };

// ---- logging, settings -------------------------------------------------------------------------------------
static HMODULE g_module;
HMODULE FfModule() { return g_module; }
static std::wstring g_dir;
static FILE* g_log;
static std::mutex g_logMutex;
void FfLog(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

Settings g_set;                 // live settings (menu edits these)
static std::mutex g_setMutex;
static FILETIME g_iniTime{};
std::wstring SettingsPath()
{
    wchar_t b[MAX_PATH];
    if (GetEnvironmentVariableW(L"FFVR_SETTINGS", b, MAX_PATH)) return b;
    return g_dir + L"\\ffvr_settings.ini";
}
static float IniF(const wchar_t* s, const wchar_t* k, float d, const std::wstring& p)
{
    wchar_t b[64]; GetPrivateProfileStringW(s, k, L"", b, 64, p.c_str());
    return b[0] ? (float)_wtof(b) : d;
}
// Game-side settings are console commands, run by the mod's UEVR Lua script (uevr_profile\scripts\ffvr_console.lua),
// which reads <UEVR profile>\data\ffvr_console.txt. Only changes are sent.
static void ApplyGameSettings(const Settings& s)
{
    static int lastDof = -1;
    static std::string lastCam;
    int dof = s.dofStrength > 0 ? 1 : 0;
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    std::wstring dir = std::wstring(appdata) + L"\\UnrealVRMod\\FFRS-Win64-Shipping\\data";
    CreateDirectoryW(dir.c_str(), nullptr);
    {   // read by ffvr_camera.lua (lift), ffvr_worldui.lua (marker depth fraction, 0 = off; window tan(half FOV))
        // and ffvr_dof.lua (depth-of-field strength 0..1)
        static const float kMarkerDepth[3] = { 0.0f, 0.4f, 0.9f };
        char cam[128]; snprintf(cam, sizeof(cam), "%.2f\n%.2f\n%.4f\n%.2f\n%d\n", s.battleLift, kMarkerDepth[s.markers3D], s.width * 0.5f / s.distance, s.dofStrength / 100.f, s.battleZoom ? 1 : 0);
        if (cam != lastCam) if (FILE* lf = _wfopen((dir + L"\\ffvr_camera.txt").c_str(), L"w")) { fputs(cam, lf); fclose(lf); lastCam = cam; }
    }
    // (r.DepthOfFieldQuality itself is switched by ffvr_dof.lua, in step with the anti-aliasing method)
    if (dof != lastDof) { lastDof = dof; FfLog("depth of field %s (%d%%)", dof ? "on" : "off", s.dofStrength); }
}
static float g_activeResScale = -1;
float ActiveResolutionScale() { return g_activeResScale < 0 ? 1.0f : g_activeResScale; }
static void LoadSettings(bool force)
{
    std::wstring p = SettingsPath();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) { if (force) SaveSettings(); return; }
    if (!force && CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return;
    g_iniTime = fa.ftLastWriteTime;
    Settings s = g_set;
    s.distance = IniF(L"Screen", L"Distance", s.distance, p);
    s.width = IniF(L"Screen", L"Width", s.width, p);
    s.aspect = IniF(L"Screen", L"Aspect", s.aspect, p);
    s.height = IniF(L"Screen", L"Height", s.height, p);
    s.stereo = IniF(L"Stereo", L"Strength", s.stereo, p);
    s.headMotion = IniF(L"Stereo", L"HeadMotion", s.headMotion, p);
    s.eyeWidth = (int)IniF(L"Render", L"EyeWidth", (float)s.eyeWidth, p);
    s.margin = IniF(L"Render", L"Margin", s.margin, p);
    s.resolutionScale = IniF(L"Render", L"ResolutionScale", s.resolutionScale, p);
    s.environment = (int)IniF(L"Environment", L"Scene", (float)s.environment, p);
    s.envFloor = (int)IniF(L"Environment", L"Floor", (float)s.envFloor, p);
    s.passthrough = IniF(L"Debug", L"Passthrough", s.passthrough ? 1.f : 0.f, p) != 0;
    s.dofStrength = (int)std::lround(IniF(L"Game", L"DepthOfField", (float)s.dofStrength, p));
    if (s.dofStrength == 1) s.dofStrength = 100;   // older settings files: 1 = on
    s.battleLift = IniF(L"Game", L"BattleIntroLift", s.battleLift, p);
    s.markers3D = (int)IniF(L"Game", L"BattleMarkers3D", (float)s.markers3D, p);
    s.battleZoom = IniF(L"Game", L"BattleZoom", s.battleZoom ? 1.f : 0.f, p) != 0;
    s.Clamp();
    { std::lock_guard<std::mutex> lk(g_setMutex); g_set = s; }
    if (g_activeResScale < 0) g_activeResScale = s.resolutionScale;   // what the launcher gave UEVR for this run
    ApplyGameSettings(s);
    FfLog("settings: distance %.2f m, width %.2f m, aspect %.3f, height %.2f m, stereo %.2f, head motion %.2f, eye width %d, env %d%s",
          s.distance, s.width, s.aspect, s.height, s.stereo, s.headMotion, s.eyeWidth, s.environment, s.passthrough ? ", PASSTHROUGH" : "");
}
void SaveSettings()
{
    Settings s; { std::lock_guard<std::mutex> lk(g_setMutex); s = g_set; }
    std::wstring p = SettingsPath();
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    fprintf(f, "; FFResonanceVR settings (also changed from the in-game VR menu)\n");
    fprintf(f, "[Screen]\nDistance=%.3f\nWidth=%.3f\nAspect=%.4f\nHeight=%.3f\n", s.distance, s.width, s.aspect, s.height);
    fprintf(f, "[Stereo]\nStrength=%.3f\nHeadMotion=%.3f\n", s.stereo, s.headMotion);
    fprintf(f, "[Render]\nEyeWidth=%d\nMargin=%.3f\nResolutionScale=%.2f\n", s.eyeWidth, s.margin, s.resolutionScale);
    fprintf(f, "[Environment]\nScene=%d\nFloor=%d\n", s.environment, s.envFloor);
    fprintf(f, "[Game]\nDepthOfField=%d\nBattleIntroLift=%.1f\nBattleMarkers3D=%d\nBattleZoom=%d\n", s.dofStrength, s.battleLift, s.markers3D, s.battleZoom ? 1 : 0);
    fprintf(f, "[Debug]\nPassthrough=%d\n", s.passthrough ? 1 : 0);
    fclose(f);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
}
Settings CurrentSettings() { std::lock_guard<std::mutex> lk(g_setMutex); return g_set; }
// read by the mod's UEVR plugin, which applies it as UEVR's OpenXR_ResolutionScale
extern "C" __declspec(dllexport) float FfvrGetResolutionScale() { return CurrentSettings().resolutionScale; }
void SetSettings(const Settings& s) { { std::lock_guard<std::mutex> lk(g_setMutex); g_set = s; g_set.Clamp(); } SaveSettings(); ApplyGameSettings(CurrentSettings()); }

// ---- next-layer dispatch -----------------------------------------------------------------------------------
static PFN_xrGetInstanceProcAddr nxGIPA;
static XrInstance g_instance = XR_NULL_HANDLE;
#define NX(name) static PFN_##name nx_##name
NX(xrDestroyInstance); NX(xrCreateSession); NX(xrDestroySession); NX(xrCreateReferenceSpace); NX(xrCreateActionSpace);
NX(xrDestroySpace); NX(xrLocateSpace); NX(xrLocateViews); NX(xrWaitFrame); NX(xrEndFrame); NX(xrEnumerateViewConfigurationViews);
NX(xrPollEvent); NX(xrCreateSwapchain); NX(xrDestroySwapchain); NX(xrEnumerateSwapchainImages); NX(xrAcquireSwapchainImage);
NX(xrWaitSwapchainImage); NX(xrReleaseSwapchainImage); NX(xrBeginFrame); NX(xrEnumerateSwapchainFormats);
PFN_xrEnumerateSwapchainFormats Nx_EnumerateSwapchainFormats() { return nx_xrEnumerateSwapchainFormats; }
PFN_xrCreateSwapchain Nx_CreateSwapchain() { return nx_xrCreateSwapchain; }
PFN_xrDestroySwapchain Nx_DestroySwapchain() { return nx_xrDestroySwapchain; }
PFN_xrEnumerateSwapchainImages Nx_EnumerateSwapchainImages() { return nx_xrEnumerateSwapchainImages; }
PFN_xrAcquireSwapchainImage Nx_AcquireSwapchainImage() { return nx_xrAcquireSwapchainImage; }
PFN_xrWaitSwapchainImage Nx_WaitSwapchainImage() { return nx_xrWaitSwapchainImage; }
PFN_xrReleaseSwapchainImage Nx_ReleaseSwapchainImage() { return nx_xrReleaseSwapchainImage; }
PFN_xrLocateViews Nx_LocateViews() { return nx_xrLocateViews; }

// ---- state -------------------------------------------------------------------------------------------------
enum class Kind { Other, View, Local, Stage, Action };
static std::mutex g_spaceMutex;
static std::map<XrSpace, Kind> g_spaces;
static Kind KindOf(XrSpace s) { std::lock_guard<std::mutex> lk(g_spaceMutex); auto it = g_spaces.find(s); return it == g_spaces.end() ? Kind::Other : it->second; }
static bool IsWorld(Kind k) { return k == Kind::Local || k == Kind::Stage; }

static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_ourView = XR_NULL_HANDLE, g_ourLocal = XR_NULL_HANDLE, g_ourStage = XR_NULL_HANDLE;
static std::atomic<float> g_headHeight{ -1.0f };
float RecentreHeadHeight() { return g_headHeight.load(); }
XrSpace LayerLocalSpace() { return g_ourLocal; }
static ID3D12Device* g_dev = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;

static std::mutex g_poseMutex;
static P g_S;                       // screen frame in LOCAL
static bool g_haveS = false;
static std::atomic<bool> g_recenterRequest{ false };
static P g_lastHeadL;               // last valid real head pose in LOCAL
static std::atomic<long long> g_frame{ 0 };
static std::atomic<long long> g_lastDisplayTime{ 0 };
static std::atomic<long long> g_lastWaitQpcNs{ 0 };   // when that predicted display time was handed out (QPC, ns)
static long long QpcNs() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart); }
void RequestRecenter() { g_recenterRequest = true; }

static bool LocateReal(XrSpace space, XrSpace base, XrTime t, P& out)
{
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (nx_xrLocateSpace(space, base, t, &loc) != XR_SUCCESS) return false;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return false;
    out = FromXr(loc.pose);
    return true;
}
// pose of a base space in our LOCAL
static bool BaseInLocal(XrSpace base, XrTime t, P& out)
{
    Kind k = KindOf(base);
    if (k == Kind::Local) { out = P{}; return true; }   // UEVR's LOCAL equals ours (both identity-offset)
    return LocateReal(base, g_ourLocal, t, out);
}
static P YawOnly(const P& head)
{
    V3 f = Rot(head.q, { 0, 0, -1 });
    float yaw = std::atan2(-f.x, -f.z);
    return { AxisAngle({ 0, 1, 0 }, yaw), head.p };
}
// real head in LOCAL at time t (falls back to the last valid pose); performs a pending recentre
static P HeadLocal(XrTime t)
{
    P h;
    std::lock_guard<std::mutex> lk(g_poseMutex);
    if (LocateReal(g_ourView, g_ourLocal, t, h)) g_lastHeadL = h; else h = g_lastHeadL;
    if (!g_haveS || g_recenterRequest.exchange(false)) {
        g_S = YawOnly(h);
        g_haveS = true;
        // how high the head is above the real floor (for the surroundings' floor)
        P hs; float hh = -1;
        if (g_ourStage && LocateReal(g_ourView, g_ourStage, t, hs) && hs.p.y > 0.3f && hs.p.y < 2.5f) hh = hs.p.y;
        g_headHeight = hh;
        FfLog("recentred: viewer at [%.3f %.3f %.3f], head %.2f m above the floor", h.p.x, h.p.y, h.p.z, hh);
    }
    return h;
}
static P CurrentS() { std::lock_guard<std::mutex> lk(g_poseMutex); return g_S; }

static Screen ScreenNow(const Settings& s) { return { { 0, s.height, -s.distance }, s.width * 0.5f, s.width / s.aspect * 0.5f }; }

static XrFovf OffAxisFov(const V3& e, const Screen& sc, float margin)
{
    float D = std::max(0.05f, e.z - sc.c.z);
    // the frustum through the screen, widened by margin (a fraction of the screen size) on every side: renders a
    // border that is cropped away again in xrEndFrame (works around engine effects that misbehave at narrow FOVs)
    float mx = sc.hw * 2 * margin, my = sc.hh * 2 * margin;
    XrFovf f;
    f.angleLeft = std::atan((sc.c.x - sc.hw - mx - e.x) / D);
    f.angleRight = std::atan((sc.c.x + sc.hw + mx - e.x) / D);
    f.angleUp = std::atan((sc.c.y + sc.hh + my - e.y) / D);
    f.angleDown = std::atan((sc.c.y - sc.hh - my - e.y) / D);
    // keep the frustum sane when the head is far off to one side (the screen is then seen edge-on)
    const float lim = 1.45f;
    f.angleLeft = std::max(-lim, std::min(f.angleLeft, lim - 0.02f));
    f.angleRight = std::max(f.angleLeft + 0.02f, std::min(f.angleRight, lim));
    f.angleDown = std::max(-lim, std::min(f.angleDown, lim - 0.02f));
    f.angleUp = std::max(f.angleDown + 0.02f, std::min(f.angleUp, lim));
    return f;
}

// fake (S-frame) head position and eye positions for the real head / eyes in LOCAL
static void FakeEyes(const P& headL, const P eyesL[2], const Settings& st, V3& headF, V3 eyesF[2])
{
    P Si = Inv(CurrentS());
    V3 h = Compose(Si, headL).p;
    headF = h * st.headMotion;
    for (int i = 0; i < 2; ++i) {
        V3 e = Compose(Si, eyesL[i]).p;
        eyesF[i] = headF + (e - h) * st.stereo;
    }
}

// ---- intercepted functions ---------------------------------------------------------------------------------
static bool Active() { return !CurrentSettings().passthrough && g_session != XR_NULL_HANDLE; }

// UEVR creates its desktop-sized swapchains back to back: UI (game Slate UI), UI_RIGHT (2D-screen mode), FRAMEWORK_UI
// (its own ImGui menu). The game-UI ones are placed on the screen plane, like a TV's on-screen display.
struct SwapInfo { XrSwapchain h; uint32_t w, hgt; int64_t fmt; };
static std::mutex g_swapMutex;
static std::vector<SwapInfo> g_created;
static XrSwapchain g_uiSwap[2] = {}, g_fwSwap = XR_NULL_HANDLE;
static XrResult XRAPI_CALL ff_xrCreateSwapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    XrResult r = nx_xrCreateSwapchain(s, ci, out);
    if (r != XR_SUCCESS || s != g_session) return r;
    std::lock_guard<std::mutex> lk(g_swapMutex);
    g_created.push_back({ *out, ci->width, ci->height, ci->format });
    size_t n = g_created.size();
    if (n >= 3) {
        auto& a = g_created[n - 3]; auto& b = g_created[n - 2]; auto& c = g_created[n - 1];
        if (a.w == b.w && b.w == c.w && a.hgt == b.hgt && b.hgt == c.hgt && a.fmt == b.fmt && b.fmt == c.fmt) {
            g_uiSwap[0] = a.h; g_uiSwap[1] = b.h; g_fwSwap = c.h;
            FfLog("UEVR UI swapchains identified (%ux%u)", a.w, a.hgt);
        }
    }
    FfLog("swapchain %ux%u format %lld samples %u", ci->width, ci->height, (long long)ci->format, ci->sampleCount);
    return r;
}
// Which swapchains received a new image since the last xrEndFrame (UEVR's synced-sequential mode renders one eye
// per frame into its own swapchain; the other eye's image is the previous frame's).
static std::mutex g_relMutex;
static std::map<XrSwapchain, int> g_releasedSinceEnd;
static XrResult XRAPI_CALL ff_xrReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo* ri)
{
    XrResult r = nx_xrReleaseSwapchainImage(sc, ri);
    if (r == XR_SUCCESS) { std::lock_guard<std::mutex> lk(g_relMutex); g_releasedSinceEnd[sc]++; }
    return r;
}
static FILE* g_csv = nullptr;

// The projection UEVR actually renders with, reported by the mod's UEVR plugin once per frame (tangents l, r, u, d).
static std::mutex g_projMutex;
static float g_ueTan[2][4] = {};
static bool g_haveUeProj = false;
static long long g_ueProjQpcNs = 0;
extern "C" __declspec(dllexport) void FfvrReportProjection(int eye, const float* m)
{
    if (eye < 0 || eye > 1 || !m || m[0] == 0 || m[5] == 0) return;
    float rl = 2 / m[0], srl = -m[8] * rl, tb = 2 / m[5], stb = -m[9] * tb;
    std::lock_guard<std::mutex> lk(g_projMutex);
    g_ueTan[eye][0] = (srl - rl) / 2; g_ueTan[eye][1] = (srl + rl) / 2;
    g_ueTan[eye][2] = (stb + tb) / 2; g_ueTan[eye][3] = (stb - tb) / 2;
    g_haveUeProj = true; g_ueProjQpcNs = QpcNs();
}
// While some game screens are open (the quests menu: the game is paused), UEVR keeps rendering with fresh head data
// but submits frozen frame information: an old display time and the eye views of the last frame before the screen
// opened. The picture then no longer matches the frustum it is placed with (it wobbles when the head moves). In that
// case the eye view is rebuilt from the projection the engine really uses (reported by the mod's UEVR plugin): the
// frustum through the screen rectangle fixes the eye position. Returns false if there is no usable report.
static bool ViewFromProjection(uint32_t eye, const Screen& sc, float margin, XrCompositionLayerProjectionView& pv)
{
    float t[4];
    {
        std::lock_guard<std::mutex> lk(g_projMutex);
        if (!g_haveUeProj || QpcNs() - g_ueProjQpcNs > 200000000ll) return false;
        memcpy(t, g_ueTan[eye], sizeof(t));
    }
    const float tL = t[0], tR = t[1], tU = t[2], tD = t[3];
    if (!(tR - tL > 0.01f) || !(tU - tD > 0.01f)) return false;
    const float k = 1 + 2 * margin;
    float D = sc.hw * 2 * k / (tR - tL), Dv = sc.hh * 2 * k / (tU - tD);
    if (!(D > 0.05f && D < 100) || std::fabs(Dv - D) > 0.03f * D) return false;   // (a clamped or foreign frustum)
    V3 e{ sc.c.x - sc.hw * k - tL * D, sc.c.y - sc.hh * k - tD * D, sc.c.z + D };
    pv.pose = ToXr(P{ Q{}, e });
    pv.fov = { std::atan(tL), std::atan(tR), std::atan(tU), std::atan(tD) };
    return true;
}
// mismatch statistics between the submitted FOV and UEVR's projection (degrees)
static double g_projErrMax = 0, g_projErrSum = 0; static long long g_projErrN = 0;
static bool IsGameUi(XrSwapchain h) { std::lock_guard<std::mutex> lk(g_swapMutex); return h && (h == g_uiSwap[0] || h == g_uiSwap[1]); }

static XrResult XRAPI_CALL ff_xrEnumerateViewConfigurationViews(XrInstance inst, XrSystemId sys, XrViewConfigurationType t, uint32_t cap, uint32_t* count, XrViewConfigurationView* v)
{
    XrResult r = nx_xrEnumerateViewConfigurationViews(inst, sys, t, cap, count, v);
    if (r != XR_SUCCESS || !cap || !v || t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return r;
    Settings s = CurrentSettings();
    if (s.passthrough) return r;
    const float k = 1 + 2 * s.margin;   // the resolution scale is applied by UEVR (set by the mod's plugin)
    uint32_t w = (uint32_t)std::lround(s.eyeWidth * k), h = (uint32_t)std::lround(s.eyeWidth / s.aspect * k);
    { std::wstring p = SettingsPath(); int dw = GetPrivateProfileIntW(L"Debug", L"RenderWidth", 0, p.c_str()), dh = GetPrivateProfileIntW(L"Debug", L"RenderHeight", 0, p.c_str()); if (dw > 0 && dh > 0) { w = dw; h = dh; } }
    for (uint32_t i = 0; i < *count && i < cap; ++i) {
        FfLog("view %u: runtime recommends %ux%u, window renders %ux%u", i, v[i].recommendedImageRectWidth, v[i].recommendedImageRectHeight, w, h);
        v[i].recommendedImageRectWidth = std::min(w, v[i].maxImageRectWidth);
        v[i].recommendedImageRectHeight = std::min(h, v[i].maxImageRectHeight);
    }
    return r;
}

static XrResult XRAPI_CALL ff_xrCreateSession(XrInstance inst, const XrSessionCreateInfo* ci, XrSession* out)
{
    XrResult r = nx_xrCreateSession(inst, ci, out);
    if (r != XR_SUCCESS) return r;
    g_session = *out;
    for (auto* n = (const XrBaseInStructure*)ci->next; n; n = n->next)
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            auto* b = (const XrGraphicsBindingD3D12KHR*)n;
            g_dev = b->device; g_queue = b->queue;
        }
    XrReferenceSpaceCreateInfo sci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    sci.poseInReferenceSpace.orientation.w = 1;
    sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    nx_xrCreateReferenceSpace(*out, &sci, &g_ourView);
    sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    nx_xrCreateReferenceSpace(*out, &sci, &g_ourLocal);
    sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (nx_xrCreateReferenceSpace(*out, &sci, &g_ourStage) != XR_SUCCESS) { g_ourStage = XR_NULL_HANDLE; FfLog("no STAGE space: the surroundings keep their captured floor height"); }
    g_haveS = false;
    if (!g_csv && GetPrivateProfileIntW(L"Debug", L"FrameCsv", 0, SettingsPath().c_str())) {
        g_csv = _wfopen((g_dir + L"\\ffvr_frames.csv").c_str(), L"w");
        if (g_csv) fprintf(g_csv, "frame,displayTime,eye,swapchain,released,px,py,pz,fovL,fovR,fovU,fovD,rx,ry,rw,rh\n");
    }
    FfLog("session created (%s)", g_dev ? "D3D12" : "not D3D12: menu disabled");
    MenuInit(inst, *out, g_dev, g_queue);
    return r;
}

static XrResult XRAPI_CALL ff_xrDestroySession(XrSession s)
{
    if (s == g_session) {
        MenuShutdown();
        if (g_ourView) nx_xrDestroySpace(g_ourView);
        if (g_ourLocal) nx_xrDestroySpace(g_ourLocal);
        if (g_ourStage) nx_xrDestroySpace(g_ourStage);
        g_ourView = g_ourLocal = g_ourStage = XR_NULL_HANDLE;
        g_session = XR_NULL_HANDLE; g_dev = nullptr; g_queue = nullptr;
        FfLog("session destroyed after %lld frames", g_frame.load());
    }
    return nx_xrDestroySession(s);
}

static XrResult XRAPI_CALL ff_xrCreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* out)
{
    XrResult r = nx_xrCreateReferenceSpace(s, ci, out);
    if (r == XR_SUCCESS) {
        Kind k = ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? Kind::View :
                 ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? Kind::Local :
                 ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? Kind::Stage : Kind::Other;
        const auto& q = ci->poseInReferenceSpace;
        bool identity = std::fabs(q.orientation.w) > 0.9999f && std::fabs(q.position.x) + std::fabs(q.position.y) + std::fabs(q.position.z) < 1e-5f;
        if (!identity) { FfLog("reference space %d with an offset pose: passed through", (int)ci->referenceSpaceType); k = Kind::Other; }
        std::lock_guard<std::mutex> lk(g_spaceMutex);
        g_spaces[*out] = k;
    }
    return r;
}
static XrResult XRAPI_CALL ff_xrCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out)
{
    XrResult r = nx_xrCreateActionSpace(s, ci, out);
    if (r == XR_SUCCESS) { std::lock_guard<std::mutex> lk(g_spaceMutex); g_spaces[*out] = Kind::Action; }
    return r;
}
static XrResult XRAPI_CALL ff_xrDestroySpace(XrSpace s)
{
    { std::lock_guard<std::mutex> lk(g_spaceMutex); g_spaces.erase(s); }
    return nx_xrDestroySpace(s);
}

static XrResult XRAPI_CALL ff_xrLocateViews(XrSession s, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* views)
{
    XrResult r = nx_xrLocateViews(s, li, vs, cap, count, views);
    if (r != XR_SUCCESS || !cap || !views || *count < 2 || s != g_session || !Active()) return r;
    Kind k = KindOf(li->space);
    if (k != Kind::View && !IsWorld(k)) return r;
    P base;
    if (k == Kind::View) {
        P headL = HeadLocal(li->displayTime);
        base = headL;
    } else if (!BaseInLocal(li->space, li->displayTime, base)) return r;
    P headL = HeadLocal(li->displayTime);
    P eyesL[2] = { Compose(base, FromXr(views[0].pose)), Compose(base, FromXr(views[1].pose)) };
    Settings st = CurrentSettings();
    V3 headF, eyesF[2];
    FakeEyes(headL, eyesL, st, headF, eyesF);
    Screen sc = ScreenNow(st);
    for (int i = 0; i < 2; ++i) {
        V3 p = k == Kind::View ? eyesF[i] - headF : eyesF[i];
        views[i].pose = ToXr(P{ Q{}, p });
        views[i].fov = OffAxisFov(eyesF[i], sc, st.margin);
    }
    vs->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    return r;
}

static XrResult XRAPI_CALL ff_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime t, XrSpaceLocation* loc)
{
    XrResult r = nx_xrLocateSpace(space, baseSpace, t, loc);
    if (r != XR_SUCCESS || !Active()) return r;
    Kind ks = KindOf(space), kb = KindOf(baseSpace);
    if (!IsWorld(kb) || (ks != Kind::View && ks != Kind::Action)) return r;
    P base;
    if (!BaseInLocal(baseSpace, t, base)) return r;
    Settings st = CurrentSettings();
    P Si = Inv(CurrentS());
    XrSpaceVelocity* vel = nullptr;
    for (auto* n = (XrBaseOutStructure*)loc->next; n; n = n->next) if (n->type == XR_TYPE_SPACE_VELOCITY) vel = (XrSpaceVelocity*)n;
    if (ks == Kind::View) {
        P headL = HeadLocal(t);
        V3 h = Compose(Si, headL).p * st.headMotion;
        loc->pose = ToXr(P{ Q{}, h });
        loc->locationFlags |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if (vel) {
            Q rq = Mul(Si.q, base.q);
            V3 lv = Rot(rq, { vel->linearVelocity.x, vel->linearVelocity.y, vel->linearVelocity.z }) * st.headMotion;
            vel->linearVelocity = { lv.x, lv.y, lv.z };
            vel->angularVelocity = { 0, 0, 0 };
        }
    } else if (loc->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        P fake = Compose(Si, Compose(base, FromXr(loc->pose)));
        loc->pose = ToXr(fake);
        if (vel) {
            Q rq = Mul(Si.q, base.q);
            V3 lv = Rot(rq, { vel->linearVelocity.x, vel->linearVelocity.y, vel->linearVelocity.z });
            V3 av = Rot(rq, { vel->angularVelocity.x, vel->angularVelocity.y, vel->angularVelocity.z });
            vel->linearVelocity = { lv.x, lv.y, lv.z }; vel->angularVelocity = { av.x, av.y, av.z };
        }
    }
    return r;
}

static XrResult XRAPI_CALL ff_xrWaitFrame(XrSession s, const XrFrameWaitInfo* wi, XrFrameState* fs)
{
    XrResult r = nx_xrWaitFrame(s, wi, fs);
    if (r == XR_SUCCESS && s == g_session) {
        g_lastDisplayTime = fs->predictedDisplayTime; g_lastWaitQpcNs = QpcNs();
        static int n = 0;
        if (++n % 45 == 0) LoadSettings(false);
        MenuPoll();
    }
    return r;
}

// The image rect of one eye that shows the screen, and the part of the screen it covers (S coordinates).
static bool EyeQuad(const XrCompositionLayerProjectionView& pv, const Screen& sc, XrRect2Di& rect, V3& centre, XrExtent2Df& size)
{
    P e = FromXr(pv.pose);
    if (AngleDeg(e.q, Q{}) > 0.5f) {
        static bool once = false;
        if (!once) { once = true; FfLog("WARNING: submitted eye orientation is not the screen's (%.2f deg); using the full image", AngleDeg(e.q, Q{})); }
        rect = pv.subImage.imageRect; centre = sc.c; size = { sc.hw * 2, sc.hh * 2 };
        return true;
    }
    float D = e.p.z - sc.c.z;
    if (D < 0.05f) return false;
    float tL = std::tan(pv.fov.angleLeft), tR = std::tan(pv.fov.angleRight), tU = std::tan(pv.fov.angleUp), tD = std::tan(pv.fov.angleDown);
    float a0 = std::max(tL, (sc.c.x - sc.hw - e.p.x) / D), a1 = std::min(tR, (sc.c.x + sc.hw - e.p.x) / D);
    float b0 = std::max(tD, (sc.c.y - sc.hh - e.p.y) / D), b1 = std::min(tU, (sc.c.y + sc.hh - e.p.y) / D);
    if (a1 <= a0 || b1 <= b0) return false;
    const XrRect2Di& ir = pv.subImage.imageRect;
    int x0 = ir.offset.x + (int)std::floor((a0 - tL) / (tR - tL) * ir.extent.width + 0.001f);
    int x1 = ir.offset.x + (int)std::ceil((a1 - tL) / (tR - tL) * ir.extent.width - 0.001f);
    int y0 = ir.offset.y + (int)std::floor((tU - b1) / (tU - tD) * ir.extent.height + 0.001f);
    int y1 = ir.offset.y + (int)std::ceil((tU - b0) / (tU - tD) * ir.extent.height - 0.001f);
    x0 = std::max(x0, ir.offset.x); x1 = std::min(x1, ir.offset.x + ir.extent.width);
    y0 = std::max(y0, ir.offset.y); y1 = std::min(y1, ir.offset.y + ir.extent.height);
    if (x1 - x0 < 2 || y1 - y0 < 2) return false;
    rect = { { x0, y0 }, { x1 - x0, y1 - y0 } };
    // the screen region those whole pixels show
    float ta0 = tL + (float)(x0 - ir.offset.x) / ir.extent.width * (tR - tL);
    float ta1 = tL + (float)(x1 - ir.offset.x) / ir.extent.width * (tR - tL);
    float tb1 = tU - (float)(y0 - ir.offset.y) / ir.extent.height * (tU - tD);
    float tb0 = tU - (float)(y1 - ir.offset.y) / ir.extent.height * (tU - tD);
    float X0 = e.p.x + ta0 * D, X1 = e.p.x + ta1 * D, Y0 = e.p.y + tb0 * D, Y1 = e.p.y + tb1 * D;
    centre = { (X0 + X1) * 0.5f, (Y0 + Y1) * 0.5f, sc.c.z };
    size = { X1 - X0, Y1 - Y0 };
    return true;
}

static XrResult XRAPI_CALL ff_xrEndFrame(XrSession s, const XrFrameEndInfo* ei)
{
    if (s != g_session || !Active() || !ei) return nx_xrEndFrame(s, ei);
    long long frame = ++g_frame;
    Settings st = CurrentSettings();
    Screen sc = ScreenNow(st);
    P S = CurrentS();
    // our own display time: the latest predicted one, carried forward if xrWaitFrame has stopped being called
    XrTime tNow = ei->displayTime;
    bool staleTime = false;
    if (g_lastDisplayTime.load() > 0) {
        long long age = QpcNs() - g_lastWaitQpcNs.load();
        (void)age;
        // the runtime's latest predicted display time (runtimes such as SteamVR reject times they did not hand out)
        XrTime est = g_lastDisplayTime.load();
        if (est - ei->displayTime > 30000000ll) { tNow = est; staleTime = true; }
    }
    static bool wasStale = false;
    if (staleTime != wasStale) {
        wasStale = staleTime;
        FfLog(staleTime ? "UEVR submits a stale display time (%.0f ms old): using the layer's own time and the engine's projection" : "UEVR's display time is current again (%.0f ms)", (double)(tNow - ei->displayTime) / 1e6);
    }

    // the main projection layer = the one with the largest image area (UEVR also submits a tiny dummy one for VD)
    int mainIdx = -1; long long best = 0;
    for (uint32_t i = 0; i < ei->layerCount; ++i) {
        auto* L = ei->layers[i];
        if (L && L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto* pl = (const XrCompositionLayerProjection*)L;
            long long a = 0;
            for (uint32_t v = 0; v < pl->viewCount; ++v) a += (long long)pl->views[v].subImage.imageRect.extent.width * pl->views[v].subImage.imageRect.extent.height;
            if (a > best) { best = a; mainIdx = (int)i; }
        }
    }

    std::vector<const XrCompositionLayerBaseHeader*> out;
    std::vector<XrCompositionLayerQuad> quads; quads.reserve(ei->layerCount + 4);
    std::vector<XrCompositionLayerCylinderKHR> cyls; cyls.reserve(ei->layerCount);
    MenuAddBackground(tNow, S, sc, out);   // environment (projection) layer, if any
    for (uint32_t i = 0; i < ei->layerCount; ++i) {
        auto* L = ei->layers[i];
        if (!L) continue;
        if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            if ((int)i != mainIdx) continue;
            auto* pl = (const XrCompositionLayerProjection*)L;
            P base;
            if (!IsWorld(KindOf(pl->space)) || !BaseInLocal(pl->space, tNow, base)) { out.push_back(L); continue; }
            P toBase = Compose(Inv(base), S);   // S coordinates -> the layer's space
            if (frame == 1 || frame % 600 == 0)
                for (uint32_t v = 0; v < pl->viewCount && v < 2; ++v)
                    for (auto* n = (const XrBaseInStructure*)pl->views[v].next; n; n = n->next)
                        if (n->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                            auto* di = (const XrCompositionLayerDepthInfoKHR*)n;
                            FfLog("eye %u depth: swapchain %p rect %d,%d %dx%d slice %u, depth %.3f..%.3f, near %.4f far %.4g (colour swapchain %p rect %d,%d %dx%d slice %u)",
                                  v, (void*)di->subImage.swapchain, di->subImage.imageRect.offset.x, di->subImage.imageRect.offset.y, di->subImage.imageRect.extent.width,
                                  di->subImage.imageRect.extent.height, di->subImage.imageArrayIndex, di->minDepth, di->maxDepth, di->nearZ, di->farZ,
                                  (void*)pl->views[v].subImage.swapchain, pl->views[v].subImage.imageRect.offset.x, pl->views[v].subImage.imageRect.offset.y,
                                  pl->views[v].subImage.imageRect.extent.width, pl->views[v].subImage.imageRect.extent.height, pl->views[v].subImage.imageArrayIndex);
                        }
            for (uint32_t v = 0; v < pl->viewCount && v < 2; ++v) {
                XrRect2Di rect; V3 c; XrExtent2Df size;
                XrCompositionLayerProjectionView view = pl->views[v];
                {   // submitted view against the projection in use: a large mismatch means frozen frame information
                    float sub[4] = { view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown };
                    double mis = 0; bool have;
                    { std::lock_guard<std::mutex> lk(g_projMutex); have = g_haveUeProj; if (have) for (int k = 0; k < 4; ++k) mis = std::max(mis, std::fabs((double)std::atan(g_ueTan[v][k]) - sub[k]) / kDeg); }
                    if (have && (staleTime || mis > 0.5)) ViewFromProjection(v, sc, st.margin, view);
                }
                if (!EyeQuad(view, sc, rect, c, size)) continue;
                XrCompositionLayerQuad q{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                q.layerFlags = 0;
                q.space = pl->space;
                q.eyeVisibility = v == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                q.subImage = pl->views[v].subImage;
                q.subImage.imageRect = rect;
                q.pose = ToXr(Compose(toBase, P{ Q{}, c }));
                q.size = size;
                quads.push_back(q);
                out.push_back((const XrCompositionLayerBaseHeader*)&quads.back());
                if (g_csv) {
                    int rel; { std::lock_guard<std::mutex> lk(g_relMutex); rel = g_releasedSinceEnd[pl->views[v].subImage.swapchain]; }
                    const auto& pv = pl->views[v];
                    fprintf(g_csv, "%lld,%lld,%u,%p,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%d,%d,%d\n", frame, (long long)ei->displayTime, v, (void*)pv.subImage.swapchain, rel,
                            pv.pose.position.x, pv.pose.position.y, pv.pose.position.z, pv.fov.angleLeft / kDeg, pv.fov.angleRight / kDeg, pv.fov.angleUp / kDeg, pv.fov.angleDown / kDeg,
                            rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
                }
                {
                    std::lock_guard<std::mutex> lk(g_projMutex);
                    if (g_haveUeProj) {
                        const auto& f = pl->views[v].fov;
                        float sub[4] = { f.angleLeft, f.angleRight, f.angleUp, f.angleDown };
                        double e = 0;
                        for (int k = 0; k < 4; ++k) e = std::max(e, std::fabs((double)std::atan(g_ueTan[v][k]) - sub[k]) / kDeg);
                        g_projErrMax = std::max(g_projErrMax, e); g_projErrSum += e; ++g_projErrN;
                    }
                }
                if (frame % 600 == 0 && v == 1 && g_projErrN) {
                    FfLog("projection vs submitted FOV over the last %lld eye-frames: mean %.3f deg, max %.3f deg; UEVR now L%.2f R%.2f U%.2f D%.2f deg",
                          g_projErrN, g_projErrSum / g_projErrN, g_projErrMax, std::atan(g_ueTan[1][0]) / kDeg, std::atan(g_ueTan[1][1]) / kDeg,
                          std::atan(g_ueTan[1][2]) / kDeg, std::atan(g_ueTan[1][3]) / kDeg);
                    g_projErrMax = g_projErrSum = 0; g_projErrN = 0;
                }
                if (frame == 1 || frame % 600 == 0)
                    FfLog("frame %lld eye %u: fov L%.1f R%.1f U%.1f D%.1f, rect %d,%d %dx%d of %dx%d, quad %.3fx%.3f at S[%.3f %.3f %.3f], eye S[%.3f %.3f %.3f]",
                          frame, v, pl->views[v].fov.angleLeft / kDeg, pl->views[v].fov.angleRight / kDeg, pl->views[v].fov.angleUp / kDeg, pl->views[v].fov.angleDown / kDeg,
                          rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height, pl->views[v].subImage.imageRect.extent.width, pl->views[v].subImage.imageRect.extent.height,
                          size.width, size.height, c.x, c.y, c.z, pl->views[v].pose.position.x, pl->views[v].pose.position.y, pl->views[v].pose.position.z);
            }
        } else if (L->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            auto* ql = (const XrCompositionLayerQuad*)L;
            P base;
            if (!IsWorld(KindOf(ql->space)) || !BaseInLocal(ql->space, tNow, base)) { out.push_back(L); continue; }
            quads.push_back(*ql);
            if (IsGameUi(ql->subImage.swapchain)) {
                // the game's HUD/menus: on the screen, 2 mm in front of it, fitted inside it with the UI's aspect
                const auto& ir = ql->subImage.imageRect;
                float uiAspect = ir.extent.height > 0 ? (float)ir.extent.width / ir.extent.height : sc.hw / sc.hh;
                float w = sc.hw * 2, h = w / uiAspect;
                if (h > sc.hh * 2) { h = sc.hh * 2; w = h * uiAspect; }
                quads.back().pose = ToXr(Compose(Compose(Inv(base), S), P{ Q{}, { sc.c.x, sc.c.y, sc.c.z + 0.002f } }));
                quads.back().size = { w, h };
            } else {
                quads.back().pose = ToXr(Compose(Compose(Inv(base), S), FromXr(ql->pose)));
            }
            out.push_back((const XrCompositionLayerBaseHeader*)&quads.back());
        } else if (L->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            auto* cl = (const XrCompositionLayerCylinderKHR*)L;
            P base;
            if (!IsWorld(KindOf(cl->space)) || !BaseInLocal(cl->space, tNow, base)) { out.push_back(L); continue; }
            cyls.push_back(*cl);
            cyls.back().pose = ToXr(Compose(Compose(Inv(base), S), FromXr(cl->pose)));
            out.push_back((const XrCompositionLayerBaseHeader*)&cyls.back());
        } else {
            out.push_back(L);
        }
    }
    MenuAddOverlay(tNow, S, sc, out);       // the VR menu quad, if open
    XrFrameEndInfo e2 = *ei;
    e2.displayTime = tNow;
    e2.layerCount = (uint32_t)out.size();
    e2.layers = out.data();
    XrResult r = nx_xrEndFrame(s, &e2);
    if (r == XR_ERROR_TIME_INVALID && g_lastDisplayTime.load() > 0 && e2.displayTime != g_lastDisplayTime.load()) {
        // (SteamVR: UEVR's time was not one it predicted, e.g. while the game loads) - once more with the latest one
        static int n = 0; if (n++ < 20) FfLog("xrEndFrame: display time rejected, resubmitting with the latest predicted time");
        e2.displayTime = g_lastDisplayTime.load();
        r = nx_xrEndFrame(s, &e2);
    }
    { std::lock_guard<std::mutex> lk(g_relMutex); g_releasedSinceEnd.clear(); }
    if (r != XR_SUCCESS) { static int n = 0; if (n++ < 20) FfLog("xrEndFrame failed %d (%u layers)", (int)r, e2.layerCount); }
    return r;
}

static XrResult XRAPI_CALL ff_xrPollEvent(XrInstance inst, XrEventDataBuffer* ev)
{
    XrResult r = nx_xrPollEvent(inst, ev);
    if (r == XR_SUCCESS && ev->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        auto* e = (XrEventDataReferenceSpaceChangePending*)ev;
        if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) { FfLog("runtime recentred LOCAL space: recentring the screen"); RequestRecenter(); }
    }
    if (r == XR_SUCCESS && ev->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto* e = (XrEventDataSessionStateChanged*)ev;
        FfLog("session state %d", (int)e->state);
        MenuSessionFocus(e->state == XR_SESSION_STATE_FOCUSED);   // the runtime's own menu took over: close ours
    }
    return r;
}

static XrResult XRAPI_CALL ff_xrDestroyInstance(XrInstance inst)
{
    FfLog("instance destroyed");
    if (inst == g_instance) g_instance = XR_NULL_HANDLE;
    return nx_xrDestroyInstance(inst);
}

// ---- dispatch ----------------------------------------------------------------------------------------------
static XrResult XRAPI_CALL ff_xrGetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn)
{
#define HOOK(n) if (!strcmp(name, #n)) { *fn = (PFN_xrVoidFunction)ff_##n; return nx_##n ? XR_SUCCESS : nxGIPA(inst, name, fn); }
    if (name && fn) {
        if (!strcmp(name, "xrGetInstanceProcAddr")) { *fn = (PFN_xrVoidFunction)ff_xrGetInstanceProcAddr; return XR_SUCCESS; }
        HOOK(xrDestroyInstance) HOOK(xrCreateSession) HOOK(xrDestroySession) HOOK(xrCreateReferenceSpace) HOOK(xrCreateActionSpace)
        HOOK(xrDestroySpace) HOOK(xrLocateSpace) HOOK(xrLocateViews) HOOK(xrWaitFrame) HOOK(xrEndFrame)
        HOOK(xrEnumerateViewConfigurationViews) HOOK(xrPollEvent) HOOK(xrCreateSwapchain) HOOK(xrReleaseSwapchainImage)
    }
#undef HOOK
    return nxGIPA(inst, name, fn);
}

static XrResult XRAPI_CALL ff_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* li, XrInstance* inst)
{
    if (!li || !li->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    nxGIPA = li->nextInfo->nextGetInstanceProcAddr;
    XrApiLayerCreateInfo next = *li;
    next.nextInfo = li->nextInfo->next;
    XrResult r = li->nextInfo->nextCreateApiLayerInstance(info, &next, inst);
    if (r != XR_SUCCESS) { FfLog("next xrCreateApiLayerInstance failed %d", (int)r); return r; }
    g_instance = *inst;
#define LOADFN(n) nxGIPA(*inst, #n, (PFN_xrVoidFunction*)&nx_##n)
    LOADFN(xrDestroyInstance); LOADFN(xrCreateSession); LOADFN(xrDestroySession); LOADFN(xrCreateReferenceSpace); LOADFN(xrCreateActionSpace);
    LOADFN(xrDestroySpace); LOADFN(xrLocateSpace); LOADFN(xrLocateViews); LOADFN(xrWaitFrame); LOADFN(xrEndFrame);
    LOADFN(xrEnumerateViewConfigurationViews); LOADFN(xrPollEvent); LOADFN(xrCreateSwapchain); LOADFN(xrDestroySwapchain);
    LOADFN(xrEnumerateSwapchainImages); LOADFN(xrAcquireSwapchainImage); LOADFN(xrWaitSwapchainImage); LOADFN(xrReleaseSwapchainImage);
    LOADFN(xrBeginFrame); LOADFN(xrEnumerateSwapchainFormats);
#undef LOADFN
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    auto gip = (PFN_xrGetInstanceProperties)nullptr;
    nxGIPA(*inst, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&gip);
    if (gip && gip(*inst, &ip) == XR_SUCCESS) FfLog("instance created for '%s' on runtime '%s' %u.%u.%u", info->applicationInfo.applicationName, ip.runtimeName,
                                                    XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return r;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo_* li, const char* layerName, XrNegotiateApiLayerRequest_* rq)
{
    if (!li || !rq || li->structType != LIS_LOADER_INFO || rq->structType != LIS_API_LAYER_REQUEST) return XR_ERROR_INITIALIZATION_FAILED;
    if (li->minInterfaceVersion > 1 || li->maxInterfaceVersion < 1) return XR_ERROR_INITIALIZATION_FAILED;
    if (!g_log) {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(g_module, p, MAX_PATH);
        g_dir = p; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/"));
        std::wstring logp = g_dir + L"\\ffvr_layer.log";
        wchar_t env[MAX_PATH];
        if (GetEnvironmentVariableW(L"FFVR_LOG", env, MAX_PATH)) logp = env;
        g_log = _wfopen(logp.c_str(), L"w");
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        FfLog("FFResonanceVR window layer loaded (%s) into %ls", layerName ? layerName : "?", exe);
        LoadSettings(true);
    }
    rq->layerInterfaceVersion = 1;
    rq->layerApiVersion = XR_CURRENT_API_VERSION;
    rq->getInstanceProcAddr = ff_xrGetInstanceProcAddr;
    rq->createApiLayerInstance = ff_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_module = m; DisableThreadLibraryCalls(m); }
    return TRUE;
}
