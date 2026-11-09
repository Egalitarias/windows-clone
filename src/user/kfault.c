#include "user.h"

// USER_SELFTEST=9 only: asks the kernel to fault on purpose inside a syscall
// (SYS_TEST_KFAULT). The argument "1" makes it hold the scheduler's preempt lock
// first. The kernel must kill just this process; if the call ever returns, or the
// kernel dies, the test fails.
int main(void) {
  char arg[8];

  getarg(arg, sizeof(arg));
  puts("kfault: faulting in the kernel\r\n");
  syscall3(SYS_TEST_KFAULT, arg[0] == '1' ? 1 : 0, 0, 0);
  puts("kfault: BUG, syscall returned\r\n");
  return 0;
}
