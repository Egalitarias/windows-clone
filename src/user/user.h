#ifndef USER_H
#define USER_H

// Minimal user-space runtime: syscall wrappers for WinClone programs.
// Numbers and conventions match src/kernel/syscall.h.

#include <stdint.h>

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_YIELD   2
#define SYS_GETPID  3
#define SYS_READ    4

static inline long syscall3(long nr, long a1, long a2, long a3) {
  long result;
  // SYSCALL clobbers RCX (return RIP) and R11 (RFLAGS).
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                   : "rcx", "r11", "memory");
  return result;
}

static inline void exit(int code) {
  syscall3(SYS_EXIT, code, 0, 0);
  for (;;) {
  }
}

static inline long write(int fd, const void *buf, unsigned long len) {
  return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

// Read from the keyboard (fd 0). Blocks; returns 0 at end of input.
static inline long read(int fd, void *buf, unsigned long len) {
  return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

static inline long yield(void) { return syscall3(SYS_YIELD, 0, 0, 0); }
static inline long getpid(void) { return syscall3(SYS_GETPID, 0, 0, 0); }

static inline unsigned long strlen(const char *s) {
  unsigned long n = 0;
  while (s[n] != 0) {
    ++n;
  }
  return n;
}

static inline void puts(const char *s) { write(1, s, strlen(s)); }

// Print an unsigned decimal number.
static inline void put_uint(unsigned long v) {
  char buf[24];
  int i = 23;
  buf[i] = 0;
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0);
  puts(buf + i);
}

#endif // USER_H
