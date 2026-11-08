#include "syswin.h"
#include "fb.h"
#include "font8x8.h"
#include "irqlock.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "syscall.h"
#include "timer.h"
#include "wm.h"

#define UW_FREE    0
#define UW_PENDING 1      // allocated by a syscall; the desktop must create the window
#define UW_ACTIVE  2
#define UW_DEAD    3      // the user closed the window; the program has not noticed yet
#define UW_CLOSING 4      // released by the program or its exit; the desktop cleans up
#define UW_RESERVED 5     // slot taken by a syscall that is still setting it up

#define EV_RING 32U
#define CREATE_TIMEOUT_MS 3000U

typedef struct {
  volatile int state;
  uint64_t owner;                       // pid of the creating program, 0 once released
  wm_window_t *win;                     // desktop task only
  char title[WM_TITLE_CHARS];
  char new_title[WM_TITLE_CHARS];
  volatile int title_dirty;
  int w, h;                             // surface (= client area) size
  uint64_t surf_phys;
  uint64_t surf_pages;
  uint32_t *surf;
  volatile int dirty;
  syswin_event_t ev[EV_RING];
  volatile uint32_t ev_head;
  volatile uint32_t ev_tail;
} uwin_t;

static uwin_t g_uw[SYSWIN_MAX];

static int cas_state(uwin_t *u, int from, int to) {
  return __sync_bool_compare_and_swap(&u->state, from, to);
}

// ---- events (pushed by the desktop task, popped by the program) ---------------

static void ev_push(uwin_t *u, uint32_t type, int x, int y, uint32_t ch) {
  uint64_t f = irq_save();

  if (u->ev_tail - u->ev_head < EV_RING) {
    syswin_event_t *e = &u->ev[u->ev_tail % EV_RING];
    e->type = type;
    e->x = x;
    e->y = y;
    e->ch = ch;
    u->ev_tail = u->ev_tail + 1;
  } else if (type == SYSWIN_EV_CLOSE) {
    syswin_event_t *e = &u->ev[(u->ev_tail - 1) % EV_RING];   // never lose the close
    e->type = type;
    e->x = e->y = 0;
    e->ch = 0;
  }
  irq_restore(f);
  scheduler_wake_all(TASK_WAIT_INPUT);
}

static int ev_pop(uwin_t *u, syswin_event_t *out) {
  uint64_t f = irq_save();
  int got = 0;

  if (u->ev_head != u->ev_tail) {
    *out = u->ev[u->ev_head % EV_RING];
    u->ev_head = u->ev_head + 1;
    got = 1;
  }
  irq_restore(f);
  return got;
}

// ---- window callbacks (desktop task) -------------------------------------------

static void uw_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  uwin_t *u = (uwin_t *)win->data;

  if (u->state == UW_ACTIVE && u->surf != 0) {
    fb_blit(cx, cy, cw < u->w ? cw : u->w, ch < u->h ? ch : u->h, u->surf, u->w);
  }
}

static void uw_key(wm_window_t *win, char c) {
  uwin_t *u = (uwin_t *)win->data;

  if (u->state == UW_ACTIVE) {
    ev_push(u, SYSWIN_EV_KEY, 0, 0, (uint32_t)(unsigned char)c);
  }
}

static void uw_mouse(wm_window_t *win, int lx, int ly, int event) {
  uwin_t *u = (uwin_t *)win->data;
  uint32_t type;

  if (u->state != UW_ACTIVE) {
    return;
  }
  if (event == WM_MOUSE_DOWN) {
    type = SYSWIN_EV_MOUSE_DOWN;
  } else if (event == WM_MOUSE_MOVE) {
    type = SYSWIN_EV_MOUSE_MOVE;
  } else if (event == WM_MOUSE_UP) {
    type = SYSWIN_EV_MOUSE_UP;
  } else {
    return;
  }
  ev_push(u, type, lx, ly, 0);
}

static void uw_closing(wm_window_t *win) {
  uwin_t *u = (uwin_t *)win->data;

  u->win = 0;
  // Closed by the user (the X button): tell the program. If the program released
  // the window itself (CLOSING) there is nobody to tell.
  if (cas_state(u, UW_ACTIVE, UW_DEAD)) {
    ev_push(u, SYSWIN_EV_CLOSE, 0, 0, 0);
  }
}

// ---- desktop side ----------------------------------------------------------------

static void release_surface(uwin_t *u) {
  uint64_t i;

  if (u->surf_phys != 0) {
    for (i = 0; i < u->surf_pages; ++i) {
      pmm_free_page(u->surf_phys + i * 4096ULL);
    }
  }
  u->surf_phys = 0;
  u->surf_pages = 0;
  u->surf = 0;
}

void syswin_poll(void) {
  int i;

  for (i = 0; i < SYSWIN_MAX; ++i) {
    uwin_t *u = &g_uw[i];
    int st = u->state;

    if (st == UW_PENDING) {
      wm_window_t *win = wm_create(u->title, 180 + 30 * i, 80 + 30 * i, u->w + 2, u->h + WM_TITLE_H + 1);

      if (win != 0) {
        win->data = u;
        win->paint = uw_paint;
        win->key = uw_key;
        win->mouse = uw_mouse;
        win->closing = uw_closing;
        u->win = win;
      }
      if (win == 0) {
        cas_state(u, UW_PENDING, UW_CLOSING);       // the creator sees ENOMEM-like failure
      } else if (!cas_state(u, UW_PENDING, UW_ACTIVE)) {
        wm_close(win);                              // the program vanished meanwhile
      }
    } else if (st == UW_ACTIVE) {
      if (u->title_dirty && u->win != 0) {
        u->title_dirty = 0;
        wm_set_title(u->win, u->new_title);
      }
      if (u->dirty && u->win != 0) {
        u->dirty = 0;
        wm_invalidate_window(u->win);
      }
    }
    if (u->state == UW_CLOSING) {
      if (u->win != 0) {
        wm_close(u->win);
      }
      u->win = 0;
      release_surface(u);
      u->state = UW_FREE;
    }
  }
}

// ---- syscall side ------------------------------------------------------------------

static uwin_t *get_win(uint64_t id, int need_active) {
  uwin_t *u;
  int st;

  if (id >= SYSWIN_MAX) {
    return 0;
  }
  u = &g_uw[id];
  st = u->state;
  if (u->owner == 0 || u->owner != process_current_pid()) {
    return 0;
  }
  if (need_active ? st != UW_ACTIVE : (st != UW_ACTIVE && st != UW_DEAD)) {
    return 0;
  }
  return u;
}

static void wait_ms(uint64_t ms) {
  uint64_t until = timer_get_ms() + ms;

  if (scheduler_is_running()) {
    scheduler_block_current(TASK_WAIT_SLEEP, until);
  }
  __asm__ volatile("sti; hlt; cli" : : : "memory");
  scheduler_wait_end();
}

int64_t syswin_create(uint64_t title_uaddr, uint64_t w, uint64_t h) {
  char title[WM_TITLE_CHARS];
  uint64_t pid = process_current_pid();
  uwin_t *u = 0;
  uint64_t bytes, start;
  int owned = 0;
  int i;
  int64_t r;

  if (pid == 0) {
    return SYSCALL_ENODEV;
  }
  if (w < SYSWIN_MIN_SIZE || h < SYSWIN_MIN_SIZE || w > SYSWIN_MAX_W || h > SYSWIN_MAX_H) {
    return SYSCALL_EINVAL;
  }
  r = sys_copy_user_string(title_uaddr, title, sizeof(title));
  if (r == SYSCALL_ENAMETOOLONG) {
    title[sizeof(title) - 1] = 0;                   // long titles are cut
  } else if (r < 0) {
    return r;
  }

  for (i = 0; i < SYSWIN_MAX; ++i) {
    if (g_uw[i].state != UW_FREE && g_uw[i].owner == pid) {
      ++owned;
    }
  }
  if (owned >= SYSWIN_MAX_PER_PROC) {
    return SYSCALL_EMFILE;
  }
  for (i = 0; i < SYSWIN_MAX; ++i) {
    if (g_uw[i].state == UW_FREE && cas_state(&g_uw[i], UW_FREE, UW_RESERVED)) {
      u = &g_uw[i];
      break;
    }
  }
  if (u == 0) {
    return SYSCALL_ENOMEM;
  }

  bytes = w * h * 4ULL;
  u->surf_pages = (bytes + 4095ULL) / 4096ULL;
  u->surf_phys = pmm_alloc_pages(u->surf_pages);
  if (u->surf_phys == 0 || !paging_map_range(u->surf_phys, u->surf_pages * 4096ULL)) {
    release_surface(u);
    u->state = UW_FREE;
    return SYSCALL_ENOMEM;
  }
  memset((void *)(uintptr_t)u->surf_phys, 0, (size_t)(u->surf_pages * 4096ULL));
  u->surf = (uint32_t *)(uintptr_t)u->surf_phys;
  u->w = (int)w;
  u->h = (int)h;
  u->win = 0;
  u->dirty = 0;
  u->title_dirty = 0;
  u->ev_head = u->ev_tail = 0;
  str_copy(u->title, title, sizeof(u->title));
  u->owner = pid;
  u->state = UW_PENDING;                            // hand over to the desktop task

  // The desktop task creates the window. Wait for it, but not forever: without a
  // running desktop nothing would ever answer.
  start = timer_get_ms();
  while (u->state == UW_PENDING && timer_get_ms() - start < CREATE_TIMEOUT_MS) {
    wait_ms(10);
  }
  if (u->state == UW_ACTIVE || u->state == UW_DEAD) {          // dead = closed again already
    return (int64_t)(u - g_uw);
  }
  cas_state(u, UW_PENDING, UW_CLOSING);             // the desktop frees it (owner stays for process exit)
  return SYSCALL_ENODEV;
}

int64_t syswin_close(uint64_t id) {
  uwin_t *u = get_win(id, 0);

  if (u == 0) {
    return SYSCALL_EBADF;
  }
  u->owner = 0;
  u->state = UW_CLOSING;                            // the desktop closes and frees it
  return 0;
}

int64_t syswin_title(uint64_t id, uint64_t title_uaddr) {
  uwin_t *u = get_win(id, 1);
  char title[WM_TITLE_CHARS];
  int64_t r;

  if (u == 0) {
    return SYSCALL_EBADF;
  }
  r = sys_copy_user_string(title_uaddr, title, sizeof(title));
  if (r == SYSCALL_ENAMETOOLONG) {
    title[sizeof(title) - 1] = 0;
  } else if (r < 0) {
    return r;
  }
  str_copy(u->new_title, title, sizeof(u->new_title));
  u->title_dirty = 1;                               // the desktop applies it
  return 0;
}

void syswin_process_exit(uint64_t pid) {
  int i;

  if (pid == 0) {
    return;
  }
  for (i = 0; i < SYSWIN_MAX; ++i) {
    uwin_t *u = &g_uw[i];

    if (u->owner == pid && u->state != UW_FREE) {
      u->owner = 0;
      u->state = UW_CLOSING;
    }
  }
}

// Clip a drawing request to the surface. Returns 0 if nothing is left.
static int clip_op(const uwin_t *u, const syswin_op_t *op, int *x0, int *y0, int *x1, int *y1) {
  int64_t ax = op->x, ay = op->y;
  int64_t bx = ax + op->w, by = ay + op->h;

  if (op->w <= 0 || op->h <= 0) {
    return 0;
  }
  *x0 = (int)(ax < 0 ? 0 : ax);
  *y0 = (int)(ay < 0 ? 0 : ay);
  *x1 = (int)(bx > u->w ? u->w : bx);
  *y1 = (int)(by > u->h ? u->h : by);
  return *x1 > *x0 && *y1 > *y0;
}

static int64_t fetch_op(uint64_t uaddr, syswin_op_t *op) {
  if (!paging_user_range_ok(uaddr, sizeof(*op))) {
    return SYSCALL_EFAULT;
  }
  memcpy(op, (const void *)(uintptr_t)uaddr, sizeof(*op));
  return 0;
}

int64_t syswin_blit(uint64_t id, uint64_t op_uaddr) {
  uwin_t *u = get_win(id, 1);
  syswin_op_t op;
  int x0, y0, x1, y1, y;
  int64_t r;

  if (u == 0) {
    return SYSCALL_EBADF;
  }
  r = fetch_op(op_uaddr, &op);
  if (r < 0) {
    return r;
  }
  if (op.w > 4096 || op.h > 4096) {
    return SYSCALL_EINVAL;
  }
  if (op.w > 0 && op.h > 0 && !paging_user_range_ok(op.ptr, (uint64_t)op.w * (uint64_t)op.h * 4ULL)) {
    return SYSCALL_EFAULT;
  }
  if (!clip_op(u, &op, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  for (y = y0; y < y1; ++y) {
    const uint32_t *src = (const uint32_t *)(uintptr_t)op.ptr + (int64_t)(y - op.y) * op.w + (x0 - op.x);

    memcpy(u->surf + (int64_t)y * u->w + x0, src, (size_t)(x1 - x0) * 4U);
  }
  u->dirty = 1;
  return 0;
}

int64_t syswin_fill(uint64_t id, uint64_t op_uaddr) {
  uwin_t *u = get_win(id, 1);
  syswin_op_t op;
  int x0, y0, x1, y1, x, y;
  int64_t r;

  if (u == 0) {
    return SYSCALL_EBADF;
  }
  r = fetch_op(op_uaddr, &op);
  if (r < 0) {
    return r;
  }
  if (!clip_op(u, &op, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  for (y = y0; y < y1; ++y) {
    uint32_t *row = u->surf + (int64_t)y * u->w;

    for (x = x0; x < x1; ++x) {
      row[x] = op.color & 0x00FFFFFFu;
    }
  }
  u->dirty = 1;
  return 0;
}

int64_t syswin_text(uint64_t id, uint64_t op_uaddr) {
  uwin_t *u = get_win(id, 1);
  syswin_op_t op;
  char buf[256];
  int64_t r;
  int scale, cx, i;

  if (u == 0) {
    return SYSCALL_EBADF;
  }
  r = fetch_op(op_uaddr, &op);
  if (r < 0) {
    return r;
  }
  r = sys_copy_user_string(op.ptr, buf, sizeof(buf));
  if (r == SYSCALL_ENAMETOOLONG) {
    buf[sizeof(buf) - 1] = 0;
  } else if (r < 0) {
    return r;
  }
  scale = op.scale < 1 ? 1 : (op.scale > 8 ? 8 : (int)op.scale);
  cx = op.x;
  for (i = 0; buf[i] != 0; ++i) {
    unsigned char c = (unsigned char)buf[i];
    const unsigned char *glyph;
    int row, col;

    if (c < 0x20 || c > 0x7E) {
      c = '?';
    }
    glyph = g_font8x8[c - 0x20];
    for (row = 0; row < 8; ++row) {
      for (col = 0; col < 8; ++col) {
        int px, py;

        if (!(glyph[row] & (1u << col))) {
          continue;
        }
        for (py = 0; py < scale; ++py) {
          for (px = 0; px < scale; ++px) {
            int sx = cx + col * scale + px;
            int sy = op.y + row * scale + py;

            if (sx >= 0 && sy >= 0 && sx < u->w && sy < u->h) {
              u->surf[(int64_t)sy * u->w + sx] = op.color & 0x00FFFFFFu;
            }
          }
        }
      }
    }
    cx += 8 * scale;
  }
  u->dirty = 1;
  return 0;
}

int64_t syswin_event(uint64_t id, uint64_t out_uaddr, uint64_t block) {
  uwin_t *u = get_win(id, 0);
  syswin_event_t ev;

  if (u == 0) {
    return SYSCALL_EBADF;
  }
  if (!paging_user_range_ok(out_uaddr, sizeof(ev))) {
    return SYSCALL_EFAULT;
  }
  for (;;) {
    if (ev_pop(u, &ev)) {
      memcpy((void *)(uintptr_t)out_uaddr, &ev, sizeof(ev));
      scheduler_wait_end();
      return 1;
    }
    if (!block) {
      return 0;
    }
    if (u->state != UW_ACTIVE || !scheduler_is_running()) {
      scheduler_wait_end();
      return SYSCALL_EBADF;                         // closed, and nothing is left to read
    }
    // Same pattern as read(0): leave the run queue until the next event. Interrupts
    // are off, so nothing can be pushed between the check and the block.
    scheduler_block_current(TASK_WAIT_INPUT, 0);
    __asm__ volatile("sti; hlt; cli" : : : "memory");
  }
}

#ifdef KERNEL_USER_SELFTEST
int syswin_test_active(void) {
  int i, n = 0;

  for (i = 0; i < SYSWIN_MAX; ++i) {
    if (g_uw[i].state == UW_ACTIVE) {
      ++n;
    }
  }
  return n;
}

int syswin_test_used(void) {
  int i, n = 0;

  for (i = 0; i < SYSWIN_MAX; ++i) {
    if (g_uw[i].state != UW_FREE) {
      ++n;
    }
  }
  return n;
}

// Deliver an event to every open window, as the window manager's callbacks would.
// A CLOSE also marks the window dead, as closing it with the mouse does.
void syswin_test_inject(uint32_t type, int x, int y, uint32_t ch) {
  int i;

  for (i = 0; i < SYSWIN_MAX; ++i) {
    uwin_t *u = &g_uw[i];

    if (u->state != UW_ACTIVE) {
      continue;
    }
    if (type == SYSWIN_EV_CLOSE) {
      cas_state(u, UW_ACTIVE, UW_DEAD);
    }
    ev_push(u, type, x, y, ch);
  }
}
#endif
