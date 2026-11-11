// A tiny C runtime for test programs: no CRT startup, no exceptions, no TLS. The compiler's
// memcpy/memset calls and the stack probe, and the entry points the linker is told to use.
#include <windows.h>

#pragma clang optimize off
void *memcpy(void *d, const void *s, size_t n) {
  void *r = d;

  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return r;
}

void *memset(void *d, int c, size_t n) {
  void *r = d;

  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return r;
}

void *memmove(void *d, const void *s, size_t n) {
  unsigned char *dd = (unsigned char *)d;
  const unsigned char *ss = (const unsigned char *)s;

  if (dd <= ss || dd >= ss + n) {
    return memcpy(d, s, n);
  }
  while (n != 0) {
    --n;
    dd[n] = ss[n];
  }
  return d;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  size_t i;

  for (i = 0; i < n; ++i) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }
  return 0;
}
#pragma clang optimize on

__asm__(".text\n.globl __chkstk\n__chkstk:\n\tret\n");


// The compiler references this when floating point is used (the real CRT defines it).
int _fltused = 1;
