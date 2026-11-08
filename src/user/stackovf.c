#include "user.h"

// Recurses until the stack runs out. The page below the stack is unmapped, so the
// kernel must kill this process with a page fault and keep running.
__attribute__((noinline)) static unsigned long recurse(unsigned long depth) {
  volatile char pad[256];

  pad[0] = (char)depth;
  pad[255] = (char)(depth >> 8);
  return (depth == (unsigned long)-1 ? 0 : recurse(depth + 1)) + pad[0] + pad[255];
}

int main(void) {
  puts("stackovf: recursing\r\n");
  return (int)recurse(0);
}
