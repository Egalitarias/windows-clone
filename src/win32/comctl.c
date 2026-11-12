// The common controls of comctl32 for the Win32 runtime: progress bar (msctls_progress32), status bar
// (msctls_statusbar32), trackbar (msctls_trackbar32), up-down (msctls_updown32) and tab control
// (SysTabControl32). They are window classes registered at startup, like the standard controls in
// controls.c, so InitCommonControls and InitCommonControlsEx have nothing to do. Notifications go to the
// parent the way Windows does: WM_HSCROLL / WM_VSCROLL for the trackbar and the up-down, WM_NOTIFY for the
// tab control and the up-down (UDN_DELTAPOS). Drawing uses the built-in 8x8 font.
//
// Not here: list view, tree view, tool bar, header, rebar, tool tips, animation, date pickers, image
// lists, owner draw, custom draw, marquee progress bars, tab scrolling and multiple tab rows, up-down
// auto-buddy / arrow keys / thousands separators.
#define WINCOMMCTRLAPI __declspec(dllexport)
#include "internal.h"
#include <commctrl.h>

#define HI RGB(255, 255, 255)
#define LO RGB(128, 128, 128)
#define DK RGB(105, 105, 105)

static COLORREF face(void) { return GetSysColor(COLOR_BTNFACE); }
static DWORD style_of(HWND h) { return (DWORD)GetWindowLongPtrA(h, GWL_STYLE); }

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  RECT rc = {l, t, r, b};

  if (r <= l || b <= t) {
    return;
  }
  SetBkColor(dc, c);
  ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &rc, 0, 0, 0);
}

static void text(HDC dc, int x, int y, const char *s, int n, COLORREF c) {
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, c);
  TextOutA(dc, x, y, s, n);
}

// A raised (or, with `down`, sunken) rectangle: white and gray lines, the face inside.
static void bevel(HDC dc, int l, int t, int r, int b, int down) {
  fill(dc, l, t, r, b, face());
  fill(dc, l, t, r - 1, t + 1, down ? DK : HI);
  fill(dc, l, t, l + 1, b - 1, down ? DK : HI);
  fill(dc, l, b - 1, r, b, down ? HI : DK);
  fill(dc, r - 1, t, r, b, down ? HI : DK);
}

static int slen(const char *s) { int n = 0; while (s[n] != 0) ++n; return n; }
static void scopy(char *d, const char *s, int max) {
  int i;

  for (i = 0; i < max - 1 && s != 0 && s[i] != 0; ++i) d[i] = s[i];
  d[i] = 0;
}

// WM_NOTIFY to the parent. Returns what the parent's procedure returns.
static LRESULT notify(HWND h, UINT code, void *nm_in, int size) {
  NMHDR own;
  NMHDR *nm = nm_in != 0 ? (NMHDR *)nm_in : &own;
  HWND p = GetParent(h);

  (void)size;
  nm->hwndFrom = h;
  nm->idFrom = (UINT_PTR)GetDlgCtrlID(h);
  nm->code = code;
  return p != 0 ? SendMessageA(p, WM_NOTIFY, nm->idFrom, (LPARAM)nm) : 0;
}

static int sx(LPARAM lp) { return (short)(lp & 0xFFFF); }
static int sy(LPARAM lp) { return (short)((lp >> 16) & 0xFFFF); }

// ---- Progress bar ----------------------------------------------------------------------------------------

typedef struct {
  int lo, hi, pos, step;
  COLORREF bar, bk;
} pb_t;

static int pb_clamp(const pb_t *p, int v) { return v < p->lo ? p->lo : (v > p->hi ? p->hi : v); }

static void pb_paint(HWND h, pb_t *p) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT r;
  DWORD st = style_of(h);
  int w, hh, vert = (st & PBS_VERTICAL) != 0, l, t, rr, b, len, filled, i;
  COLORREF bar = p->bar == CLR_DEFAULT ? RGB(6, 176, 37) : p->bar;

  GetClientRect(h, &r);
  w = r.right;
  hh = r.bottom;
  fill(dc, 0, 0, w, 1, LO);                                      // the sunken frame
  fill(dc, 0, 0, 1, hh, LO);
  fill(dc, 0, hh - 1, w, hh, HI);
  fill(dc, w - 1, 0, w, hh, HI);
  l = 1;
  t = 1;
  rr = w - 1;
  b = hh - 1;
  fill(dc, l, t, rr, b, p->bk == CLR_DEFAULT ? face() : p->bk);
  len = vert ? b - t : rr - l;
  filled = p->hi > p->lo ? (int)((long long)(p->pos - p->lo) * len / (p->hi - p->lo)) : 0;
  if (st & PBS_SMOOTH) {
    if (vert) fill(dc, l, b - filled, rr, b, bar);
    else fill(dc, l, t, l + filled, b, bar);
  } else {
    for (i = 0; i * 10 < filled; ++i) {                          // blocks 8 wide with a gap of 2
      int e = i * 10 + 8 < filled ? i * 10 + 8 : filled;

      if (vert) fill(dc, l, b - e, rr, b - i * 10, bar);
      else fill(dc, l + i * 10, t, l + e, b, bar);
    }
  }
  EndPaint(h, &ps);
}

static LRESULT CALLBACK progress_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  pb_t *p = (pb_t *)rt_wnd_ctl(h);
  int old;

  switch (m) {
    case WM_NCCREATE:
      p = rt_alloc(sizeof(*p));
      if (p == 0) {
        return 0;
      }
      p->lo = 0;
      p->hi = 100;
      p->pos = 0;
      p->step = 10;
      p->bar = CLR_DEFAULT;
      p->bk = CLR_DEFAULT;
      rt_wnd_set_ctl(h, p);
      return 1;
    case WM_NCDESTROY:
      rt_free(p);
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (p != 0) pb_paint(h, p);
      return 0;
    case WM_SIZE:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case PBM_SETRANGE: case PBM_SETRANGE32:
      if (p == 0) return 0;
      old = MAKELONG(p->lo, p->hi);
      p->lo = m == PBM_SETRANGE ? (int)LOWORD(lp) : (int)wp;
      p->hi = m == PBM_SETRANGE ? (int)HIWORD(lp) : (int)lp;
      if (p->hi < p->lo) p->hi = p->lo;
      p->pos = pb_clamp(p, p->pos);
      InvalidateRect(h, 0, FALSE);
      return old;
    case PBM_GETRANGE:
      if (p == 0) return 0;
      if (lp != 0) {
        ((PBRANGE *)lp)->iLow = p->lo;
        ((PBRANGE *)lp)->iHigh = p->hi;
      }
      return wp ? p->lo : p->hi;
    case PBM_SETPOS: case PBM_DELTAPOS:
      if (p == 0) return 0;
      old = p->pos;
      p->pos = pb_clamp(p, m == PBM_SETPOS ? (int)wp : p->pos + (int)wp);
      InvalidateRect(h, 0, FALSE);
      return old;
    case PBM_STEPIT:
      if (p == 0) return 0;
      old = p->pos;
      p->pos += p->step;
      if (p->pos > p->hi) p->pos = p->lo;
      if (p->pos < p->lo) p->pos = p->lo;
      InvalidateRect(h, 0, FALSE);
      return old;
    case PBM_SETSTEP:
      if (p == 0) return 0;
      old = p->step;
      p->step = (int)wp;
      return old;
    case PBM_GETPOS:
      return p != 0 ? p->pos : 0;
    case PBM_GETSTEP:
      return p != 0 ? p->step : 0;
    case PBM_SETBARCOLOR: case PBM_SETBKCOLOR: {
      COLORREF prev;

      if (p == 0) return 0;
      prev = m == PBM_SETBARCOLOR ? p->bar : p->bk;
      if (m == PBM_SETBARCOLOR) p->bar = (COLORREF)lp;
      else p->bk = (COLORREF)lp;
      InvalidateRect(h, 0, FALSE);
      return (LRESULT)prev;
    }
    case PBM_GETBARCOLOR:
      return p != 0 ? (LRESULT)p->bar : 0;
    case PBM_GETBKCOLOR:
      return p != 0 ? (LRESULT)p->bk : 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- Status bar ------------------------------------------------------------------------------------------

#define STB_MAX 16
#define STB_H 22

typedef struct {
  int n;                      // parts
  int edge[STB_MAX];          // right edge of each part, -1 = the right end of the bar
  char txt[STB_MAX][96];
  int type[STB_MAX];
  int simple, minh;
  char stxt[96];
} stb_t;

static void stb_rect(HWND h, const stb_t *s, int i, RECT *r) {
  RECT c;

  GetClientRect(h, &c);
  r->left = i > 0 ? s->edge[i - 1] : 0;
  r->right = s->edge[i] < 0 ? c.right : s->edge[i];
  r->top = 0;
  r->bottom = c.bottom;
}

static void stb_pane(HDC dc, int l, int r, int hh, int last, const char *s, int type) {
  int pl = l == 0 ? 2 : l + 1, pr = r - (last ? 2 : 1), n, room;

  if (!(type & SBT_NOBORDERS)) {
    fill(dc, pl, 2, pr, 3, (type & SBT_POPOUT) ? HI : LO);        // sunken, or raised with SBT_POPOUT
    fill(dc, pl, 2, pl + 1, hh - 2, (type & SBT_POPOUT) ? HI : LO);
    fill(dc, pl, hh - 3, pr, hh - 2, (type & SBT_POPOUT) ? LO : HI);
    fill(dc, pr - 1, 2, pr, hh - 2, (type & SBT_POPOUT) ? LO : HI);
  }
  n = slen(s);
  room = (pr - pl - 8) / 8;
  if (n > room) n = room > 0 ? room : 0;
  text(dc, pl + 4, (hh - 8) / 2, s, n, RGB(0, 0, 0));
}

static void stb_paint(HWND h, stb_t *s) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT c, pr;
  int i;

  GetClientRect(h, &c);
  fill(dc, 0, 0, c.right, c.bottom, face());
  if (s->simple) {
    stb_pane(dc, 0, c.right, c.bottom, 1, s->stxt, 0);
  } else {
    for (i = 0; i < s->n; ++i) {
      stb_rect(h, s, i, &pr);
      stb_pane(dc, pr.left, pr.right, c.bottom, i == s->n - 1, s->txt[i], s->type[i]);
    }
  }
  if (style_of(h) & SBARS_SIZEGRIP) {
    for (i = 0; i < 3; ++i) {                                     // the grip: diagonal rows of dots
      int k;

      for (k = 0; k <= i; ++k) {
        fill(dc, c.right - 4 - 4 * k, c.bottom - 4 - 4 * (2 - i), c.right - 2 - 4 * k, c.bottom - 2 - 4 * (2 - i), LO);
      }
    }
  }
  EndPaint(h, &ps);
}

// The bar sits against the bottom (or, with CCS_TOP, the top) of its parent, as wide as the parent's client area.
static void stb_autosize(HWND h, stb_t *s) {
  HWND p = GetParent(h);
  RECT pr;
  DWORD st = style_of(h);
  int hh = s->minh > STB_H ? s->minh : STB_H;

  if (p == 0 || (st & CCS_NORESIZE)) {
    return;
  }
  GetClientRect(p, &pr);
  MoveWindow(h, 0, (st & CCS_TOP) ? 0 : pr.bottom - hh, pr.right, hh, TRUE);
}

static void stb_settext(stb_t *s, int part, const char *t) {
  int i = part & 0xFF, type = part & 0xFF00;

  if (i == 255) {
    scopy(s->stxt, t, sizeof(s->stxt));
    return;
  }
  if (i < s->n) {
    scopy(s->txt[i], t, sizeof(s->txt[i]));
    s->type[i] = type;
  }
}

static LRESULT CALLBACK statusbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  stb_t *s = (stb_t *)rt_wnd_ctl(h);
  int i;

  switch (m) {
    case WM_NCCREATE:
      s = rt_alloc(sizeof(*s));
      if (s == 0) {
        return 0;
      }
      rt_memset(s, 0, sizeof(*s));
      s->n = 1;
      s->edge[0] = -1;
      rt_wnd_set_ctl(h, s);
      return 1;
    case WM_NCDESTROY:
      rt_free(s);
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_CREATE:
      if (s != 0) {
        const CREATESTRUCTA *cs = (const CREATESTRUCTA *)lp;

        if (cs != 0 && cs->lpszName != 0) scopy(s->txt[0], cs->lpszName, sizeof(s->txt[0]));
        stb_autosize(h, s);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (s != 0) stb_paint(h, s);
      return 0;
    case WM_SIZE:
      if (s != 0) stb_autosize(h, s);
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_SETTEXT:
      if (s == 0) return FALSE;
      scopy(s->txt[0], (const char *)lp, sizeof(s->txt[0]));
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case WM_GETTEXT:
      if (s == 0 || lp == 0 || wp == 0) return 0;
      scopy((char *)lp, s->txt[0], (int)wp);
      return slen((char *)lp);
    case WM_GETTEXTLENGTH:
      return s != 0 ? slen(s->txt[0]) : 0;
    case SB_SETPARTS: {
      const int *e = (const int *)lp;

      if (s == 0 || e == 0 || wp == 0 || wp > STB_MAX) return FALSE;
      for (i = 0; i < (int)wp; ++i) {
        s->edge[i] = e[i];
        if (i >= s->n) {
          s->txt[i][0] = 0;
          s->type[i] = 0;
        }
      }
      s->n = (int)wp;
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    }
    case SB_GETPARTS: {
      int *e = (int *)lp;

      if (s == 0) return 0;
      for (i = 0; e != 0 && i < (int)wp && i < s->n; ++i) e[i] = s->edge[i];
      return s->n;
    }
    case SB_SETTEXTA:
      if (s == 0) return FALSE;
      stb_settext(s, (int)wp, (const char *)lp);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case SB_GETTEXTA: case SB_GETTEXTLENGTHA: {
      const char *t;
      int type = 0;

      if (s == 0) return 0;
      i = (int)wp & 0xFF;
      if (i == 255) {
        t = s->stxt;
      } else if (i < s->n) {
        t = s->txt[i];
        type = s->type[i];
      } else {
        return 0;
      }
      if (m == SB_GETTEXTA && lp != 0) scopy((char *)lp, t, 96);
      return (LRESULT)(slen(t) | (type << 16));
    }
    case SB_SIMPLE:
      if (s == 0) return FALSE;
      s->simple = wp != 0;
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case SB_ISSIMPLE:
      return s != 0 && s->simple;
    case SB_SETMINHEIGHT:
      if (s != 0) s->minh = (int)wp;
      return 0;
    case SB_GETRECT:
      if (s == 0 || lp == 0 || (int)wp >= s->n) return FALSE;
      stb_rect(h, s, (int)wp, (RECT *)lp);
      return TRUE;
    case SB_GETBORDERS:
      if (lp != 0) {
        ((int *)lp)[0] = 2;
        ((int *)lp)[1] = 2;
        ((int *)lp)[2] = 2;
      }
      return TRUE;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- Trackbar --------------------------------------------------------------------------------------------

#define TB_PAD 7                // the thumb centre's distance from the end of the control at the minimum
#define TB_THICK 21             // the thumb across, 11 along

typedef struct {
  int lo, hi, pos, page, line, freq;
  int drag, dragoff, pressed, sent;
} tb_t;

static int tb_vert(HWND h) { return (style_of(h) & TBS_VERT) != 0; }

static int tb_clamp(const tb_t *t, int v) { return v < t->lo ? t->lo : (v > t->hi ? t->hi : v); }

static int tb_len(HWND h) {
  RECT r;

  GetClientRect(h, &r);
  return tb_vert(h) ? r.bottom : r.right;
}

static int tb_center(HWND h, const tb_t *t, int pos) {
  int span = tb_len(h) - 2 * TB_PAD - 1;

  return TB_PAD + (t->hi > t->lo ? (int)((long long)(pos - t->lo) * span / (t->hi - t->lo)) : 0);
}

static int tb_pos_at(HWND h, const tb_t *t, int c) {
  int span = tb_len(h) - 2 * TB_PAD - 1;

  if (span <= 0 || t->hi <= t->lo) {
    return t->lo;
  }
  return tb_clamp(t, t->lo + (int)(((long long)(c - TB_PAD) * (t->hi - t->lo) + span / 2) / span));
}

static void tb_thumb(HWND h, const tb_t *t, RECT *r) {
  RECT c;
  int ce = tb_center(h, t, t->pos), cross;

  GetClientRect(h, &c);
  cross = tb_vert(h) ? c.right : c.bottom;
  cross = (cross - TB_THICK) / 2;
  if (tb_vert(h)) SetRect(r, cross, ce - 5, cross + TB_THICK, ce + 6);
  else SetRect(r, ce - 5, cross, ce + 6, cross + TB_THICK);
}

static void tb_paint(HWND h, tb_t *t) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT c, th;
  DWORD st = style_of(h);
  int vert = tb_vert(h), len, cross, mid, v;

  GetClientRect(h, &c);
  fill(dc, 0, 0, c.right, c.bottom, face());
  len = vert ? c.bottom : c.right;
  cross = vert ? c.right : c.bottom;
  mid = cross / 2;
  if (vert) {                                                     // the channel
    fill(dc, mid - 2, 5, mid + 2, len - 5, RGB(227, 227, 227));
    fill(dc, mid - 2, 5, mid + 2, 6, LO);
    fill(dc, mid - 2, 5, mid - 1, len - 5, LO);
    fill(dc, mid + 1, 5, mid + 2, len - 5, HI);
    fill(dc, mid - 2, len - 6, mid + 2, len - 5, HI);
  } else {
    fill(dc, 5, mid - 2, len - 5, mid + 2, RGB(227, 227, 227));
    fill(dc, 5, mid - 2, len - 5, mid - 1, LO);
    fill(dc, 5, mid - 2, 6, mid + 2, LO);
    fill(dc, 5, mid + 1, len - 5, mid + 2, HI);
    fill(dc, len - 6, mid - 2, len - 5, mid + 2, HI);
  }
  if ((st & TBS_AUTOTICKS) && !(st & TBS_NOTICKS)) {
    int f = t->freq > 0 ? t->freq : 1;

    for (v = t->lo; v <= t->hi; v += f) {
      int ce = tb_center(h, t, v), edge = (cross + TB_THICK) / 2 + 2;

      if (vert) fill(dc, edge, ce, edge + 4, ce + 1, RGB(0, 0, 0));
      else fill(dc, ce, edge, ce + 1, edge + 4, RGB(0, 0, 0));
    }
  }
  tb_thumb(h, t, &th);
  fill(dc, th.left, th.top, th.right, th.bottom, face());
  fill(dc, th.left, th.top, th.right - 1, th.top + 1, HI);
  fill(dc, th.left, th.top, th.left + 1, th.bottom - 1, HI);
  fill(dc, th.left, th.bottom - 1, th.right, th.bottom, DK);
  fill(dc, th.right - 1, th.top, th.right, th.bottom, DK);
  if (GetFocus() == h) {
    fill(dc, th.left + 2, th.top + 2, th.right - 3, th.top + 3, RGB(0, 0, 0));
    fill(dc, th.left + 2, th.bottom - 3, th.right - 3, th.bottom - 2, RGB(0, 0, 0));
  }
  EndPaint(h, &ps);
}

// A scroll notification to the parent: the thumb codes carry the position.
static void tb_send(HWND h, tb_t *t, int code) {
  HWND p = GetParent(h);
  int hi = (code == TB_THUMBTRACK || code == TB_THUMBPOSITION) ? (t->pos & 0xFFFF) : 0;

  if (p != 0) {
    SendMessageA(p, tb_vert(h) ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(code, hi), (LPARAM)h);
  }
}

static void tb_setpos(HWND h, tb_t *t, int pos, int code) {
  pos = tb_clamp(t, pos);
  if (pos != t->pos) {
    t->pos = pos;
    InvalidateRect(h, 0, FALSE);
  }
  tb_send(h, t, code);
}

static LRESULT CALLBACK trackbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  tb_t *t = (tb_t *)rt_wnd_ctl(h);
  int old;

  switch (m) {
    case WM_NCCREATE:
      t = rt_alloc(sizeof(*t));
      if (t == 0) {
        return 0;
      }
      rt_memset(t, 0, sizeof(*t));
      t->hi = 100;
      t->page = 20;
      t->line = 1;
      t->freq = 1;
      rt_wnd_set_ctl(h, t);
      return 1;
    case WM_NCDESTROY:
      rt_free(t);
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (t != 0) tb_paint(h, t);
      return 0;
    case WM_SIZE: case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
      RECT th;
      int c = tb_vert(h) ? sy(lp) : sx(lp);

      if (t == 0) return 0;
      SetFocus(h);
      tb_thumb(h, t, &th);
      SetCapture(h);
      t->pressed = 1;
      if (sx(lp) >= th.left && sx(lp) < th.right && sy(lp) >= th.top && sy(lp) < th.bottom) {
        t->drag = 1;
        t->dragoff = c - tb_center(h, t, t->pos);
      } else {
        tb_setpos(h, t, t->pos + (c < tb_center(h, t, t->pos) ? -t->page : t->page), c < tb_center(h, t, t->pos) ? TB_PAGEUP : TB_PAGEDOWN);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (t != 0 && t->drag) {
        int np = tb_pos_at(h, t, (tb_vert(h) ? sy(lp) : sx(lp)) - t->dragoff);

        if (np != t->pos) {
          t->pos = np;
          InvalidateRect(h, 0, FALSE);
          tb_send(h, t, TB_THUMBTRACK);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (t != 0 && t->pressed) {
        ReleaseCapture();
        if (t->drag) {
          tb_send(h, t, TB_THUMBPOSITION);
        }
        t->drag = 0;
        t->pressed = 0;
        tb_send(h, t, TB_ENDTRACK);
      }
      return 0;
    case WM_KEYDOWN: {
      int code = -1, np = t != 0 ? t->pos : 0;

      if (t == 0) return 0;
      switch (wp) {
        case VK_LEFT: case VK_UP: code = TB_LINEUP; np -= t->line; break;
        case VK_RIGHT: case VK_DOWN: code = TB_LINEDOWN; np += t->line; break;
        case VK_PRIOR: code = TB_PAGEUP; np -= t->page; break;
        case VK_NEXT: code = TB_PAGEDOWN; np += t->page; break;
        case VK_HOME: code = TB_TOP; np = t->lo; break;
        case VK_END: code = TB_BOTTOM; np = t->hi; break;
        default: break;
      }
      if (code >= 0) {
        t->sent = 1;
        tb_setpos(h, t, np, code);
      }
      return 0;
    }
    case WM_KEYUP:
      if (t != 0 && t->sent) {
        t->sent = 0;
        tb_send(h, t, TB_ENDTRACK);
      }
      return 0;
    case TBM_GETPOS:
      return t != 0 ? t->pos : 0;
    case TBM_SETPOS:
      if (t == 0) return 0;
      t->pos = tb_clamp(t, (int)lp);
      if (wp) InvalidateRect(h, 0, FALSE);
      return 0;
    case TBM_GETRANGEMIN:
      return t != 0 ? t->lo : 0;
    case TBM_GETRANGEMAX:
      return t != 0 ? t->hi : 0;
    case TBM_SETRANGE: case TBM_SETRANGEMIN: case TBM_SETRANGEMAX:
      if (t == 0) return 0;
      if (m == TBM_SETRANGE) {
        t->lo = (short)LOWORD(lp);
        t->hi = (short)HIWORD(lp);
      } else if (m == TBM_SETRANGEMIN) {
        t->lo = (int)lp;
      } else {
        t->hi = (int)lp;
      }
      if (t->hi < t->lo) t->hi = t->lo;
      t->pos = tb_clamp(t, t->pos);
      if (wp) InvalidateRect(h, 0, FALSE);
      return 0;
    case TBM_SETTICFREQ:
      if (t != 0) t->freq = (int)wp > 0 ? (int)wp : 1;
      InvalidateRect(h, 0, FALSE);
      return 0;
    case TBM_CLEARTICS:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case TBM_SETPAGESIZE:
      if (t == 0) return 0;
      old = t->page;
      t->page = (int)lp > 0 ? (int)lp : 1;
      return old;
    case TBM_GETPAGESIZE:
      return t != 0 ? t->page : 0;
    case TBM_SETLINESIZE:
      if (t == 0) return 0;
      old = t->line;
      t->line = (int)lp > 0 ? (int)lp : 1;
      return old;
    case TBM_GETLINESIZE:
      return t != 0 ? t->line : 0;
    case TBM_GETTHUMBRECT:
      if (t != 0 && lp != 0) tb_thumb(h, t, (RECT *)lp);
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- Up-down ---------------------------------------------------------------------------------------------

#define UD_TIMER 1
#define UD_W 17

typedef struct {
  int lo, hi, pos;
  HWND buddy;
  int down;                   // the button held: 1 = the "up" one (increase), 2 = the other, 0 = none
  int first;                  // the next timer tick is the first repeat
} ud_t;

static int ud_horz(HWND h) { return (style_of(h) & UDS_HORZ) != 0; }

static int ud_min(const ud_t *u) { return u->lo < u->hi ? u->lo : u->hi; }
static int ud_max(const ud_t *u) { return u->lo < u->hi ? u->hi : u->lo; }
static int ud_clamp(const ud_t *u, int v) { return v < ud_min(u) ? ud_min(u) : (v > ud_max(u) ? ud_max(u) : v); }

static void ud_buddy_text(HWND h, ud_t *u) {
  char b[16];
  int v = u->pos, n = 0, i;
  char tmp[16];

  if (u->buddy == 0 || !(style_of(h) & UDS_SETBUDDYINT) || !IsWindow(u->buddy)) {
    return;
  }
  if (v < 0) {
    b[n++] = '-';
    v = -v;
  }
  i = 0;
  do {
    tmp[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v > 0);
  while (i > 0) b[n++] = tmp[--i];
  b[n] = 0;
  SetWindowTextA(u->buddy, b);
}

// Which button is at (x, y): 1 = increase, 2 = decrease, 0 = none.
static int ud_part(HWND h, int x, int y) {
  RECT r;

  GetClientRect(h, &r);
  if (x < 0 || y < 0 || x >= r.right || y >= r.bottom) {
    return 0;
  }
  if (ud_horz(h)) return x >= r.right / 2 ? 1 : 2;
  return y < r.bottom / 2 ? 1 : 2;
}

static void ud_glyph(HDC dc, int cx, int cy, int dir, int pressed) {
  int k;
  COLORREF c = RGB(0, 0, 0);

  cx += pressed;
  cy += pressed;
  for (k = 0; k < 3; ++k) {                                       // dir: 0 up, 1 down, 2 left, 3 right
    int a = 2 - k, b = k;

    if (dir == 0) fill(dc, cx - b, cy - 1 + k, cx + b + 1, cy + k, c);
    else if (dir == 1) fill(dc, cx - a, cy - 1 + k, cx + a + 1, cy + k, c);
    else if (dir == 2) fill(dc, cx - 1 + k, cy - b, cx + k, cy + b + 1, c);
    else fill(dc, cx - 1 + k, cy - a, cx + k, cy + a + 1, c);
  }
}

static void ud_paint(HWND h, ud_t *u) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT r;
  int hz = ud_horz(h), mid;

  GetClientRect(h, &r);
  if (hz) {
    mid = r.right / 2;
    bevel(dc, 0, 0, mid, r.bottom, u->down == 2);
    bevel(dc, mid, 0, r.right, r.bottom, u->down == 1);
    ud_glyph(dc, mid / 2, r.bottom / 2, 2, u->down == 2);
    ud_glyph(dc, mid + (r.right - mid) / 2, r.bottom / 2, 3, u->down == 1);
  } else {
    mid = r.bottom / 2;
    bevel(dc, 0, 0, r.right, mid, u->down == 1);
    bevel(dc, 0, mid, r.right, r.bottom, u->down == 2);
    ud_glyph(dc, r.right / 2, mid / 2 + 1, 0, u->down == 1);
    ud_glyph(dc, r.right / 2, mid + (r.bottom - mid) / 2, 1, u->down == 2);
  }
  EndPaint(h, &ps);
}

// One step of `delta`: the parent may cancel or change it (UDN_DELTAPOS), then the position, the buddy and the parent follow.
static void ud_step(HWND h, ud_t *u, int delta) {
  NMUPDOWN nm;
  int np;
  HWND p = GetParent(h);

  nm.iPos = u->pos;
  nm.iDelta = delta;
  if (notify(h, UDN_DELTAPOS, &nm, sizeof(nm)) != 0) {
    return;
  }
  np = u->pos + nm.iDelta;
  if (np > ud_max(u)) np = (style_of(h) & UDS_WRAP) ? ud_min(u) : ud_max(u);
  if (np < ud_min(u)) np = (style_of(h) & UDS_WRAP) ? ud_max(u) : ud_min(u);
  u->pos = np;
  ud_buddy_text(h, u);
  InvalidateRect(h, 0, FALSE);
  if (p != 0) {
    SendMessageA(p, ud_horz(h) ? WM_HSCROLL : WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, np & 0xFFFF), (LPARAM)h);
  }
}

static void ud_set_buddy(HWND h, ud_t *u, HWND b) {
  DWORD st = style_of(h);

  u->buddy = b;
  if (b != 0 && (st & (UDS_ALIGNRIGHT | UDS_ALIGNLEFT)) && GetParent(b) == GetParent(h)) {
    RECT r;
    POINT pt;
    int w, bh;
    HWND p = GetParent(h);

    GetWindowRect(b, &r);
    pt.x = r.left;
    pt.y = r.top;
    ScreenToClient(p, &pt);
    w = r.right - r.left;
    bh = r.bottom - r.top;
    if (st & UDS_ALIGNRIGHT) {
      MoveWindow(b, pt.x, pt.y, w - UD_W, bh, TRUE);
      MoveWindow(h, pt.x + w - UD_W, pt.y, UD_W, bh, TRUE);
    } else {
      MoveWindow(h, pt.x, pt.y, UD_W, bh, TRUE);
      MoveWindow(b, pt.x + UD_W, pt.y, w - UD_W, bh, TRUE);
    }
  }
  ud_buddy_text(h, u);
}

static LRESULT CALLBACK updown_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  ud_t *u = (ud_t *)rt_wnd_ctl(h);
  int old;

  switch (m) {
    case WM_NCCREATE:
      u = rt_alloc(sizeof(*u));
      if (u == 0) {
        return 0;
      }
      rt_memset(u, 0, sizeof(*u));
      u->hi = 100;
      rt_wnd_set_ctl(h, u);
      return 1;
    case WM_NCDESTROY:
      if (u != 0) KillTimer(h, UD_TIMER);
      rt_free(u);
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (u != 0) ud_paint(h, u);
      return 0;
    case WM_SIZE: case WM_ENABLE:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
      if (u == 0) return 0;
      u->down = ud_part(h, sx(lp), sy(lp));
      if (u->down != 0) {
        SetCapture(h);
        InvalidateRect(h, 0, FALSE);
        ud_step(h, u, u->down == 1 ? 1 : -1);
        u->first = 1;
        SetTimer(h, UD_TIMER, 400, 0);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (u != 0 && u->down != 0) {
        int on = ud_part(h, sx(lp), sy(lp)) == u->down;

        if (!on) {                                               // the mouse left the button: no repeat until it is back
          KillTimer(h, UD_TIMER);
          u->first = 2;
        } else if (u->first == 2) {
          u->first = 0;
          SetTimer(h, UD_TIMER, 100, 0);
        }
      }
      return 0;
    case WM_TIMER:
      if (u != 0 && wp == UD_TIMER && u->down != 0) {
        if (u->first == 1) {
          u->first = 0;
          SetTimer(h, UD_TIMER, 100, 0);
        }
        ud_step(h, u, u->down == 1 ? 1 : -1);
      }
      return 0;
    case WM_LBUTTONUP:
      if (u != 0 && u->down != 0) {
        HWND p = GetParent(h);

        KillTimer(h, UD_TIMER);
        ReleaseCapture();
        u->down = 0;
        InvalidateRect(h, 0, FALSE);
        if (p != 0) {
          SendMessageA(p, ud_horz(h) ? WM_HSCROLL : WM_VSCROLL, MAKEWPARAM(SB_ENDSCROLL, 0), (LPARAM)h);
        }
      }
      return 0;
    case UDM_SETRANGE: case UDM_SETRANGE32:
      if (u == 0) return 0;
      if (m == UDM_SETRANGE) {
        u->hi = (short)LOWORD(lp);
        u->lo = (short)HIWORD(lp);
      } else {
        u->lo = (int)wp;
        u->hi = (int)lp;
      }
      u->pos = ud_clamp(u, u->pos);
      ud_buddy_text(h, u);
      InvalidateRect(h, 0, FALSE);
      return 0;
    case UDM_GETRANGE:
      return u != 0 ? (LRESULT)MAKELONG(u->hi, u->lo) : 0;
    case UDM_GETRANGE32:
      if (u != 0) {
        if (wp) *(int *)wp = u->lo;
        if (lp) *(int *)lp = u->hi;
      }
      return 0;
    case UDM_SETPOS: case UDM_SETPOS32:
      if (u == 0) return 0;
      old = u->pos;
      u->pos = ud_clamp(u, m == UDM_SETPOS ? (int)(short)LOWORD(lp) : (int)lp);
      ud_buddy_text(h, u);
      InvalidateRect(h, 0, FALSE);
      return old;
    case UDM_GETPOS: case UDM_GETPOS32: {
      int err = 0;

      if (u == 0) return 0;
      if (u->buddy != 0 && (style_of(h) & UDS_SETBUDDYINT) && IsWindow(u->buddy)) {   // the buddy's text is the position
        char b[24];
        int i = 0, neg = 0, v = 0, any = 0;

        GetWindowTextA(u->buddy, b, sizeof(b));
        if (b[0] == '-') {
          neg = 1;
          i = 1;
        }
        for (; b[i] >= '0' && b[i] <= '9'; ++i) {
          v = v * 10 + (b[i] - '0');
          any = 1;
        }
        if (!any || b[i] != 0) {
          err = 1;
        } else {
          u->pos = ud_clamp(u, neg ? -v : v);
        }
      }
      if (m == UDM_GETPOS32) {
        if (lp != 0) *(BOOL *)lp = err;
        return u->pos;
      }
      return (LRESULT)MAKELONG(u->pos & 0xFFFF, err);
    }
    case UDM_SETBUDDY:
      if (u == 0) return 0;
      old = 0;
      {
        HWND prev = u->buddy;

        ud_set_buddy(h, u, (HWND)wp);
        (void)old;
        return (LRESULT)prev;
      }
    case UDM_GETBUDDY:
      return u != 0 ? (LRESULT)u->buddy : 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- Tab control -----------------------------------------------------------------------------------------

#define TAB_MAX 32
#define TAB_H 22

typedef struct {
  int n, sel;
  char txt[TAB_MAX][64];
  LPARAM lp[TAB_MAX];
} tab_t;

static int tab_width(const tab_t *t, int i) { return slen(t->txt[i]) * 8 + 16; }

// Tab i: unselected tabs are two pixels shorter, so their top is lower.
static void tab_rect(const tab_t *t, int i, RECT *r) {
  int k, l = 2;

  for (k = 0; k < i; ++k) l += tab_width(t, k);
  SetRect(r, l, 0, l + tab_width(t, i), TAB_H);
}

static int tab_hit(const tab_t *t, int x, int y) {
  int i;

  for (i = 0; i < t->n; ++i) {
    RECT r;

    tab_rect(t, i, &r);
    if (x >= r.left && x < r.right && y >= (i == t->sel ? 0 : 2) && y < TAB_H) {
      return i;
    }
  }
  return -1;
}

static void tab_paint(HWND h, tab_t *t) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT c, r;
  int i;

  GetClientRect(h, &c);
  fill(dc, 0, 0, c.right, c.bottom, face());
  fill(dc, 0, TAB_H - 1, c.right - 1, TAB_H, HI);                // the page below the tabs
  fill(dc, 0, TAB_H - 1, 1, c.bottom - 1, HI);
  fill(dc, 0, c.bottom - 1, c.right, c.bottom, DK);
  fill(dc, c.right - 1, TAB_H - 1, c.right, c.bottom, DK);
  for (i = 0; i < t->n; ++i) {
    int top = i == t->sel ? 0 : 2, n = slen(t->txt[i]);

    tab_rect(t, i, &r);
    fill(dc, r.left, top, r.right, TAB_H, face());
    fill(dc, r.left, top, r.right - 1, top + 1, HI);
    fill(dc, r.left, top, r.left + 1, TAB_H, HI);
    fill(dc, r.right - 1, top, r.right, TAB_H - 1, DK);
    if (i == t->sel) {
      fill(dc, r.left + 1, TAB_H - 1, r.right - 1, TAB_H, face());       // joined to the page
    }
    text(dc, r.left + 8, top + (TAB_H - top - 8) / 2, t->txt[i], n, RGB(0, 0, 0));
  }
  EndPaint(h, &ps);
}

static void tab_select(HWND h, tab_t *t, int i) {
  if (i == t->sel || i < 0 || i >= t->n) {
    return;
  }
  if (notify(h, TCN_SELCHANGING, 0, 0) != 0) {
    return;
  }
  t->sel = i;
  InvalidateRect(h, 0, FALSE);
  notify(h, TCN_SELCHANGE, 0, 0);
}

static LRESULT CALLBACK tab_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  tab_t *t = (tab_t *)rt_wnd_ctl(h);
  int i;

  switch (m) {
    case WM_NCCREATE:
      t = rt_alloc(sizeof(*t));
      if (t == 0) {
        return 0;
      }
      rt_memset(t, 0, sizeof(*t));
      t->sel = -1;
      rt_wnd_set_ctl(h, t);
      return 1;
    case WM_NCDESTROY:
      rt_free(t);
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (t != 0) tab_paint(h, t);
      return 0;
    case WM_SIZE:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
      if (t != 0) {
        SetFocus(h);
        tab_select(h, t, tab_hit(t, sx(lp), sy(lp)));
      }
      return 0;
    case TCM_GETITEMCOUNT:
      return t != 0 ? t->n : 0;
    case TCM_GETROWCOUNT:
      return 1;
    case TCM_GETCURSEL: case TCM_GETCURFOCUS:
      return t != 0 ? t->sel : -1;
    case TCM_SETCURSEL:
      if (t == 0 || (int)wp >= t->n || (int)wp < -1) return -1;
      i = t->sel;
      t->sel = (int)wp;
      InvalidateRect(h, 0, FALSE);
      return i;
    case TCM_INSERTITEMA: {
      const TCITEMA *it = (const TCITEMA *)lp;
      int at = (int)wp;

      if (t == 0 || it == 0 || t->n >= TAB_MAX) return -1;
      if (at < 0 || at > t->n) at = t->n;
      for (i = t->n; i > at; --i) {
        scopy(t->txt[i], t->txt[i - 1], sizeof(t->txt[i]));
        t->lp[i] = t->lp[i - 1];
      }
      t->txt[at][0] = 0;
      t->lp[at] = 0;
      if ((it->mask & TCIF_TEXT) && it->pszText != 0) scopy(t->txt[at], it->pszText, sizeof(t->txt[at]));
      if (it->mask & TCIF_PARAM) t->lp[at] = it->lParam;
      ++t->n;
      if (t->sel < 0) t->sel = 0;
      else if (at <= t->sel) ++t->sel;
      InvalidateRect(h, 0, FALSE);
      return at;
    }
    case TCM_SETITEMA: case TCM_GETITEMA: {
      TCITEMA *it = (TCITEMA *)lp;

      if (t == 0 || it == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      if (m == TCM_SETITEMA) {
        if ((it->mask & TCIF_TEXT) && it->pszText != 0) scopy(t->txt[wp], it->pszText, sizeof(t->txt[wp]));
        if (it->mask & TCIF_PARAM) t->lp[wp] = it->lParam;
        InvalidateRect(h, 0, FALSE);
      } else {
        if ((it->mask & TCIF_TEXT) && it->pszText != 0 && it->cchTextMax > 0) scopy(it->pszText, t->txt[wp], it->cchTextMax);
        if (it->mask & TCIF_PARAM) it->lParam = t->lp[wp];
      }
      return TRUE;
    }
    case TCM_DELETEITEM:
      if (t == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      for (i = (int)wp; i + 1 < t->n; ++i) {
        scopy(t->txt[i], t->txt[i + 1], sizeof(t->txt[i]));
        t->lp[i] = t->lp[i + 1];
      }
      --t->n;
      if (t->n == 0) t->sel = -1;
      else if (t->sel > (int)wp || t->sel >= t->n) --t->sel;
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case TCM_DELETEALLITEMS:
      if (t != 0) {
        t->n = 0;
        t->sel = -1;
      }
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case TCM_GETITEMRECT:
      if (t == 0 || lp == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      tab_rect(t, (int)wp, (RECT *)lp);
      ((RECT *)lp)->top = (int)wp == t->sel ? 0 : 2;
      return TRUE;
    case TCM_HITTEST: {
      TCHITTESTINFO *hi = (TCHITTESTINFO *)lp;

      if (t == 0 || hi == 0) return -1;
      i = tab_hit(t, hi->pt.x, hi->pt.y);
      hi->flags = i >= 0 ? TCHT_ONITEM : TCHT_NOWHERE;
      return i;
    }
    case TCM_ADJUSTRECT: {
      RECT *r = (RECT *)lp;

      if (r == 0) return 0;
      if (wp) {                                                    // display area -> the window rectangle that has it
        r->left -= 3;
        r->top -= TAB_H + 2;
        r->right += 3;
        r->bottom += 3;
      } else {                                                     // window rectangle -> the display area
        r->left += 3;
        r->top += TAB_H + 2;
        r->right -= 3;
        r->bottom -= 3;
      }
      return 0;
    }
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- API and registration --------------------------------------------------------------------------------

EXPORT void WINAPI InitCommonControls(void) {}

EXPORT BOOL WINAPI InitCommonControlsEx(const INITCOMMONCONTROLSEX *picce) {
  return picce != 0 && picce->dwSize >= sizeof(INITCOMMONCONTROLSEX);
}

EXPORT HWND WINAPI CreateStatusWindowA(LONG style, LPCSTR text_, HWND parent, UINT id) {
  return CreateWindowExA(0, STATUSCLASSNAMEA, text_, (DWORD)style, 0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, GetModuleHandleA(0), 0);
}

void rt_comctl_register(void) {
  static const struct { const char *name; WNDPROC proc; UINT style; } kClasses[] = {
    {PROGRESS_CLASSA, progress_proc, 0},
    {STATUSCLASSNAMEA, statusbar_proc, 0},
    {TRACKBAR_CLASSA, trackbar_proc, CS_DBLCLKS},
    {UPDOWN_CLASSA, updown_proc, CS_DBLCLKS},
    {WC_TABCONTROLA, tab_proc, CS_DBLCLKS},
    {WC_LISTVIEWA, rt_listview_proc, CS_DBLCLKS},
  };
  int i;

  for (i = 0; i < (int)(sizeof(kClasses) / sizeof(kClasses[0])); ++i) {
    WNDCLASSEXA wc;

    rt_memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = kClasses[i].style;
    wc.lpfnWndProc = kClasses[i].proc;
    wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClasses[i].name;
    RegisterClassExA(&wc);
  }
}
