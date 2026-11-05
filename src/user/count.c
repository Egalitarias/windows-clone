#include "user.h"

// Busy loop long enough to span many 1 ms timer ticks, so the scheduler has to
// preempt it in ring 3 (and run other tasks) before it finishes.
int main(void) {
  volatile unsigned long sum = 0;
  unsigned long i;

  puts("count: start\r\n");
  for (i = 0; i < 60000000UL; ++i) {
    sum += i;
  }
  puts("count: done, sum is ");
  put_uint(sum);
  puts("\r\n");
  yield();
  return 9;
}
