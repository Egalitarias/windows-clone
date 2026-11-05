#include "user.h"

// Prints a line every 500 ms using sleep(), with the time each sleep really took.
int main(void) {
  int i;

  puts("tick: five ticks, sleeping 500 ms between them\r\n");
  for (i = 1; i <= 5; ++i) {
    long slept = sleep_ms(500);

    puts("tick ");
    put_uint((unsigned long)i);
    puts(" (slept ");
    put_uint((unsigned long)slept);
    puts(" ms)\r\n");
  }
  puts("tick: done\r\n");
  return 5;
}
