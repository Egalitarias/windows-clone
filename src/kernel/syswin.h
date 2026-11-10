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

#define SYSWIN_MAX          8          // windows on the system
#define SYSWIN_MAX_PER_PROC 2
#define SYSWIN_MIN_SIZE     16
#define SYSWIN_MAX_W        800
#define SYSWIN_MAX_H        600

// Event types, as stored in syswin_event_t.type. Keep in sync with src/user/user.h.
#define SYSWIN_EV_KEY        1         // ch = character ('\b', '\n' or printable)
#define SYSWIN_EV_MOUSE_DOWN 2         // x, y; ch = modifier bits (SYSWIN_MOD_*), also for MOVE, UP and DOUBLE
#define SYSWIN_EV_MOUSE_MOVE 3         // only while the button is held
#define SYSWIN_EV_MOUSE_UP   4
#define SYSWIN_EV_CLOSE      5         // the user closed the window; call win_close()
#define SYSWIN_EV_MOUSE_DOUBLE 6       // x, y; sent right after the MOUSE_DOWN of a double click
#define SYSWIN_EV_RESIZE     7         // x, y = the new client size; the surface is new and cleared: redraw

// Non-printing keys arrive as EV_KEY with these codes in ch (same values as PS2_KEY_*).
#define SYSWIN_KEY_LEFT   0x1C
#define SYSWIN_KEY_RIGHT  0x1D
#define SYSWIN_KEY_UP     0x1E
#define SYSWIN_KEY_DOWN   0x1F
#define SYSWIN_KEY_DELETE 0x7F
#define SYSWIN_KEY_HOME   0x80
#define SYSWIN_KEY_END    0x81
#define SYSWIN_KEY_PGUP   0x82
#define SYSWIN_KEY_PGDN   0x83
// For EV_KEY, x holds the modifier bits that were down:
#define SYSWIN_MOD_SHIFT  0x1
#define SYSWIN_MOD_CTRL   0x2
#define SYSWIN_MOD_ALT    0x4

#define SYSWIN_CLIP_MAX   4096          // clipboard size in bytes

#define SYSWIN_WAIT_MAX_MS 3600000ULL

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
int64_t syswin_title(uint64_t id, uint64_t title_uaddr);
int64_t syswin_blit(uint64_t id, uint64_t op_uaddr);
int64_t syswin_fill(uint64_t id, uint64_t op_uaddr);
int64_t syswin_text(uint64_t id, uint64_t op_uaddr);
// -> 1 event stored, 0 none (non-blocking). With block != 0 waits for one.
int64_t syswin_event(uint64_t id, uint64_t out_uaddr, uint64_t block);

// Like syswin_event with block, but gives up after timeout_ms: -> 1 event stored, 0 timed out.
int64_t syswin_wait(uint64_t id, uint64_t out_uaddr, uint64_t timeout_ms);
// Move the window; its top-left corner goes to (x, y) on the screen (kept reachable).
int64_t syswin_move(uint64_t id, int64_t x, int64_t y);
// New client size (same limits as create). The surface is replaced by a cleared one, so
// the program must redraw. Returns once the desktop has applied it.
int64_t syswin_resize(uint64_t id, uint64_t w, uint64_t h);

// System clipboard (plain bytes, shared by all programs). set replaces it; get copies up
// to max bytes and returns how many it copied (the clipboard may be longer).
int64_t syswin_clip_set(uint64_t buf_uaddr, uint64_t len);
int64_t syswin_clip_get(uint64_t buf_uaddr, uint64_t max);

// Let the user resize the window by dragging its edge or maximizing it. The surface
// then follows the window (same limits as create) and the program gets EV_RESIZE with
// the new size and must redraw. Without this the window frame still resizes, but the
// surface keeps its size.
int64_t syswin_resizable(uint64_t id, uint64_t enable);

// Windows still available to programs, and (desktop task only) bring the windows that
// program `pid` owns to the front, restoring them if they were minimized.
int syswin_free_slots(void);
void syswin_raise_pid(uint64_t pid);

// The program with this pid is gone: release its windows (closed by syswin_poll).
void syswin_process_exit(uint64_t pid);

// Desktop loop hook. Does all window manager work for user windows.
void syswin_poll(void);

// Copy a NUL-terminated string out of user memory, page by page. Returns 0 or a
// negative SYSCALL_E* code (EFAULT, ENAMETOOLONG). (Implemented in sysfile.c.)
int sys_copy_user_string(uint64_t uaddr, char *out, uint64_t max);

#ifdef KERNEL_USER_SELFTEST
// Ask the desktop task to resize every resizable window as if the user had dragged its
// edge to this client size; syswin_test_size reports the first window's surface size.
void syswin_test_resize(int client_w, int client_h);
int syswin_test_size(int *w, int *h);
int syswin_test_pixel(int x, int y);         // 0x00RRGGBB of a pixel of the first open window, -1 if none
int syswin_test_count(uint32_t color);       // pixels of that colour in the first open window
int syswin_test_active(void);   // windows the program can use right now
int syswin_test_used(void);     // slots in use in any state
void syswin_test_inject(uint32_t type, int x, int y, uint32_t ch);
#endif

#endif // KERNEL_SYSWIN_H
