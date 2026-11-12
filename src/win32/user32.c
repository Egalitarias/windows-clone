// user32 for the Win32 runtime: window classes, windows, the message loop, input, timers and
// MessageBox, on top of WinClone's window syscalls (one pixel surface per window, 800 x 600 at
// most, two top-level windows per process). Child windows (WS_CHILD) are the runtime's own: they
// have no surface, they draw on their top-level window's, translated and clipped by the device
// context. The standard controls (BUTTON, STATIC, EDIT, LISTBOX) are in controls.c. See internal.h.
//
// Messages: GetMessage/PeekMessage turn kernel input events into Windows messages (WM_KEYDOWN +
// WM_CHAR + WM_KEYUP for a key press, WM_LBUTTONDOWN/UP/MOUSEMOVE, WM_MOUSEWHEEL, WM_SIZE,
// WM_CLOSE) and generate WM_PAINT for invalidated windows and WM_TIMER for timers. Painting
// goes straight to the window surface (see gdi32.c). Mouse input goes to the child window under
// the pointer (or the one that captured the mouse), keys to the focus window. When a window is
// painted its children are repainted over it (there is no clipping of children out of the parent).
#include "internal.h"

#define MAX_CLASSES 24
#define MAX_WINDOWS 64
#define MAX_TIMERS  8
#define QUEUE       64

typedef struct {
  int used;
  WNDPROC proc;
  char name[48];
  HBRUSH bg;
  UINT style;
  HINSTANCE inst;
  LPCSTR menu;                  // lpszMenuName: the menu a window of the class gets (a resource name or id)
} cls_t;

typedef struct wnd wnd_t;
struct wnd {
  int used;
  int sysid;                    // the kernel's window id (a child's: its top-level window's)
  WNDPROC proc;
  cls_t *cls;
  int x, y;                     // a child: where it is in the parent's client area
  int w, h;                     // client size
  int visible;
  int invalid;                  // WM_PAINT due
  int erase;                    // erase the background in BeginPaint
  RECT inv;                     // what to repaint
  char *text;                   // title or caption (malloc'd)
  DWORD style, exstyle;
  LONG_PTR userdata, id;
  LONG_PTR extra[8];            // cbWndExtra bytes
  HINSTANCE inst;
  int enabled;
  wnd_t *parent, *first, *last, *next, *prev;   // the tree of child windows; z-order is the list order
  wnd_t *focus;                 // top-level: the child with the keyboard focus (0: the window itself)
  void *ctl;                    // state of a standard control
  void *menu;                   // top-level: its menu bar (menu.c), drawn in the top RT_MENU_H pixels of the surface
  int menubar;                  // RT_MENU_H if it has a menu, else 0: the client area starts that far down
};

typedef struct {
  int used;
  HWND hwnd;
  UINT_PTR id;
  UINT ms;
  DWORD next;
  TIMERPROC proc;
} timer_t_;

static cls_t g_cls[MAX_CLASSES];
static wnd_t g_wnd[MAX_WINDOWS];
static timer_t_ g_timer[MAX_TIMERS];
static MSG g_q[QUEUE];
static unsigned char g_qraw[QUEUE];   // 1: a key, 2: a mouse message, still addressed to the top-level window (see resolve_input)
static int g_qh, g_qt;
static wnd_t *g_capture;
static int g_quit, g_quit_code;
static int g_controls_ready;
static int g_lbutton;
static int g_mods;                // SCEV modifier bits of the last key event
static UINT_PTR g_next_timer_id = 1000;

static wnd_t *w_get(HWND h) {
  wnd_t *w = (wnd_t *)h;

  if (w < g_wnd || w >= g_wnd + MAX_WINDOWS || !w->used) {
    return 0;
  }
  return w;
}

static HWND w_hwnd(wnd_t *w) { return (HWND)w; }

static wnd_t *top_of(wnd_t *w) {
  while (w->parent != 0) {
    w = w->parent;
  }
  return w;
}

// Where the window's (0,0) is on its top-level window's surface.
static void origin(const wnd_t *w, int *ox, int *oy) {
  int x = 0, y = 0;

  for (; w->parent != 0; w = w->parent) {
    x += w->x;
    y += w->y;
  }
  *ox = x;
  *oy = y + w->menubar;
}

// The part of the surface the window may draw on: its client area cut by those of its ancestors.
static void clip_of(const wnd_t *w, RECT *r) {
  int ox, oy;

  origin(w, &ox, &oy);
  r->left = ox;
  r->top = oy;
  r->right = ox + w->w;
  r->bottom = oy + w->h;
  if (w->parent != 0) {
    RECT p;

    clip_of(w->parent, &p);
    if (r->left < p.left) r->left = p.left;
    if (r->top < p.top) r->top = p.top;
    if (r->right > p.right) r->right = p.right;
    if (r->bottom > p.bottom) r->bottom = p.bottom;
  }
}

static dc_t *dc_for(wnd_t *w) {
  int ox, oy;
  RECT clip;

  origin(w, &ox, &oy);
  clip_of(w, &clip);
  return gdi_dc_alloc(w->sysid, ox, oy, &clip);
}

static int shown(const wnd_t *w) {
  for (; w != 0; w = w->parent) {
    if (!w->visible) {
      return 0;
    }
  }
  return 1;
}

static void set_text(wnd_t *w, const char *t) {
  size_t n = t != 0 ? rt_strlen(t) : 0;
  char *c = rt_alloc(n + 1);

  if (c == 0) {
    return;
  }
  rt_memcpy(c, t != 0 ? t : "", n);
  c[n] = 0;
  rt_free(w->text);
  w->text = c;
}

// For controls.c: the per-window state and text of a standard control.
void *rt_wnd_ctl(HWND h) { wnd_t *w = w_get(h); return w != 0 ? w->ctl : 0; }
void rt_wnd_set_ctl(HWND h, void *p) { wnd_t *w = w_get(h); if (w != 0) w->ctl = p; }
const char *rt_wnd_text(HWND h) { wnd_t *w = w_get(h); return w != 0 && w->text != 0 ? w->text : ""; }
void rt_wnd_set_text(HWND h, const char *t) { wnd_t *w = w_get(h); if (w != 0) set_text(w, t); }

static int q_push_kind(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int raw) {
  int next = (g_qt + 1) % QUEUE;
  MSG *m;

  if (next == g_qh) {
    return 0;                                   // full: drop
  }
  g_qraw[g_qt] = (unsigned char)raw;
  m = &g_q[g_qt];
  m->hwnd = hwnd;
  m->message = msg;
  m->wParam = wp;
  m->lParam = lp;
  m->time = GetTickCount();
  m->pt.x = m->pt.y = 0;
  g_qt = next;
  return 1;
}

static int q_push(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return q_push_kind(hwnd, msg, wp, lp, 0); }

static int resolve_input(MSG *m, int raw);

// Input is queued addressed to its top-level window and aimed when it is taken from the queue, so
// that a click that gave a control the focus decides where the keys queued after it go.
static int q_pop(MSG *out, int remove) {
  while (g_qh != g_qt) {
    int raw = g_qraw[g_qh];

    *out = g_q[g_qh];
    if (remove) {
      g_qh = (g_qh + 1) % QUEUE;
    }
    if (raw == 0 || resolve_input(out, raw)) {
      return 1;
    }
    if (!remove) {
      g_qh = (g_qh + 1) % QUEUE;                // a message that goes nowhere (a disabled window): drop it
    }
  }
  return 0;
}

static LPARAM mklp(int x, int y) { return (LPARAM)(((unsigned)y & 0xFFFFu) << 16 | ((unsigned)x & 0xFFFFu)); }

// ---- Window classes ---------------------------------------------------------------------------

EXPORT ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *wc) {
  int i;

  if (!g_controls_ready) {
    g_controls_ready = 1;
    rt_controls_register();
  }
  for (i = 0; i < MAX_CLASSES; ++i) {
    if (!g_cls[i].used) {
      cls_t *c = &g_cls[i];

      c->used = 1;
      c->proc = wc->lpfnWndProc;
      rt_copy(c->name, wc->lpszClassName != 0 && ((uintptr_t)wc->lpszClassName >> 16) != 0 ? wc->lpszClassName : "", sizeof(c->name));
      c->bg = wc->hbrBackground;
      c->style = wc->style;
      c->inst = wc->hInstance;
      c->menu = wc->lpszMenuName;
      return (ATOM)(i + 1);
    }
  }
  SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  return 0;
}

EXPORT ATOM WINAPI RegisterClassA(const WNDCLASSA *wc) {
  WNDCLASSEXA ex;

  ex.cbSize = sizeof(ex);
  ex.style = wc->style;
  ex.lpfnWndProc = wc->lpfnWndProc;
  ex.cbClsExtra = wc->cbClsExtra;
  ex.cbWndExtra = wc->cbWndExtra;
  ex.hInstance = wc->hInstance;
  ex.hIcon = wc->hIcon;
  ex.hCursor = wc->hCursor;
  ex.hbrBackground = wc->hbrBackground;
  ex.lpszMenuName = wc->lpszMenuName;
  ex.lpszClassName = wc->lpszClassName;
  ex.hIconSm = 0;
  return RegisterClassExA(&ex);
}

EXPORT BOOL WINAPI UnregisterClassA(LPCSTR name, HINSTANCE inst) {
  int i;

  (void)inst;
  for (i = 0; i < MAX_CLASSES; ++i) {
    if (g_cls[i].used && ((uintptr_t)name >> 16) != 0 && rt_stricmp(g_cls[i].name, name) == 0) {
      g_cls[i].used = 0;
      return TRUE;
    }
  }
  return FALSE;
}

static cls_t *find_class(LPCSTR name) {
  int i;

  if (!g_controls_ready) {
    g_controls_ready = 1;
    rt_controls_register();
  }
  if (((uintptr_t)name >> 16) == 0) {
    i = (int)(uintptr_t)name - 1;
    return i >= 0 && i < MAX_CLASSES && g_cls[i].used ? &g_cls[i] : 0;
  }
  for (i = 0; i < MAX_CLASSES; ++i) {
    if (g_cls[i].used && rt_stricmp(g_cls[i].name, name) == 0) {
      return &g_cls[i];
    }
  }
  return 0;
}

EXPORT HICON WINAPI LoadIconA(HINSTANCE inst, LPCSTR name) { (void)inst; (void)name; return (HICON)(uintptr_t)1; }
EXPORT HCURSOR WINAPI LoadCursorA(HINSTANCE inst, LPCSTR name) { (void)inst; (void)name; return (HCURSOR)(uintptr_t)1; }
EXPORT HCURSOR WINAPI SetCursor(HCURSOR c) { return c; }
EXPORT int WINAPI ShowCursor(BOOL show) { return show ? 1 : 0; }
EXPORT BOOL WINAPI GetCursorPos(LPPOINT p) { p->x = 0; p->y = 0; return TRUE; }
EXPORT BOOL WINAPI SetCursorPos(int x, int y) { (void)x; (void)y; return TRUE; }

// ---- Windows --------------------------------------------------------------------------------------

#define FRAME_W 16                      // outer size minus client size
#define FRAME_H 39

static void clamp_client(int *w, int *h) {
  if (*w < 16) *w = 16;
  if (*h < 16) *h = 16;
  if (*w > 800) *w = 800;
  if (*h > 600) *h = 600;
}

static LRESULT call_proc(wnd_t *w, UINT msg, WPARAM wp, LPARAM lp) {
  return w->proc != 0 ? w->proc(w_hwnd(w), msg, wp, lp) : DefWindowProcA(w_hwnd(w), msg, wp, lp);
}

static void unlink_child(wnd_t *w) {
  wnd_t *p = w->parent;

  if (p == 0) {
    return;
  }
  if (w->prev != 0) w->prev->next = w->next; else p->first = w->next;
  if (w->next != 0) w->next->prev = w->prev; else p->last = w->prev;
  w->prev = w->next = 0;
}

static void invalidate(wnd_t *w, const RECT *r, int erase) {
  RECT whole = {0, 0, w->w, w->h};

  if (r == 0) {
    r = &whole;
  }
  if (r->right <= r->left || r->bottom <= r->top) {
    return;
  }
  if (!w->invalid) {
    w->inv = *r;
  } else {
    if (r->left < w->inv.left) w->inv.left = r->left;
    if (r->top < w->inv.top) w->inv.top = r->top;
    if (r->right > w->inv.right) w->inv.right = r->right;
    if (r->bottom > w->inv.bottom) w->inv.bottom = r->bottom;
  }
  w->invalid = 1;
  if (erase) {
    w->erase = 1;
  }
}

EXPORT HWND WINAPI CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cw, int ch,
                                   HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
  cls_t *c = find_class(cls);
  wnd_t *w = 0;
  int i, cwid, chei, hm;
  sl_t id;
  CREATESTRUCTA cs;
  char t[64];

  if (c == 0) {
    SetLastError(ERROR_CANNOT_FIND_WND_CLASS);
    return 0;
  }
  if ((style & WS_CHILD) && w_get(parent) == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);            // a child needs a parent
    return 0;
  }
  for (i = 0; i < MAX_WINDOWS; ++i) {
    if (!g_wnd[i].used) {
      w = &g_wnd[i];
      break;
    }
  }
  if (w == 0) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  rt_memset(w, 0, sizeof(*w));
  if (!(style & WS_CHILD) && menu == 0 && c->menu != 0) {
    menu = LoadMenuA(inst != 0 ? inst : c->inst, c->menu);
  }
  hm = !(style & WS_CHILD) && rt_menu_valid(menu);
  if (style & WS_CHILD) {
    wnd_t *p = w_get(parent);

    if (cw == CW_USEDEFAULT || cw < 0) cw = 0;
    if (ch == CW_USEDEFAULT || ch < 0) ch = 0;
    cwid = cw;
    chei = ch;
    id = p->sysid;
    w->parent = p;
    w->x = x == CW_USEDEFAULT ? 0 : x;
    w->y = y == CW_USEDEFAULT ? 0 : y;
    w->prev = p->last;                                // on top of its siblings
    if (p->last != 0) p->last->next = w; else p->first = w;
    p->last = w;
  } else {
    if (cw == CW_USEDEFAULT) cw = 640;
    if (ch == CW_USEDEFAULT) ch = 480;
    cwid = cw - FRAME_W;
    chei = ch - FRAME_H - (hm ? RT_MENU_H : 0);
    clamp_client(&cwid, &chei);
    if (hm && chei > 600 - RT_MENU_H) chei = 600 - RT_MENU_H;
    rt_copy(t, title != 0 ? title : "", sizeof(t));
    id = sc3(KS_WIN_CREATE, (sl_t)t, cwid, chei + (hm ? RT_MENU_H : 0));
    if (id < 0) {
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);          // the kernel allows two windows per program
      return 0;
    }
  }
  w->used = 1;
  w->sysid = (int)id;
  w->proc = c->proc;
  w->cls = c;
  w->w = cwid;
  w->h = chei;
  w->style = style;
  w->exstyle = exstyle;
  w->inst = inst;
  w->enabled = (style & WS_DISABLED) == 0;
  w->id = hm ? 0 : (LONG_PTR)menu;
  if (hm) {
    w->menu = menu;
    w->menubar = RT_MENU_H;
  }
  set_text(w, title != 0 && ((uintptr_t)title >> 16) != 0 ? title : "");
  if (!(style & WS_CHILD)) {
    if (style & WS_THICKFRAME) {
      sc3(28, id, 1, 0);                              // win_resizable
    }
    if (x != CW_USEDEFAULT && y != CW_USEDEFAULT) {
      sc3(KS_WIN_MOVE, id, x, y);
    }
  }
  rt_memset(&cs, 0, sizeof(cs));
  cs.lpCreateParams = param;
  cs.hInstance = inst;
  cs.hMenu = menu;
  cs.style = (LONG)style;
  cs.x = x;
  cs.y = y;
  cs.cx = cw;
  cs.cy = ch;
  cs.lpszName = title;
  cs.lpszClass = cls;
  cs.dwExStyle = exstyle;
  cs.hwndParent = parent;
  if (call_proc(w, WM_NCCREATE, 0, (LPARAM)&cs) == 0 || (int)call_proc(w, WM_CREATE, 0, (LPARAM)&cs) == -1) {
    if (w->parent == 0) {
      sc3(KS_WIN_CLOSE, id, 0, 0);
    }
    unlink_child(w);
    rt_free(w->text);
    w->used = 0;
    return 0;
  }
  call_proc(w, WM_SIZE, SIZE_RESTORED, mklp(w->w, w->h));
  if (style & WS_VISIBLE) {
    w->visible = 1;
    invalidate(w, 0, 1);
  }
  return w_hwnd(w);
}

// Repaint what a child leaves behind (its parent shows through) when it hides, moves or goes.
static void invalidate_behind(wnd_t *w, const RECT *r) {
  if (w->parent != 0) {
    invalidate(w->parent, r, 1);
  }
}

EXPORT BOOL WINAPI ShowWindow(HWND h, int cmd) {
  wnd_t *w = w_get(h);
  BOOL was;

  if (w == 0) {
    return FALSE;
  }
  was = w->visible;
  w->visible = cmd != SW_HIDE;
  if (w->visible) {
    invalidate(w, 0, 1);
  } else if (was && w->parent != 0) {
    RECT r = {w->x, w->y, w->x + w->w, w->y + w->h};

    invalidate_behind(w, &r);
  }
  return was;
}

// A window is being painted (its WM_PAINT is about to be delivered): what it draws first covers its
// children, so they are drawn again afterwards.
static void painted(wnd_t *w) {
  wnd_t *c;

  w->invalid = 0;
  if (w->menu != 0) {
    rt_menu_draw_bar(w_hwnd(w));
  }
  for (c = w->first; c != 0; c = c->next) {
    invalidate(c, 0, 1);
  }
}

static void paint_now(wnd_t *w) {
  wnd_t *c;

  if (!w->visible) {
    return;
  }
  if (w->invalid) {
    call_proc(w, WM_PAINT, 0, 0);
    painted(w);
  }
  for (c = w->first; c != 0; c = c->next) {
    paint_now(c);
  }
}

EXPORT BOOL WINAPI UpdateWindow(HWND h) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  paint_now(w);
  return TRUE;
}

static void destroy_tree(wnd_t *w, int notify) {
  wnd_t *top = top_of(w);

  while (w->first != 0) {
    wnd_t *c = w->first;

    if (notify) {
      DestroyWindow(w_hwnd(c));
    } else {
      destroy_tree(c, 0);
      unlink_child(c);
      rt_free(c->text);
      c->used = 0;
    }
    if (w->first == c) {                               // the window procedure refused to die: force it
      unlink_child(c);
      c->used = 0;
    }
  }
  if (g_capture == w) {
    g_capture = 0;
  }
  if (top->focus == w) {
    top->focus = 0;
  }
}

EXPORT BOOL WINAPI DestroyWindow(HWND h) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  destroy_tree(w, 1);
  if (w->parent != 0 && shown(w)) {
    RECT r = {w->x, w->y, w->x + w->w, w->y + w->h};

    invalidate_behind(w, &r);
  }
  call_proc(w, WM_DESTROY, 0, 0);
  call_proc(w, WM_NCDESTROY, 0, 0);
  if (w->used) {
    if (w->parent == 0) {
      sc3(KS_WIN_CLOSE, w->sysid, 0, 0);
    }
    unlink_child(w);
    rt_free(w->text);
    w->text = 0;
    if (w->menu != 0) {
      rt_menu_destroy((HMENU)w->menu);
    }
    w->used = 0;
  }
  return TRUE;
}

EXPORT BOOL WINAPI CloseWindow(HWND h) { (void)h; return TRUE; }
EXPORT BOOL WINAPI IsWindow(HWND h) { return w_get(h) != 0; }
EXPORT BOOL WINAPI IsWindowVisible(HWND h) { wnd_t *w = w_get(h); return w != 0 && shown(w); }
EXPORT BOOL WINAPI IsWindowEnabled(HWND h) {
  wnd_t *w = w_get(h);

  for (; w != 0; w = w->parent) {
    if (!w->enabled) {
      return FALSE;
    }
  }
  return h != 0;
}
EXPORT BOOL WINAPI EnableWindow(HWND h, BOOL e) {
  wnd_t *w = w_get(h);
  BOOL was;

  if (w == 0) {
    return FALSE;
  }
  was = !w->enabled;
  if ((w->enabled != 0) != (e != 0)) {
    w->enabled = e != 0;
    if (w->parent != 0 && !w->enabled && top_of(w)->focus == w) {
      top_of(w)->focus = 0;
    }
    call_proc(w, WM_ENABLE, (WPARAM)(e != 0), 0);
    invalidate(w, 0, 0);
  }
  return was;
}
EXPORT BOOL WINAPI IsIconic(HWND h) { (void)h; return FALSE; }
EXPORT BOOL WINAPI IsZoomed(HWND h) { (void)h; return FALSE; }
EXPORT HWND WINAPI GetParent(HWND h) { wnd_t *w = w_get(h); return w != 0 ? w_hwnd(w->parent) : 0; }
EXPORT HWND WINAPI GetDesktopWindow(void) { return 0; }

static wnd_t *first_top(void) {
  int i;

  for (i = 0; i < MAX_WINDOWS; ++i) {
    if (g_wnd[i].used && g_wnd[i].parent == 0) {
      return &g_wnd[i];
    }
  }
  return 0;
}

EXPORT HWND WINAPI GetFocus(void) {
  wnd_t *t = first_top();

  return t == 0 ? 0 : w_hwnd(t->focus != 0 ? t->focus : t);
}
EXPORT HWND WINAPI GetActiveWindow(void) { return w_hwnd(first_top()); }
EXPORT HWND WINAPI GetForegroundWindow(void) { return w_hwnd(first_top()); }

EXPORT HWND WINAPI SetFocus(HWND h) {
  wnd_t *w = w_get(h), *t, *old;

  if (w == 0) {
    return GetFocus();
  }
  t = top_of(w);
  old = t->focus != 0 ? t->focus : t;
  if (old == w) {
    return w_hwnd(old);
  }
  if (!IsWindowEnabled(h)) {
    return w_hwnd(old);
  }
  t->focus = 0;
  call_proc(old, WM_KILLFOCUS, (WPARAM)w_hwnd(w), 0);
  t->focus = w == t ? 0 : w;
  call_proc(w, WM_SETFOCUS, (WPARAM)w_hwnd(old), 0);
  return w_hwnd(old);
}

EXPORT HWND WINAPI SetActiveWindow(HWND h) { (void)h; return GetActiveWindow(); }
EXPORT BOOL WINAPI SetForegroundWindow(HWND h) { (void)h; return TRUE; }
EXPORT HWND WINAPI SetCapture(HWND h) { wnd_t *old = g_capture; g_capture = w_get(h); return w_hwnd(old); }
EXPORT BOOL WINAPI ReleaseCapture(void) { g_capture = 0; return TRUE; }
EXPORT HWND WINAPI GetCapture(void) { return w_hwnd(g_capture); }
EXPORT BOOL WINAPI MessageBeep(UINT t) { (void)t; return TRUE; }

EXPORT BOOL WINAPI InvalidateRect(HWND h, const RECT *r, BOOL erase) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  invalidate(w, r, erase);
  return TRUE;
}

EXPORT BOOL WINAPI ValidateRect(HWND h, const RECT *r) {
  wnd_t *w = w_get(h);

  (void)r;
  if (w == 0) {
    return FALSE;
  }
  w->invalid = 0;
  return TRUE;
}

EXPORT BOOL WINAPI GetUpdateRect(HWND h, LPRECT r, BOOL erase) {
  wnd_t *w = w_get(h);

  (void)erase;
  if (w == 0 || !w->invalid) {
    return FALSE;
  }
  if (r != 0) {
    *r = w->inv;
  }
  return TRUE;
}

EXPORT BOOL WINAPI GetClientRect(HWND h, LPRECT r) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  r->left = 0;
  r->top = 0;
  r->right = w->w;
  r->bottom = w->h;
  return TRUE;
}

EXPORT BOOL WINAPI GetWindowRect(HWND h, LPRECT r) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  if (w->parent != 0) {                                    // "screen" = the top-level window's client area
    int ox, oy;

    origin(w, &ox, &oy);
    r->left = ox;
    r->top = oy;
    r->right = ox + w->w;
    r->bottom = oy + w->h;
    return TRUE;
  }
  r->left = 0;
  r->top = 0;
  r->right = w->w + FRAME_W;
  r->bottom = w->h + FRAME_H;
  return TRUE;
}

EXPORT BOOL WINAPI AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) {
  (void)style;
  if (menu) r->top -= RT_MENU_H;
  r->left -= FRAME_W / 2;
  r->right += FRAME_W / 2;
  r->top -= FRAME_H - FRAME_W / 2;
  r->bottom += FRAME_W / 2;
  return TRUE;
}

EXPORT BOOL WINAPI AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex) { (void)ex; return AdjustWindowRect(r, style, menu); }
EXPORT BOOL WINAPI ClientToScreen(HWND h, LPPOINT p) {
  wnd_t *w = w_get(h);
  int ox, oy;

  if (w == 0) {
    return FALSE;
  }
  origin(w, &ox, &oy);
  p->x += ox;
  p->y += oy;
  return TRUE;
}

EXPORT BOOL WINAPI ScreenToClient(HWND h, LPPOINT p) {
  wnd_t *w = w_get(h);
  int ox, oy;

  if (w == 0) {
    return FALSE;
  }
  origin(w, &ox, &oy);
  p->x -= ox;
  p->y -= oy;
  return TRUE;
}

EXPORT int WINAPI MapWindowPoints(HWND from, HWND to, LPPOINT pts, UINT n) {
  wnd_t *a = w_get(from), *b = w_get(to);
  int ax = 0, ay = 0, bx = 0, by = 0;
  UINT i;

  if (a != 0) origin(a, &ax, &ay);
  if (b != 0) origin(b, &bx, &by);
  for (i = 0; i < n; ++i) {
    pts[i].x += ax - bx;
    pts[i].y += ay - by;
  }
  return (int)(((unsigned)(ay - by) & 0xFFFFu) << 16 | ((unsigned)(ax - bx) & 0xFFFFu));
}

static void do_resize(wnd_t *w, int cw, int ch) {
  clamp_client(&cw, &ch);
  if (cw == w->w && ch == w->h) {
    return;
  }
  if (ch + w->menubar > 600) {
    ch = 600 - w->menubar;
  }
  sc3(KS_WIN_RESIZE, w->sysid, cw, ch + w->menubar);
  w->w = cw;
  w->h = ch;
  q_push(w_hwnd(w), WM_SIZE, SIZE_RESTORED, mklp(cw, ch));
  invalidate(w, 0, 1);
}

// A child window takes a new place and size in its parent's client area.
static void move_child(wnd_t *w, int x, int y, int cw, int ch) {
  RECT old = {w->x, w->y, w->x + w->w, w->y + w->h};
  int resized = cw != w->w || ch != w->h;

  if (cw < 0) cw = 0;
  if (ch < 0) ch = 0;
  w->x = x;
  w->y = y;
  w->w = cw;
  w->h = ch;
  if (resized) {
    call_proc(w, WM_SIZE, SIZE_RESTORED, mklp(cw, ch));
  }
  if (shown(w)) {
    invalidate_behind(w, &old);
    invalidate(w, 0, 1);
  }
}

EXPORT BOOL WINAPI MoveWindow(HWND h, int x, int y, int cw, int ch, BOOL repaint) {
  wnd_t *w = w_get(h);

  (void)repaint;
  if (w == 0) {
    return FALSE;
  }
  if (w->parent != 0) {
    move_child(w, x, y, cw, ch);
    return TRUE;
  }
  sc3(KS_WIN_MOVE, w->sysid, x, y);
  do_resize(w, cw - FRAME_W, ch - FRAME_H - w->menubar);
  return TRUE;
}

EXPORT BOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
  wnd_t *w = w_get(h);

  (void)after;
  if (w == 0) {
    return FALSE;
  }
  if (w->parent != 0) {
    move_child(w, (flags & SWP_NOMOVE) ? w->x : x, (flags & SWP_NOMOVE) ? w->y : y, (flags & SWP_NOSIZE) ? w->w : cx,
               (flags & SWP_NOSIZE) ? w->h : cy);
    if (flags & SWP_SHOWWINDOW) ShowWindow(h, SW_SHOW);
    if (flags & SWP_HIDEWINDOW) ShowWindow(h, SW_HIDE);
    return TRUE;
  }
  if (!(flags & SWP_NOMOVE)) {
    sc3(KS_WIN_MOVE, w->sysid, x, y);
  }
  if (!(flags & SWP_NOSIZE)) {
    do_resize(w, cx - FRAME_W, cy - FRAME_H - w->menubar);
  }
  return TRUE;
}

EXPORT BOOL WINAPI SetWindowTextA(HWND h, LPCSTR t) {
  if (w_get(h) == 0) {
    return FALSE;
  }
  SendMessageA(h, WM_SETTEXT, 0, (LPARAM)t);
  return TRUE;
}

EXPORT int WINAPI GetWindowTextA(HWND h, LPSTR buf, int n) {
  if (w_get(h) == 0 || n <= 0) {
    return 0;
  }
  return (int)SendMessageA(h, WM_GETTEXT, (WPARAM)n, (LPARAM)buf);
}

EXPORT int WINAPI GetWindowTextLengthA(HWND h) { return w_get(h) != 0 ? (int)SendMessageA(h, WM_GETTEXTLENGTH, 0, 0) : 0; }

EXPORT LONG_PTR WINAPI GetWindowLongPtrA(HWND h, int idx) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return 0;
  }
  switch (idx) {
    case GWLP_USERDATA: return w->userdata;
    case GWLP_WNDPROC: return (LONG_PTR)w->proc;
    case GWL_STYLE: return (LONG_PTR)w->style;
    case GWL_EXSTYLE: return (LONG_PTR)w->exstyle;
    case GWLP_HINSTANCE: return (LONG_PTR)w->inst;
    case GWLP_ID: return w->id;
    case GWLP_HWNDPARENT: return (LONG_PTR)w->parent;
    default: return idx >= 0 && idx < 64 ? w->extra[idx / 8] : 0;
  }
}

EXPORT LONG_PTR WINAPI SetWindowLongPtrA(HWND h, int idx, LONG_PTR v) {
  wnd_t *w = w_get(h);
  LONG_PTR old = GetWindowLongPtrA(h, idx);

  if (w == 0) {
    return 0;
  }
  switch (idx) {
    case GWLP_USERDATA: w->userdata = v; break;
    case GWLP_WNDPROC: w->proc = (WNDPROC)v; break;
    case GWL_STYLE: w->style = (DWORD)v; break;
    case GWL_EXSTYLE: w->exstyle = (DWORD)v; break;
    case GWLP_ID: w->id = v; break;
    default:
      if (idx >= 0 && idx < 64) {
        w->extra[idx / 8] = v;
      }
      break;
  }
  return old;
}

EXPORT LONG WINAPI GetWindowLongA(HWND h, int idx) { return (LONG)GetWindowLongPtrA(h, idx); }
EXPORT LONG WINAPI SetWindowLongA(HWND h, int idx, LONG v) { return (LONG)SetWindowLongPtrA(h, idx, v); }

// ---- Default window procedure, messages -----------------------------------------------------------------

EXPORT LRESULT WINAPI DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  wnd_t *w = w_get(h);

  switch (msg) {
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_NCCREATE:
      return 1;
    case WM_ERASEBKGND: {
      HDC dc = (HDC)wp;
      RECT r;

      if (w != 0 && dc != 0) {
        GetClientRect(h, &r);
        FillRect(dc, &r, w->cls != 0 && w->cls->bg != 0 ? w->cls->bg : (HBRUSH)(uintptr_t)(COLOR_WINDOW + 1));
      }
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;

      BeginPaint(h, &ps);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_SETTEXT:
      if (w != 0) {
        set_text(w, (LPCSTR)lp);
        if (w->parent == 0) {
          char t[64];

          rt_copy(t, w->text != 0 ? w->text : "", sizeof(t));
          sc3(KS_WIN_TITLE, w->sysid, (sl_t)t, 0);
        }
      }
      return TRUE;
    case WM_GETTEXT: {
      const char *t = w != 0 && w->text != 0 ? w->text : "";

      if (wp == 0) {
        return 0;
      }
      rt_copy((char *)lp, t, wp);
      return (LRESULT)rt_strlen((char *)lp);
    }
    case WM_GETTEXTLENGTH:
      return w != 0 && w->text != 0 ? (LRESULT)rt_strlen(w->text) : 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORMSGBOX:
      SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
      SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
      return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSCROLLBAR:
      SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
      SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
      return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_NCHITTEST:
      return HTCLIENT;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_SETFONT:
      if (w != 0) w->extra[7] = (LONG_PTR)wp;
      return 0;
    case WM_GETFONT:
      return w != 0 ? (LRESULT)w->extra[7] : 0;
    default:
      return 0;
  }
}

EXPORT LRESULT WINAPI DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, msg, wp, lp); }

// ---- Menu bar ------------------------------------------------------------------------------------------------

HMENU rt_wnd_menu(HWND h) { wnd_t *w = w_get(h); return w != 0 ? (HMENU)w->menu : 0; }

int rt_wnd_surface(HWND h, int *sysid, int *cw, int *ch, int *mb) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return 0;
  }
  *sysid = w->sysid;
  *cw = w->w;
  *ch = w->h;
  *mb = w->menubar;
  return 1;
}

EXPORT HMENU WINAPI GetMenu(HWND h) { wnd_t *w = w_get(h); return w != 0 && w->parent == 0 ? (HMENU)w->menu : 0; }

EXPORT BOOL WINAPI SetMenu(HWND h, HMENU m) {
  wnd_t *w = w_get(h);
  int mb;

  if (w == 0 || w->parent != 0) {
    return FALSE;
  }
  mb = rt_menu_valid(m) ? RT_MENU_H : 0;
  w->menu = mb ? m : 0;
  if (mb != w->menubar) {                               // the client area keeps its size: the surface grows or shrinks
    int ch = w->h > 600 - mb ? 600 - mb : w->h;

    sc3(KS_WIN_RESIZE, w->sysid, w->w, ch + mb);
    w->menubar = mb;
    w->h = ch;
    q_push(h, WM_SIZE, SIZE_RESTORED, mklp(w->w, w->h));
  }
  invalidate(w, 0, 1);
  return TRUE;
}

EXPORT BOOL WINAPI DrawMenuBar(HWND h) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  if (w->menu != 0) {
    rt_menu_draw_bar(h);
  }
  return TRUE;
}

// ---- Child windows and dialog items ---------------------------------------------------------------------

EXPORT HWND WINAPI GetWindow(HWND h, UINT cmd) {
  wnd_t *w = w_get(h), *r = 0, *p;

  if (w == 0) {
    return 0;
  }
  p = w->parent;
  switch (cmd) {
    case GW_HWNDFIRST: r = p != 0 ? p->first : w; break;
    case GW_HWNDLAST: r = p != 0 ? p->last : w; break;
    case GW_HWNDNEXT: r = w->next; break;
    case GW_HWNDPREV: r = w->prev; break;
    case GW_CHILD: r = w->first; break;
    default: break;
  }
  return w_hwnd(r);
}

EXPORT BOOL WINAPI IsChild(HWND parent, HWND h) {
  wnd_t *w = w_get(h), *p = w_get(parent);

  for (; w != 0 && p != 0; w = w->parent) {
    if (w->parent == p) {
      return TRUE;
    }
  }
  return FALSE;
}

EXPORT HWND WINAPI GetDlgItem(HWND h, int id) {
  wnd_t *w = w_get(h), *c;

  if (w != 0) {
    for (c = w->first; c != 0; c = c->next) {
      if ((int)c->id == id) {
        return w_hwnd(c);
      }
    }
  }
  return 0;
}

EXPORT int WINAPI GetDlgCtrlID(HWND h) { wnd_t *w = w_get(h); return w != 0 ? (int)w->id : 0; }

static BOOL enum_children(wnd_t *w, WNDENUMPROC proc, LPARAM lp) {
  wnd_t *c, *next;

  for (c = w->first; c != 0; c = next) {
    next = c->next;
    if (!proc(w_hwnd(c), lp) || !enum_children(c, proc, lp)) {
      return FALSE;
    }
  }
  return TRUE;
}

EXPORT BOOL WINAPI EnumChildWindows(HWND h, WNDENUMPROC proc, LPARAM lp) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  enum_children(w, proc, lp);
  return TRUE;
}

EXPORT LRESULT WINAPI SendDlgItemMessageA(HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp) {
  HWND c = GetDlgItem(dlg, id);

  return c != 0 ? SendMessageA(c, msg, wp, lp) : 0;
}

EXPORT BOOL WINAPI SetDlgItemTextA(HWND dlg, int id, LPCSTR t) { HWND c = GetDlgItem(dlg, id); return c != 0 && SetWindowTextA(c, t); }

EXPORT UINT WINAPI GetDlgItemTextA(HWND dlg, int id, LPSTR buf, int n) {
  HWND c = GetDlgItem(dlg, id);

  if (c == 0) {
    if (n > 0) buf[0] = 0;
    return 0;
  }
  return (UINT)GetWindowTextA(c, buf, n);
}

EXPORT BOOL WINAPI SetDlgItemInt(HWND dlg, int id, UINT v, BOOL is_signed) {
  char t[16], r[16];
  int n = 0, i = 0, neg = is_signed && (int)v < 0;
  unsigned u = neg ? 0u - v : v;

  do {
    r[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u != 0);
  if (neg) t[i++] = '-';
  while (n > 0) t[i++] = r[--n];
  t[i] = 0;
  return SetDlgItemTextA(dlg, id, t);
}

EXPORT UINT WINAPI GetDlgItemInt(HWND dlg, int id, BOOL *ok, BOOL is_signed) {
  char t[32];
  const char *p = t;
  unsigned long long v = 0;
  int neg = 0, digits = 0;

  GetDlgItemTextA(dlg, id, t, sizeof(t));
  while (*p == ' ') ++p;
  if (is_signed && *p == '-') { neg = 1; ++p; }
  for (; *p >= '0' && *p <= '9'; ++p) {
    v = v * 10 + (unsigned)(*p - '0');
    ++digits;
  }
  if (ok != 0) *ok = digits > 0 && *p == 0 && v <= (is_signed ? (neg ? 0x80000000ull : 0x7FFFFFFFull) : 0xFFFFFFFFull);
  return (UINT)(neg ? 0u - (unsigned)v : (unsigned)v);
}

EXPORT BOOL WINAPI CheckDlgButton(HWND dlg, int id, UINT check) { return (BOOL)(SendDlgItemMessageA(dlg, id, BM_SETCHECK, check, 0), GetDlgItem(dlg, id) != 0); }
EXPORT UINT WINAPI IsDlgButtonChecked(HWND dlg, int id) { return (UINT)SendDlgItemMessageA(dlg, id, BM_GETCHECK, 0, 0); }

EXPORT BOOL WINAPI CheckRadioButton(HWND dlg, int first, int last, int check) {
  int i;

  for (i = first; i <= last; ++i) {
    SendDlgItemMessageA(dlg, i, BM_SETCHECK, i == check ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  return TRUE;
}

EXPORT LRESULT WINAPI CallWindowProcA(WNDPROC p, HWND h, UINT msg, WPARAM wp, LPARAM lp) { return p(h, msg, wp, lp); }

EXPORT LRESULT WINAPI SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  wnd_t *w = w_get(h);

  return w != 0 ? call_proc(w, msg, wp, lp) : 0;
}

EXPORT BOOL WINAPI PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return q_push(h, msg, wp, lp); }

EXPORT VOID WINAPI PostQuitMessage(int code) {
  g_quit = 1;
  g_quit_code = code;
}

EXPORT BOOL WINAPI TranslateMessage(const MSG *m) { (void)m; return TRUE; }   // WM_CHAR is generated with the key

EXPORT LRESULT WINAPI DispatchMessageA(const MSG *m) {
  wnd_t *w;

  if (m->message == WM_TIMER && m->lParam != 0) {
    ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, m->wParam, GetTickCount());
    return 0;
  }
  w = w_get(m->hwnd);
  return w != 0 ? call_proc(w, m->message, m->wParam, m->lParam) : 0;
}

// ---- Input events -> messages -----------------------------------------------------------------------------

static WPARAM mouse_keys(int mods) {
  return (WPARAM)((g_lbutton ? MK_LBUTTON : 0) | ((mods & 1) ? MK_SHIFT : 0) | ((mods & 2) ? MK_CONTROL : 0));
}

static void key_message(wnd_t *top, unsigned ch, int mods) {
  UINT vk = 0;
  int chr = 0;
  HWND h = w_hwnd(top);                              // aimed at the window with the focus when taken from the queue

  g_mods = mods;
  switch (ch) {
    case SCKEY_LEFT: vk = VK_LEFT; break;
    case SCKEY_RIGHT: vk = VK_RIGHT; break;
    case SCKEY_UP: vk = VK_UP; break;
    case SCKEY_DOWN: vk = VK_DOWN; break;
    case SCKEY_DELETE: vk = VK_DELETE; break;
    case SCKEY_HOME: vk = VK_HOME; break;
    case SCKEY_END: vk = VK_END; break;
    case SCKEY_PGUP: vk = VK_PRIOR; break;
    case SCKEY_PGDN: vk = VK_NEXT; break;
    case 8: vk = VK_BACK; chr = 8; break;
    case 9: vk = VK_TAB; chr = 9; break;
    case 10: case 13: vk = VK_RETURN; chr = 13; break;
    case 27: vk = VK_ESCAPE; chr = 27; break;
    default:
      if (ch >= 1 && ch <= 26) {                         // Ctrl+letter
        vk = 'A' + ch - 1;
        chr = (int)ch;
      } else if (ch >= 'a' && ch <= 'z') {
        vk = ch - 'a' + 'A';
        chr = (int)ch;
      } else if (ch >= 32 && ch < 127) {
        vk = ch;
        chr = (int)ch;
      }
      break;
  }
  if (vk == 0) {
    return;
  }
  q_push_kind(h, WM_KEYDOWN, vk, 1, 1);
  if (chr != 0) {
    q_push_kind(h, WM_CHAR, (WPARAM)chr, 1, 1);
  }
  q_push_kind(h, WM_KEYUP, vk, (LPARAM)(1u | 0xC0000000u), 1);
}

// The topmost visible child window at (x, y), given in `w`'s client coordinates (w itself if none).
static wnd_t *hit_test(wnd_t *w, int x, int y) {
  wnd_t *c;

  for (c = w->last; c != 0; c = c->prev) {
    if (c->visible && x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h) {
      return hit_test(c, x - c->x, y - c->y);
    }
  }
  return w;
}

// A mouse message, addressed to the top-level window with surface coordinates; resolve_input
// aims it at the window under the pointer (or the one that captured the mouse).
static void mouse_message(wnd_t *top, UINT msg, WPARAM wp, int x, int y) {
  q_push_kind(w_hwnd(top), msg, wp, mklp(x, y), 2);
}

static int resolve_input(MSG *m, int raw) {
  wnd_t *top = w_get(m->hwnd), *t;

  if (top == 0 || top->parent != 0) {
    return 1;
  }
  if (raw == 1) {
    t = top->focus != 0 ? top->focus : top;
  } else {
    int x = (short)(m->lParam & 0xFFFF), y = (short)((m->lParam >> 16) & 0xFFFF), ox, oy;

    t = g_capture != 0 && top_of(g_capture) == top ? g_capture : hit_test(top, x, y - top->menubar);
    if (!IsWindowEnabled(w_hwnd(t))) {
      return 0;
    }
    if (m->message == WM_LBUTTONDBLCLK && (t->cls == 0 || !(t->cls->style & CS_DBLCLKS))) {
      return 0;
    }
    origin(t, &ox, &oy);
    m->lParam = mklp(x - ox, y - oy);
  }
  m->hwnd = w_hwnd(t);
  return 1;
}

// Turn one kernel event into messages. Returns 0 when the window is gone.
static void handle_event(wnd_t *w, const sc_winev_t *ev) {
  HWND h = w_hwnd(w);

  if (w->menubar != 0) {                                  // the menu bar takes the mouse over it, and Alt+letter
    if (ev->type == SCEV_MOUSE_DOWN && ev->y < w->menubar) {
      rt_menu_track_bar(h, ev->x);
      return;
    }
    if ((ev->type == SCEV_MOUSE_MOVE || ev->type == SCEV_MOUSE_UP || ev->type == SCEV_MOUSE_DOUBLE) && ev->y < w->menubar) {
      return;
    }
    if (ev->type == SCEV_KEY && (ev->x & 4) && rt_menu_alt_key(h, ev->ch)) {
      return;
    }
  }

  switch (ev->type) {
    case SCEV_KEY:
      key_message(w, ev->ch, ev->x);
      break;
    case SCEV_MOUSE_DOWN:
      g_lbutton = 1;
      mouse_message(w, WM_LBUTTONDOWN, mouse_keys((int)ev->ch), ev->x, ev->y);
      break;
    case SCEV_MOUSE_DOUBLE:
      mouse_message(w, WM_LBUTTONDBLCLK, mouse_keys((int)ev->ch), ev->x, ev->y);
      break;
    case SCEV_MOUSE_MOVE:
      mouse_message(w, WM_MOUSEMOVE, mouse_keys((int)ev->ch), ev->x, ev->y);
      break;
    case SCEV_MOUSE_UP:
      g_lbutton = 0;
      mouse_message(w, WM_LBUTTONUP, mouse_keys((int)ev->ch), ev->x, ev->y);
      break;
    case SCEV_WHEEL:
      mouse_message(w, WM_MOUSEWHEEL, (WPARAM)(((unsigned)((int)ev->ch * 120) & 0xFFFFu) << 16) | mouse_keys(0), ev->x, ev->y);
      break;
    case SCEV_CLOSE:
      q_push(h, WM_CLOSE, 0, 0);
      break;
    case SCEV_RESIZE:
      w->w = ev->x;
      w->h = ev->y - w->menubar;
      q_push(h, WM_SIZE, SIZE_RESTORED, mklp(ev->x, ev->y));
      invalidate(w, 0, 1);
      break;
    default:
      break;
  }
}

// Pull pending events from every window. Returns how many were taken.
static int pump_events(void) {
  int i, n = 0;

  for (i = 0; i < MAX_WINDOWS; ++i) {
    wnd_t *w = &g_wnd[i];
    sc_winev_t ev;

    while (w->used && w->parent == 0) {
      sl_t r = sc3(KS_WIN_EVENT, w->sysid, (sl_t)&ev, 0);

      if (r < 0) {                                       // the kernel closed it
        destroy_tree(w, 0);
        rt_free(w->text);
        w->text = 0;
        w->used = 0;
        q_push(w_hwnd(w), WM_DESTROY, 0, 0);
        break;
      }
      if (r == 0) {
        break;
      }
      handle_event(w, &ev);
      ++n;
    }
  }
  return n;
}

// ---- Timers ------------------------------------------------------------------------------------------------

EXPORT UINT_PTR WINAPI SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC proc) {
  int i;

  for (i = 0; i < MAX_TIMERS; ++i) {                      // replace a timer with the same window and id
    if (g_timer[i].used && g_timer[i].hwnd == h && h != 0 && g_timer[i].id == id) {
      g_timer[i].ms = ms;
      g_timer[i].next = GetTickCount() + ms;
      g_timer[i].proc = proc;
      return id;
    }
  }
  for (i = 0; i < MAX_TIMERS; ++i) {
    if (!g_timer[i].used) {
      g_timer[i].used = 1;
      g_timer[i].hwnd = h;
      g_timer[i].id = (h == 0 || id == 0) ? g_next_timer_id++ : id;
      g_timer[i].ms = ms < 10 ? 10 : ms;
      g_timer[i].next = GetTickCount() + g_timer[i].ms;
      g_timer[i].proc = proc;
      return g_timer[i].id;
    }
  }
  return 0;
}

EXPORT BOOL WINAPI KillTimer(HWND h, UINT_PTR id) {
  int i;

  for (i = 0; i < MAX_TIMERS; ++i) {
    if (g_timer[i].used && g_timer[i].hwnd == h && g_timer[i].id == id) {
      g_timer[i].used = 0;
      return TRUE;
    }
  }
  return FALSE;
}

static int pump_timers(void) {
  DWORD now = GetTickCount();
  int i, n = 0;

  for (i = 0; i < MAX_TIMERS; ++i) {
    timer_t_ *t = &g_timer[i];

    if (t->used && (int)(now - t->next) >= 0) {
      t->next = now + t->ms;
      q_push(t->hwnd, WM_TIMER, t->id, (LPARAM)t->proc);
      ++n;
    }
  }
  return n;
}

static DWORD next_timer_wait(void) {
  DWORD now = GetTickCount(), best = 1000;
  int i;

  for (i = 0; i < MAX_TIMERS; ++i) {
    if (g_timer[i].used) {
      DWORD d = (int)(g_timer[i].next - now) > 0 ? g_timer[i].next - now : 0;

      if (d < best) {
        best = d;
      }
    }
  }
  return best;
}

// ---- Getting messages -------------------------------------------------------------------------------------

static int live_windows(void) {
  int i, n = 0;

  for (i = 0; i < MAX_WINDOWS; ++i) {
    n += g_wnd[i].used && g_wnd[i].parent == 0;
  }
  return n;
}

// The first window, parents before their children, that is shown and needs painting.
static wnd_t *find_invalid(wnd_t *w) {
  wnd_t *c, *r;

  if (!w->visible) {
    return 0;
  }
  if (w->invalid) {
    return w;
  }
  for (c = w->first; c != 0; c = c->next) {
    r = find_invalid(c);
    if (r != 0) {
      return r;
    }
  }
  return 0;
}

// A WM_PAINT for the first window that needs one (and that is now considered handled).
static int make_paint(MSG *m) {
  int i;

  for (i = 0; i < MAX_WINDOWS; ++i) {
    wnd_t *w = &g_wnd[i], *p;

    if (w->used && w->parent == 0 && (p = find_invalid(w)) != 0) {
      m->hwnd = w_hwnd(p);
      m->message = WM_PAINT;
      m->wParam = 0;
      m->lParam = 0;
      m->time = GetTickCount();
      m->pt.x = m->pt.y = 0;
      return 1;
    }
  }
  return 0;
}

static int quit_message(MSG *m) {
  m->hwnd = 0;
  m->message = WM_QUIT;
  m->wParam = (WPARAM)g_quit_code;
  m->lParam = 0;
  m->time = GetTickCount();
  return 1;
}

// One attempt to produce a message without waiting. Returns 1 if *m was filled.
static int try_message(MSG *m, int remove) {
  if (q_pop(m, remove)) {
    return 1;
  }
  pump_timers();
  pump_events();
  if (q_pop(m, remove)) {
    return 1;
  }
  if (make_paint(m)) {
    return 1;
  }
  if (g_quit) {
    return quit_message(m);
  }
  return 0;
}

// Sleep until something may have happened: input on the only window, or a timer is due.
static void wait_for_something(void) {
  DWORD ms = next_timer_wait();
  int i, one = -1, count = 0;

  for (i = 0; i < MAX_WINDOWS; ++i) {
    if (g_wnd[i].used && g_wnd[i].parent == 0) {
      one = i;
      ++count;
    }
  }
  if (count == 1) {
    sc_winev_t ev;
    sl_t r = sc3(KS_WIN_WAIT, g_wnd[one].sysid, (sl_t)&ev, ms > 100 ? 100 : (sl_t)ms);

    if (r < 0) {
      destroy_tree(&g_wnd[one], 0);
      rt_free(g_wnd[one].text);
      g_wnd[one].text = 0;
      g_wnd[one].used = 0;
      q_push(w_hwnd(&g_wnd[one]), WM_DESTROY, 0, 0);
    } else if (r > 0) {
      handle_event(&g_wnd[one], &ev);
    }
  } else {
    sc3(KS_SLEEP, ms > 8 ? 8 : (sl_t)ms, 0, 0);
  }
}

EXPORT BOOL WINAPI GetMessageA(LPMSG m, HWND hwnd, UINT lo, UINT hi) {
  (void)hwnd;
  (void)lo;
  (void)hi;
  for (;;) {
    if (try_message(m, 1)) {
      if (m->message == WM_PAINT) {
        wnd_t *w = w_get(m->hwnd);

        if (w != 0) {
          RECT keep = w->inv;

          painted(w);                                    // delivered: BeginPaint reads the saved rectangle
          w->inv = keep;
        }
      }
      return m->message != WM_QUIT;
    }
    if (live_windows() == 0 && next_timer_wait() >= 1000) {
      quit_message(m);                                   // nothing can ever arrive: end like WM_QUIT
      return FALSE;
    }
    wait_for_something();
  }
}

EXPORT BOOL WINAPI PeekMessageA(LPMSG m, HWND hwnd, UINT lo, UINT hi, UINT remove) {
  (void)hwnd;
  (void)lo;
  (void)hi;
  if (!try_message(m, (remove & PM_REMOVE) != 0)) {
    return FALSE;
  }
  if (m->message == WM_PAINT && (remove & PM_REMOVE)) {
    wnd_t *w = w_get(m->hwnd);

    if (w != 0) {
      RECT keep = w->inv;

      painted(w);
      w->inv = keep;
    }
  }
  return TRUE;
}

EXPORT BOOL WINAPI GetMessageW(LPMSG m, HWND h, UINT lo, UINT hi) { return GetMessageA(m, h, lo, hi); }
EXPORT BOOL WINAPI PeekMessageW(LPMSG m, HWND h, UINT lo, UINT hi, UINT r) { return PeekMessageA(m, h, lo, hi, r); }
EXPORT LRESULT WINAPI DispatchMessageW(const MSG *m) { return DispatchMessageA(m); }

// ---- Painting ------------------------------------------------------------------------------------------------

EXPORT HDC WINAPI GetDC(HWND h) {
  wnd_t *w = w_get(h);

  return w != 0 ? (HDC)dc_for(w) : 0;
}

EXPORT HDC WINAPI GetWindowDC(HWND h) { return GetDC(h); }

EXPORT int WINAPI ReleaseDC(HWND h, HDC dc) {
  (void)h;
  gdi_dc_free(gdi_dc(dc));
  return 1;
}

EXPORT HDC WINAPI BeginPaint(HWND h, LPPAINTSTRUCT ps) {
  wnd_t *w = w_get(h);
  HDC dc;

  if (w == 0) {
    return 0;
  }
  dc = (HDC)dc_for(w);
  rt_memset(ps, 0, sizeof(*ps));
  ps->hdc = dc;
  ps->rcPaint = w->inv.right > w->inv.left ? w->inv : (RECT){0, 0, w->w, w->h};
  if (ps->rcPaint.right > w->w) ps->rcPaint.right = w->w;
  if (ps->rcPaint.bottom > w->h) ps->rcPaint.bottom = w->h;
  if (w->erase) {                                        // erase what is about to be painted
    w->erase = 0;
    ps->fErase = TRUE;
    if (call_proc(w, WM_ERASEBKGND, (WPARAM)dc, 0) == 0) {
      FillRect(dc, &ps->rcPaint, w->cls != 0 && w->cls->bg != 0 ? w->cls->bg : (HBRUSH)(uintptr_t)(COLOR_WINDOW + 1));
    }
  }
  w->inv.left = w->inv.top = w->inv.right = w->inv.bottom = 0;
  w->invalid = 0;
  return dc;
}

EXPORT BOOL WINAPI EndPaint(HWND h, const PAINTSTRUCT *ps) {
  (void)h;
  gdi_dc_free(gdi_dc(ps->hdc));
  return TRUE;
}

// ---- Misc ----------------------------------------------------------------------------------------------------

EXPORT int WINAPI GetSystemMetrics(int idx) {
  sl_t s;

  switch (idx) {
    case SM_CXSCREEN: s = sc3(KS_SCREEN, 0, 0, 0); return (int)(s >> 16);
    case SM_CYSCREEN: s = sc3(KS_SCREEN, 0, 0, 0); return (int)(s & 0xFFFF);
    case SM_CXVSCROLL: case SM_CYHSCROLL: return 17;
    case SM_CYCAPTION: return 23;
    case SM_CXBORDER: case SM_CYBORDER: return 1;
    case SM_CXFRAME: case SM_CYFRAME: return 8;
    case SM_CXICON: case SM_CYICON: return 32;
    default: return 0;
  }
}

EXPORT SHORT WINAPI GetKeyState(int vk) {
  switch (vk) {
    case VK_SHIFT: return (g_mods & 1) ? (SHORT)0x8000 : 0;
    case VK_CONTROL: return (g_mods & 2) ? (SHORT)0x8000 : 0;
    case VK_MENU: return (g_mods & 4) ? (SHORT)0x8000 : 0;
    case VK_LBUTTON: return g_lbutton ? (SHORT)0x8000 : 0;
    default: return 0;
  }
}

EXPORT SHORT WINAPI GetAsyncKeyState(int vk) { return GetKeyState(vk); }
EXPORT DWORD WINAPI GetSysColor(int i) { return gdi_sys_color(i); }
EXPORT HBRUSH WINAPI GetSysColorBrush(int i) { return gdi_sys_brush(i); }
EXPORT LPSTR WINAPI CharUpperA(LPSTR s) { if (((uintptr_t)s >> 16) == 0) { char c = (char)(uintptr_t)s; return (LPSTR)(uintptr_t)(c >= 'a' && c <= 'z' ? c - 32 : c); } for (char *p = s; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32); return s; }
EXPORT LPSTR WINAPI CharLowerA(LPSTR s) { if (((uintptr_t)s >> 16) == 0) { char c = (char)(uintptr_t)s; return (LPSTR)(uintptr_t)(c >= 'A' && c <= 'Z' ? c + 32 : c); } for (char *p = s; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32); return s; }
EXPORT LPSTR WINAPI CharNextA(LPCSTR s) { return (LPSTR)(*s ? s + 1 : s); }

EXPORT int WINAPIV wsprintfA(LPSTR buf, LPCSTR fmt, ...) {
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = rt_vsnprintf(buf, 1024, fmt, ap);
  va_end(ap);
  return n;
}

EXPORT int WINAPI wvsprintfA(LPSTR buf, LPCSTR fmt, va_list ap) { return rt_vsnprintf(buf, 1024, fmt, ap); }

// ---- Keyboard navigation: IsDialogMessage -----------------------------------------------------------------

static int is_tabstop(wnd_t *c) { return (c->style & WS_TABSTOP) && c->visible && IsWindowEnabled(w_hwnd(c)); }

// The next (or previous) tab stop among the children of `dlg`, going round.
static wnd_t *next_tab(wnd_t *dlg, wnd_t *from, int back) {
  wnd_t *c = from;
  int n;

  for (n = 0; n <= MAX_WINDOWS; ++n) {
    if (back) {
      c = c != 0 && c->prev != 0 ? c->prev : dlg->last;
    } else {
      c = c != 0 && c->next != 0 ? c->next : dlg->first;
    }
    if (c == 0) {
      return 0;
    }
    if (is_tabstop(c)) {
      return c;
    }
    if (c == from) {
      break;
    }
  }
  return 0;
}

EXPORT HWND WINAPI GetNextDlgTabItem(HWND dlg, HWND from, BOOL back) {
  wnd_t *d = w_get(dlg);

  return d != 0 ? w_hwnd(next_tab(d, w_get(from), back)) : 0;
}

EXPORT HWND WINAPI GetNextDlgGroupItem(HWND dlg, HWND from, BOOL back) { return GetNextDlgTabItem(dlg, from, back); }

static wnd_t *default_button(wnd_t *dlg) {
  wnd_t *c;

  for (c = dlg->first; c != 0; c = c->next) {
    if ((c->style & 0xF) == BS_DEFPUSHBUTTON && c->cls != 0 && rt_stricmp(c->cls->name, "BUTTON") == 0 && c->visible && IsWindowEnabled(w_hwnd(c))) {
      return c;
    }
  }
  return 0;
}

// Tab and Shift+Tab move the focus between the controls that have WS_TABSTOP, Enter presses the
// focused push button (or the default one), Escape sends IDCANCEL; those keys and their
// WM_CHAR are consumed. Anything else is left for the caller to dispatch.
EXPORT BOOL WINAPI IsDialogMessageA(HWND dlg, LPMSG m) {
  wnd_t *d = w_get(dlg), *t, *f;

  if (d == 0 || (m->hwnd != dlg && !IsChild(dlg, m->hwnd))) {
    return FALSE;
  }
  if (m->message != WM_KEYDOWN && m->message != WM_CHAR && m->message != WM_KEYUP) {
    return FALSE;
  }
  f = d->focus != 0 ? d->focus : 0;
  if (f != 0 && (SendMessageA(w_hwnd(f), WM_GETDLGCODE, 0, 0) & DLGC_WANTALLKEYS) != 0) {
    return FALSE;
  }
  if (m->wParam == VK_TAB || (m->message == WM_CHAR && m->wParam == 9)) {
    if (m->message == WM_KEYDOWN) {
      t = next_tab(d, f, (g_mods & 1) != 0);
      if (t != 0) {
        SetFocus(w_hwnd(t));
      }
    }
    return TRUE;
  }
  if (m->wParam == VK_RETURN || (m->message == WM_CHAR && m->wParam == 13)) {
    if (f != 0 && (SendMessageA(w_hwnd(f), WM_GETDLGCODE, 0, 0) & DLGC_WANTCHARS) != 0 && (f->style & ES_MULTILINE)) {
      return FALSE;
    }
    if (m->message == WM_KEYDOWN) {
      t = f != 0 && f->cls != 0 && rt_stricmp(f->cls->name, "BUTTON") == 0 && (f->style & 0xF) <= BS_DEFPUSHBUTTON ? f : default_button(d);
      if (t != 0) {
        SendMessageA(w_hwnd(t), BM_CLICK, 0, 0);
      }
    }
    return TRUE;
  }
  if (m->wParam == VK_ESCAPE || (m->message == WM_CHAR && m->wParam == 27)) {
    if (m->message == WM_KEYDOWN) {
      SendMessageA(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    }
    return TRUE;
  }
  return FALSE;
}

EXPORT BOOL WINAPI IsDialogMessageW(HWND dlg, LPMSG m) { return IsDialogMessageA(dlg, m); }

// ---- MessageBox: a small modal window of its own -------------------------------------------------------------

static const struct { UINT mask; int n; int ids[3]; const char *labels[3]; } kBoxes[] = {
  {MB_OK, 1, {IDOK}, {"OK"}},
  {MB_OKCANCEL, 2, {IDOK, IDCANCEL}, {"OK", "Cancel"}},
  {MB_ABORTRETRYIGNORE, 3, {IDABORT, IDRETRY, IDIGNORE}, {"Abort", "Retry", "Ignore"}},
  {MB_YESNOCANCEL, 3, {IDYES, IDNO, IDCANCEL}, {"Yes", "No", "Cancel"}},
  {MB_YESNO, 2, {IDYES, IDNO}, {"Yes", "No"}},
  {MB_RETRYCANCEL, 2, {IDRETRY, IDCANCEL}, {"Retry", "Cancel"}},
};

EXPORT int WINAPI MessageBoxA(HWND parent, LPCSTR text, LPCSTR caption, UINT type) {
  int kind = (int)(type & 0xF), nb, i, lines = 1, maxlen = 0, cur = 0, w, h, bw = 74, bx0, by;
  const typeof(kBoxes[0]) *box;
  sc_winev_t ev;
  sl_t id;
  int result = IDOK;
  char t[64];
  const char *p;

  (void)parent;
  box = &kBoxes[kind < 6 ? kind : 0];
  nb = box->n;
  for (p = text; *p; ++p) {
    if (*p == '\n') {
      ++lines;
      cur = 0;
    } else if (*p != '\r') {
      if (++cur > maxlen) {
        maxlen = cur;
      }
    }
  }
  w = maxlen * 8 + 48;
  if (w < nb * (bw + 8) + 32) w = nb * (bw + 8) + 32;
  if (w > 780) w = 780;
  h = lines * 12 + 90;
  rt_copy(t, caption != 0 ? caption : "Message", sizeof(t));
  id = sc3(KS_WIN_CREATE, (sl_t)t, w, h);
  if (id < 0) {
    sc3(KS_NOTIFY, (sl_t)text, 0, 0);                       // no window free: a toast has to do
    return box->ids[0];
  }
  bx0 = (w - nb * (bw + 8) + 8) / 2;
  by = h - 40;
  for (;;) {
    sl_t r;
    sc_winop_t op;
    char line[96];
    int ly = 20, li = 0;

    op.x = 0; op.y = 0; op.w = w; op.h = h; op.color = 0x00F0F0F0u; op.scale = 0; op.ptr = 0;
    sc3(KS_WIN_FILL, id, (sl_t)&op, 0);
    for (p = text;; ++p) {
      if (*p == '\n' || *p == 0) {
        line[li] = 0;
        op.x = 24; op.y = ly; op.w = 0; op.h = 0; op.color = 0; op.scale = 1; op.ptr = (uint64_t)(uintptr_t)line;
        sc3(KS_WIN_TEXT, id, (sl_t)&op, 0);
        ly += 12;
        li = 0;
        if (*p == 0) break;
      } else if (*p != '\r' && li < 95) {
        line[li++] = *p;
      }
    }
    for (i = 0; i < nb; ++i) {
      int bx = bx0 + i * (bw + 8), tl = (int)rt_strlen(box->labels[i]);

      op.x = bx; op.y = by; op.w = bw; op.h = 24; op.color = i == 0 ? 0x003A78D6u : 0x00E1E1E1u; op.scale = 0; op.ptr = 0;
      sc3(KS_WIN_FILL, id, (sl_t)&op, 0);
      op.x = bx + (bw - tl * 8) / 2; op.y = by + 8; op.w = 0; op.h = 0; op.color = i == 0 ? 0x00FFFFFFu : 0; op.scale = 1;
      op.ptr = (uint64_t)(uintptr_t)box->labels[i];
      sc3(KS_WIN_TEXT, id, (sl_t)&op, 0);
    }
    r = sc3(KS_WIN_WAIT, id, (sl_t)&ev, 1000);
    if (r < 0) {
      result = box->ids[nb - 1];
      break;
    }
    if (r == 0) {
      continue;
    }
    if (ev.type == SCEV_CLOSE) {
      result = nb == 1 ? box->ids[0] : (kind == 4 ? IDNO : IDCANCEL);
      if (kind == MB_YESNO) result = IDNO;
      break;
    }
    if (ev.type == SCEV_KEY && (ev.ch == '\n' || ev.ch == 13)) {
      result = box->ids[0];
      break;
    }
    if (ev.type == SCEV_KEY && ev.ch == 27 && kind != MB_YESNO) {
      result = nb == 1 ? box->ids[0] : IDCANCEL;
      break;
    }
    if (ev.type == SCEV_MOUSE_DOWN && ev.y >= by && ev.y < by + 24) {
      for (i = 0; i < nb; ++i) {
        int bx = bx0 + i * (bw + 8);

        if (ev.x >= bx && ev.x < bx + bw) {
          result = box->ids[i];
          goto done;
        }
      }
    }
  }
done:
  sc3(KS_WIN_CLOSE, id, 0, 0);
  return result;
}
