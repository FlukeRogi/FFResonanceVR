// FFResonanceVR in-game VR menu.
//
// Drawn on the CPU with the game's own assets (extracted by tools\extract_assets.ps1 into FFResonanceVR\assets: the
// SE-GARRITHR title font and SE-VOGEN text font, the System Menu's list bars, frame, cursor and button prompts),
// uploaded to an OpenXR swapchain of our own and shown as a quad in front of the player.
//
// Input: keyboard through a low-level hook (F10 toggles; arrows/WASD, Enter/Space, Esc/Backspace; keys are kept from
// the game while the menu is open) and gamepad through FfvrMenuXInput(), called by the mod's UEVR plugin from
// UEVR's XInput hook (so VR controllers that UEVR maps to a gamepad work too): L3+R3 toggles, D-pad/left stick,
// A confirms, B closes; the game sees a neutral pad while the menu is open.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <d3d12.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "ffvr_menu.h"
#include "ffvr_canvas.h"

extern HMODULE FfModule();
XrSpace LayerLocalSpace();
PFN_xrEnumerateSwapchainFormats Nx_EnumerateSwapchainFormats();

// ---- assets -------------------------------------------------------------------------------------------------
struct Assets {
    bool loaded = false, ok = false;
    Font title, text, textBold;
    Image bar, barSel, frame, cursor, arrow, line, keyA, keyB, keyUD, keyLR, keyL3, keyR3, kEnter, kEsc, kUp, kDown, kLeft, kRight;
};
static Assets g_a;
static std::wstring AssetsDir()
{
    wchar_t p[MAX_PATH]; GetModuleFileNameW(FfModule(), p, MAX_PATH);
    std::wstring d = p; d = d.substr(0, d.find_last_of(L"\\/"));   // build
    d = d.substr(0, d.find_last_of(L"\\/"));                        // project root
    return d + L"\\assets\\";
}
static void LoadAssets()
{
    if (g_a.loaded) return;
    g_a.loaded = true;
    std::wstring d = AssetsDir();
    bool ok = g_a.title.Load(d + L"SE-GARRITHR.ttf") & g_a.text.Load(d + L"SE-VOGEN.ttf");
    g_a.textBold.Load(d + L"SE-VOGEB___.ttf");
    ok &= g_a.bar.Load(d + L"T_mainmenu_Listlabel.png") & g_a.barSel.Load(d + L"T_mainmenu_Listlabel_Select.png") &
          g_a.frame.Load(d + L"T_CommonInsideFrame.png") & g_a.cursor.Load(d + L"MenuIcon_FingerCursor_R.png");
    g_a.arrow.Load(d + L"T_TabArrow.png"); g_a.line.Load(d + L"T_Menu_line_center.png");
    g_a.keyA.Load(d + L"KEY_WINpad_BUTTON_A.png"); g_a.keyB.Load(d + L"KEY_WINpad_BUTTON_B.png");
    g_a.keyUD.Load(d + L"KEY_WINpad_CROSSKEY_UPDOWN.png"); g_a.keyLR.Load(d + L"KEY_WINpad_CROSSKEY_RIGHTLEFT.png");
    g_a.keyL3.Load(d + L"KEY_WINpad_STICK_L_PUSH.png"); g_a.keyR3.Load(d + L"KEY_WINpad_STICK_R_PUSH.png");
    g_a.kEnter.Load(d + L"Key_Win_Enter.png"); g_a.kEsc.Load(d + L"Key_Win_Esc.png");
    g_a.kUp.Load(d + L"Key_Win_Up.png"); g_a.kDown.Load(d + L"Key_Win_Down.png");
    g_a.kLeft.Load(d + L"Key_Win_Left.png"); g_a.kRight.Load(d + L"Key_Win_Right.png");
    g_a.ok = ok;
    FfLog("menu assets %s (%ls)", ok ? "loaded" : "MISSING - run tools\\extract_assets.ps1", d.c_str());
}

// ---- menu model -----------------------------------------------------------------------------------------------
enum ItemId { I_RECENTRE, I_DISTANCE, I_SIZE, I_HEIGHT, I_ENV, I_FLOOR, I_STEREO, I_PARALLAX, I_RES, I_DOF, I_LIFT, I_ZOOM, I_MARKERS, I_RESET, I_CLOSE, I_COUNT };
static const char* kLabels[I_COUNT] = { "Recentre View", "Screen Distance", "Screen Size", "Screen Height", "Surroundings", "Surroundings Floor", "3D Strength",
                                        "Head Parallax", "Resolution Scale", "Depth of Field", "Battle Intro Lift", "Battle Zoom", "3D Battle Markers", "Reset to Defaults", "Close" };
static bool IsValue(int i) { return i >= I_DISTANCE && i <= I_MARKERS; }

static std::mutex g_m;
static bool g_open = false;
static int g_sel = 0;
static bool g_dirty = true;
static double g_flash = 0;          // time of the last confirm (brief highlight)

static std::string ValueText(int i, const Settings& s)
{
    char b[64] = "";
    switch (i) {
    case I_DISTANCE: snprintf(b, 64, "%.1f m", s.distance); break;
    case I_SIZE: { float diag = s.width * std::sqrt(1 + 1 / (s.aspect * s.aspect)) / 0.0254f; snprintf(b, 64, "%.1f m  (%.0f\")", s.width, diag); break; }
    case I_HEIGHT: snprintf(b, 64, "%+.2f m", s.height); break;
    case I_ENV: snprintf(b, 64, "%s", kEnvs[std::max(0, std::min(s.environment, kEnvCount - 1))].name); break;
    case I_FLOOR: snprintf(b, 64, "%s", s.envFloor ? (RecentreHeadHeight() > 0 ? "Real floor" : "Real floor (n/a)") : "Seated"); break;
    case I_STEREO: snprintf(b, 64, "%.0f%%", s.stereo * 100); break;
    case I_PARALLAX: snprintf(b, 64, "%.0f%%", s.headMotion * 100); break;
    case I_RES:
        if (std::fabs(s.resolutionScale - ActiveResolutionScale()) > 0.001f) snprintf(b, 64, "%.0f%%  (at next start)", s.resolutionScale * 100);
        else snprintf(b, 64, "%.0f%%  (%d x %d)", s.resolutionScale * 100, (int)std::lround(s.eyeWidth * s.resolutionScale), (int)std::lround(s.eyeWidth / s.aspect * s.resolutionScale));
        break;
    case I_DOF: if (s.dofStrength <= 0) snprintf(b, 64, "Off"); else snprintf(b, 64, "%d%%", s.dofStrength); break;
    case I_LIFT: if (s.battleLift <= 0) snprintf(b, 64, "Off"); else snprintf(b, 64, "%.0f m", s.battleLift); break;
    case I_ZOOM: snprintf(b, 64, "%s", s.battleZoom ? "Follow game" : "Off"); break;
    case I_MARKERS: { static const char* kM[3] = { "Off", "Standard", "Close" }; snprintf(b, 64, "%s", kM[s.markers3D]); break; }
    }
    return b;
}

static void Adjust(int dir)
{
    Settings s = CurrentSettings();
    switch (g_sel) {
    case I_DISTANCE: s.distance = std::round((s.distance + dir * 0.1f) * 10) / 10; break;
    case I_SIZE: s.width = std::round((s.width + dir * 0.1f) * 10) / 10; break;
    case I_HEIGHT: s.height = std::round((s.height + dir * 0.05f) * 100) / 100; break;
    case I_ENV: s.environment = (s.environment + dir + kEnvCount) % kEnvCount; break;
    case I_FLOOR: s.envFloor = !s.envFloor; break;
    case I_STEREO: s.stereo = std::round((s.stereo + dir * 0.1f) * 10) / 10; break;
    case I_PARALLAX: s.headMotion = std::round((s.headMotion + dir * 0.1f) * 10) / 10; break;
    case I_RES: s.resolutionScale = std::round((s.resolutionScale + dir * 0.1f) * 10) / 10; break;
    case I_DOF: s.dofStrength = std::max(0, std::min(100, (s.dofStrength / 10 + dir) * 10)); break;
    case I_LIFT: s.battleLift = std::round(s.battleLift + dir * 1.0f); break;
    case I_ZOOM: s.battleZoom = !s.battleZoom; break;
    case I_MARKERS: s.markers3D = (s.markers3D + dir + 3) % 3; break;
    default: return;
    }
    SetSettings(s);
    g_dirty = true;
}
static void Confirm()
{
    g_flash = GetTickCount64() / 1000.0;
    switch (g_sel) {
    case I_RECENTRE: RequestRecenter(); break;
    case I_DOF: { Settings s = CurrentSettings(); s.dofStrength = s.dofStrength > 0 ? 0 : 100; SetSettings(s); break; }   // off <-> the game's own
    case I_MARKERS: case I_ENV: case I_FLOOR: case I_ZOOM: Adjust(1); break;
    case I_RESET: {
        // keeps the render size (it applies at the next start), the surroundings and the debug switch
        Settings cur = CurrentSettings(), d;
        d.eyeWidth = cur.eyeWidth; d.margin = cur.margin; d.resolutionScale = cur.resolutionScale;
        d.environment = cur.environment; d.envFloor = cur.envFloor; d.passthrough = cur.passthrough;
        SetSettings(d);
        break;
    }
    case I_CLOSE: g_open = false; break;
    }
    g_dirty = true;
}
static void SetOpen(bool open)
{
    if (g_open == open) return;
    g_open = open;
    if (open) g_sel = 0;
    g_dirty = true;
    FfLog("menu %s", open ? "opened" : "closed");
}

// ---- drawing ----------------------------------------------------------------------------------------------
static const int CW = 1024, CH = 975;
static Canvas g_canvas;

static float SmallCaps(Canvas& c, Font& f, float x, float y, const std::string& s, int bigSize, int smallSize, float track, Col col, bool measureOnly)
{
    // the game's menu titles: capitals, the first letter of each word larger
    float x0 = x; bool first = true;
    for (char ch : s) {
        if (ch == ' ') { x += smallSize * 0.45f; first = true; continue; }
        std::string one(1, (char)toupper((unsigned char)ch));
        int size = (first || isupper((unsigned char)ch)) ? bigSize : smallSize;
        x += measureOnly ? f.Measure(one, size) : c.Text(f, size, x, y, one, col);
        x += track; first = false;
    }
    return x - x0 - track;
}

// The System Menu's inner frame (a thin line with concave quarter-circle corners), drawn at the canvas resolution:
// the game's T_CommonInsideFrame is only 128 px and looked soft and smeared when stretched over the whole menu.
// Anti-aliased from the outline's signed distance; t = line width in pixels.
static void InsideFrame(Canvas& c, float x, float y, float w, float h, float r, float t, Col col)
{
    const float cx[4] = { x, x + w, x, x + w }, cy[4] = { y, y, y + h, y + h };
    const int x0 = std::max(0, (int)(x - 2)), x1 = std::min(c.w - 1, (int)(x + w + 2));
    const int y0 = std::max(0, (int)(y - 2)), y1 = std::min(c.h - 1, (int)(y + h + 2));
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            const float in = std::min(std::min(fx - x, x + w - fx), std::min(fy - y, y + h - fy));
            if (in > r + t + 1) { px = std::max(px, (int)(x + w - r - t - 2)); continue; }   // skip the interior of the row
            float sd = -in;                                         // rectangle: negative inside
            for (int k = 0; k < 4; ++k) sd = std::max(sd, r - std::hypot(fx - cx[k], fy - cy[k]));   // minus the corner circles
            const float a = std::min(1.f, std::max(0.f, t * 0.5f + 0.5f - std::fabs(sd)));
            if (a > 0) c.Blend(px, py, col.r * col.a * a, col.g * col.a * a, col.b * col.a * a, col.a * a);
        }
}

static void DrawKey(Canvas& c, const Image& im, float x, float y, float size)
{
    if (!im.Valid()) return;
    float w = size * im.w / (float)im.h;
    c.Blit(im, x, y, w, size);
}

static void Render()
{
    Canvas& c = g_canvas;
    if (c.w != CW) c.Resize(CW, CH);
    c.Clear();
    Settings s = CurrentSettings();
    const Col white = Rgb(255, 255, 255), dim = Rgb(200, 212, 220);
    // window: light outer line, slate-blue panel, the game's inner frame with concave corners
    c.FillRect(4, 4, CW - 8, CH - 8, Rgb(12, 18, 24, 0.55f));
    c.Outline(6, 6, CW - 12, CH - 12, 2, Rgb(226, 232, 236, 0.92f));
    c.VGradient(11, 11, CW - 22, CH - 22, Rgb(55, 78, 93, 0.97f), Rgb(25, 39, 49, 0.97f));
    InsideFrame(c, 34, 34, CW - 68, CH - 68, 22, 1.6f, Rgb(205, 216, 224, 0.6f));
    // title
    const char* title = "VR Settings";
    float tw = SmallCaps(c, g_a.title, 0, 0, title, 62, 48, 5, white, true);
    SmallCaps(c, g_a.title, (CW - tw) / 2, 112, title, 62, 48, 5, white, false);
    if (g_a.line.Valid()) c.Blit(g_a.line, CW / 2 - 300, 134, 600, 3, Rgb(200, 220, 235, 0.7f));
    // items
    const float bx = 178, bw = CW - 2 * 178 + 20, bh = 40, y0 = 156, step = 45;
    for (int i = 0; i < I_COUNT; ++i) {
        float y = y0 + i * step;
        bool sel = i == g_sel;
        c.Blit9(sel ? g_a.barSel : g_a.bar, bx, y, bw, bh, 14, 12, sel ? Col{} : Rgb(255, 255, 255, 0.92f));
        c.Text(g_a.text, 28, bx + 22, y + 31, kLabels[i], sel ? white : dim);
        if (IsValue(i)) {
            std::string v = ValueText(i, s);
            float vw = g_a.text.Measure(v, 28);
            float vx = bx + bw - 60 - vw;
            c.Text(g_a.text, 28, vx, y + 31, v, sel ? white : dim);
            if (sel && g_a.arrow.Valid()) {
                c.Blit(g_a.arrow, vx - 36, y + 8, 26, 26, white);
                c.Blit(g_a.arrow, bx + bw - 48, y + 8, 26, 26, white, 0, 0, -1, -1, true);
            }
        }
        if (sel) c.Blit(g_a.cursor, bx - 60, y - 11, 64, 64);
    }
    // footer: button prompts (gamepad / keyboard)
    float fy = CH - 128;
    if (g_a.line.Valid()) c.Blit(g_a.line, 60, fy - 14, CW - 120, 3, Rgb(200, 220, 235, 0.5f));
    float x = 70; const float ks = 30, ty = fy + 23;
    auto prompt = [&](const Image& pad, const Image& k1, const Image* k2, const char* label) {
        DrawKey(c, pad, x, fy, ks); x += ks * pad.w / std::max(1.f, (float)pad.h) + 6;
        DrawKey(c, k1, x, fy, ks); x += ks * k1.w / std::max(1.f, (float)k1.h) + 2;
        if (k2) { DrawKey(c, *k2, x, fy, ks); x += ks * k2->w / std::max(1.f, (float)k2->h); }
        x += 10; x += c.Text(g_a.text, 24, x, ty, label, dim); x += 36;
    };
    prompt(g_a.keyUD, g_a.kUp, &g_a.kDown, "Select");
    prompt(g_a.keyLR, g_a.kLeft, &g_a.kRight, "Adjust");
    // the game's pad textures are named the other way round: BUTTON_B shows "A" (confirm), BUTTON_A shows "B"
    prompt(g_a.keyB, g_a.kEnter, nullptr, "Confirm");
    prompt(g_a.keyA, g_a.kEsc, nullptr, "Close");
    // how to open/close the menu
    std::string hint = "Menu:";
    float hx = CW - 70 - (g_a.text.Measure(hint, 22) + 8 + 2 * ks + 4 + 12 + g_a.text.Measure("+", 22) + g_a.text.Measure("or F10", 22) + 10);
    hx += c.Text(g_a.text, 22, hx, fy + 64, hint, dim) + 8;
    DrawKey(c, g_a.keyL3, hx, fy + 42, ks); hx += ks + 2;
    hx += c.Text(g_a.text, 22, hx, fy + 64, "+", dim) + 2;
    DrawKey(c, g_a.keyR3, hx, fy + 42, ks); hx += ks + 10;
    c.Text(g_a.text, 22, hx, fy + 64, "or F10", dim);
}

// ---- input ----------------------------------------------------------------------------------------------------
enum Btn { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_OK, B_BACK, B_COUNT };
static std::atomic<bool> g_keyHeld[B_COUNT];
static std::atomic<bool> g_padHeld[B_COUNT];
static std::atomic<int> g_toggleRequests{ 0 };
static std::atomic<long long> g_lastPadTick{ 0 };

static bool GameIsForeground()
{
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static int KeyToBtn(DWORD vk)
{
    switch (vk) {
    case VK_UP: case 'W': return B_UP;
    case VK_DOWN: case 'S': return B_DOWN;
    case VK_LEFT: case 'A': return B_LEFT;
    case VK_RIGHT: case 'D': return B_RIGHT;
    case VK_RETURN: case VK_SPACE: return B_OK;
    case VK_ESCAPE: case VK_BACK: return B_BACK;
    }
    return -1;
}
static HHOOK g_hook = nullptr;
static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && GameIsForeground()) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN, up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (k->vkCode == VK_F12 && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {   // Ctrl+F12: quick recentre
            if (down) RequestRecenter();
            return 1;
        }
        if (k->vkCode == VK_F10) {
            static bool wasDown = false;
            if (down && !wasDown) ++g_toggleRequests;
            wasDown = down;
            return 1;   // F10 never reaches the game
        }
        if (g_open) {
            int b = KeyToBtn(k->vkCode);
            if (b >= 0) g_keyHeld[b] = down ? true : (up ? false : g_keyHeld[b].load());
            bool system = k->vkCode == VK_LMENU || k->vkCode == VK_RMENU || k->vkCode == VK_LWIN || k->vkCode == VK_RWIN || k->vkCode == VK_TAB || (k->flags & LLKHF_ALTDOWN);
            if (!system) return 1;   // the game does not see keys while the menu is open
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}
static DWORD WINAPI HookThread(void*)
{
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, FfModule(), 0);
    FfLog("keyboard hook %s", g_hook ? "installed" : "FAILED");
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

// Called by the UEVR plugin for every XInputGetState. Returns true when it neutralised the state for the game.
extern "C" __declspec(dllexport) bool FfvrMenuXInput(unsigned int result, unsigned int userIndex, void* statePtr)
{
    if (result != ERROR_SUCCESS || !statePtr) return false;
    auto* st = (XINPUT_STATE*)statePtr;
    const WORD b = st->Gamepad.wButtons;
    static bool chordWas = false, suppress = false;
    bool chord = (b & XINPUT_GAMEPAD_LEFT_THUMB) && (b & XINPUT_GAMEPAD_RIGHT_THUMB);
    if (chord && !chordWas) ++g_toggleRequests;
    chordWas = chord;
    g_lastPadTick = (long long)GetTickCount64();
    const float lx = st->Gamepad.sThumbLX / 32767.f, ly = st->Gamepad.sThumbLY / 32767.f;
    g_padHeld[B_UP] = (b & XINPUT_GAMEPAD_DPAD_UP) || ly > 0.6f;
    g_padHeld[B_DOWN] = (b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -0.6f;
    g_padHeld[B_LEFT] = (b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -0.6f;
    g_padHeld[B_RIGHT] = (b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 0.6f;
    g_padHeld[B_OK] = (b & XINPUT_GAMEPAD_A) != 0;
    g_padHeld[B_BACK] = (b & XINPUT_GAMEPAD_B) != 0;
    // While open, and after closing until every input is released, the game sees a neutral pad.
    if (g_open || chord) suppress = true;
    bool idle = b == 0 && std::fabs(lx) < 0.3f && std::fabs(ly) < 0.3f && st->Gamepad.bLeftTrigger < 30 && st->Gamepad.bRightTrigger < 30 &&
                std::abs(st->Gamepad.sThumbRX) < 10000 && std::abs(st->Gamepad.sThumbRY) < 10000;
    if (!g_open && idle) suppress = false;
    if (suppress) { st->Gamepad = {}; return true; }
    (void)userIndex;
    return false;
}

static std::atomic<bool> g_closeRequest{ false };
static bool g_testMode = false;   // tests\menu_xinput_test.exe: no game window
void MenuSessionFocus(bool focused)
{
    if (!focused) g_closeRequest = true;
}

void MenuPoll()
{
    // toggles (F10, L3+R3)
    for (int n = g_toggleRequests.exchange(0); n > 0; --n) { std::lock_guard<std::mutex> lk(g_m); SetOpen(!g_open); }
    // The menu never stays open while the game is not in charge of input: when the runtime's own menu takes the
    // headset (session no longer focused) or the game window loses the desktop focus, it closes, so the gamepad
    // cannot be left blocked by a menu nobody can see or reach.
    static bool wasFg = true;
    bool fg = g_testMode || GameIsForeground();
    if (fg != wasFg) { FfLog("game window %s the desktop focus", fg ? "has" : "lost"); wasFg = fg; }
    if (g_closeRequest.exchange(false) || (!fg && g_open)) {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_open) { FfLog("menu closed: game or headset session lost focus"); g_open = false; g_dirty = true; }
    }
    if (!g_open) {
        for (auto& k : g_keyHeld) k = false;
        return;
    }
    // edges with auto-repeat (0.4 s, then every 0.09 s; slower for confirm/back = no repeat)
    static bool was[B_COUNT];
    static double next[B_COUNT];
    double now = GetTickCount64() / 1000.0;
    bool padFresh = (long long)GetTickCount64() - g_lastPadTick < 500;
    std::lock_guard<std::mutex> lk(g_m);
    for (int i = 0; i < B_COUNT; ++i) {
        bool held = g_keyHeld[i] || (padFresh && g_padHeld[i]);
        bool fire = false;
        if (held && !was[i]) { fire = true; next[i] = now + 0.4; }
        else if (held && i <= B_RIGHT && now >= next[i]) { fire = true; next[i] = now + 0.09; }
        was[i] = held;
        if (!fire) continue;
        switch (i) {
        case B_UP: g_sel = (g_sel + I_COUNT - 1) % I_COUNT; g_dirty = true; break;
        case B_DOWN: g_sel = (g_sel + 1) % I_COUNT; g_dirty = true; break;
        case B_LEFT: Adjust(-1); break;
        case B_RIGHT: Adjust(+1); break;
        case B_OK: Confirm(); break;
        case B_BACK: SetOpen(false); break;
        }
    }
}

extern "C" __declspec(dllexport) void FfvrMenuPollForTest() { g_testMode = true; MenuPoll(); }

// ---- OpenXR resources ---------------------------------------------------------------------------------------
static XrSession g_ses = XR_NULL_HANDLE;
static ID3D12Device* g_dev = nullptr;
static ID3D12CommandQueue* g_q = nullptr;
static XrSwapchain g_sc = XR_NULL_HANDLE;
static std::vector<ID3D12Resource*> g_images;
static bool g_bgra = false, g_haveImage = false, g_failed = false;
static ID3D12Resource* g_upload = nullptr;
static ID3D12CommandAllocator* g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12Fence* g_fence = nullptr;
static UINT64 g_fenceVal = 0;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
static XrCompositionLayerQuad g_quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };

void MenuInit(XrInstance, XrSession s, ID3D12Device* dev, ID3D12CommandQueue* q)
{
    g_ses = s; g_dev = dev; g_q = q; g_haveImage = false; g_failed = false; g_dirty = true;
    EnvInit(s, dev, q);
    static bool hookStarted = false;
    if (!hookStarted) { hookStarted = true; CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr)); }
}

void MenuShutdown()
{
    EnvShutdown();
    if (g_sc) Nx_DestroySwapchain()(g_sc);
    g_sc = XR_NULL_HANDLE; g_images.clear();
    if (g_fence && g_fence->GetCompletedValue() < g_fenceVal) { HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr); g_fence->SetEventOnCompletion(g_fenceVal, e); WaitForSingleObject(e, 1000); CloseHandle(e); }
    if (g_list) g_list->Release(); if (g_alloc) g_alloc->Release(); if (g_upload) g_upload->Release(); if (g_fence) g_fence->Release();
    g_list = nullptr; g_alloc = nullptr; g_upload = nullptr; g_fence = nullptr;
    g_ses = XR_NULL_HANDLE; g_dev = nullptr; g_q = nullptr; g_haveImage = false;
}

static bool CreateResources()
{
    if (g_sc) return true;
    if (g_failed || !g_dev || !g_q) return false;
    g_failed = true;   // until proven otherwise
    uint32_t n = 0;
    Nx_EnumerateSwapchainFormats()(g_ses, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    Nx_EnumerateSwapchainFormats()(g_ses, n, &n, fmts.data());
    int64_t fmt = 0;
    for (int64_t want : { (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB })
        if (!fmt) for (auto f : fmts) if (f == want) { fmt = f; break; }
    if (!fmt) { FfLog("menu: no sRGB 8-bit swapchain format"); return false; }
    g_bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = CW; ci.height = CH; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (Nx_CreateSwapchain()(g_ses, &ci, &g_sc) != XR_SUCCESS) { FfLog("menu: xrCreateSwapchain failed"); g_sc = XR_NULL_HANDLE; return false; }
    Nx_EnumerateSwapchainImages()(g_sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    Nx_EnumerateSwapchainImages()(g_sc, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& i : imgs) g_images.push_back(i.texture);
    D3D12_RESOURCE_DESC rd = g_images[0]->GetDesc();
    UINT rows; UINT64 rowBytes, total;
    g_dev->GetCopyableFootprints(&rd, 0, 1, 0, &g_fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&g_upload)) ||
        FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc)) ||
        FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_list)) ||
        FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence))) {
        FfLog("menu: D3D12 resources failed"); return false;
    }
    g_list->Close();
    g_failed = false;
    FfLog("menu swapchain %dx%d, %s, %u images", CW, CH, g_bgra ? "BGRA sRGB" : "RGBA sRGB", n);
    return true;
}

static bool Upload()
{
    if (g_fence->GetCompletedValue() < g_fenceVal) return false;   // previous upload still in flight: try next frame
    std::vector<uint8_t> px;
    g_canvas.ToRGBA8(px, g_bgra);
    uint8_t* dst = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(g_upload->Map(0, &none, (void**)&dst))) return false;
    for (int y = 0; y < CH; ++y) memcpy(dst + g_fp.Offset + (size_t)y * g_fp.Footprint.RowPitch, px.data() + (size_t)y * CW * 4, (size_t)CW * 4);
    g_upload->Unmap(0, nullptr);
    uint32_t idx = 0;
    if (Nx_AcquireSwapchainImage()(g_sc, nullptr, &idx) != XR_SUCCESS) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
    if (Nx_WaitSwapchainImage()(g_sc, &wi) != XR_SUCCESS) { Nx_ReleaseSwapchainImage()(g_sc, nullptr); return false; }
    g_alloc->Reset(); g_list->Reset(g_alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = g_images[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION d{ g_images[idx], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sloc{ g_upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; sloc.PlacedFootprint = g_fp;
    g_list->CopyTextureRegion(&d, 0, 0, 0, &sloc, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_q->ExecuteCommandLists(1, lists);
    g_q->Signal(g_fence, ++g_fenceVal);
    Nx_ReleaseSwapchainImage()(g_sc, nullptr);
    g_haveImage = true;
    return true;
}

void MenuAddBackground(XrTime t, const P& S, const Screen&, std::vector<const XrCompositionLayerBaseHeader*>& out)
{
    EnvAddLayer(t, S, CurrentSettings().environment, out);
}

void MenuAddOverlay(XrTime, const P& S, const Screen& sc, std::vector<const XrCompositionLayerBaseHeader*>& out)
{
    bool open, dirty;
    { std::lock_guard<std::mutex> lk(g_m); open = g_open; dirty = g_dirty; }
    if (!open) return;
    LoadAssets();
    if (!g_a.ok || !CreateResources()) return;
    if (dirty || !g_haveImage) {
        std::lock_guard<std::mutex> lk(g_m);
        Render();
        if (Upload()) g_dirty = false;
    }
    if (!g_haveImage) return;
    // in front of the viewer, a little below eye level; closer than the screen
    Settings st = CurrentSettings();
    float dz = std::max(0.6f, std::min(1.3f, st.distance - 0.15f));
    const float w = 0.95f;
    g_quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    g_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    g_quad.space = LayerLocalSpace();
    g_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_quad.subImage.swapchain = g_sc;
    g_quad.subImage.imageRect = { { 0, 0 }, { CW, CH } };
    g_quad.pose = ToXr(Compose(S, P{ Q{}, { 0, -0.08f, -dz } }));
    g_quad.size = { w, w * CH / CW };
    out.push_back((const XrCompositionLayerBaseHeader*)&g_quad);
    (void)sc;
}

// Offline preview for design work: renders the menu to a BMP (no VR needed).
extern "C" __declspec(dllexport) bool FfvrMenuPreview(const wchar_t* bmpPath, int selected)
{
    LoadAssets();
    if (!g_a.ok) return false;
    g_sel = selected % I_COUNT;
    Render();
    std::vector<uint8_t> px; g_canvas.ToRGBA8(px, true);
    FILE* f = _wfopen(bmpPath, L"wb");
    if (!f) return false;
    BITMAPFILEHEADER bf{}; BITMAPINFOHEADER bi{};
    bf.bfType = 0x4D42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + CW * CH * 4;
    bi.biSize = sizeof(bi); bi.biWidth = CW; bi.biHeight = -CH; bi.biPlanes = 1; bi.biBitCount = 32;
    fwrite(&bf, sizeof(bf), 1, f); fwrite(&bi, sizeof(bi), 1, f);
    // composite over a mid grey so the preview shows translucency
    for (size_t i = 0; i < px.size(); i += 4) { float a = px[i + 3] / 255.f; for (int c = 0; c < 3; ++c) px[i + c] = (uint8_t)std::min(255.f, px[i + c] + 70 * (1 - a)); px[i + 3] = 255; }
    fwrite(px.data(), px.size(), 1, f);
    fclose(f);
    return true;
}
