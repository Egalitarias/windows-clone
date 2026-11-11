// Floating point test, built WITH SSE (unlike the rest of the runtime): the same long
// double computation twice must give bit-identical results even though other programs doing
// the same run in between (the kernel keeps each task's SSE registers apart). Exit code 0 = ok.
#include <windows.h>

static double work(double x, int n) {
  int i;

  for (i = 0; i < n; ++i) {
    x = x * 1.0000003 + 0.25 - x / 1000003.0;
  }
  return x;
}

int main(void) {
  char arg[32];
  double seed = 1.5, a, b;
  DWORD n;
  const char *p = GetCommandLineA();

  while (*p != 0 && *p != ' ') {
    ++p;
  }
  while (*p == ' ') {
    ++p;
  }
  if (*p != 0) {
    seed += (double)*p;                       // the program's argument makes each copy different
  }
  a = work(seed, 4000000);
  b = work(seed, 4000000);
  wsprintfA(arg, "fp: %d %d\r\n", (int)a, (int)b);
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), arg, (DWORD)lstrlenA(arg), &n, 0);
  return a == b ? 0 : 1;
}
