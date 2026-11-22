// ODE Virtual HMD - a hardware-free OpenXR runtime for automated tests.
//
// Selected per process with XR_RUNTIME_JSON=<...>\ode_vhmd32.json (or 64). The system's real OpenXR
// runtime registration is never touched.
//
//  * Implements the OpenXR 1.0 core subset a D3D11 VR app needs + XR_KHR_D3D11_enable and
//    XR_KHR_win32_convert_performance_counter_time.
//  * Head motion is scripted and a pure function of XrTime, so every frame's "true" pose is known.
//  * Every xrEndFrame is recorded to frames.jsonl: submitted poses/FOVs, the error against the scripted
//    head pose at that display time, how many frames stale the pose was, eye-image statistics.
//  * Selected frames are dumped as BMPs; call-order / validity problems are logged as VIOLATIONs.
//  * summary.json is written when the instance is destroyed (or the process exits).
//
// Configuration: ode_vhmd.ini next to the DLL, or the file named by ODE_VHMD_CONFIG.
// Environment overrides: ODE_VHMD_OUT, ODE_VHMD_MAXFRAMES, ODE_VHMD_MOTION, ODE_VHMD_PACED, ODE_VHMD_HZ,
// ODE_VHMD_STATSEVERY, ODE_VHMD_DUMPFRAMES, ODE_VHMD_FOCUSLOSS, ODE_VHMD_NOISE (see docs/VHMD.md).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ================================================================================================
// math (OpenXR convention: right-handed, +Y up, -Z forward)
// ================================================================================================
struct V3 { float x = 0, y = 0, z = 0; };
struct Q { float x = 0, y = 0, z = 0, w = 1; };
struct P { Q q; V3 p; };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
static float Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
static Q Mul(Q a, Q b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Q Conj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
static V3 Rot(Q q, V3 v)
{
    Q r = Mul(Mul(q, { v.x, v.y, v.z, 0 }), Conj(q));
    return { r.x, r.y, r.z };
}
static Q Norm(Q q)
{
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-8f ? Q{ q.x / l, q.y / l, q.z / l, q.w / l } : Q{};
}
static Q AxisAngle(V3 axis, float rad) { float s = std::sin(rad * 0.5f); return { axis.x * s, axis.y * s, axis.z * s, std::cos(rad * 0.5f) }; }
static const float kDeg = 3.14159265358979f / 180.0f;
static Q FromYPR(float yawDeg, float pitchDeg, float rollDeg)
{
    return Mul(Mul(AxisAngle({ 0, 1, 0 }, yawDeg * kDeg), AxisAngle({ 1, 0, 0 }, pitchDeg * kDeg)), AxisAngle({ 0, 0, 1 }, rollDeg * kDeg));
}
static P Compose(const P& a, const P& b) { return { Mul(a.q, b.q), a.p + Rot(a.q, b.p) }; }
static P Inv(const P& a) { Q c = Conj(a.q); return { c, Rot(c, -a.p) }; }
static float AngleDeg(Q a, Q b)
{
    // 2*atan2(|v|, |w|) stays accurate for tiny angles, where acos(w) of a float near 1 does not.
    Q d = Mul(Conj(Norm(a)), Norm(b));
    double v = std::sqrt((double)d.x * d.x + (double)d.y * d.y + (double)d.z * d.z);
    return (float)(2.0 * std::atan2(v, std::fabs((double)d.w)) / kDeg);
}
static P FromXr(const XrPosef& p) { return { Norm({ p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }), { p.position.x, p.position.y, p.position.z } }; }
static XrPosef ToXr(const P& p) { XrPosef r; r.orientation = { p.q.x, p.q.y, p.q.z, p.q.w }; r.position = { p.p.x, p.p.y, p.p.z }; return r; }
static void YPR(Q q, float& yaw, float& pitch, float& roll)
{
    V3 f = Rot(q, { 0, 0, -1 }), u = Rot(q, { 0, 1, 0 });
    yaw = std::atan2(-f.x, -f.z) / kDeg;
    pitch = std::asin(std::max(-1.0f, std::min(1.0f, f.y))) / kDeg;
    V3 r = Rot(Conj(FromYPR(yaw, pitch, 0)), u);
    roll = std::atan2(-r.x, r.y) / kDeg;
}

// ================================================================================================
// config, logging, time
// ================================================================================================
struct Config {
    uint32_t eyeW = 1832, eyeH = 1920;
    float hz = 90.0f;
    float fovL = -50, fovR = 43, fovU = 47, fovD = -53;   // left eye, degrees; right eye is mirrored
    float ipd = 0.063f, cantDeg = 0.0f, floorHeight = 1.6f;
    std::string motion = "sweep";                         // static | sweep | yaw | file
    std::wstring motionFile;
    float motionScale = 1.0f;
    bool paced = true;                                    // false: virtual clock, one period per xrWaitFrame
    int maxFrames = 0;                                    // >0: request session exit after this many frames
    int statsEvery = 30;                                  // eye-image statistics every N frames (0 = off)
    std::set<int> dumpFrames{ 1, 60, 300 };
    int dumpEvery = 0;
    std::wstring outDir;
};
static Config g_cfg;
static std::wstring g_dllDir;
static std::mutex g_logMutex;
static FILE* g_log = nullptr;
static FILE* g_frames = nullptr;

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::atomic<int> g_violations{ 0 };
static std::map<std::string, int> g_violationKinds;
static std::set<std::string> g_unsupported;
static void Violation(const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    ++g_violations;
    { std::lock_guard<std::mutex> lk(g_logMutex); g_violationKinds[buf]++; }
    Log("VIOLATION: %s", buf);
}

static int64_t g_qpcFreq = 0;
static int64_t NowNs()
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)g_qpcFreq);
}

static std::wstring IniPath()
{
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"ODE_VHMD_CONFIG", env, MAX_PATH)) return env;
    return g_dllDir + L"\\ode_vhmd.ini";
}
static std::wstring IniS(const wchar_t* sec, const wchar_t* key, const wchar_t* def)
{
    wchar_t b[1024]; GetPrivateProfileStringW(sec, key, def, b, 1024, IniPath().c_str());
    std::wstring r = b;
    size_t semi = r.find(L';'); if (semi != std::wstring::npos) r = r.substr(0, semi);
    while (!r.empty() && iswspace(r.back())) r.pop_back();
    return r;
}
static float IniF(const wchar_t* s, const wchar_t* k, float d) { auto v = IniS(s, k, L""); return v.empty() ? d : (float)_wtof(v.c_str()); }
static int IniI(const wchar_t* s, const wchar_t* k, int d) { auto v = IniS(s, k, L""); return v.empty() ? d : _wtoi(v.c_str()); }
static std::wstring Env(const wchar_t* n) { wchar_t b[1024]; return GetEnvironmentVariableW(n, b, 1024) ? std::wstring(b) : std::wstring(); }

static void LoadConfig()
{
    Config& c = g_cfg;
    c.eyeW = (uint32_t)IniI(L"Display", L"EyeWidth", (int)c.eyeW);
    c.eyeH = (uint32_t)IniI(L"Display", L"EyeHeight", (int)c.eyeH);
    c.hz = IniF(L"Display", L"RefreshHz", c.hz);
    c.fovL = IniF(L"Display", L"FovLeftDeg", c.fovL);
    c.fovR = IniF(L"Display", L"FovRightDeg", c.fovR);
    c.fovU = IniF(L"Display", L"FovUpDeg", c.fovU);
    c.fovD = IniF(L"Display", L"FovDownDeg", c.fovD);
    c.ipd = IniF(L"Display", L"IPD", c.ipd);
    c.cantDeg = IniF(L"Display", L"CantDeg", c.cantDeg);
    c.floorHeight = IniF(L"Tracking", L"FloorHeight", c.floorHeight);
    c.motion = Narrow(IniS(L"Tracking", L"Motion", L"sweep"));
    c.motionFile = IniS(L"Tracking", L"MotionFile", L"");
    c.motionScale = IniF(L"Tracking", L"MotionScale", c.motionScale);
    c.paced = IniI(L"Timing", L"Paced", 1) != 0;
    c.maxFrames = IniI(L"Run", L"MaxFrames", 0);
    c.statsEvery = IniI(L"Capture", L"StatsEvery", c.statsEvery);
    c.dumpEvery = IniI(L"Capture", L"DumpEvery", 0);
    std::wstring df = IniS(L"Capture", L"DumpFrames", L"1,60,300");
    c.dumpFrames.clear();
    for (wchar_t* tok = wcstok(&df[0], L", ", nullptr); tok; tok = wcstok(nullptr, L", ", nullptr)) if (_wtoi(tok) > 0) c.dumpFrames.insert(_wtoi(tok));
    c.outDir = IniS(L"Capture", L"OutDir", L"");
    // environment overrides (used by test scripts)
    if (auto v = Env(L"ODE_VHMD_OUT"); !v.empty()) c.outDir = v;
    if (auto v = Env(L"ODE_VHMD_MAXFRAMES"); !v.empty()) c.maxFrames = _wtoi(v.c_str());
    if (auto v = Env(L"ODE_VHMD_MOTION"); !v.empty()) c.motion = Narrow(v);
    if (auto v = Env(L"ODE_VHMD_HZ"); !v.empty()) c.hz = (float)_wtof(v.c_str());
    if (auto v = Env(L"ODE_VHMD_PACED"); !v.empty()) c.paced = _wtoi(v.c_str()) != 0;
    // image statistics read eye images back synchronously (~15-20 ms each): 0 for timing measurements
    if (auto v = Env(L"ODE_VHMD_STATSEVERY"); !v.empty()) c.statsEvery = _wtoi(v.c_str());
    if (auto v = Env(L"ODE_VHMD_DUMPFRAMES"); !v.empty()) {
        c.dumpFrames.clear();
        for (wchar_t* tok = wcstok(&v[0], L", ", nullptr); tok; tok = wcstok(nullptr, L", ", nullptr)) if (_wtoi(tok) > 0) c.dumpFrames.insert(_wtoi(tok));
    }
    if (c.outDir.empty()) {
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        c.outDir = std::wstring(tmp) + L"ode_vhmd\\" + fs::path(exe).stem().wstring() + L"-" + std::to_wstring(GetCurrentProcessId());
    }
    if (c.hz < 1) c.hz = 90;
}

// ================================================================================================
// scripted head motion: pure function of time (seconds since the session began)
// ================================================================================================
struct Key { float t, yaw, pitch, roll, x, y, z; };
static std::vector<Key> g_keys;

static void LoadMotionFile()
{
    if (g_cfg.motionFile.empty()) return;
    FILE* f = _wfopen(g_cfg.motionFile.c_str(), L"r");
    if (!f) { Log("motion file not found: %s", Narrow(g_cfg.motionFile).c_str()); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        Key k;
        if (sscanf(line, " %f , %f , %f , %f , %f , %f , %f", &k.t, &k.yaw, &k.pitch, &k.roll, &k.x, &k.y, &k.z) == 7) g_keys.push_back(k);
    }
    fclose(f);
    Log("motion file: %zu keys", g_keys.size());
}

// Head pose in LOCAL space (origin = head start position, so the pose at t=0 is identity).
static P HeadAt(double t)
{
    const float s = g_cfg.motionScale;
    const double tau = 6.283185307179586;
    float yaw = 0, pitch = 0, roll = 0, x = 0, y = 0, z = 0;
    if (g_cfg.motion == "sweep") {
        yaw = (float)(30 * std::sin(tau * t / 8.0)); pitch = (float)(10 * std::sin(tau * t / 5.0)); roll = (float)(5 * std::sin(tau * t / 11.0));
        x = (float)(0.10 * std::sin(tau * t / 6.0)); y = (float)(0.05 * std::sin(tau * t / 4.0)); z = (float)(0.10 * std::sin(tau * t / 7.0));
    } else if (g_cfg.motion == "yaw") {
        yaw = (float)std::fmod(45.0 * t, 360.0);
    } else if (g_cfg.motion == "file" && !g_keys.empty()) {
        double len = g_keys.back().t > 0 ? g_keys.back().t : 1;
        double tt = std::fmod(t, len);
        size_t i = 0;
        while (i + 1 < g_keys.size() && g_keys[i + 1].t < tt) ++i;
        const Key& a = g_keys[i];
        const Key& b = g_keys[std::min(i + 1, g_keys.size() - 1)];
        float u = b.t > a.t ? (float)((tt - a.t) / (b.t - a.t)) : 0.0f;
        auto L = [&](float p, float q) { return p + (q - p) * u; };
        yaw = L(a.yaw, b.yaw); pitch = L(a.pitch, b.pitch); roll = L(a.roll, b.roll); x = L(a.x, b.x); y = L(a.y, b.y); z = L(a.z, b.z);
    }
    return { FromYPR(yaw * s, pitch * s, roll * s), { x * s, y * s, z * s } };
}

static P EyeOffset(int eye)
{
    float sign = eye == 0 ? -1.0f : 1.0f;
    return { AxisAngle({ 0, 1, 0 }, -sign * g_cfg.cantDeg * kDeg), { sign * g_cfg.ipd * 0.5f, 0, 0 } };
}
static XrFovf EyeFov(int eye)
{
    XrFovf f;
    if (eye == 0) { f.angleLeft = g_cfg.fovL * kDeg; f.angleRight = g_cfg.fovR * kDeg; }
    else { f.angleLeft = -g_cfg.fovR * kDeg; f.angleRight = -g_cfg.fovL * kDeg; }
    f.angleUp = g_cfg.fovU * kDeg; f.angleDown = g_cfg.fovD * kDeg;
    return f;
}

// ================================================================================================
// objects
// ================================================================================================
struct Instance;
struct Session;
struct Swapchain {
    Session* s = nullptr;
    XrSwapchainCreateInfo ci{};
    std::vector<ID3D11Texture2D*> tex;
    std::vector<ID3D12Resource*> res;   // D3D12 sessions (tex stays empty)
    size_t Count() const { return tex.empty() ? res.size() : tex.size(); }
    uint32_t next = 0;
    std::deque<uint32_t> acquired;
    bool waited = false;
    int lastReleased = -1;
};
struct Space {
    Session* s = nullptr;
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    P pose;
    int hand = -1;      // action space: 0 left, 1 right controller (then 'type' is unused)
    bool grip = false;  // action space: the grip pose (else the aim pose)
};
struct Session {
    Instance* inst = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D12Device* dev12 = nullptr;          // D3D12 binding (dev/ctx null)
    ID3D12CommandQueue* queue12 = nullptr;
    ID3D12CommandAllocator* alloc12 = nullptr;
    ID3D12GraphicsCommandList* list12 = nullptr;
    ID3D12Fence* fence12 = nullptr;
    uint64_t fenceVal = 0;
    HANDLE fenceEvent = nullptr;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exitRequested = false;
    std::mutex fm;
    int pendingWaits = 0;
    bool frameBegun = false;
    int64_t t0 = 0;                 // XrTime when the session began (motion time origin)
    int64_t nextVsync = 0, period = 0, lastPredicted = 0;
    std::set<int64_t> predicted;    // recently predicted display times
    int frame = 0, emptyFrames = 0;
    int64_t firstEnd = 0, lastEnd = 0;
    std::set<Space*> spaces;
    std::set<Swapchain*> swapchains;
};
struct Instance {
    std::mutex em;
    std::deque<XrEventDataBuffer> events;
    std::set<std::string> exts;
    std::string appName, engineName;
    bool d3dReqQueried = false, d3d12ReqQueried = false;
    std::set<Session*> sessions;
    std::vector<std::string> paths;
    LUID luid{};
};

// handle registry
static std::mutex g_hm;
static std::set<void*> g_live;
template <class H> static H MakeH(void* p) { std::lock_guard<std::mutex> lk(g_hm); g_live.insert(p); return (H)(uintptr_t)p; }
template <class T, class H> static T* Get(H h)
{
    void* p = (void*)(uintptr_t)h;
    std::lock_guard<std::mutex> lk(g_hm);
    return g_live.count(p) ? (T*)p : nullptr;
}
static void Forget(void* p) { std::lock_guard<std::mutex> lk(g_hm); g_live.erase(p); }

// statistics for the summary
struct Stats {
    int frames = 0, framesWithLayers = 0;
    double sumAngErr = 0, maxAngErr = 0, sumPosErr = 0, maxPosErr = 0;
    std::map<int, int> lagHist;
    std::string swapFormat, appName;
    uint32_t swapW = 0, swapH = 0;
    double fps = 0;
    std::vector<std::string> dumps;
    double lastMean[2][3] = {};
    bool haveMean = false;
    int quadFrames = 0;   // quad layers with image statistics
};
static Stats g_stats;
static std::mutex g_statsMutex;
static bool g_summaryWritten = false;

static void PushEvent(Instance* in, const void* ev, size_t size)
{
    XrEventDataBuffer b{ XR_TYPE_EVENT_DATA_BUFFER };
    memcpy(&b, ev, std::min(size, sizeof(b)));
    std::lock_guard<std::mutex> lk(in->em);
    in->events.push_back(b);
}
static const char* StateName(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE"; case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED"; case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED"; case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING"; case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}
static void SetState(Session* s, XrSessionState st)
{
    s->state = st;
    XrEventDataSessionStateChanged e{ XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
    e.session = (XrSession)(uintptr_t)s;
    e.state = st;
    e.time = NowNs();
    PushEvent(s->inst, &e, sizeof(e));
    Log("session state -> %s", StateName(st));
}
static void RequestExit(Session* s, const char* why)
{
    if (s->exitRequested) return;
    s->exitRequested = true;
    Log("requesting session exit (%s)", why);
    if (s->running) {
        if (s->state == XR_SESSION_STATE_FOCUSED) SetState(s, XR_SESSION_STATE_VISIBLE);
        if (s->state == XR_SESSION_STATE_VISIBLE) SetState(s, XR_SESSION_STATE_SYNCHRONIZED);
        SetState(s, XR_SESSION_STATE_STOPPING);
    }
}

// Simulated Touch controllers (ODE_VHMD_HANDS=1): a pure function of time like the head, in LOCAL space.
// Left hand: fixed place, swings +-30 degrees in yaw (6 s). Right hand: a 10 cm circle in the X/Y plane (4 s),
// pitching +-20 degrees (5 s). The grip pose sits 5 cm behind and 2 cm below the aim pose, pitched 40 degrees.
static bool HandsOn()
{
    static const bool on = [] { wchar_t b[8]; DWORD n = GetEnvironmentVariableW(L"ODE_VHMD_HANDS", b, 8); return n > 0 && n < 8 && b[0] != L'0'; }();
    return on;
}
static P HandAim(int hand, double t)
{
    const double tau = 6.283185307179586;
    if (hand == 0) return { FromYPR((float)(30 * std::sin(tau * t / 6.0)), 0, 0), { -0.20f, -0.30f, -0.35f } };
    return { FromYPR(0, (float)(20 * std::sin(tau * t / 5.0)), 0),
             { 0.20f + (float)(0.10 * std::cos(tau * t / 4.0)), -0.30f + (float)(0.10 * std::sin(tau * t / 4.0)), -0.35f } };
}
static P HandPose(int hand, bool grip, double t)
{
    P aim = HandAim(hand, t);
    return grip ? Compose(aim, { AxisAngle({ 1, 0, 0 }, -40 * kDeg), { 0, -0.02f, 0.05f } }) : aim;
}

// World = STAGE space. LOCAL sits at the head's start position, FloorHeight above the stage origin.
static P SpaceWorld(const Space* sp, XrTime t)
{
    P local{ {}, { 0, g_cfg.floorHeight, 0 } };
    double sec = (double)(t - sp->s->t0) * 1e-9;
    if (sp->hand >= 0) return Compose(Compose(local, HandPose(sp->hand, sp->grip, sec)), sp->pose);
    switch (sp->type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW: return Compose(Compose(local, HeadAt(sec)), sp->pose);
    case XR_REFERENCE_SPACE_TYPE_LOCAL: return Compose(local, sp->pose);
    default: return sp->pose;
    }
}
static P HeadWorld(const Session* s, XrTime t)
{
    P local{ {}, { 0, g_cfg.floorHeight, 0 } };
    return Compose(local, HeadAt((double)(t - s->t0) * 1e-9));
}

// ================================================================================================
// summary
// ================================================================================================
// final=false: periodic snapshot (so a run that is killed on timeout still leaves a summary).
static void WriteSummary(const char* why, bool final = true)
{
    std::lock_guard<std::mutex> lk(g_statsMutex);
    if (g_summaryWritten) return;
    if (final) g_summaryWritten = true;
    std::wstring p = g_cfg.outDir + L"\\summary.json";
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    const Stats& s = g_stats;
    int n = std::max(1, s.framesWithLayers);
    fprintf(f, "{\n  \"runtime\": \"ODE Virtual HMD\",\n  \"reason\": \"%s\",\n  \"app\": \"%s\",\n", why, s.appName.c_str());
    fprintf(f, "  \"frames\": %d,\n  \"framesWithLayers\": %d,\n  \"fps\": %.2f,\n", s.frames, s.framesWithLayers, s.fps);
    fprintf(f, "  \"swapchain\": { \"format\": \"%s\", \"width\": %u, \"height\": %u },\n", s.swapFormat.c_str(), s.swapW, s.swapH);
    fprintf(f, "  \"poseErrorDeg\": { \"mean\": %.4f, \"max\": %.4f },\n  \"posErrorMm\": { \"mean\": %.3f, \"max\": %.3f },\n",
            s.sumAngErr / n, s.maxAngErr, s.sumPosErr / n, s.maxPosErr);
    fprintf(f, "  \"poseLagFrames\": {");
    bool first = true;
    for (auto& kv : s.lagHist) { fprintf(f, "%s \"%d\": %d", first ? "" : ",", kv.first, kv.second); first = false; }
    fprintf(f, " },\n");
    if (s.haveMean)
        fprintf(f, "  \"lastEyeMeanRGB\": [[%.1f, %.1f, %.1f], [%.1f, %.1f, %.1f]],\n", s.lastMean[0][0], s.lastMean[0][1], s.lastMean[0][2], s.lastMean[1][0], s.lastMean[1][1], s.lastMean[1][2]);
    fprintf(f, "  \"violations\": %d,\n  \"violationKinds\": [", g_violations.load());
    first = true;
    for (auto& kv : g_violationKinds) {
        std::string esc; for (char c : kv.first) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
        fprintf(f, "%s\n    { \"what\": \"%s\", \"count\": %d }", first ? "" : ",", esc.c_str(), kv.second); first = false;
    }
    fprintf(f, "%s],\n  \"unsupportedFunctions\": [", first ? "" : "\n  ");
    first = true;
    for (auto& u : g_unsupported) { fprintf(f, "%s\"%s\"", first ? "" : ", ", u.c_str()); first = false; }
    fprintf(f, "],\n  \"dumps\": [");
    first = true;
    for (auto& d : s.dumps) { std::string esc; for (char c : d) { if (c == '\\') esc += '\\'; esc += c; } fprintf(f, "%s\"%s\"", first ? "" : ", ", esc.c_str()); first = false; }
    fprintf(f, "]\n}\n");
    fclose(f);
    if (final) Log("summary written (%s): %d frames, %d violations", why, s.frames, g_violations.load());
}

// ================================================================================================
// image readback
// ================================================================================================
static DXGI_FORMAT Typeless(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    default: return f;
    }
}
static const char* FormatName(int64_t f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB"; case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB"; case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT"; case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    default: return "other";
    }
}
static const int64_t kFormats[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                                    // depth (XR_KHR_composition_layer_depth; D3D12 sessions only)
                                    DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D16_UNORM };
static bool IsDepthFormat(int64_t f) { return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_D16_UNORM; }

// Reads an image rect back; computes mean RGB / non-black fraction and optionally writes a BMP.
static bool ReadBack(Session* s, Swapchain* sc, uint32_t idx, const XrRect2Di& rc, const std::wstring& bmpPath, double mean[3], double& nonBlack)
{
    DXGI_FORMAT fmt = (DXGI_FORMAT)sc->ci.format;
    bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool rgb10 = fmt == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!bgra && !rgba && !rgb10) return false;
    struct { void* pData; UINT RowPitch; } m{};
    struct { UINT Width, Height; } d{};
    ID3D11Texture2D* st = nullptr;
    ID3D12Resource* rb = nullptr;
    if (s->dev12) {
        // D3D12: copy into a readback buffer on the app's queue and wait for it
        ID3D12Resource* src = sc->res[idx];
        D3D12_RESOURCE_DESC rd = src->GetDesc();
        if (rd.SampleDesc.Count > 1) return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowBytes, total;
        s->dev12->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s->dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return false;
        s->alloc12->Reset();
        s->list12->Reset(s->alloc12, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s->list12->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{ rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION srcl{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; srcl.SubresourceIndex = 0;
        s->list12->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        s->list12->ResourceBarrier(1, &b);
        s->list12->Close();
        ID3D12CommandList* lists[] = { s->list12 };
        s->queue12->ExecuteCommandLists(1, lists);
        s->queue12->Signal(s->fence12, ++s->fenceVal);
        if (s->fence12->GetCompletedValue() < s->fenceVal) { s->fence12->SetEventOnCompletion(s->fenceVal, s->fenceEvent); WaitForSingleObject(s->fenceEvent, 2000); }
        D3D12_RANGE r{ 0, (SIZE_T)total };
        if (FAILED(rb->Map(0, &r, &m.pData))) { rb->Release(); return false; }
        m.RowPitch = fp.Footprint.RowPitch; d.Width = (UINT)rd.Width; d.Height = rd.Height;
    } else {
        D3D11_TEXTURE2D_DESC td; sc->tex[idx]->GetDesc(&td);
        if (td.SampleDesc.Count > 1) return false;
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        td.ArraySize = 1; td.MipLevels = 1;
        if (FAILED(s->dev->CreateTexture2D(&td, nullptr, &st))) return false;
        s->ctx->CopySubresourceRegion(st, 0, 0, 0, 0, sc->tex[idx], D3D11CalcSubresource(0, 0, sc->ci.mipCount), nullptr);
        D3D11_MAPPED_SUBRESOURCE mm;
        if (FAILED(s->ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm))) { st->Release(); return false; }
        m.pData = mm.pData; m.RowPitch = mm.RowPitch; d.Width = td.Width; d.Height = td.Height;
    }
    int x0 = std::max(0, rc.offset.x), y0 = std::max(0, rc.offset.y);
    int w = std::min<int>(rc.extent.width, (int)d.Width - x0), h = std::min<int>(rc.extent.height, (int)d.Height - y0);
    double sum[3] = {}; uint64_t lit = 0, total = 0;
    FILE* f = nullptr;
    std::vector<uint8_t> row;
    if (!bmpPath.empty() && w > 0 && h > 0) {
        f = _wfopen(bmpPath.c_str(), L"wb");
        if (f) {
            int rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4;
            BITMAPFILEHEADER bf = {}; BITMAPINFOHEADER bi = {};
            bf.bfType = 0x4D42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + (rowBytes + pad) * h;
            bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
            fwrite(&bf, sizeof(bf), 1, f); fwrite(&bi, sizeof(bi), 1, f);
            row.resize(rowBytes + pad);
        }
    }
    for (int yy = h - 1; yy >= 0; --yy) {   // bottom-up for BMP
        const uint32_t* src = (const uint32_t*)((const uint8_t*)m.pData + (size_t)(y0 + yy) * m.RowPitch) + x0;
        for (int xx = 0; xx < w; ++xx) {
            uint32_t px = src[xx];
            uint8_t r, g, b;
            if (bgra) { b = px & 255; g = (px >> 8) & 255; r = (px >> 16) & 255; }
            else if (rgb10) { r = (px >> 2) & 255; g = (px >> 12) & 255; b = (px >> 22) & 255; }
            else { r = px & 255; g = (px >> 8) & 255; b = (px >> 16) & 255; }
            sum[0] += r; sum[1] += g; sum[2] += b;
            if (r > 8 || g > 8 || b > 8) ++lit;
            ++total;
            if (f) { row[xx * 3 + 0] = b; row[xx * 3 + 1] = g; row[xx * 3 + 2] = r; }
        }
        if (f) fwrite(row.data(), row.size(), 1, f);
    }
    if (f) fclose(f);
    if (rb) { D3D12_RANGE none{ 0, 0 }; rb->Unmap(0, &none); rb->Release(); }
    if (st) { s->ctx->Unmap(st, 0); st->Release(); }
    for (int i = 0; i < 3; ++i) mean[i] = total ? sum[i] / total : 0;
    nonBlack = total ? (double)lit / total : 0;
    return true;
}

// ================================================================================================
// OpenXR entry points
// ================================================================================================
#define XRFN(name) static XrResult XRAPI_CALL vh_##name
static const char* kExtensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME };
static const uint32_t kExtVersions[] = { XR_KHR_D3D11_enable_SPEC_VERSION, XR_KHR_D3D12_enable_SPEC_VERSION, XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION, XR_KHR_composition_layer_depth_SPEC_VERSION };

XRFN(xrEnumerateApiLayerProperties)(uint32_t cap, uint32_t* count, XrApiLayerProperties*) { if (!count) return XR_ERROR_VALIDATION_FAILURE; *count = 0; (void)cap; return XR_SUCCESS; }

XRFN(xrEnumerateInstanceExtensionProperties)(const char* layer, uint32_t cap, uint32_t* count, XrExtensionProperties* props)
{
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    const uint32_t n = (uint32_t)(sizeof(kExtensions) / sizeof(kExtensions[0]));
    *count = n;
    if (!cap) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < n; ++i) {
        if (props[i].type != XR_TYPE_EXTENSION_PROPERTIES) Violation("xrEnumerateInstanceExtensionProperties: element type not XR_TYPE_EXTENSION_PROPERTIES");
        strcpy(props[i].extensionName, kExtensions[i]);
        props[i].extensionVersion = kExtVersions[i];
    }
    return XR_SUCCESS;
}

XRFN(xrCreateInstance)(const XrInstanceCreateInfo* ci, XrInstance* out)
{
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->type != XR_TYPE_INSTANCE_CREATE_INFO) { Violation("xrCreateInstance: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    if (XR_VERSION_MAJOR(ci->applicationInfo.apiVersion) != 1) return XR_ERROR_API_VERSION_UNSUPPORTED;
    auto* in = new Instance();
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        bool ok = false;
        for (auto e : kExtensions) if (!strcmp(e, ci->enabledExtensionNames[i])) ok = true;
        if (!ok) { Log("xrCreateInstance: unsupported extension %s", ci->enabledExtensionNames[i]); delete in; return XR_ERROR_EXTENSION_NOT_PRESENT; }
        in->exts.insert(ci->enabledExtensionNames[i]);
    }
    if (ci->enabledApiLayerCount) { delete in; return XR_ERROR_API_LAYER_NOT_PRESENT; }
    in->appName = ci->applicationInfo.applicationName;
    in->engineName = ci->applicationInfo.engineName;
    { std::lock_guard<std::mutex> lk(g_statsMutex); g_stats.appName = in->appName; }
    Log("xrCreateInstance: app '%s' engine '%s' api %u.%u.%u", in->appName.c_str(), in->engineName.c_str(),
        XR_VERSION_MAJOR(ci->applicationInfo.apiVersion), XR_VERSION_MINOR(ci->applicationInfo.apiVersion), XR_VERSION_PATCH(ci->applicationInfo.apiVersion));
    *out = MakeH<XrInstance>(in);
    return XR_SUCCESS;
}

XRFN(xrDestroySession)(XrSession h);
XRFN(xrDestroyInstance)(XrInstance h)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    std::vector<Session*> ss(in->sessions.begin(), in->sessions.end());
    for (auto* s : ss) vh_xrDestroySession((XrSession)(uintptr_t)s);
    Forget(in);
    delete in;
    WriteSummary("xrDestroyInstance");
    return XR_SUCCESS;
}

XRFN(xrGetInstanceProperties)(XrInstance h, XrInstanceProperties* p)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    strcpy(p->runtimeName, "ODE Virtual HMD");
    return XR_SUCCESS;
}

XRFN(xrPollEvent)(XrInstance h, XrEventDataBuffer* ev)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!ev) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lk(in->em);
    if (in->events.empty()) return XR_EVENT_UNAVAILABLE;
    *ev = in->events.front();
    in->events.pop_front();
    return XR_SUCCESS;
}

XRFN(xrResultToString)(XrInstance, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE])
{
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, r == XR_SUCCESS ? "XR_SUCCESS" : "XR_RESULT_%d", (int)r);
    return XR_SUCCESS;
}
XRFN(xrStructureTypeToString)(XrInstance, XrStructureType t, char buf[XR_MAX_STRUCTURE_NAME_SIZE])
{
    snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_STRUCTURE_TYPE_%d", (int)t);
    return XR_SUCCESS;
}

static const XrSystemId kSystemId = 1;
XRFN(xrGetSystem)(XrInstance h, const XrSystemGetInfo* gi, XrSystemId* out)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (!gi || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *out = kSystemId;
    return XR_SUCCESS;
}

XRFN(xrGetSystemProperties)(XrInstance h, XrSystemId sys, XrSystemProperties* p)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (sys != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->systemId = sys;
    p->vendorId = 0x0DE;
    strcpy(p->systemName, "ODE Virtual HMD");
    p->graphicsProperties.maxLayerCount = 16;
    p->graphicsProperties.maxSwapchainImageWidth = 8192;
    p->graphicsProperties.maxSwapchainImageHeight = 8192;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XRFN(xrEnumerateEnvironmentBlendModes)(XrInstance h, XrSystemId sys, XrViewConfigurationType, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (sys != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    *count = 1;
    if (cap) { if (cap < 1) return XR_ERROR_SIZE_INSUFFICIENT; modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; }
    return XR_SUCCESS;
}

XRFN(xrEnumerateViewConfigurations)(XrInstance h, XrSystemId sys, uint32_t cap, uint32_t* count, XrViewConfigurationType* t)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (sys != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    *count = 1;
    if (cap) t[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XRFN(xrGetViewConfigurationProperties)(XrInstance h, XrSystemId sys, XrViewConfigurationType t, XrViewConfigurationProperties* p)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (sys != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    p->viewConfigurationType = t;
    p->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

XRFN(xrEnumerateViewConfigurationViews)(XrInstance h, XrSystemId sys, XrViewConfigurationType t, uint32_t cap, uint32_t* count, XrViewConfigurationView* v)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    if (sys != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    *count = 2;
    if (!cap) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; ++i) {
        if (v[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) Violation("xrEnumerateViewConfigurationViews: element type not set");
        v[i].recommendedImageRectWidth = g_cfg.eyeW; v[i].recommendedImageRectHeight = g_cfg.eyeH;
        v[i].maxImageRectWidth = 8192; v[i].maxImageRectHeight = 8192;
        v[i].recommendedSwapchainSampleCount = 1; v[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XRFN(xrGetD3D11GraphicsRequirementsKHR)(XrInstance h, XrSystemId sys, XrGraphicsRequirementsD3D11KHR* req)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (sys != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!req) return XR_ERROR_VALIDATION_FAILURE;
    IDXGIFactory1* f = nullptr;
    IDXGIAdapter1* a = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
        // like a real runtime on a hybrid PC: the high-performance GPU, not whatever Windows lists first
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6))) {
            f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&a);
            f6->Release();
        }
        if (a || SUCCEEDED(f->EnumAdapters1(0, &a))) {
            DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
            in->luid = d.AdapterLuid;
            Log("graphics adapter: %s", Narrow(d.Description).c_str());
            a->Release();
        }
        f->Release();
    }
    req->adapterLuid = in->luid;
    req->minFeatureLevel = D3D_FEATURE_LEVEL_10_0;
    in->d3dReqQueried = true;
    return XR_SUCCESS;
}

XRFN(xrGetD3D12GraphicsRequirementsKHR)(XrInstance h, XrSystemId sys, XrGraphicsRequirementsD3D12KHR* req)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!req) return XR_ERROR_VALIDATION_FAILURE;
    XrGraphicsRequirementsD3D11KHR r11{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XrResult r = vh_xrGetD3D11GraphicsRequirementsKHR(h, sys, &r11);
    if (r != XR_SUCCESS) return r;
    req->adapterLuid = r11.adapterLuid;
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    in->d3d12ReqQueried = true;
    return XR_SUCCESS;
}

XRFN(xrCreateSession)(XrInstance h, const XrSessionCreateInfo* ci, XrSession* out)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    const XrGraphicsBindingD3D11KHR* gb = nullptr;
    const XrGraphicsBindingD3D12KHR* gb12 = nullptr;
    for (auto* n = (const XrBaseInStructure*)ci->next; n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) gb = (const XrGraphicsBindingD3D11KHR*)n;
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) gb12 = (const XrGraphicsBindingD3D12KHR*)n;
    }
    if (gb12) {
        if (!in->d3d12ReqQueried) { Violation("xrCreateSession before xrGetD3D12GraphicsRequirementsKHR"); return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING; }
        if (!gb12->device || !gb12->queue) { Violation("D3D12 binding without device/queue"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
        if (!in->exts.count(XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) { Violation("D3D12 binding without enabling XR_KHR_D3D12_enable"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
        LUID l = gb12->device->GetAdapterLuid();
        if (memcmp(&l, &in->luid, sizeof(LUID)) != 0) Violation("D3D12 device is not on the adapter from xrGetD3D12GraphicsRequirementsKHR");
        auto* s = new Session();
        s->inst = in;
        s->dev12 = gb12->device; s->dev12->AddRef();
        s->queue12 = gb12->queue; s->queue12->AddRef();
        if (FAILED(s->dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&s->alloc12)) ||
            FAILED(s->dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->alloc12, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&s->list12)) ||
            FAILED(s->dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&s->fence12))) {
            Log("xrCreateSession: D3D12 helper objects failed");
            return XR_ERROR_RUNTIME_FAILURE;
        }
        s->list12->Close();
        s->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s->period = (int64_t)(1e9 / g_cfg.hz);
        s->t0 = NowNs();
        in->sessions.insert(s);
        *out = MakeH<XrSession>(s);
        Log("xrCreateSession: D3D12, %s clock at %.1f Hz", g_cfg.paced ? "paced" : "virtual", g_cfg.hz);
        SetState(s, XR_SESSION_STATE_IDLE);
        SetState(s, XR_SESSION_STATE_READY);
        return XR_SUCCESS;
    }
    if (!in->d3dReqQueried) { Violation("xrCreateSession before xrGetD3D11GraphicsRequirementsKHR"); return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING; }
    if (!gb || !gb->device) { Violation("xrCreateSession without a D3D11 graphics binding"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    if (!in->exts.count(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) { Violation("D3D11 binding without enabling XR_KHR_D3D11_enable"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    IDXGIDevice* dd = nullptr;
    if (SUCCEEDED(gb->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd))) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dd->GetAdapter(&ad))) {
            DXGI_ADAPTER_DESC d; ad->GetDesc(&d);
            if (memcmp(&d.AdapterLuid, &in->luid, sizeof(LUID)) != 0) Violation("D3D11 device is not on the adapter from xrGetD3D11GraphicsRequirementsKHR");
            ad->Release();
        }
        dd->Release();
    }
    auto* s = new Session();
    s->inst = in;
    s->dev = gb->device; s->dev->AddRef();
    s->dev->GetImmediateContext(&s->ctx);
    s->period = (int64_t)(1e9 / g_cfg.hz);
    s->t0 = NowNs();
    in->sessions.insert(s);
    *out = MakeH<XrSession>(s);
    Log("xrCreateSession: D3D11 feature level %x, %s clock at %.1f Hz", (unsigned)s->dev->GetFeatureLevel(), g_cfg.paced ? "paced" : "virtual", g_cfg.hz);
    SetState(s, XR_SESSION_STATE_IDLE);
    SetState(s, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XRFN(xrDestroySwapchain)(XrSwapchain h);
XRFN(xrDestroySpace)(XrSpace h);
XRFN(xrDestroySession)(XrSession h)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    std::vector<Swapchain*> scs(s->swapchains.begin(), s->swapchains.end());
    for (auto* sc : scs) vh_xrDestroySwapchain((XrSwapchain)(uintptr_t)sc);
    std::vector<Space*> sps(s->spaces.begin(), s->spaces.end());
    for (auto* sp : sps) vh_xrDestroySpace((XrSpace)(uintptr_t)sp);
    if (s->ctx) s->ctx->Release();
    if (s->dev) s->dev->Release();
    if (s->list12) s->list12->Release();
    if (s->alloc12) s->alloc12->Release();
    if (s->fence12) s->fence12->Release();
    if (s->fenceEvent) CloseHandle(s->fenceEvent);
    if (s->queue12) s->queue12->Release();
    if (s->dev12) s->dev12->Release();
    s->inst->sessions.erase(s);
    Forget(s);
    Log("xrDestroySession after %d frames", s->frame);
    delete s;
    return XR_SUCCESS;
}

XRFN(xrBeginSession)(XrSession h, const XrSessionBeginInfo* bi)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!bi) return XR_ERROR_VALIDATION_FAILURE;
    if (s->running) return XR_ERROR_SESSION_RUNNING;
    if (s->state != XR_SESSION_STATE_READY) { Violation("xrBeginSession while not READY"); return XR_ERROR_SESSION_NOT_READY; }
    if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    s->running = true;
    s->t0 = NowNs();
    s->nextVsync = 0;
    SetState(s, XR_SESSION_STATE_SYNCHRONIZED);
    SetState(s, XR_SESSION_STATE_VISIBLE);
    SetState(s, XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XRFN(xrEndSession)(XrSession h)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != XR_SESSION_STATE_STOPPING) { Violation("xrEndSession while not STOPPING"); return XR_ERROR_SESSION_NOT_STOPPING; }
    s->running = false;
    s->frameBegun = false; s->pendingWaits = 0;
    SetState(s, XR_SESSION_STATE_IDLE);
    if (s->exitRequested) SetState(s, XR_SESSION_STATE_EXITING);
    return XR_SUCCESS;
}

XRFN(xrRequestExitSession)(XrSession h)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    RequestExit(s, "app called xrRequestExitSession");
    return XR_SUCCESS;
}

XRFN(xrEnumerateReferenceSpaces)(XrSession h, uint32_t cap, uint32_t* count, XrReferenceSpaceType* t)
{
    if (!Get<Session>(h)) return XR_ERROR_HANDLE_INVALID;
    *count = 3;
    if (!cap) return XR_SUCCESS;
    if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    t[0] = XR_REFERENCE_SPACE_TYPE_VIEW; t[1] = XR_REFERENCE_SPACE_TYPE_LOCAL; t[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
    return XR_SUCCESS;
}

XRFN(xrCreateReferenceSpace)(XrSession h, const XrReferenceSpaceCreateInfo* ci, XrSpace* out)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    const auto& q = ci->poseInReferenceSpace.orientation;
    float ql = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (std::fabs(ql - 1.0f) > 0.01f) { Violation("xrCreateReferenceSpace: non-unit orientation (|q| = %.3f)", ql); return XR_ERROR_POSE_INVALID; }
    auto* sp = new Space();
    sp->s = s; sp->type = ci->referenceSpaceType; sp->pose = FromXr(ci->poseInReferenceSpace);
    s->spaces.insert(sp);
    *out = MakeH<XrSpace>(sp);
    return XR_SUCCESS;
}

XRFN(xrGetReferenceSpaceBoundsRect)(XrSession h, XrReferenceSpaceType t, XrExtent2Df* b)
{
    if (!Get<Session>(h)) return XR_ERROR_HANDLE_INVALID;
    if (t == XR_REFERENCE_SPACE_TYPE_STAGE) { b->width = 3; b->height = 3; return XR_SUCCESS; }
    b->width = b->height = 0;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XRFN(xrDestroySpace)(XrSpace h)
{
    auto* sp = Get<Space>(h);
    if (!sp) return XR_ERROR_HANDLE_INVALID;
    sp->s->spaces.erase(sp);
    Forget(sp);
    delete sp;
    return XR_SUCCESS;
}

XRFN(xrLocateSpace)(XrSpace h, XrSpace baseH, XrTime t, XrSpaceLocation* loc)
{
    auto* sp = Get<Space>(h);
    auto* base = Get<Space>(baseH);
    if (!sp || !base) return XR_ERROR_HANDLE_INVALID;
    if (!loc) return XR_ERROR_VALIDATION_FAILURE;
    if (t <= 0) { Violation("xrLocateSpace with time %lld", (long long)t); return XR_ERROR_TIME_INVALID; }
    if ((sp->hand >= 0 || base->hand >= 0) && !HandsOn()) {   // controllers off: no pose
        loc->locationFlags = 0; loc->pose = ToXr(P{});
        return XR_SUCCESS;
    }
    P rel =Compose(Inv(SpaceWorld(base, t)), SpaceWorld(sp, t));
    // ODE_VHMD_NOISE=<deg>: white orientation noise on every xrLocateSpace answer, like a real runtime's pose
    // prediction (each query re-predicts from fresh sensor data). Frames are still judged against the clean script.
    static const float noiseDeg = (float)_wtof(Env(L"ODE_VHMD_NOISE").c_str());
    if (noiseDeg > 0) {
        static std::atomic<uint32_t> seed{ 12345 };
        auto rnd = [&]() { uint32_t s = seed.load(); s = s * 1664525u + 1013904223u; seed = s; return ((s >> 8) / 16777216.0f) * 2 - 1; };
        float k = noiseDeg * 0.0174533f * 0.5f;
        Q n{ rnd() * k, rnd() * k, rnd() * k, 1 };
        float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z + 1);
        n.x /= l; n.y /= l; n.z /= l; n.w /= l;
        rel.q = Mul(rel.q, n);
    }
    loc->pose = ToXr(rel);
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    for (auto* n = (XrBaseOutStructure*)loc->next; n; n = n->next) {
        if (n->type == XR_TYPE_SPACE_VELOCITY) {
            // angular velocity (base space) from the clean script, like a runtime's IMU-based estimate
            auto* v = (XrSpaceVelocity*)n;
            const XrTime h2 = 1000000;   // +-1 ms
            P a = Compose(Inv(SpaceWorld(base, t - h2)), SpaceWorld(sp, t - h2));
            P b = Compose(Inv(SpaceWorld(base, t + h2)), SpaceWorld(sp, t + h2));
            Q d = Mul(b.q, Conj(a.q));
            if (d.w < 0) { d.x = -d.x; d.y = -d.y; d.z = -d.z; d.w = -d.w; }
            float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            float k = s > 1e-9f ? 2 * std::atan2(s, d.w) / s / 0.002f : 0.0f;
            v->velocityFlags = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            v->linearVelocity = {}; v->angularVelocity = { d.x * k, d.y * k, d.z * k };
        }
    }
    return XR_SUCCESS;
}

XRFN(xrLocateViews)(XrSession h, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* views)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!li || !vs || !count) return XR_ERROR_VALIDATION_FAILURE;
    if (li->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    auto* sp = Get<Space>(li->space);
    if (!sp) return XR_ERROR_HANDLE_INVALID;
    if (li->displayTime <= 0) { Violation("xrLocateViews with time %lld", (long long)li->displayTime); return XR_ERROR_TIME_INVALID; }
    *count = 2;
    if (!cap) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                         XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    P inv = Inv(SpaceWorld(sp, li->displayTime));
    P head = HeadWorld(s, li->displayTime);
    for (int i = 0; i < 2; ++i) {
        if (views[i].type != XR_TYPE_VIEW) Violation("xrLocateViews: XrView type not set");
        views[i].pose = ToXr(Compose(inv, Compose(head, EyeOffset(i))));
        views[i].fov = EyeFov(i);
    }
    return XR_SUCCESS;
}

XRFN(xrEnumerateSwapchainFormats)(XrSession h, uint32_t cap, uint32_t* count, int64_t* f)
{
    if (!Get<Session>(h)) return XR_ERROR_HANDLE_INVALID;
    const uint32_t n = (uint32_t)(sizeof(kFormats) / sizeof(kFormats[0]));
    *count = n;
    if (!cap) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < n; ++i) f[i] = kFormats[i];
    return XR_SUCCESS;
}

XRFN(xrCreateSwapchain)(XrSession h, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (std::find(std::begin(kFormats), std::end(kFormats), ci->format) == std::end(kFormats)) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (!ci->width || !ci->height || ci->width > 8192 || ci->height > 8192 || ci->faceCount != 1 || !ci->arraySize || !ci->mipCount)
        return XR_ERROR_VALIDATION_FAILURE;
    if (s->dev12) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = ci->width; rd.Height = ci->height;
        rd.DepthOrArraySize = (UINT16)ci->arraySize; rd.MipLevels = (UINT16)ci->mipCount;
        rd.Format = Typeless((DXGI_FORMAT)ci->format); rd.SampleDesc.Count = std::max(1u, ci->sampleCount);
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const bool depth = IsDepthFormat(ci->format);
        rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        auto* sc = new Swapchain();
        sc->s = s; sc->ci = *ci; sc->ci.next = nullptr;
        for (int i = 0; i < 3; ++i) {
            ID3D12Resource* r = nullptr;
            HRESULT hr = s->dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource), (void**)&r);
            if (FAILED(hr)) {
                Log("xrCreateSwapchain: D3D12 CreateCommittedResource failed %08lx", (unsigned long)hr);
                for (auto* x : sc->res) x->Release();
                delete sc;
                return XR_ERROR_RUNTIME_FAILURE;
            }
            sc->res.push_back(r);
        }
        s->swapchains.insert(sc);
        *out = MakeH<XrSwapchain>(sc);
        Log("xrCreateSwapchain (D3D12) %ux%u %s samples %u usage %llx", ci->width, ci->height, FormatName(ci->format), rd.SampleDesc.Count, (unsigned long long)ci->usageFlags);
        if (depth) return XR_SUCCESS;
        std::lock_guard<std::mutex> lk(g_statsMutex);
        g_stats.swapFormat = FormatName(ci->format); g_stats.swapW = ci->width; g_stats.swapH = ci->height;
        return XR_SUCCESS;
    }
    if (IsDepthFormat(ci->format)) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;   // (depth swapchains: D3D12 only)
    D3D11_TEXTURE2D_DESC d{};
    d.Width = ci->width; d.Height = ci->height; d.MipLevels = ci->mipCount; d.ArraySize = ci->arraySize;
    d.Format = Typeless((DXGI_FORMAT)ci->format);   // like real runtimes: typeless so the app picks the view format
    d.SampleDesc.Count = std::max(1u, ci->sampleCount);
    d.Usage = D3D11_USAGE_DEFAULT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) d.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    auto* sc = new Swapchain();
    sc->s = s; sc->ci = *ci; sc->ci.next = nullptr;
    for (int i = 0; i < 3; ++i) {
        ID3D11Texture2D* t = nullptr;
        HRESULT hr = s->dev->CreateTexture2D(&d, nullptr, &t);
        if (FAILED(hr)) {
            Log("xrCreateSwapchain: CreateTexture2D failed %08lx", (unsigned long)hr);
            for (auto* x : sc->tex) x->Release();
            delete sc;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        sc->tex.push_back(t);
    }
    s->swapchains.insert(sc);
    *out = MakeH<XrSwapchain>(sc);
    Log("xrCreateSwapchain %ux%u %s samples %u usage %llx", ci->width, ci->height, FormatName(ci->format), d.SampleDesc.Count, (unsigned long long)ci->usageFlags);
    std::lock_guard<std::mutex> lk(g_statsMutex);
    g_stats.swapFormat = FormatName(ci->format); g_stats.swapW = ci->width; g_stats.swapH = ci->height;
    return XR_SUCCESS;
}

XRFN(xrDestroySwapchain)(XrSwapchain h)
{
    auto* sc = Get<Swapchain>(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    for (auto* t : sc->tex) t->Release();
    for (auto* r : sc->res) r->Release();
    sc->s->swapchains.erase(sc);
    Forget(sc);
    delete sc;
    return XR_SUCCESS;
}

XRFN(xrEnumerateSwapchainImages)(XrSwapchain h, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
    auto* sc = Get<Swapchain>(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    *count = (uint32_t)sc->Count();
    if (!cap) return XR_SUCCESS;
    if (cap < sc->Count()) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!sc->res.empty()) {
        auto* img12 = (XrSwapchainImageD3D12KHR*)images;
        for (size_t i = 0; i < sc->res.size(); ++i) {
            if (img12[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) { Violation("xrEnumerateSwapchainImages: element type not XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR"); return XR_ERROR_VALIDATION_FAILURE; }
            img12[i].texture = sc->res[i];
        }
        return XR_SUCCESS;
    }
    auto* img = (XrSwapchainImageD3D11KHR*)images;
    for (size_t i = 0; i < sc->tex.size(); ++i) {
        if (img[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) { Violation("xrEnumerateSwapchainImages: element type not XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR"); return XR_ERROR_VALIDATION_FAILURE; }
        img[i].texture = sc->tex[i];
    }
    return XR_SUCCESS;
}

XRFN(xrAcquireSwapchainImage)(XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* idx)
{
    auto* sc = Get<Swapchain>(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!idx) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->acquired.size() >= sc->Count()) { Violation("xrAcquireSwapchainImage: all images already acquired"); return XR_ERROR_CALL_ORDER_INVALID; }
    *idx = sc->next;
    sc->acquired.push_back(sc->next);
    sc->next = (sc->next + 1) % (uint32_t)sc->Count();
    return XR_SUCCESS;
}

XRFN(xrWaitSwapchainImage)(XrSwapchain h, const XrSwapchainImageWaitInfo* wi)
{
    auto* sc = Get<Swapchain>(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!wi) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->acquired.empty() || sc->waited) { Violation("xrWaitSwapchainImage without an acquired, unwaited image"); return XR_ERROR_CALL_ORDER_INVALID; }
    sc->waited = true;
    return XR_SUCCESS;
}

XRFN(xrReleaseSwapchainImage)(XrSwapchain h, const XrSwapchainImageReleaseInfo*)
{
    auto* sc = Get<Swapchain>(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (sc->acquired.empty() || !sc->waited) { Violation("xrReleaseSwapchainImage without a waited image"); return XR_ERROR_CALL_ORDER_INVALID; }
    sc->lastReleased = (int)sc->acquired.front();
    sc->acquired.pop_front();
    sc->waited = false;
    return XR_SUCCESS;
}

static void SleepUntilNs(int64_t target)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t now = NowNs();
    if (target - now > 1500000 && timer) {
        LARGE_INTEGER due; due.QuadPart = -(target - now - 1000000) / 100;   // 100 ns units, relative
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
    while (NowNs() < target) YieldProcessor();
}

XRFN(xrWaitFrame)(XrSession h, const XrFrameWaitInfo*, XrFrameState* fs)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!fs) return XR_ERROR_VALIDATION_FAILURE;
    if (!s->running) { Violation("xrWaitFrame while the session is not running"); return XR_ERROR_SESSION_NOT_RUNNING; }
    int64_t display;
    {
        std::lock_guard<std::mutex> lk(s->fm);
        if (s->pendingWaits > 0 && !s->frameBegun) Violation("xrWaitFrame called again without xrBeginFrame");
    }
    if (g_cfg.paced) {
        int64_t now = NowNs();
        if (!s->nextVsync || now - s->nextVsync > 4 * s->period) s->nextVsync = now + s->period;
        SleepUntilNs(s->nextVsync);
        display = s->nextVsync + s->period;   // one frame of latency, like a real compositor
        s->nextVsync += s->period;
    } else {
        if (!s->nextVsync) s->nextVsync = s->t0;
        s->nextVsync += s->period;
        display = s->nextVsync + s->period;
    }
    {
        std::lock_guard<std::mutex> lk(s->fm);
        ++s->pendingWaits;
        s->lastPredicted = display;
        s->predicted.insert(display);
        while (s->predicted.size() > 16) s->predicted.erase(s->predicted.begin());
    }
    fs->predictedDisplayTime = display;
    fs->predictedDisplayPeriod = s->period;
    fs->shouldRender = (s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED) ? XR_TRUE : XR_FALSE;
    // ODE_VHMD_FOCUSLOSS=<start s>,<duration s>: like opening the headset's dashboard - the session drops to VISIBLE
    // with shouldRender off, then comes back to FOCUSED with a LOCAL-space recentre (as Virtual Desktop did).
    static double lossStart = -1, lossDur = 0;
    static int lossPhase = -1;
    if (lossPhase < 0) {
        lossPhase = 0;
        char v[64] = "";
        if (GetEnvironmentVariableA("ODE_VHMD_FOCUSLOSS", v, sizeof(v)) && sscanf(v, "%lf,%lf", &lossStart, &lossDur) == 2) Log("focus loss scripted at %.1f s for %.1f s", lossStart, lossDur);
        else lossPhase = 2;
    }
    if (lossPhase < 2) {
        double sec = (double)(NowNs() - s->t0) * 1e-9;
        if (lossPhase == 0 && sec >= lossStart && s->state == XR_SESSION_STATE_FOCUSED) { SetState(s, XR_SESSION_STATE_VISIBLE); lossPhase = 1; }
        if (lossPhase == 1 && sec >= lossStart + lossDur) {
            SetState(s, XR_SESSION_STATE_FOCUSED);
            XrEventDataReferenceSpaceChangePending rc{ XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING };
            rc.session = (XrSession)(uintptr_t)s; rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rc.changeTime = NowNs();
            PushEvent(s->inst, &rc, sizeof(rc));
            Log("focus restored, LOCAL space recentred");
            lossPhase = 2;
        }
        if (lossPhase == 1) fs->shouldRender = XR_FALSE;
    }
    return XR_SUCCESS;
}

XRFN(xrBeginFrame)(XrSession h, const XrFrameBeginInfo*)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    std::lock_guard<std::mutex> lk(s->fm);
    if (s->pendingWaits <= 0) { Violation("xrBeginFrame without a preceding xrWaitFrame"); return XR_ERROR_CALL_ORDER_INVALID; }
    --s->pendingWaits;
    if (s->frameBegun) return XR_FRAME_DISCARDED;
    s->frameBegun = true;
    return XR_SUCCESS;
}

static std::string Vec(const V3& v) { char b[96]; snprintf(b, 96, "[%.4f, %.4f, %.4f]", v.x, v.y, v.z); return b; }

XRFN(xrEndFrame)(XrSession h, const XrFrameEndInfo* ei)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ei) return XR_ERROR_VALIDATION_FAILURE;
    {
        std::lock_guard<std::mutex> lk(s->fm);
        if (!s->frameBegun) { Violation("xrEndFrame without xrBeginFrame"); return XR_ERROR_CALL_ORDER_INVALID; }
        s->frameBegun = false;
        if (!s->predicted.count(ei->displayTime)) Violation("xrEndFrame displayTime is not a predicted display time");
    }
    if (ei->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) { Violation("xrEndFrame blend mode %d", (int)ei->environmentBlendMode); return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED; }
    if (ei->layerCount > 16) return XR_ERROR_LAYER_LIMIT_EXCEEDED;

    int frame = ++s->frame;
    int64_t now = NowNs();
    if (!s->firstEnd) s->firstEnd = now;
    s->lastEnd = now;
    // DumpFrames / DumpEvery count frames with content (the n-th frame that carries layers).
    int contentFrame = 0;
    if (ei->layerCount) { std::lock_guard<std::mutex> lk(g_statsMutex); contentFrame = g_stats.framesWithLayers + 1; }
    bool dump = contentFrame && (g_cfg.dumpFrames.count(contentFrame) || (g_cfg.dumpEvery > 0 && contentFrame % g_cfg.dumpEvery == 0));
    // on-demand dump for test scripts: create <out>\dump.req; the next frame with layers is saved and the file removed
    if (contentFrame && frame % 10 == 0) {
        std::wstring req = g_cfg.outDir + L"\\dump.req";
        if (GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(req.c_str()); dump = true; Log("dump requested at content frame %d", contentFrame); }
    }
    bool stats = dump || (g_cfg.statsEvery > 0 && frame % g_cfg.statsEvery == 0);

    std::string json = "{\"frame\": " + std::to_string(frame) + ", \"displayTime\": " + std::to_string((long long)ei->displayTime) +
                       ", \"t\": " + std::to_string((ei->displayTime - s->t0) * 1e-9) + ", \"layers\": [";
    double frameAng = -1, framePos = -1; int frameLag = 0;
    int quadLayers = 0;
    for (uint32_t li = 0; li < ei->layerCount; ++li) {
        const XrCompositionLayerBaseHeader* L = ei->layers[li];
        if (!L) { Violation("xrEndFrame: null layer pointer"); return XR_ERROR_LAYER_INVALID; }
        if (li) json += ", ";
        if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto* pl = (const XrCompositionLayerProjection*)L;
            auto* sp = Get<Space>(pl->space);
            if (!sp) { Violation("projection layer space invalid"); return XR_ERROR_HANDLE_INVALID; }
            if (pl->viewCount != 2) { Violation("projection layer viewCount %u", pl->viewCount); return XR_ERROR_VALIDATION_FAILURE; }
            json += "{\"type\": \"projection\", \"views\": [";
            P spaceW = SpaceWorld(sp, ei->displayTime);
            for (int v = 0; v < 2; ++v) {
                const auto& pv = pl->views[v];
                auto* sc = Get<Swapchain>(pv.subImage.swapchain);
                if (!sc) { Violation("projection view %d swapchain invalid", v); return XR_ERROR_HANDLE_INVALID; }
                if (sc->lastReleased < 0) { Violation("projection view %d swapchain has no released image", v); return XR_ERROR_LAYER_INVALID; }
                const auto& r = pv.subImage.imageRect;
                if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width <= 0 || r.extent.height <= 0 ||
                    (uint32_t)(r.offset.x + r.extent.width) > sc->ci.width || (uint32_t)(r.offset.y + r.extent.height) > sc->ci.height) {
                    Violation("projection view %d imageRect outside the swapchain", v);
                    return XR_ERROR_SWAPCHAIN_RECT_INVALID;
                }
                // pose error vs the scripted eye pose at this display time, and the best-matching frame lag
                P sub = Compose(spaceW, FromXr(pv.pose));
                P expect = Compose(HeadWorld(s, ei->displayTime), EyeOffset(v));
                float ang = AngleDeg(sub.q, expect.q), pos = Len(sub.p - expect.p) * 1000.0f;
                int bestLag = 0; float bestErr = 1e9f;
                for (int k = -6; k <= 6; ++k) {
                    P e = Compose(HeadWorld(s, ei->displayTime + k * s->period), EyeOffset(v));
                    float err = AngleDeg(sub.q, e.q) + Len(sub.p - e.p) * 10.0f;   // 1 cm ~ 0.1 deg
                    if (err < bestErr) { bestErr = err; bestLag = -k; }
                }
                if (v == 0) { frameAng = ang; framePos = pos; frameLag = bestLag; }
                else { frameAng = std::max(frameAng, (double)ang); framePos = std::max(framePos, (double)pos); }
                float yaw, pitch, roll; YPR(sub.q, yaw, pitch, roll);
                char b[640];
                snprintf(b, sizeof(b), "%s{\"pos\": %s, \"ypr\": [%.3f, %.3f, %.3f], \"fovDeg\": [%.2f, %.2f, %.2f, %.2f], \"rect\": [%d, %d, %d, %d], "
                         "\"swapchain\": [%u, %u], \"image\": %d, \"angErrDeg\": %.4f, \"posErrMm\": %.3f, \"lagFrames\": %d",
                         v ? ", " : "", Vec(sub.p).c_str(), yaw, pitch, roll, pv.fov.angleLeft / kDeg, pv.fov.angleRight / kDeg, pv.fov.angleUp / kDeg,
                         pv.fov.angleDown / kDeg, r.offset.x, r.offset.y, r.extent.width, r.extent.height, sc->ci.width, sc->ci.height, sc->lastReleased,
                         ang, pos, bestLag);
                json += b;
                if (stats) {
                    double mean[3]; double lit = 0;
                    std::wstring bmp;
                    if (dump) {
                        wchar_t name[64]; swprintf(name, 64, L"content_%05d_%c.bmp", contentFrame, v ? L'R' : L'L');
                        bmp = g_cfg.outDir + L"\\" + name;
                    }
                    if (ReadBack(s, sc, (uint32_t)sc->lastReleased, r, bmp, mean, lit)) {
                        snprintf(b, sizeof(b), ", \"meanRGB\": [%.1f, %.1f, %.1f], \"nonBlack\": %.3f", mean[0], mean[1], mean[2], lit);
                        json += b;
                        std::lock_guard<std::mutex> lk(g_statsMutex);
                        for (int c = 0; c < 3; ++c) g_stats.lastMean[v][c] = mean[c];
                        g_stats.haveMean = true;
                        if (!bmp.empty()) { g_stats.dumps.push_back(Narrow(bmp)); json += ", \"bmp\": \"" + Narrow(fs::path(bmp).filename().wstring()) + "\""; }
                    }
                }
                json += "}";
            }
            json += "]}";
        } else if (L->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            auto* q = (const XrCompositionLayerQuad*)L;
            auto* sp = Get<Space>(q->space);
            auto* sc = Get<Swapchain>(q->subImage.swapchain);
            if (!sp || !sc) { Violation("quad layer space/swapchain invalid"); return XR_ERROR_HANDLE_INVALID; }
            if (sc->lastReleased < 0) { Violation("quad layer swapchain has no released image"); return XR_ERROR_LAYER_INVALID; }
            const auto& r = q->subImage.imageRect;
            if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width <= 0 || r.extent.height <= 0 ||
                (uint32_t)(r.offset.x + r.extent.width) > sc->ci.width || (uint32_t)(r.offset.y + r.extent.height) > sc->ci.height) {
                Violation("quad imageRect outside the swapchain");
                return XR_ERROR_SWAPCHAIN_RECT_INVALID;
            }
            if (q->size.width <= 0 || q->size.height <= 0) { Violation("quad size %.3fx%.3f", q->size.width, q->size.height); return XR_ERROR_VALIDATION_FAILURE; }
            P w = Compose(SpaceWorld(sp, ei->displayTime), FromXr(q->pose));
            char b[512];
            snprintf(b, sizeof(b), "{\"type\": \"quad\", \"pos\": %s, \"size\": [%.3f, %.3f], \"rect\": [%d, %d, %d, %d], \"swapchain\": [%u, %u], \"image\": %d",
                     Vec(w.p).c_str(), q->size.width, q->size.height, r.offset.x, r.offset.y, r.extent.width, r.extent.height, sc->ci.width, sc->ci.height, sc->lastReleased);
            json += b;
            if (stats) {
                double mean[3]; double lit = 0;
                std::wstring bmp;
                if (dump) { wchar_t name[64]; swprintf(name, 64, L"content_%05d_quad%u.bmp", contentFrame, li); bmp = g_cfg.outDir + L"\\" + name; }
                if (ReadBack(s, sc, (uint32_t)sc->lastReleased, r, bmp, mean, lit)) {
                    snprintf(b, sizeof(b), ", \"meanRGB\": [%.1f, %.1f, %.1f], \"nonBlack\": %.3f", mean[0], mean[1], mean[2], lit);
                    json += b;
                    std::lock_guard<std::mutex> lk(g_statsMutex);
                    for (int c = 0; c < 3; ++c) g_stats.lastMean[0][c] = g_stats.lastMean[1][c] = mean[c];
                    g_stats.haveMean = true;
                    g_stats.quadFrames++;
                    if (!bmp.empty()) { g_stats.dumps.push_back(Narrow(bmp)); json += ", \"bmp\": \"" + Narrow(fs::path(bmp).filename().wstring()) + "\""; }
                }
            }
            json += "}";
            ++quadLayers;
        } else {
            json += "{\"type\": " + std::to_string((int)L->type) + "}";
            Violation("unsupported layer type %d", (int)L->type);
        }
    }
    json += "]}";
    {
        std::lock_guard<std::mutex> lk(g_statsMutex);
        Stats& st = g_stats;
        st.frames = frame;
        if (frameAng >= 0) {
            ++st.framesWithLayers;
            st.sumAngErr += frameAng; st.maxAngErr = std::max(st.maxAngErr, frameAng);
            st.sumPosErr += framePos; st.maxPosErr = std::max(st.maxPosErr, framePos);
            st.lagHist[frameLag]++;
        } else if (quadLayers) {
            ++st.framesWithLayers;
        } else {
            ++s->emptyFrames;
        }
        if (frame > 1 && s->lastEnd > s->firstEnd) st.fps = (frame - 1) * 1e9 / (double)(s->lastEnd - s->firstEnd);
    }
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_frames) { fputs(json.c_str(), g_frames); fputc('\n', g_frames); if (frame % 30 == 0 || dump) fflush(g_frames); }
    }
    if (frame == 1 || frame % 300 == 0) Log("frame %d (%s)", frame, ei->layerCount ? "layers" : "empty");
    if (frame % 90 == 0) WriteSummary("periodic", false);
    // MaxFrames counts frames that carried layers: an app may run its session for a long time before it
    // has anything to show (ODE starts the session while the game is still loading).
    int withLayers;
    { std::lock_guard<std::mutex> lk(g_statsMutex); withLayers = g_stats.framesWithLayers; }
    if (g_cfg.maxFrames > 0 && withLayers >= g_cfg.maxFrames) RequestExit(s, "MaxFrames reached");
    return XR_SUCCESS;
}

XRFN(xrStringToPath)(XrInstance h, const char* str, XrPath* out)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!str || !out || str[0] != '/') return XR_ERROR_PATH_FORMAT_INVALID;
    for (size_t i = 0; i < in->paths.size(); ++i) if (in->paths[i] == str) { *out = i + 1; return XR_SUCCESS; }
    in->paths.push_back(str);
    *out = in->paths.size();
    return XR_SUCCESS;
}
XRFN(xrPathToString)(XrInstance h, XrPath p, uint32_t cap, uint32_t* count, char* buf)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!p || p > in->paths.size()) return XR_ERROR_PATH_INVALID;
    const std::string& s = in->paths[(size_t)p - 1];
    *count = (uint32_t)s.size() + 1;
    if (!cap) return XR_SUCCESS;
    if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, s.c_str(), *count);
    return XR_SUCCESS;
}

// ================================================================================================
// input: actions, bound to the simulated Touch controllers (ODE_VHMD_HANDS=1; without it the controllers are off:
// every action inactive, no pose). The inputs are a pure function of the time of the last xrSyncActions:
//   right trigger 0..1 (3 s cycle); left squeeze 1 during the first second of every 4; left stick circling at 0.8
//   (5 s); right lower button (A) in seconds 2.0-2.5 of every 4, right upper (B) in 3.0-3.5; left stick click in
//   1.0-1.3 of every 6; left menu in 10.0-10.2 of every 20.
// ================================================================================================
struct ActionSet { Instance* inst = nullptr; std::string name; };
struct Action { ActionSet* set = nullptr; XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT; std::string name; std::vector<XrPath> sub; std::vector<std::string> bind; };
static std::mutex g_am;
static bool g_attached = false;
static double g_syncSec = -1;
static int g_hapticCalls = 0;

static std::string PathStr(Instance* in, XrPath p) { return p && p <= in->paths.size() ? in->paths[(size_t)p - 1] : std::string(); }
static int HandOfPath(const std::string& p) { return p.rfind("/user/hand/left", 0) == 0 ? 0 : p.rfind("/user/hand/right", 0) == 0 ? 1 : -1; }
// The simulated controller's value for one binding path: x (and y for the stick). false = no such input.
static bool SimInput(const std::string& path, double t, float& x, float& y)
{
    const double tau = 6.283185307179586;
    int hand = HandOfPath(path);
    if (hand < 0) return false;
    std::string in = path.substr(hand == 0 ? 15 : 16);
    auto win = [&](double period, double a, double b) { double m = std::fmod(t, period); return m >= a && m < b ? 1.0f : 0.0f; };
    x = y = 0;
    float sx = hand == 0 ? (float)(0.8 * std::sin(tau * t / 5.0)) : 0.0f, sy = hand == 0 ? (float)(0.8 * std::cos(tau * t / 5.0)) : 0.0f;
    if (in == "/input/trigger/value") { x = hand == 1 ? (float)(0.5 - 0.5 * std::cos(tau * t / 3.0)) : 0.0f; return true; }
    if (in == "/input/squeeze/value") { x = hand == 0 ? win(4, 0, 1) : 0.0f; return true; }
    if (in == "/input/thumbstick") { x = sx; y = sy; return true; }
    if (in == "/input/thumbstick/x") { x = sx; return true; }
    if (in == "/input/thumbstick/y") { x = sy; return true; }
    if (in == "/input/thumbstick/click") { x = hand == 0 ? win(6, 1, 1.3) : 0.0f; return true; }
    if (in == "/input/x/click" && hand == 0) return true;
    if (in == "/input/y/click" && hand == 0) return true;
    if (in == "/input/a/click" && hand == 1) { x = win(4, 2, 2.5); return true; }
    if (in == "/input/b/click" && hand == 1) { x = win(4, 3, 3.5); return true; }
    if (in == "/input/menu/click" && hand == 0) { x = win(20, 10, 10.2); return true; }
    if (in == "/input/grip/pose" || in == "/input/aim/pose" || in == "/output/haptic") return true;
    return false;
}

XRFN(xrCreateActionSet)(XrInstance h, const XrActionSetCreateInfo* ci, XrActionSet* out)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_ACTION_SET_CREATE_INFO) { Violation("xrCreateActionSet: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    if (!ci->actionSetName[0] || !ci->localizedActionSetName[0]) { Violation("xrCreateActionSet: empty name"); return XR_ERROR_NAME_INVALID; }
    auto* a = new ActionSet(); a->inst = in; a->name = ci->actionSetName;
    *out = MakeH<XrActionSet>(a);
    return XR_SUCCESS;
}
XRFN(xrDestroyActionSet)(XrActionSet h) { auto* a = Get<ActionSet>(h); if (!a) return XR_ERROR_HANDLE_INVALID; Forget(a); return XR_SUCCESS; }
XRFN(xrCreateAction)(XrActionSet h, const XrActionCreateInfo* ci, XrAction* out)
{
    auto* set = Get<ActionSet>(h);
    if (!set) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_ACTION_CREATE_INFO) { Violation("xrCreateAction: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    if (g_attached) { Violation("xrCreateAction after xrAttachSessionActionSets"); return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED; }
    for (const char* c = ci->actionName; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) { Violation("xrCreateAction: name '%s' not a valid path level", ci->actionName); return XR_ERROR_PATH_FORMAT_INVALID; }
    if (!ci->actionName[0] || !ci->localizedActionName[0]) { Violation("xrCreateAction: empty name"); return XR_ERROR_NAME_INVALID; }
    auto* a = new Action(); a->set = set; a->type = ci->actionType; a->name = ci->actionName;
    for (uint32_t i = 0; i < ci->countSubactionPaths; ++i) a->sub.push_back(ci->subactionPaths[i]);
    *out = MakeH<XrAction>(a);
    return XR_SUCCESS;
}
XRFN(xrDestroyAction)(XrAction h) { auto* a = Get<Action>(h); if (!a) return XR_ERROR_HANDLE_INVALID; Forget(a); return XR_SUCCESS; }
XRFN(xrSuggestInteractionProfileBindings)(XrInstance h, const XrInteractionProfileSuggestedBinding* sb)
{
    auto* in = Get<Instance>(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!sb || sb->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) { Violation("xrSuggestInteractionProfileBindings: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    if (g_attached) { Violation("xrSuggestInteractionProfileBindings after xrAttachSessionActionSets"); return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED; }
    std::string prof = PathStr(in, sb->interactionProfile);
    const bool touch = prof == "/interaction_profiles/oculus/touch_controller";
    std::lock_guard<std::mutex> lk(g_am);
    for (uint32_t i = 0; i < sb->countSuggestedBindings; ++i) {
        auto* a = Get<Action>(sb->suggestedBindings[i].action);
        if (!a) return XR_ERROR_HANDLE_INVALID;
        std::string p = PathStr(in, sb->suggestedBindings[i].binding);
        if (!touch) continue;   // other controllers: accepted, not simulated
        float x, y;
        if (!SimInput(p, 0, x, y)) { Violation("xrSuggestInteractionProfileBindings: '%s' is not an input of the Touch controller", p.c_str()); return XR_ERROR_PATH_UNSUPPORTED; }
        const bool pose = p.find("/pose") != std::string::npos, haptic = p.find("/output/") != std::string::npos;
        if (pose != (a->type == XR_ACTION_TYPE_POSE_INPUT) || haptic != (a->type == XR_ACTION_TYPE_VIBRATION_OUTPUT))
            Violation("xrSuggestInteractionProfileBindings: action '%s' bound to '%s' (type mismatch)", a->name.c_str(), p.c_str());
        a->bind.push_back(p);
    }
    Log("input: %u bindings suggested for %s%s", sb->countSuggestedBindings, prof.c_str(), touch ? " (the simulated controllers)" : " (not simulated)");
    return XR_SUCCESS;
}
XRFN(xrAttachSessionActionSets)(XrSession h, const XrSessionActionSetsAttachInfo* ai)
{
    if (!Get<Session>(h)) return XR_ERROR_HANDLE_INVALID;
    if (!ai || ai->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO || !ai->countActionSets) { Violation("xrAttachSessionActionSets: bad info"); return XR_ERROR_VALIDATION_FAILURE; }
    if (g_attached) { Violation("xrAttachSessionActionSets called twice"); return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED; }
    g_attached = true;
    Log("input: %u action set(s) attached, simulated controllers %s", ai->countActionSets, HandsOn() ? "on" : "off (ODE_VHMD_HANDS not set)");
    return XR_SUCCESS;
}
XRFN(xrSyncActions)(XrSession h, const XrActionsSyncInfo* si)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!si || si->type != XR_TYPE_ACTIONS_SYNC_INFO) { Violation("xrSyncActions: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    if (!g_attached) { Violation("xrSyncActions before xrAttachSessionActionSets"); return XR_ERROR_ACTIONSET_NOT_ATTACHED; }
    if (s->state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
    std::lock_guard<std::mutex> lk(g_am);
    g_syncSec = (double)(s->lastPredicted - s->t0) * 1e-9;
    return XR_SUCCESS;
}
// The action's value for the asked hand: the largest of its bindings. false = not active.
static bool ActionValue(Session* s, const XrActionStateGetInfo* gi, XrActionType want, float& x, float& y)
{
    x = y = 0;
    auto* a = Get<Action>(gi->action);
    if (!a) return false;
    if (a->type != want) { Violation("xrGetActionState*: action '%s' has another type", a->name.c_str()); return false; }
    std::string sub = PathStr(s->inst, gi->subactionPath);
    std::lock_guard<std::mutex> lk(g_am);
    if (!HandsOn() || g_syncSec < 0) return false;
    bool any = false;
    for (auto& b : a->bind) {
        if (!sub.empty() && b.rfind(sub, 0) != 0) continue;
        float bx, by;
        if (!SimInput(b, g_syncSec, bx, by)) continue;
        if (!any || std::fabs(bx) + std::fabs(by) > std::fabs(x) + std::fabs(y)) { x = bx; y = by; }
        any = true;
    }
    return any;
}
XRFN(xrGetActionStateFloat)(XrSession h, const XrActionStateGetInfo* gi, XrActionStateFloat* st)
{
    auto* s = Get<Session>(h);
    if (!s || !gi || !st) return XR_ERROR_HANDLE_INVALID;
    float x, y; st->isActive = ActionValue(s, gi, XR_ACTION_TYPE_FLOAT_INPUT, x, y) ? XR_TRUE : XR_FALSE;
    st->currentState = x; st->changedSinceLastSync = XR_FALSE; st->lastChangeTime = s->lastPredicted;
    return XR_SUCCESS;
}
XRFN(xrGetActionStateBoolean)(XrSession h, const XrActionStateGetInfo* gi, XrActionStateBoolean* st)
{
    auto* s = Get<Session>(h);
    if (!s || !gi || !st) return XR_ERROR_HANDLE_INVALID;
    float x, y; st->isActive = ActionValue(s, gi, XR_ACTION_TYPE_BOOLEAN_INPUT, x, y) ? XR_TRUE : XR_FALSE;
    st->currentState = x > 0.5f ? XR_TRUE : XR_FALSE; st->changedSinceLastSync = XR_FALSE; st->lastChangeTime = s->lastPredicted;
    return XR_SUCCESS;
}
XRFN(xrGetActionStateVector2f)(XrSession h, const XrActionStateGetInfo* gi, XrActionStateVector2f* st)
{
    auto* s = Get<Session>(h);
    if (!s || !gi || !st) return XR_ERROR_HANDLE_INVALID;
    float x, y; st->isActive = ActionValue(s, gi, XR_ACTION_TYPE_VECTOR2F_INPUT, x, y) ? XR_TRUE : XR_FALSE;
    st->currentState = { x, y }; st->changedSinceLastSync = XR_FALSE; st->lastChangeTime = s->lastPredicted;
    return XR_SUCCESS;
}
XRFN(xrGetActionStatePose)(XrSession h, const XrActionStateGetInfo* gi, XrActionStatePose* st)
{
    auto* s = Get<Session>(h);
    if (!s || !gi || !st) return XR_ERROR_HANDLE_INVALID;
    float x, y; st->isActive = ActionValue(s, gi, XR_ACTION_TYPE_POSE_INPUT, x, y) ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
XRFN(xrCreateActionSpace)(XrSession h, const XrActionSpaceCreateInfo* ci, XrSpace* out)
{
    auto* s = Get<Session>(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) { Violation("xrCreateActionSpace: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    auto* a = Get<Action>(ci->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) { Violation("xrCreateActionSpace: '%s' is not a pose action", a->name.c_str()); return XR_ERROR_ACTION_TYPE_MISMATCH; }
    std::string sub = PathStr(s->inst, ci->subactionPath);
    auto* sp = new Space();
    sp->s = s; sp->pose = FromXr(ci->poseInActionSpace);
    sp->hand = HandOfPath(sub);
    {
        std::lock_guard<std::mutex> lk(g_am);
        for (auto& b : a->bind) {
            if (!sub.empty() && b.rfind(sub, 0) != 0) continue;
            if (sp->hand < 0) sp->hand = HandOfPath(b);
            sp->grip = b.find("/grip/pose") != std::string::npos;
            break;
        }
    }
    if (sp->hand < 0) sp->hand = 0;
    s->spaces.insert(sp);
    *out = MakeH<XrSpace>(sp);
    return XR_SUCCESS;
}
XRFN(xrApplyHapticFeedback)(XrSession h, const XrHapticActionInfo* hi, const XrHapticBaseHeader* hf)
{
    auto* s = Get<Session>(h);
    if (!s || !hi || !hf) return XR_ERROR_HANDLE_INVALID;
    auto* a = Get<Action>(hi->action);
    if (!a || a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT) { Violation("xrApplyHapticFeedback: not a vibration action"); return XR_ERROR_ACTION_TYPE_MISMATCH; }
    if (hf->type != XR_TYPE_HAPTIC_VIBRATION) { Violation("xrApplyHapticFeedback: wrong struct type"); return XR_ERROR_VALIDATION_FAILURE; }
    auto* v = (const XrHapticVibration*)hf;
    ++g_hapticCalls;
    Log("input: haptic #%d on %s: %.0f ms, amplitude %.2f", g_hapticCalls, PathStr(s->inst, hi->subactionPath).c_str(), v->duration / 1e6, v->amplitude);
    return XR_SUCCESS;
}
XRFN(xrStopHapticFeedback)(XrSession h, const XrHapticActionInfo*) { return Get<Session>(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID; }
XRFN(xrGetCurrentInteractionProfile)(XrSession h, XrPath top, XrInteractionProfileState* st)
{
    auto* s = Get<Session>(h);
    if (!s || !st) return XR_ERROR_HANDLE_INVALID;
    (void)top;
    st->interactionProfile = XR_NULL_PATH;
    if (HandsOn() && g_attached) {
        const char* p = "/interaction_profiles/oculus/touch_controller";
        auto& ps = s->inst->paths;
        size_t i = 0;
        for (; i < ps.size(); ++i) if (ps[i] == p) break;
        if (i == ps.size()) ps.push_back(p);
        st->interactionProfile = i + 1;
    }
    return XR_SUCCESS;
}

XRFN(xrConvertWin32PerformanceCounterToTimeKHR)(XrInstance h, const LARGE_INTEGER* pc, XrTime* t)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    *t = (XrTime)((double)pc->QuadPart * 1e9 / (double)g_qpcFreq);
    return XR_SUCCESS;
}
XRFN(xrConvertTimeToWin32PerformanceCounterKHR)(XrInstance h, XrTime t, LARGE_INTEGER* pc)
{
    if (!Get<Instance>(h)) return XR_ERROR_HANDLE_INVALID;
    pc->QuadPart = (LONGLONG)((double)t * (double)g_qpcFreq / 1e9);
    return XR_SUCCESS;
}

// ================================================================================================
// dispatch
// ================================================================================================
XRFN(xrGetInstanceProcAddr)(XrInstance h, const char* name, PFN_xrVoidFunction* fn);

struct FnEntry { const char* name; PFN_xrVoidFunction fn; const char* ext; bool preInstance; };
#define E(n) { #n, (PFN_xrVoidFunction)vh_##n, nullptr, false }
#define EX(n, ext) { #n, (PFN_xrVoidFunction)vh_##n, ext, false }
#define EP(n) { #n, (PFN_xrVoidFunction)vh_##n, nullptr, true }
static const FnEntry kFns[] = {
    EP(xrEnumerateApiLayerProperties), EP(xrEnumerateInstanceExtensionProperties), EP(xrCreateInstance), EP(xrGetInstanceProcAddr),
    E(xrDestroyInstance), E(xrGetInstanceProperties), E(xrPollEvent), E(xrResultToString), E(xrStructureTypeToString),
    E(xrGetSystem), E(xrGetSystemProperties), E(xrEnumerateEnvironmentBlendModes), E(xrEnumerateViewConfigurations),
    E(xrGetViewConfigurationProperties), E(xrEnumerateViewConfigurationViews), E(xrCreateSession), E(xrDestroySession),
    E(xrBeginSession), E(xrEndSession), E(xrRequestExitSession), E(xrEnumerateReferenceSpaces), E(xrCreateReferenceSpace),
    E(xrGetReferenceSpaceBoundsRect), E(xrDestroySpace), E(xrLocateSpace), E(xrLocateViews), E(xrEnumerateSwapchainFormats),
    E(xrCreateSwapchain), E(xrDestroySwapchain), E(xrEnumerateSwapchainImages), E(xrAcquireSwapchainImage),
    E(xrWaitSwapchainImage), E(xrReleaseSwapchainImage), E(xrWaitFrame), E(xrBeginFrame), E(xrEndFrame),
    E(xrStringToPath), E(xrPathToString),
    E(xrCreateActionSet), E(xrDestroyActionSet), E(xrCreateAction), E(xrDestroyAction), E(xrSuggestInteractionProfileBindings),
    E(xrAttachSessionActionSets), E(xrSyncActions), E(xrGetActionStateFloat), E(xrGetActionStateBoolean), E(xrGetActionStateVector2f),
    E(xrGetActionStatePose), E(xrCreateActionSpace), E(xrApplyHapticFeedback), E(xrStopHapticFeedback), E(xrGetCurrentInteractionProfile),
    EX(xrGetD3D11GraphicsRequirementsKHR, XR_KHR_D3D11_ENABLE_EXTENSION_NAME),
    EX(xrGetD3D12GraphicsRequirementsKHR, XR_KHR_D3D12_ENABLE_EXTENSION_NAME),
    EX(xrConvertWin32PerformanceCounterToTimeKHR, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME),
    EX(xrConvertTimeToWin32PerformanceCounterKHR, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME),
};

XRFN(xrGetInstanceProcAddr)(XrInstance h, const char* name, PFN_xrVoidFunction* fn)
{
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    *fn = nullptr;
    Instance* in = h ? Get<Instance>(h) : nullptr;
    if (h && !in) return XR_ERROR_HANDLE_INVALID;
    for (auto& e : kFns) {
        if (strcmp(e.name, name)) continue;
        if (!in && !e.preInstance) return XR_ERROR_HANDLE_INVALID;
        if (e.ext && in && !in->exts.count(e.ext)) return XR_ERROR_FUNCTION_UNSUPPORTED;
        *fn = e.fn;
        return XR_SUCCESS;
    }
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_unsupported.insert(name).second && g_log) { fprintf(g_log, "unsupported function requested: %s\n", name); fflush(g_log); }
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---- one-time init (outside DllMain: file I/O under the loader lock is unsafe) ----------------
static HMODULE g_module = nullptr;
static std::once_flag g_initOnce;
static bool g_inited = false;
static void InitOnce()
{
    std::call_once(g_initOnce, [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
        wchar_t p[MAX_PATH]; GetModuleFileNameW(g_module, p, MAX_PATH);
        g_dllDir = p; g_dllDir = g_dllDir.substr(0, g_dllDir.find_last_of(L"\\/"));
        LoadConfig();
        std::error_code ec;
        fs::create_directories(g_cfg.outDir, ec);
        g_log = _wfopen((g_cfg.outDir + L"\\vhmd.log").c_str(), L"w");
        g_frames = _wfopen((g_cfg.outDir + L"\\frames.jsonl").c_str(), L"w");
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log("ODE Virtual HMD (%s) loaded into %s", sizeof(void*) == 8 ? "x64" : "x86", Narrow(exe).c_str());
        Log("display %ux%u per eye @ %.1f Hz, FOV L%.0f R%.0f U%.0f D%.0f, IPD %.1f mm, motion '%s', %s, MaxFrames %d",
            g_cfg.eyeW, g_cfg.eyeH, g_cfg.hz, g_cfg.fovL, g_cfg.fovR, g_cfg.fovU, g_cfg.fovD, g_cfg.ipd * 1000, g_cfg.motion.c_str(),
            g_cfg.paced ? "paced" : "virtual clock", g_cfg.maxFrames);
        Log("output: %s", Narrow(g_cfg.outDir).c_str());
        if (g_cfg.motion == "file") LoadMotionFile();
        g_inited = true;
    });
}

// ---- loader negotiation (from openxr_loader_negotiation.h) -------------------------------------
enum { kLoaderInfo = 1, kRuntimeRequest = 3 };
struct NegotiateLoaderInfo { int structType; uint32_t structVersion; size_t structSize; uint32_t minInterfaceVersion, maxInterfaceVersion; XrVersion minApiVersion, maxApiVersion; };
struct NegotiateRuntimeRequest { int structType; uint32_t structVersion; size_t structSize; uint32_t runtimeInterfaceVersion; XrVersion runtimeApiVersion; PFN_xrGetInstanceProcAddr getInstanceProcAddr; };

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const NegotiateLoaderInfo* li, NegotiateRuntimeRequest* rq)
{
    InitOnce();
    if (!li || !rq || li->structType != kLoaderInfo || rq->structType != kRuntimeRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (li->minInterfaceVersion > 1 || li->maxInterfaceVersion < 1) return XR_ERROR_INITIALIZATION_FAILED;
    rq->runtimeInterfaceVersion = 1;
    rq->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 34);
    rq->getInstanceProcAddr = vh_xrGetInstanceProcAddr;
    Log("negotiated with loader (interface 1, API 1.0)");
    return XR_SUCCESS;
}

extern "C" XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance h, const char* name, PFN_xrVoidFunction* fn) { InitOnce(); return vh_xrGetInstanceProcAddr(h, name, fn); }

// ================================================================================================
BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = mod;
        DisableThreadLibraryCalls(mod);
    } else if (reason == DLL_PROCESS_DETACH && g_inited) {
        // Last resort if the app never destroyed its instance (crash-free exit without cleanup).
        WriteSummary(reserved ? "process exit" : "runtime unloaded");
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_frames) { fclose(g_frames); g_frames = nullptr; }
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
