// menu_xinput_test: checks the gamepad path of the VR menu (ffvr_layer.dll!FfvrMenuXInput) without a game.
// L3+R3 must request a toggle and be hidden from the game; after MenuPoll() opens the menu, every pad state is
// neutralised; once closed (B) the pad stays neutral until released, then passes through again.
#include <windows.h>
#include <xinput.h>
#include <cstdio>
typedef bool (*XFn)(unsigned, unsigned, void*);
typedef void (*PollFn)();
int main()
{
    HMODULE m = LoadLibraryW(L"ffvr_layer.dll");
    auto x = (XFn)GetProcAddress(m, "FfvrMenuXInput");
    auto poll = (PollFn)GetProcAddress(m, "FfvrMenuPollForTest");
    if (!x || !poll) { printf("FAIL: exports missing\n"); return 1; }
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok:  " : "FAIL:", what); if (!ok) ++fails; };
    XINPUT_STATE s{};
    s.Gamepad.wButtons = XINPUT_GAMEPAD_A; check(!x(0, 0, &s) && s.Gamepad.wButtons == XINPUT_GAMEPAD_A, "closed menu: A passes through");
    s = {}; x(0, 0, &s);
    s.Gamepad.wButtons = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB; check(x(0, 0, &s) && s.Gamepad.wButtons == 0, "L3+R3 hidden from the game");
    poll(); s = {}; x(0, 0, &s); poll();
    s.Gamepad.wButtons = XINPUT_GAMEPAD_DPAD_DOWN; check(x(0, 0, &s) && s.Gamepad.wButtons == 0, "open menu: D-pad hidden from the game");
    s.Gamepad.sThumbLX = 30000; check(x(0, 0, &s) && s.Gamepad.sThumbLX == 0, "open menu: stick hidden");
    s = {}; x(0, 0, &s); poll();
    s.Gamepad.wButtons = XINPUT_GAMEPAD_B; x(0, 0, &s); poll();   // B closes
    s.Gamepad.wButtons = XINPUT_GAMEPAD_B; check(x(0, 0, &s) && s.Gamepad.wButtons == 0, "closed by B: B still hidden while held");
    s = {}; x(0, 0, &s);
    s.Gamepad.wButtons = XINPUT_GAMEPAD_A; check(!x(0, 0, &s) && s.Gamepad.wButtons == XINPUT_GAMEPAD_A, "after release: input passes through again");
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails ? 1 : 0;
}
