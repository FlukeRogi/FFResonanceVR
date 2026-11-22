// menu_preview: renders the VR menu to BMP files (one per selected item) without VR.
//   menu_preview.exe <out_prefix> [selected]
#include <windows.h>
#include <cstdio>
#include <string>
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: menu_preview <out_prefix> [selected]\n"); return 2; }
    HMODULE m = LoadLibraryW(L"ffvr_layer.dll");
    if (!m) { printf("cannot load ffvr_layer.dll\n"); return 1; }
    auto fn = (bool (*)(const wchar_t*, int))GetProcAddress(m, "FfvrMenuPreview");
    int sel = argc > 2 ? _wtoi(argv[2]) : 0;
    std::wstring out = std::wstring(argv[1]) + L".bmp";
    bool ok = fn && fn(out.c_str(), sel);
    wprintf(L"%s %s\n", ok ? L"wrote" : L"FAILED", out.c_str());
    return ok ? 0 : 1;
}
