#include "user.h"

// Isolation check. Every process is loaded at the same virtual addresses; each
// one stores its own pid in .data, .bss and on its stack, sleeps (so the others
// run), and verifies nobody overwrote them. Exit code 0 = intact.
static volatile unsigned long g_data = 0x1234;
static volatile unsigned long g_bss;

int main(void) {
  unsigned long pid = (unsigned long)getpid();
  volatile unsigned long local = pid * 7;
  int i;

  if (g_data != 0x1234 || g_bss != 0) {
    puts("iso: bad initial state\r\n");
    return 2;
  }
  g_data = pid;
  g_bss = pid * 3;
  for (i = 0; i < 20; ++i) {
    sleep_ms(10);
    if (g_data != pid || g_bss != pid * 3 || local != pid * 7) {
      puts("iso: memory was overwritten\r\n");
      return 1;
    }
  }
  puts("iso pid ");
  put_uint(pid);
  puts(" intact\r\n");
  return 0;
}
