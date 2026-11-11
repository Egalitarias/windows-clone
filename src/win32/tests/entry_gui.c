#include <windows.h>

extern int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

// Windows programs: WinMain(hInstance, NULL, command line, SW_SHOWDEFAULT).
void WinMainCRTStartup(void) {
  ExitProcess((UINT)WinMain(GetModuleHandleA(0), 0, GetCommandLineA(), SW_SHOWDEFAULT));
}
