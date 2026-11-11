// MessageBox test: Yes/No box; the exit code is the button pressed (IDYES = 6, IDNO = 7).
#include <windows.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)inst;
  (void)prev;
  (void)cmd;
  (void)show;
  return MessageBoxA(0, "Install the update now?\nIt takes a moment.", "WinClone", MB_YESNO | MB_ICONQUESTION);
}
