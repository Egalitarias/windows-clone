// user32 for the Win32 runtime: window classes, windows, the message loop, input, timers and
// MessageBox, on top of WinClone's window syscalls (one pixel surface per window, 800 x 600 at
// most, two windows per process, no child windows or standard controls). See internal.h.
//
// Messages: GetMessage/PeekMessage turn kernel input events into Windows messages (WM_KEYDOWN +
// WM_CHAR + WM_KEYUP for a key press, WM_LBUTTONDOWN/UP/MOUSEMOVE, WM_MOUSEWHEEL, WM_SIZE,
// WM_CLOSE) and generate WM_PAINT for invalidated windows and WM_TIMER for timers. Painting
// goes straight to the window surface (see gdi32.c); there is no clipping.
#include "internal.h"

#define MAX_CLASSES 16
#define MAX_WINDOWS 8
#define MAX_TIMERS  8
#define QUEUE       64

typedef struct {
  int used;
  WNDPROC proc;
  char name[48];
  HBRUSH bg;
  UINT style;
  HINSTANCE inst;
} cls_t;

typedef struct {
  int used;
  int sysid;                    // the kernel's window id
  WNDPROC proc;
  cls_t *cls;
  int w, h;                     // client size
  int visible;
  int invalid;                  // WM_PAINT due
  int erase;                    // erase the background in BeginPaint
  RECT inv;                     // what to repaint
  char title[64];
  DWORD style, exstyle;
  LONG_PTR userdata, id;
  HINSTANCE inst;
  int enabled;
} wnd_t;

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
static int g_qh, g_qt;
static int g_quit, g_quit_code;
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

static int q_push(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  int next = (g_qt + 1) % QUEUE;
  MSG *m;

  if (next == g_qh) {
    return 0;                                   // full: drop
  }
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

static int q_pop(MSG *out, int remove) {
  if (g_qh == g_qt) {
    return 0;
  }
  *out = g_q[g_qh];
  if (remove) {
    g_qh = (g_qh + 1) % QUEUE;
  }
  return 1;
}

static LPARAM mklp(int x, int y) { return (LPARAM)(((unsigned)y & 0xFFFFu) << 16 | ((unsigned)x & 0xFFFFu)); }

// ---- Window classes ---------------------------------------------------------------------------

EXPORT ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *wc) {
  int i;

  for (i = 0; i < MAX_CLASSES; ++i) {
    if (!g_cls[i].used) {
      cls_t *c = &g_cls[i];

      c->used = 1;
      c->proc = wc->lpfnWndProc;
      rt_copy(c->name, wc->lpszClassName != 0 && ((uintptr_t)wc->lpszClassName >> 16) != 0 ? wc->lpszClassName : "", sizeof(c->name));
      c->bg = wc->hbrBackground;
      c->style = wc->style;
      c->inst = wc->hInstance;
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

static void invalidate(wnd_t *w, const RECT *r, int erase) {
  RECT whole = {0, 0, w->w, w->h};

  if (r == 0) {
    r = &whole;
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
  int i, cwid, chei;
  sl_t id;
  CREATESTRUCTA cs;
  char t[64];

  (void)parent;
  if (c == 0) {
    SetLastError(ERROR_CANNOT_FIND_WND_CLASS);
    return 0;
  }
  if (style & WS_CHILD) {
    SetLastError(ERROR_NOT_SUPPORTED);                // no child windows or controls
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
  if (cw == CW_USEDEFAULT) cw = 640;
  if (ch == CW_USEDEFAULT) ch = 480;
  cwid = cw - FRAME_W;
  chei = ch - FRAME_H;
  clamp_client(&cwid, &chei);
  rt_copy(t, title != 0 ? title : "", sizeof(t));
  id = sc3(KS_WIN_CREATE, (sl_t)t, cwid, chei);
  if (id < 0) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);            // the kernel allows two windows per program
    return 0;
  }
  rt_memset(w, 0, sizeof(*w));
  w->used = 1;
  w->sysid = (int)id;
  w->proc = c->proc;
  w->cls = c;
  w->w = cwid;
  w->h = chei;
  w->style = style;
  w->exstyle = exstyle;
  w->inst = inst;
  w->enabled = 1;
  w->id = (LONG_PTR)menu;
  rt_copy(w->title, t, sizeof(w->title));
  if (style & WS_THICKFRAME) {
    sc3(28, id, 1, 0);                                // win_resizable
  }
  if (x != CW_USEDEFAULT && y != CW_USEDEFAULT) {
    sc3(KS_WIN_MOVE, id, x, y);
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
  if (call_proc(w, WM_NCCREATE, 0, (LPARAM)&cs) == 0 || (int)call_proc(w, WM_CREATE, 0, (LPARAM)&cs) == -1) {
    sc3(KS_WIN_CLOSE, id, 0, 0);
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
  }
  return was;
}

EXPORT BOOL WINAPI UpdateWindow(HWND h) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  if (w->invalid && w->visible) {
    call_proc(w, WM_PAINT, 0, 0);
    w->invalid = 0;
  }
  return TRUE;
}

EXPORT BOOL WINAPI DestroyWindow(HWND h) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  call_proc(w, WM_DESTROY, 0, 0);
  call_proc(w, WM_NCDESTROY, 0, 0);
  if (w->used) {
    sc3(KS_WIN_CLOSE, w->sysid, 0, 0);
    w->used = 0;
  }
  return TRUE;
}

EXPORT BOOL WINAPI CloseWindow(HWND h) { (void)h; return TRUE; }
EXPORT BOOL WINAPI IsWindow(HWND h) { return w_get(h) != 0; }
EXPORT BOOL WINAPI IsWindowVisible(HWND h) { wnd_t *w = w_get(h); return w != 0 && w->visible; }
EXPORT BOOL WINAPI IsWindowEnabled(HWND h) { wnd_t *w = w_get(h); return w != 0 && w->enabled; }
EXPORT BOOL WINAPI EnableWindow(HWND h, BOOL e) { wnd_t *w = w_get(h); BOOL was; if (w == 0) return FALSE; was = !w->enabled; w->enabled = e != 0; return was; }
EXPORT BOOL WINAPI IsIconic(HWND h) { (void)h; return FALSE; }
EXPORT BOOL WINAPI IsZoomed(HWND h) { (void)h; return FALSE; }
EXPORT HWND WINAPI GetParent(HWND h) { (void)h; return 0; }
EXPORT HWND WINAPI GetDesktopWindow(void) { return 0; }
EXPORT HWND WINAPI GetFocus(void) { int i; for (i = 0; i < MAX_WINDOWS; ++i) if (g_wnd[i].used) return w_hwnd(&g_wnd[i]); return 0; }
EXPORT HWND WINAPI GetActiveWindow(void) { return GetFocus(); }
EXPORT HWND WINAPI GetForegroundWindow(void) { return GetFocus(); }
EXPORT HWND WINAPI SetFocus(HWND h) { (void)h; return GetFocus(); }
EXPORT HWND WINAPI SetActiveWindow(HWND h) { (void)h; return GetFocus(); }
EXPORT BOOL WINAPI SetForegroundWindow(HWND h) { (void)h; return TRUE; }
EXPORT HWND WINAPI SetCapture(HWND h) { return h; }
EXPORT BOOL WINAPI ReleaseCapture(void) { return TRUE; }
EXPORT HWND WINAPI GetCapture(void) { return 0; }
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
  r->left = 0;
  r->top = 0;
  r->right = w->w + FRAME_W;
  r->bottom = w->h + FRAME_H;
  return TRUE;
}

EXPORT BOOL WINAPI AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) {
  (void)style;
  (void)menu;
  r->left -= FRAME_W / 2;
  r->right += FRAME_W / 2;
  r->top -= FRAME_H - FRAME_W / 2;
  r->bottom += FRAME_W / 2;
  return TRUE;
}

EXPORT BOOL WINAPI AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex) { (void)ex; return AdjustWindowRect(r, style, menu); }
EXPORT BOOL WINAPI ClientToScreen(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
EXPORT BOOL WINAPI ScreenToClient(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }

static void do_resize(wnd_t *w, int cw, int ch) {
  clamp_client(&cw, &ch);
  if (cw == w->w && ch == w->h) {
    return;
  }
  sc3(KS_WIN_RESIZE, w->sysid, cw, ch);
  w->w = cw;
  w->h = ch;
  q_push(w_hwnd(w), WM_SIZE, SIZE_RESTORED, mklp(cw, ch));
  invalidate(w, 0, 1);
}

EXPORT BOOL WINAPI MoveWindow(HWND h, int x, int y, int cw, int ch, BOOL repaint) {
  wnd_t *w = w_get(h);

  (void)repaint;
  if (w == 0) {
    return FALSE;
  }
  sc3(KS_WIN_MOVE, w->sysid, x, y);
  do_resize(w, cw - FRAME_W, ch - FRAME_H);
  return TRUE;
}

EXPORT BOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
  wnd_t *w = w_get(h);

  (void)after;
  if (w == 0) {
    return FALSE;
  }
  if (!(flags & SWP_NOMOVE)) {
    sc3(KS_WIN_MOVE, w->sysid, x, y);
  }
  if (!(flags & SWP_NOSIZE)) {
    do_resize(w, cx - FRAME_W, cy - FRAME_H);
  }
  return TRUE;
}

EXPORT BOOL WINAPI SetWindowTextA(HWND h, LPCSTR t) {
  wnd_t *w = w_get(h);

  if (w == 0) {
    return FALSE;
  }
  rt_copy(w->title, t != 0 ? t : "", sizeof(w->title));
  sc3(KS_WIN_TITLE, w->sysid, (sl_t)w->title, 0);
  return TRUE;
}

EXPORT int WINAPI GetWindowTextA(HWND h, LPSTR buf, int n) {
  wnd_t *w = w_get(h);

  if (w == 0 || n <= 0) {
    return 0;
  }
  rt_copy(buf, w->title, (size_t)n);
  return (int)rt_strlen(buf);
}

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
    default: return 0;
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
    default: break;
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
      SetWindowTextA(h, (LPCSTR)lp);
      return TRUE;
    case WM_GETTEXT:
      return GetWindowTextA(h, (LPSTR)lp, (int)wp);
    case WM_GETTEXTLENGTH:
      return w != 0 ? (LRESULT)rt_strlen(w->title) : 0;
    default:
      return 0;
  }
}

EXPORT LRESULT WINAPI DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, msg, wp, lp); }

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

static void key_message(wnd_t *w, unsigned ch, int mods) {
  UINT vk = 0;
  int chr = 0;
  HWND h = w_hwnd(w);

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
  q_push(h, WM_KEYDOWN, vk, 1);
  if (chr != 0) {
    q_push(h, WM_CHAR, (WPARAM)chr, 1);
  }
  q_push(h, WM_KEYUP, vk, (LPARAM)(1u | 0xC0000000u));
}

// Turn one kernel event into messages. Returns 0 when the window is gone.
static void handle_event(wnd_t *w, const sc_winev_t *ev) {
  HWND h = w_hwnd(w);

  switch (ev->type) {
    case SCEV_KEY:
      key_message(w, ev->ch, ev->x);
      break;
    case SCEV_MOUSE_DOWN:
      g_lbutton = 1;
      q_push(h, WM_LBUTTONDOWN, mouse_keys((int)ev->ch), mklp(ev->x, ev->y));
      break;
    case SCEV_MOUSE_DOUBLE:
      if (w->cls != 0 && (w->cls->style & CS_DBLCLKS)) {
        q_push(h, WM_LBUTTONDBLCLK, mouse_keys((int)ev->ch), mklp(ev->x, ev->y));
      }
      break;
    case SCEV_MOUSE_MOVE:
      q_push(h, WM_MOUSEMOVE, mouse_keys((int)ev->ch), mklp(ev->x, ev->y));
      break;
    case SCEV_MOUSE_UP:
      g_lbutton = 0;
      q_push(h, WM_LBUTTONUP, mouse_keys((int)ev->ch), mklp(ev->x, ev->y));
      break;
    case SCEV_WHEEL:
      q_push(h, WM_MOUSEWHEEL, (WPARAM)(((unsigned)((int)ev->ch * 120) & 0xFFFFu) << 16) | mouse_keys(0), mklp(ev->x, ev->y));
      break;
    case SCEV_CLOSE:
      q_push(h, WM_CLOSE, 0, 0);
      break;
    case SCEV_RESIZE:
      w->w = ev->x;
      w->h = ev->y;
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

    while (w->used) {
      sl_t r = sc3(KS_WIN_EVENT, w->sysid, (sl_t)&ev, 0);

      if (r < 0) {                                       // the kernel closed it
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
    n += g_wnd[i].used;
  }
  return n;
}

// A WM_PAINT for the first window that needs one (and that is now considered handled).
static int make_paint(MSG *m) {
  int i;

  for (i = 0; i < MAX_WINDOWS; ++i) {
    wnd_t *w = &g_wnd[i];

    if (w->used && w->visible && w->invalid) {
      m->hwnd = w_hwnd(w);
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
    if (g_wnd[i].used) {
      one = i;
      ++count;
    }
  }
  if (count == 1) {
    sc_winev_t ev;
    sl_t r = sc3(KS_WIN_WAIT, g_wnd[one].sysid, (sl_t)&ev, ms > 100 ? 100 : (sl_t)ms);

    if (r < 0) {
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
          w->invalid = 0;                                // delivered: BeginPaint reads the saved rectangle
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
      w->invalid = 0;
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

  return w != 0 ? (HDC)gdi_dc_alloc(w->sysid) : 0;
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
  dc = (HDC)gdi_dc_alloc(w->sysid);
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
