// Imports a function the runtime does not have; calling it must end the program cleanly.
#include <windows.h>

__declspec(dllimport) void WINAPI NoSuchWin32Function(void);

int main(void) {
  DWORD n;

  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "before\r\n", 8, &n, 0);
  NoSuchWin32Function();
  return 0;
}
