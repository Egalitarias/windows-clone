// Console test: stdout, the heap, the command line, a file round trip. Exit code 7 on success.
#include <windows.h>

static void out(const char *s) {
  DWORD n;

  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)lstrlenA(s), &n, 0);
}

int main(void) {
  char line[128];
  char *p;
  HANDLE f;
  DWORD n;
  int ok = 1;

  out("Hello from a Windows program running on WinClone!\r\n");
  wsprintfA(line, "command line: %s\r\n", GetCommandLineA());
  out(line);

  p = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1000);
  wsprintfA(line, "heap block at %p, first byte %d\r\n", p, p[0]);
  out(line);
  HeapFree(GetProcessHeap(), 0, p);

  f = CreateFileA("C:\\WINTEST.TXT", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (f == INVALID_HANDLE_VALUE) {
    out("CreateFile failed\r\n");
    return 1;
  }
  WriteFile(f, "written by a PE program", 23, &n, 0);
  CloseHandle(f);
  f = CreateFileA("C:\\WINTEST.TXT", GENERIC_READ, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if (f == INVALID_HANDLE_VALUE) {
    out("reopen failed\r\n");
    return 2;
  }
  ok = ReadFile(f, line, 23, &n, 0) && n == 23 && line[0] == 'w' && line[22] == 'm';
  CloseHandle(f);
  DeleteFileA("C:\\WINTEST.TXT");
  out(ok ? "file round trip: ok\r\n" : "file round trip: FAILED\r\n");
  return ok ? 7 : 3;
}
