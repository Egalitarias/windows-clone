#include "user.h"

// Never calls a syscall inside its loop, so it only stops running when the timer
// preempts it. The other tasks must keep running meanwhile. Exit code 11.
int main(void) {
  volatile unsigned long sum = 0;
  unsigned long i;

  for (i = 0; i < 400000000UL; ++i) {
    sum += i;
  }
  return 11;
}
