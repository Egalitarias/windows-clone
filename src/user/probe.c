#include "user.h"

// Hands the kernel bad pointers, bad lengths, bad descriptors and unknown syscall
// numbers. Every call must fail cleanly (no fault, no hang). Exit 0 = all failed as
// expected; otherwise the number of the first check that did not.

#define EFAULT  (-14)
#define EBADF   (-9)
#define ENOSYS  (-38)

static char g_buf[64];

static int is(long got, long want) { return got == want; }
static int fails(long got) { return got < 0; }

int main(void) {
  const void *kernel_va = (const void *)0x200000;                 // identity-mapped, supervisor only
  const void *high_va = (const void *)0xFFFF800000000000UL;       // non-canonical-ish / kernel half
  const void *unmapped = (const void *)0x00007F0000800000UL;      // user range, nothing there
  const void *wraps = (const void *)0xFFFFFFFFFFFFFFF0UL;         // ptr + len wraps around
  const void *edge = (const void *)0x00007F0000000FF0UL;          // just below the image: unmapped

  // write(): bad buffers
  if (!is(write(1, 0, 8), EFAULT)) return 1;
  if (!is(write(1, kernel_va, 8), EFAULT)) return 2;
  if (!is(write(1, high_va, 8), EFAULT)) return 3;
  if (!is(write(1, unmapped, 8), EFAULT)) return 4;
  if (!is(write(1, wraps, 64), EFAULT)) return 5;
  if (!fails(write(1, g_buf, (unsigned long)-1))) return 6;       // absurd length
  if (!is(write(1, edge, 0x20), EFAULT)) return 7;              // starts unmapped
  // read(): bad buffers (checked before anything blocks)
  if (!is(read(0, 0, 8), EFAULT)) return 8;
  if (!is(read(0, (void *)kernel_va, 8), EFAULT)) return 9;
  if (!fails(read(0, g_buf, (unsigned long)-1))) return 10;
  // descriptors
  if (!is(write(2 + 40, g_buf, 1), EBADF)) return 11;
  if (!is(close(99), EBADF)) return 12;
  if (!is(close(-1), EBADF)) return 13;
  if (!fails(read(77, g_buf, 8))) return 14;
  // paths
  if (!is(open(0, O_RDONLY), EFAULT)) return 15;
  if (!is(open((const char *)kernel_va, O_RDONLY), EFAULT)) return 16;
  if (!is(open((const char *)unmapped, O_RDONLY), EFAULT)) return 17;
  if (!fails(open("/NO/SUCH/FILE.TXT", O_RDONLY))) return 18;
  if (!fails(open("/APPS/HELLO.BIN", 99))) return 19;             // unknown flags
  if (!fails(readdir(55, 0))) return 20;
  // other calls
  if (!is(getarg(0, 16), EFAULT)) return 21;
  if (!is(getarg((char *)kernel_va, 16), EFAULT)) return 22;
  if (!fails(getarg(g_buf, 0))) return 23;
  if (!fails(win_create((const char *)kernel_va, 100, 100))) return 24;
  if (!fails(win_create("x", 100000, 100000))) return 25;
  if (!fails(win_create("x", -5, 40))) return 26;
  if (!fails(win_fill(3, 0, 0, 4, 4, 0))) return 27;              // no such window
  if (!fails(win_event(3, (win_event_t *)(void *)g_buf, 0))) return 28;
  if (!fails(win_title(3, "t"))) return 29;
  // unknown syscall numbers
  if (!is(syscall3(60, 1, 2, 3), ENOSYS)) return 30;
  if (!is(syscall3(99, 0, 0, 0), ENOSYS)) return 31;
  if (!is(syscall3(0x100000000L, 0, 0, 0), ENOSYS)) return 32;    // must not alias SYS_EXIT
  if (!is(syscall3(-1, 0, 0, 0), ENOSYS)) return 33;
  if (!is(syscall3(0x8000000000000000L, 0, 0, 0), ENOSYS)) return 34;
  // file calls added later: bad paths, buffers and descriptors
  if (!is(stat(0, (dirent_t *)(void *)g_buf), EFAULT)) return 35;
  if (!is(stat("/", (dirent_t *)kernel_va), EFAULT)) return 36;
  if (!is(stat((const char *)kernel_va, (dirent_t *)(void *)g_buf), EFAULT)) return 37;
  if (!is(unlink(0), EFAULT)) return 38;
  if (!is(mkdir((const char *)kernel_va), EFAULT)) return 39;
  if (!fails(unlink("/"))) return 40;
  if (!is(lseek(77, 0, SEEK_SET), EBADF)) return 41;
  if (!fails(lseek(1, 0, SEEK_SET))) return 42;
  if (!fails(open("/APPS/HELLO.BIN", O_RDWR | O_WRONLY))) return 43;
  // window calls added later
  if (!is(win_wait(3, (win_event_t *)(void *)g_buf, 10), EBADF)) return 44;
  if (!fails(win_move(3, 0, 0))) return 45;
  if (!fails(win_resize(3, 100, 100))) return 46;
  if (!fails(win_resize(3, 5, 5))) return 47;
  // The process still works after all that.
  puts("probe: ok\r\n");
  return 0;
}
