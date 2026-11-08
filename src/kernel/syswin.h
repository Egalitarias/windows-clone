#ifndef KERNEL_SYSWIN_H
#define KERNEL_SYSWIN_H

#include "console.h"

// Window syscalls (phase 5 of SYSCALL_PLAN.md): a user program creates a window,
// draws into a pixel surface the kernel keeps for it, and reads input events.
//
// The window manager is only ever touched by the desktop task. A syscall asks for
// work (create, close) or marks the surface dirty, and syswin_poll(), called from
// the desktop loop, does it: it creates the window, repaints dirty ones and closes
// the ones a program released or left behind. Input goes the other way: the window
// callbacks (desktop task) push events into the window's queue and wake the program.
//
// Pixels are 0x00RRGGBB. The surface is the window's client area; mouse positions
// are relative to it.

#define SYSWIN_MAX          4          // windows on the system
#define SYSWIN_MAX_PER_PROC 2
#define SYSWIN_MIN_SIZE     16
#define SYSWIN_MAX_W        800
#define SYSWIN_MAX_H        600

// Event types, as stored in syswin_event_t.type. Keep in sync with src/user/user.h.
#define SYSWIN_EV_KEY        1         // ch = character ('\b', '\n' or printable)
#define SYSWIN_EV_MOUSE_DOWN 2         // x, y
#define SYSWIN_EV_MOUSE_MOVE 3         // only while the button is held
#define SYSWIN_EV_MOUSE_UP   4
#define SYSWIN_EV_CLOSE      5         // the user closed the window; call win_close()

typedef struct {
  uint32_t type;
  int x;
  int y;
  uint32_t ch;
} syswin_event_t;

// One drawing request. Which fields count depends on the call:
//   blit: x, y, w, h and ptr (w*h pixels, row after row, no padding)
//   fill: x, y, w, h and color
//   text: x, y, ptr (NUL-terminated string), color, scale (w) (8x8 font)
typedef struct {
  int x, y, w, h;
  uint32_t color;
  uint32_t scale;
  uint64_t ptr;
} syswin_op_t;

// Each returns a non-negative result or a negative SYSCALL_E* code.
int64_t syswin_create(uint64_t title_uaddr, uint64_t w, uint64_t h);   // -> window id
int64_t syswin_close(uint64_t id);
int64_t syswin_blit(uint64_t id, uint64_t op_uaddr);
int64_t syswin_fill(uint64_t id, uint64_t op_uaddr);
int64_t syswin_text(uint64_t id, uint64_t op_uaddr);
// -> 1 event stored, 0 none (non-blocking). With block != 0 waits for one.
int64_t syswin_event(uint64_t id, uint64_t out_uaddr, uint64_t block);

// The program with this pid is gone: release its windows (closed by syswin_poll).
void syswin_process_exit(uint64_t pid);

// Desktop loop hook. Does all window manager work for user windows.
void syswin_poll(void);

// Copy a NUL-terminated string out of user memory, page by page. Returns 0 or a
// negative SYSCALL_E* code (EFAULT, ENAMETOOLONG). (Implemented in sysfile.c.)
int sys_copy_user_string(uint64_t uaddr, char *out, uint64_t max);

#ifdef KERNEL_USER_SELFTEST
int syswin_test_active(void);   // windows the program can use right now
int syswin_test_used(void);     // slots in use in any state
void syswin_test_inject(uint32_t type, int x, int y, uint32_t ch);
#endif

#endif // KERNEL_SYSWIN_H
