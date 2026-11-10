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
#define SYS_WIN_WAIT   21
#define SYS_CLIP_SET   24
#define SYS_VOLINFO    26
#define SYS_SPAWN      27
#define SYS_CLIP_GET   25
#define SYS_WIN_MOVE   22
#define SYS_WIN_RESIZE 23

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

// Volumes: volinfo(i, &d) fills d.name (label or "Disk N"), d.size (megabytes) and
// d.attr for volume i, 0 when there is one, negative past the last. A path starting
// with "/@i" names volume i exactly: "/@1/DOCS/A.TXT"; "/@1" is its root. Without
// the prefix reads search the default volume first and writes go to it.
static inline long volinfo(int index, dirent_t *out) { return syscall3(SYS_VOLINFO, index, (long)out, 0); }
// Start a program (arg may be 0) and carry on; nobody waits for it. Returns 0, or a
// negative error when there is no free slot. A program that does not exist is only
// reported on the kernel console, so stat() the path first.
static inline long spawn(const char *path, const char *arg) { return syscall3(SYS_SPAWN, (long)path, (long)arg, 0); }

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
#define EV_MOUSE_DOUBLE 6 // x, y: right after the EV_MOUSE_DOWN of a double click

// Non-printing keys arrive as EV_KEY with these values in ch.
#define KEY_LEFT   0x1C
#define KEY_RIGHT  0x1D
#define KEY_UP     0x1E
#define KEY_DOWN   0x1F
#define KEY_DELETE 0x7F
#define KEY_HOME   0x80
#define KEY_END    0x81
#define KEY_PGUP   0x82
#define KEY_PGDN   0x83
// For EV_KEY, win_event_t.x holds the modifiers that were down:
#define MOD_SHIFT  0x1
#define MOD_CTRL   0x2
#define MOD_ALT    0x4
// Ctrl+letter arrives as the usual control code (Ctrl+S = 0x13).

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
// Wait up to timeout_ms for an event: 1 = stored in *ev, 0 = timed out (0 ms just polls),
// negative = error (window gone). Use it to animate or blink a cursor without a thread.
static inline long win_wait(int id, win_event_t *ev, unsigned long timeout_ms) { return syscall3(SYS_WIN_WAIT, id, (long)ev, (long)timeout_ms); }
// Move the window so its top-left corner is at (x, y) on the screen (it stays reachable).
static inline long win_move(int id, int x, int y) { return syscall3(SYS_WIN_MOVE, id, x, y); }
// Change the client size (16..800 x 16..600). The surface comes back cleared: redraw.
static inline long win_resize(int id, int w, int h) { return syscall3(SYS_WIN_RESIZE, id, w, h); }

// The argument string the program was started with (empty if none). Returns its length.
static inline long getarg(char *buf, unsigned long len) { return syscall3(SYS_GETARG, (long)buf, (long)len, 0); }
static inline long win_title(int id, const char *title) { return syscall3(SYS_WIN_TITLE, id, (long)title, 0); }

// System clipboard, shared by all programs: up to 4096 bytes of plain text.
// clip_set replaces it (0 or a negative error); clip_get copies up to max bytes and
// returns how many.
static inline long clip_set(const void *buf, unsigned long len) { return syscall3(SYS_CLIP_SET, (long)buf, (long)len, 0); }
static inline long clip_get(void *buf, unsigned long max) { return syscall3(SYS_CLIP_GET, (long)buf, (long)max, 0); }

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
