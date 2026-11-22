// ffvr_boot.dll - injected before openxr_loader.dll. Sets this process's environment from ffvr_boot.env
// (NAME=VALUE lines, written by the launcher next to this DLL), so the OpenXR loader finds the window layer
// (XR_API_LAYER_PATH / XR_ENABLE_API_LAYERS) and, for tests, the virtual HMD (XR_RUNTIME_JSON). This works even
// though Steam restarts the game process (which loses the launcher's environment).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(m);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return TRUE;
    wcscpy(slash + 1, L"ffvr_boot.env");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return TRUE;
    static char buf[16384];
    DWORD n = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    buf[n] = 0;
    for (char* line = buf; *line;) {
        char* end = line;
        while (*end && *end != '\n') ++end;
        char* next = *end ? end + 1 : end;
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        char* eq = strchr(line, '=');
        if (eq && line[0] != ';') {
            *eq = 0;
            wchar_t name[256], val[4096];
            MultiByteToWideChar(CP_UTF8, 0, line, -1, name, 256);
            MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, val, 4096);
            SetEnvironmentVariableW(name, val[0] ? val : nullptr);
        }
        line = next;
    }
    return TRUE;
}
