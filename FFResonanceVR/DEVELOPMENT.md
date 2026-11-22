# FFResonanceVR — technical notes (settings file, how it works, building and testing)

The player-facing guide is README.md.

The game is shown on a large virtual screen floating in front of you. Each eye is rendered by the game
separately, so you get native stereo 3D. Head tracking makes the screen behave like a window into the game world:
move your head and you see around and behind things, as through a real window.

## Playing

Run **`FFResonanceVR.exe`** in this folder. Choose the VR runtime (the active one, e.g. Virtual Desktop, or SteamVR,
which it starts for you if needed) and press **Launch**. It first checks that Steam, the game, the chosen runtime and the
mod's files are installed and says what is missing if not. **Add VR to running game** switches a game you started
flat from Steam to VR. The window's title logo and fonts are read from the game (the `assets` folder, see below).
Progress of the last launch is in `build\ffvr_launcher.log`.

The batch files still work the same way:

* **Virtual Desktop (VDXR)**, or whatever OpenXR runtime is active: run `Play FFResonanceVR (VDXR or active runtime).bat`
* **SteamVR**: start SteamVR first, then run `Play FFResonanceVR (SteamVR).bat`

The launcher starts the game through Steam and adds the VR mod about 10 seconds after the game window appears.
**VR menu:** press **L3 + R3** (both sticks in) on the controller, or **F10** on the keyboard. It is styled with the game's own font and menu artwork. It has Recentre View, Screen Distance, Screen Size, Screen Height, Surroundings, Surroundings Floor, 3D Strength, Head Parallax, Resolution Scale, Depth of Field, Battle Intro Lift, Battle Zoom, 3D Battle Markers and Reset to Defaults. Navigate with the D-pad or left stick (arrow keys or WASD), adjust with left/right, confirm with A (Enter) and close with B (Esc). While the menu is open, the game does not receive your input; the menu closes by itself if the game window or the headset loses focus. **Ctrl+F12** re-centres directly. UEVR's own settings window is still on **Insert**.

The menu's fonts and artwork are read from the installed game into the `assets` folder. `FFResonanceVR.exe` does this
on its first start (log: `build\ffvr_assets.log`); `tools\extract_assets.ps1` does the same by hand. Both use
`build\assetx\assetx.exe`: the CUE4Parse extractor in `tools\assetx`, published self-contained by `build.bat` (no .NET
install needed), with the game's property mappings `build\assetx\FFRS.usmap`. It finds the game through Steam's
library folders and never downloads anything (the assets used need no Oodle).

Settings are in `build\ffvr_settings.ini`. They are re-read while the game runs:

| Setting | Meaning |
|---|---|
| `[Screen] Distance` | metres from you to the screen (default 2.5) |
| `[Screen] Width` | screen width in metres (default 3.2; the height follows `Aspect`) |
| `[Screen] Height` | screen centre above (+) or below (-) your eye height at re-centre |
| `[Environment] Scene` | surroundings around the screen (VR menu "Surroundings"): 0 none (black), 1 Airship Deck, 2 Castle Courtyard (default) |
| `[Environment] Floor` | where the surroundings' floor is (VR menu "Surroundings Floor"): 0 = Seated (the scene's own standing eye height, which feels right when you play seated; default); 1 = Real floor: at your real floor, from your head height at the last re-centre (seated, this puts you low in the scene) |
| `[Stereo] Strength` | eye-separation scale (1 = your real IPD; higher = stronger 3D, the world looks smaller) |
| `[Stereo] HeadMotion` | head-movement parallax scale (1 = a physically correct window) |
| `[Render] EyeWidth` | per-eye render width in pixels (restart needed) |
| `[Render] ResolutionScale` | render resolution scale (also in the VR menu); applies at the next game start |
| `[Game] BattleIntroLift` | metres the camera is lifted at the start of the battle-intro sweep (0 = off; default 6). The window shows more than the game's narrow intro framing, which otherwise reveals the ground under the landscape |
| `[Game] BattleZoom` | follow the game's camera zoom in battles (VR menu "Battle Zoom": Off / Follow game; default 1 = Follow game). The window always shows the same angle of the world, so the game's zooms (battle view, intro, attack close-ups) do nothing in VR; with this on, the camera moves forward along its line of sight until the battle units look as large as the game frames them (a real camera move: depth and scale stay natural). It keeps at least 3 m from the subject |
| `[Game] BattleMarkers3D` | battle markers (HP gauges, letters, target cursor) in the 3D scene: 0 off (flat), 1 Standard (40% of the way to the enemy), 2 Close (90%: right at the enemy; default). They are drawn on top of the scene, so nearer enemies never cut them off |
| `[Game] DepthOfField` | the game's depth-of-field blur in both eyes, as a percentage of its normal strength (VR menu "Depth of Field": Off, 10%–100%; 100 = the game's own look, 0 = off). While it is on, the game runs with TAA instead of its FXAA anti-aliasing (see below) |

## How it works

* **UEVR** (praydog's Universal Unreal Engine VR mod, nightly 01143, in `uevr\`) hooks the engine's stereo
  rendering in native-stereo mode (both eyes in one frame). In this mode the game's depth of field
  together with its FXAA anti-aliasing breaks the second eye (no blur, NaN blocks, and bloom floods it), so the mod
  never lets that combination occur (see Depth of field below); bloom itself is left as the game sets it. Synced
  Sequential mode has no such problem, but it swaps the eyes every few frames, which shows as the picture jumping.
* **`ffvr_layer.dll`** is an OpenXR API layer between UEVR and the runtime. It reports a head that never rotates
  and, for each eye, the off-axis frustum from that eye through the screen rectangle. In `xrEndFrame` it replaces
  UEVR's projection layer with one world-locked quad per eye. The game HUD quad is pinned onto the screen plane.
* **Surroundings** (`src\layer\ffvr_env.cpp`, scenes in `assets\env\<scene>`) are cube maps of real game locations
  (the airship deck, `Town_00Com_03`; the castle courtyard in the royal capital, `Town_01Gra_02`), rendered by the
  game itself. Each face also has a scene-depth capture. The layer turns every face into a mesh pushed out to that
  depth, so the floor, walls and towers have true stereo and head-motion parallax. It draws the mesh per eye from the
  runtime's real eye poses into its own projection layer behind the screen. Where a near object's outline meets the distance, the mesh is cut (no rubber-sheet stretching) and a second, further-out mesh fills the small gap that opens when you move your head. Depth differences are judged as stereo disparity (1/distance: a few centimetres matter at a stone by your feet, metres hardly matter at the castle walls). At outlines the near mesh reaches one 8-pixel cell past the object, so the cut cells hold only the far side (cut cells holding part of the object showed it far away, as stair-stepped fragments in the sky). The background mesh takes the next surface behind an outline (the wall behind a tree, not the sky behind the wall), and each pixel of the near picture is drawn by whichever mesh is nearer its true distance. At load, the anti-aliased pixels along depth edges get the colour of the side they are closer to, and each mesh gets its own picture (`FixFaceLayers`): in the foreground picture, pixels that the near mesh would draw at a near object's distance but that show something further away (the sky sliver the 8-pixel mesh grid cannot follow) are transparent, so the background mesh (drawn first) shows them at their real distance and outlines follow the picture smoothly; in the background picture, near objects are replaced by the colour around them, so the gaps behind outlines show sky or wall. This needs full-resolution depth (`<face>.zd`, made by `tools\pack_depth.ps1`; the courtyard has it, the deck does not and gets only the colour clean-up). Everything is fixed in the pictures at load, so it is as stable as any texture (`FFVR_ENV_MODE=0` turns it off for comparisons; `FFVR_ENV_DUMP=<folder>` writes each face's two pictures, a mask and an error map, all raw RGBA; the error map marks pixels drawn over 2 arcmin of disparity too near (red) or too far (blue) from the capture point). Per-pixel outlines decided in the shader were tried and rejected in the headset: they shimmer with head motion. Each face image reaches a little past its edge, and the shader cross-fades neighbouring faces there, so the seams between the views (and the slightly different shading of the views straight up and down) become soft gradients. The viewer is at the capture point
  (about 1.65–1.7 m above the scene's floor) facing the screen; with "Real floor" the scene is moved up or down so its
  floor is at your real floor (your head height in the runtime's STAGE space at the last re-centre).
* **Depth of field** (`uevr_profile\scripts\ffvr_dof.lua`): under UEVR's native stereo the game's depth of field only reaches the first eye while the game's own anti-aliasing (FXAA) is active; with a temporal method it works in both. TSR costs about a third of the frame rate at the window's resolution; TAA costs nothing measurable and keeps the pixel sprites crisp, so TAA is switched on while depth of field is on and the game's FXAA is put back when it is off. The strength scales every cine camera's aperture (blur size is inversely proportional to the f-stop), tracking the game's own value per camera.
* **`FFResonanceVR.exe`** (`src\app\ffvr_app.cpp`) is the launcher window; it runs `build\ffvr_launch.exe` hidden
  and shows its progress. UEVR is not redistributed (its licence is "All rights reserved"): on first start (or when
  `uevr\revision.txt` doesn't match) the launcher downloads UEVR nightly 01143 from praydog's GitHub releases
  (`praydog/UEVR-nightly`, tag `nightly-01143-4ee5c6b…`), checks its SHA-256, and unpacks UEVRBackend.dll, LuaVR.dll,
  openxr_loader.dll, openvr_api.dll and revision.txt into `uevr\` with Windows' tar. The package's third-party
  licences are in `Licenses\`, made by `tools\make_licenses.ps1` (run by `tools\make_package.ps1`).
* **`ffvr_launch.exe`** injects `ffvr_boot.dll` (per-process OpenXR environment: layer, runtime choice), then UEVR.
  Nothing is registered system-wide, and the game files are not modified.

## Development

`build.bat` builds everything (MSVC 2022 Build Tools; the asset extractor needs the .NET 10 SDK in `..\Tools\dotnet`).
The source repository leaves out everything that comes from the game or from UEVR: the property mappings
`tools\assetx\FFRS.usmap` (dump them from the running game with UE4SS: `build\ffvr_launch.exe --dll <UE4SS.dll>`), the
surroundings captures `assets\env` (make them with `tools\capture_scene.ps1`, or take them from the release zip), and
UEVR (`FFResonanceVR.exe` downloads it on first start). Automated tests run on the virtual HMD (`build\vhmd`, an
OpenXR runtime with D3D11 and D3D12 that needs no headset):

* `tools\test_ingame.ps1 -Name <run>` launches on the virtual HMD, presses Continue and takes snapshots.
* `tools\vhmd_snap.ps1`, `tools\eye_stats.ps1` and `tools\gamecmd.ps1 "<console command>"` help with those runs.
* New surroundings: start the game flat with UE4SS only
  (`build\ffvr_launch.exe --dll work\ue4ss\UE4SS.dll`, with the `FFVRExec` Lua mod, driven by `tools\ue4sslua.ps1`),
  then run `tools\capture_scene.ps1 -Name <scene> -Map <map path> -X -Y -Z -Yaw`. Colour comes from the game's own
  main view (a camera slightly wider than 90° vertically, centre square of each HighResShot: the game's screen-space lighting darkens the screen's top and bottom edges; the "GodRay" light-beam effects, the fountain water (particle effects the depth capture does not see), people, animals, speech bubbles and their shadows are hidden; each face is kept 10% beyond its edge for the cross-fade, one manual exposure calibrated against the
  game's auto exposure). Depth comes from a SceneCapture. `tools\fix_seams.ps1` matches the up and down faces to
  the side faces. `tools\survey_maps.ps1` gives quick overviews of candidate maps. Add the scene to `kEnvs` in
  `ffvr_env.cpp`.
* `build\xrtest.exe uevr\openxr_loader.dll` is a minimal UEVR-like D3D12 OpenXR app for testing the layer without the game.

Original saves are backed up in `save_backup\`. Automated tests only load saves (slots 1-8 are made read-only while tests run).
