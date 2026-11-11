#include <windows.h>

extern int main(void);

// Console programs: main() returns the exit code.
void mainCRTStartup(void) { ExitProcess((UINT)main()); }
