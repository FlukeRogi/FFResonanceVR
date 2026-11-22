@echo off
rem FFResonanceVR build (MSVC 2022 Build Tools, x64)
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
if not exist build\obj mkdir build\obj
if not exist build\vhmd mkdir build\vhmd
set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /Ithird_party /Isrc\layer /Fobuild\obj\

rem ---- launcher + boot (environment) DLL
cl %CFLAGS% src\launcher\ffvr_launch.cpp /link /OUT:build\ffvr_launch.exe user32.lib shell32.lib advapi32.lib || exit /b 1
rem the asset extractor (CUE4Parse), self-contained so players need no .NET; only when missing (slow, ~50 MB)
if not exist build\assetx\assetx.exe (
  "%~dp0..\Tools\dotnet\dotnet.exe" publish tools\assetx\assetx.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true -p:DebugType=none -o build\assetx || exit /b 1
)
rem (FFRS.usmap, the game's property mappings, is made from the running game with UE4SS; not in the source repository)
if exist tools\assetx\FFRS.usmap (copy /y tools\assetx\FFRS.usmap build\assetx\ >nul) else (echo note: tools\assetx\FFRS.usmap missing - the asset extractor needs it)
rem the launcher window, in the mod folder next to the batch files
cl %CFLAGS% /utf-8 src\app\ffvr_app.cpp /link /SUBSYSTEM:WINDOWS /OUT:FFResonanceVR.exe user32.lib gdi32.lib msimg32.lib shell32.lib advapi32.lib dwmapi.lib winhttp.lib bcrypt.lib || exit /b 1
cl %CFLAGS% /LD src\boot\ffvr_boot.cpp /link /OUT:build\ffvr_boot.dll || exit /b 1

rem ---- the window layer (OpenXR API layer)
cl %CFLAGS% /LD src\layer\ffvr_layer.cpp src\layer\ffvr_menu.cpp src\layer\ffvr_env.cpp src\layer\ffvr_canvas.cpp /link /OUT:build\ffvr_layer.dll user32.lib gdi32.lib d3d12.lib dxgi.lib d3dcompiler.lib ole32.lib windowscodecs.lib || exit /b 1
> build\XR_APILAYER_FFVR_window.json echo {"file_format_version": "1.0.0", "api_layer": {"name": "XR_APILAYER_FFVR_window", "library_path": "./ffvr_layer.dll", "api_version": "1.0", "implementation_version": "1", "description": "FFResonanceVR virtual window"}}

rem ---- virtual HMD (OpenXR test runtime, D3D11 + D3D12)
cl %CFLAGS% /LD src\vhmd\vhmd.cpp /link /DEF:src\vhmd\vhmd.def /OUT:build\vhmd\ffvr_vhmd64.dll d3d11.lib d3d12.lib dxgi.lib || exit /b 1
> build\vhmd\ffvr_vhmd64.json echo {"file_format_version": "1.0.0", "runtime": {"name": "FFVR Virtual HMD", "library_path": "./ffvr_vhmd64.dll"}}
del build\*.exp build\*.lib build\vhmd\*.exp build\vhmd\*.lib 2>nul
cl %CFLAGS% tests\xrtest.cpp /link /OUT:build\xrtest.exe d3d12.lib dxgi.lib || exit /b 1
rem ---- UEVR plugin (menu input) and menu preview tool
cl %CFLAGS% /LD src\plugin\ffvr_plugin.cpp /link /OUT:build\ffvr_plugin.dll dbghelp.lib || exit /b 1
if not exist uevr_profile\plugins mkdir uevr_profile\plugins
copy /y build\ffvr_plugin.dll uevr_profile\plugins\ >nul
cl %CFLAGS% tests\menu_preview.cpp /link /OUT:build\menu_preview.exe || exit /b 1
del build\*.exp build\*.lib 2>nul
cl %CFLAGS% tests\menu_xinput_test.cpp /link /OUT:build\menu_xinput_test.exe || exit /b 1
echo BUILD OK

