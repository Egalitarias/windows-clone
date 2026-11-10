#ifndef USER_H
#define USER_H

// Minimal user-space runtime: syscall wrappers for WinClone programs.
// Numbers and conventions match src/kernel/syscall.h.

#include <stdint.h>

#define SYS_TEST_KFAULT 100   // USER_SELFTEST=9 only
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_YIELD   2
#define SYS_GETPID  3
#define SYS_READ    4
#define SYS_SLEEP   5
#define SYS_OPEN    6
#define SYS_CLOSE   7
#define SYS_READDIR 8
#define SYS_LSEEK   17
#define SYS_STAT    18
#define SYS_UNLINK  19
#define SYS_MKDIR   20

#define SYS_WIN_CREATE 9
#define SYS_WIN_CLOSE  10
#define SYS_WIN_BLIT   11
#define SYS_WIN_FILL   12
#define SYS_WIN_TEXT   13
#define SYS_WIN_EVENT  14

#define SYS_GETARG    15
#define SYS_WIN_TITLE 16

#define O_RDONLY 0   // file or directory
#define O_WRONLY 1   // create or truncate; the data is stored by close()
#define O_RDWR   2   // read and write; keeps the contents, creates the file if missing
#define O_APPEND 4   // with O_WRONLY or O_RDWR: keep the contents and write at the end

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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

// lseek returns the new position. stat fills a dirent_t (size, attr, name) for any path.
// unlink removes a file or an empty directory; mkdir makes a directory. Both work on
// the default volume. All return 0 or a negative error (-17 EEXIST, -39 ENOTEMPTY).
static inline long lseek(int fd, long offset, int whence) { return syscall3(SYS_LSEEK, fd, offset, whence); }
static inline long stat(const char *path, dirent_t *out) { return syscall3(SYS_STAT, (long)path, (long)out, 0); }
static inline long unlink(const char *path) { return syscall3(SYS_UNLINK, (long)path, 0, 0); }
static inline long mkdir(const char *path) { return syscall3(SYS_MKDIR, (long)path, 0, 0); }

// Windows. win_create makes a window whose client area is w x h pixels and returns
// its id (or a negative error). You draw into a pixel surface (0x00RRGGBB) with
// win_blit / win_fill / win_text; the desktop shows changes shortly afterwards.
// Input arrives as events. Mouse positions are relative to the client area.
#define EV_KEY        1   // ch = character (backspace, newline or printable)
#define EV_MOUSE_DOWN 2
#define EV_MOUSE_MOVE 3   // only while the button is held
#define EV_MOUSE_UP   4
#define EV_CLOSE      5   // the user closed the window; call win_close() and exit

typedef struct {
  uint32_t type;
  int32_t x, y;
  uint32_t ch;
} win_event_t;

// One drawing request; which fields count depends on the call:
//   blit: x, y, w, h, ptr = w*h pixels (rows packed, no padding)
//   fill: x, y, w, h, color
//   text: x, y, ptr = string, color, scale = 1.. (8x8 font)
typedef struct {
  int32_t x, y, w, h;
  uint32_t color;
  uint32_t scale;
  uint64_t ptr;
} win_op_t;

static inline long win_create(const char *title, int w, int h) { return syscall3(SYS_WIN_CREATE, (long)title, w, h); }
static inline long win_close(int id) { return syscall3(SYS_WIN_CLOSE, id, 0, 0); }
static inline long win_blit(int id, int x, int y, int w, int h, const uint32_t *pixels) {
  win_op_t op = {x, y, w, h, 0, 0, (uint64_t)(unsigned long)pixels};
  return syscall3(SYS_WIN_BLIT, id, (long)&op, 0);
}
static inline long win_fill(int id, int x, int y, int w, int h, uint32_t color) {
  win_op_t op = {x, y, w, h, color, 0, 0};
  return syscall3(SYS_WIN_FILL, id, (long)&op, 0);
}
static inline long win_text(int id, int x, int y, const char *s, uint32_t color, int scale) {
  win_op_t op = {x, y, 0, 0, color, (uint32_t)scale, (uint64_t)(unsigned long)s};
  return syscall3(SYS_WIN_TEXT, id, (long)&op, 0);
}
// Next event: 1 = stored in *ev, 0 = none (block = 0), negative = error (window gone).
static inline long win_event(int id, win_event_t *ev, int block) { return syscall3(SYS_WIN_EVENT, id, (long)ev, block); }

// The argument string the program was started with (empty if none). Returns its length.
static inline long getarg(char *buf, unsigned long len) { return syscall3(SYS_GETARG, (long)buf, (long)len, 0); }
static inline long win_title(int id, const char *title) { return syscall3(SYS_WIN_TITLE, id, (long)title, 0); }

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
