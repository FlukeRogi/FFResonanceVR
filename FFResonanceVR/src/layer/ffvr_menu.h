#pragma once
// Shared declarations between the window layer and the in-game VR menu / environment.
#include <string>
#include <vector>
#include <algorithm>
#include <d3d12.h>
#include <openxr/openxr.h>
#include "ffvr_math.h"

struct Settings {
    float distance = 2.5f;     // m from the viewer (recentre position) to the screen
    float width = 3.2f;        // m
    float aspect = 16.0f / 9.0f;
    float height = 0.0f;       // m, screen centre above (+) / below (-) eye height at recentre
    float stereo = 1.0f;       // eye-separation scale (1 = your real IPD)
    float headMotion = 1.0f;   // head-movement parallax scale (1 = physically correct window)
    int eyeWidth = 2048;       // per-eye render width of the screen area (height follows the aspect)
    float margin = 0.0f;       // extra rendered border, fraction of the screen size per side (cropped away)
    float resolutionScale = 1.0f;   // UEVR's render resolution scale (applied live by the mod's UEVR plugin)
    int environment = 2;       // 0 = black void, 1.. = scenes from the game's assets (kEnvs; 2 = Castle Courtyard)
    int envFloor = 0;          // surroundings floor: 0 = "Seated" (the scene's own standing eye height; the default), 1 = at your real floor
    float battleLift = 6.0f;   // m: camera lift at the start of the battle-intro sweep (0 = off), see ffvr_camera.lua
    int markers3D = 2;         // battle unit markers in 3D (ffvr_worldui.lua): 0 off, 1 standard, 2 close
    bool battleZoom = true;    // follow the game's camera zoom in battles by moving the camera forward (ffvr_camera.lua)
    int dofStrength = 100;     // the game's depth of field, % of its normal blur size (0 = off), see ffvr_dof.lua
    bool passthrough = false;  // debug: leave UEVR's output untouched
    void Clamp()
    {
        distance = std::max(0.5f, std::min(distance, 20.0f));
        width = std::max(0.5f, std::min(width, 30.0f));
        aspect = std::max(1.0f, std::min(aspect, 3.0f));
        height = std::max(-3.0f, std::min(height, 3.0f));
        stereo = std::max(0.0f, std::min(stereo, 4.0f));
        headMotion = std::max(0.0f, std::min(headMotion, 2.0f));
        eyeWidth = std::max(640, std::min(eyeWidth, 4096));
        margin = std::max(0.0f, std::min(margin, 0.5f));
        resolutionScale = std::max(0.5f, std::min(resolutionScale, 2.0f));
        battleLift = std::max(0.0f, std::min(battleLift, 10.0f));
        markers3D = std::max(0, std::min(markers3D, 2));
        dofStrength = std::max(0, std::min(dofStrength, 100));
        environment = std::max(0, std::min(environment, 8));
        envFloor = envFloor ? 1 : 0;
    }
};
Settings CurrentSettings();
float ActiveResolutionScale();   // the scale UEVR is running with (a change applies at the next start)
void SetSettings(const Settings& s);
void SaveSettings();
void RequestRecenter();
void FfLog(const char* fmt, ...);
std::wstring SettingsPath();

struct Screen { V3 c; float hw, hh; };   // screen centre and half-extents, in the screen frame S
// next-layer functions the menu needs
PFN_xrCreateSwapchain Nx_CreateSwapchain();
PFN_xrDestroySwapchain Nx_DestroySwapchain();
PFN_xrEnumerateSwapchainImages Nx_EnumerateSwapchainImages();
PFN_xrAcquireSwapchainImage Nx_AcquireSwapchainImage();
PFN_xrWaitSwapchainImage Nx_WaitSwapchainImage();
PFN_xrReleaseSwapchainImage Nx_ReleaseSwapchainImage();
PFN_xrLocateViews Nx_LocateViews();
PFN_xrEnumerateSwapchainFormats Nx_EnumerateSwapchainFormats();
XrSpace LayerLocalSpace();
float RecentreHeadHeight();   // m above the real floor (STAGE space) at the last recentre; < 0 if the runtime gives none

// environment backdrop (ffvr_env.cpp): scenes captured from the game's maps, drawn behind the screen
struct EnvInfo { const char* folder; const char* name; };
extern const EnvInfo kEnvs[];   // index = Settings::environment (0 = none)
extern const int kEnvCount;
void EnvInit(XrSession s, ID3D12Device* dev, ID3D12CommandQueue* q);
void EnvShutdown();
void EnvAddLayer(XrTime t, const P& S, int env, std::vector<const XrCompositionLayerBaseHeader*>& out);

// menu / environment (ffvr_menu.cpp)
void MenuInit(XrInstance inst, XrSession s, ID3D12Device* dev, ID3D12CommandQueue* q);
void MenuShutdown();
void MenuPoll();   // once per frame from xrWaitFrame: input, settings
void MenuSessionFocus(bool focused);   // from xrPollEvent: losing focus closes the menu
void MenuAddBackground(XrTime t, const P& S, const Screen& sc, std::vector<const XrCompositionLayerBaseHeader*>& out);
void MenuAddOverlay(XrTime t, const P& S, const Screen& sc, std::vector<const XrCompositionLayerBaseHeader*>& out);
