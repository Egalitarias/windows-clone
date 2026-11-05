#include "user.h"

// Writes to an unmapped page. The kernel must kill this process (page fault)
// and keep running.
int main(void) {
  puts("crash: about to write to address 0\r\n");
  *(volatile int *)0 = 1;
  puts("crash: this line must never print\r\n");
  return 0;
}
