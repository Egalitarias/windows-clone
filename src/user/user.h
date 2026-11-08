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
#define SYS_SLEEP   5
#define SYS_OPEN    6
#define SYS_CLOSE   7
#define SYS_READDIR 8

#define O_RDONLY 0   // file or directory
#define O_WRONLY 1   // create or truncate; the data is stored by close()

// What readdir() fills in. attr 0x10 = directory.
typedef struct {
  uint32_t size;
  uint32_t attr;
  char name[128];
} dirent_t;

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

// Files. Paths look like "/APPS/HELLO.BIN". open() returns an fd >= 3 or a negative
// error; read() and write() also take these fds. Files are buffered whole: a file
// opened for writing reaches the disk when it is closed, and close() reports failure.
static inline long open(const char *path, int flags) { return syscall3(SYS_OPEN, (long)path, flags, 0); }
static inline long close(int fd) { return syscall3(SYS_CLOSE, fd, 0, 0); }
// Next entry of an open directory: 1 = stored, 0 = no more, negative = error.
static inline long readdir(int fd, dirent_t *out) { return syscall3(SYS_READDIR, fd, (long)out, 0); }

// Sleep for at least `ms` milliseconds; returns the time actually slept.
static inline long sleep_ms(unsigned long ms) { return syscall3(SYS_SLEEP, (long)ms, 0, 0); }

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
