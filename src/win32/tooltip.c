// The tool tip control (tooltips_class32) for the Win32 runtime. A tip is drawn over the surface of the window
// that owns it (user32.c makes a WS_POPUP tip a transparent child, since there are no popup windows with a
// surface of their own), above its siblings. Tools are added with TTM_ADDTOOL, by rectangle or by window
// (TTF_IDISHWND); mouse messages reach the tip by TTM_RELAYEVENT or, with TTF_SUBCLASS, by subclassing the
// tool's window. After the initial delay at rest over a tool the tip appears under the pointer, with the
// text of the tool, or what the owner supplies for LPSTR_TEXTCALLBACK in TTN_GETDISPINFO (a string or a
// string resource); it goes away after the autopop time, when the mouse leaves the tool or a button goes down.
// Tracking tips (TTF_TRACK) are shown by TTM_TRACKACTIVATE at the position of TTM_TRACKPOSITION.
//
// Not here: balloons and titles, animation, links, multiple lines from the text's own wrapping except at
// spaces and newlines, TTM_ADJUSTRECT, a tool that is a rectangle in a window other than the owner's top-level
// window's own client area.
#include "internal.h"
#include <commctrl.h>

#define TIP_MAXTOOLS 64
#define T_SHOW 1
#define T_POP 2
#define LINE_H 10

typedef struct {
  UINT flags;
  HWND hwnd;
  UINT_PTR id;
  RECT rect;
  HINSTANCE hinst;
  char *text;                 // an owned copy, or 0
  int kind;                   // 0: text (or none), 1: LPSTR_TEXTCALLBACK, 2: a string resource id in `res`
  UINT res;
  LPARAM lparam;
} tool_t;

typedef struct {
  tool_t tool[TIP_MAXTOOLS];
  int n;
  int active;
  int initial, reshow, autopop;
  int maxw;
  int cur;                    // the tool the tip is about to show or shows
  int state;                  // 0 idle, 1 waiting for the initial delay, 2 shown
  int px, py;                 // the pointer, in the tip's parent's client coordinates
  RECT margin;
  COLORREF bk, tx;
  char text[256];             // what is shown
  int tracking;
  int tx_, ty_;               // tracking position
  RECT last;                  // where the tip was last drawn, in its parent
  int shown_at;               // tick of the last hide, for the reshow delay
} tip_t;

static int slen_(const char *s) { return (int)rt_strlen(s); }

static tool_t *tool_find(tip_t *t, const TTTOOLINFOA *ti) {
  int i;

  for (i = 0; i < t->n; ++i) {
    if (t->tool[i].hwnd == ti->hwnd && t->tool[i].id == ti->uId) {
      return &t->tool[i];
    }
  }
  return 0;
}

static void tool_free(tool_t *tl) {
  rt_free(tl->text);
  tl->text = 0;
}

static void tool_set(tool_t *tl, const TTTOOLINFOA *ti, int cb) {
  const char *s = ti->lpszText;

  tl->flags = ti->uFlags;
  tl->hwnd = ti->hwnd;
  tl->id = ti->uId;
  tl->hinst = ti->hinst;
  tl->rect = ti->rect;
  tl->lparam = cb >= (int)TTTOOLINFOA_V2_SIZE ? ti->lParam : 0;
  tool_free(tl);
  tl->kind = 0;
  tl->res = 0;
  if (s == LPSTR_TEXTCALLBACKA) {
    tl->kind = 1;
  } else if (s != 0 && ((uintptr_t)s >> 16) == 0) {
    tl->kind = 2;
    tl->res = (UINT)(uintptr_t)s;
  } else if (s != 0) {
    tl->text = rt_alloc((size_t)slen_(s) + 1);
    if (tl->text != 0) {
      rt_copy(tl->text, s, (size_t)slen_(s) + 1);
    }
  }
}

static void tool_get(const tool_t *tl, TTTOOLINFOA *ti, int cb) {
  ti->uFlags = tl->flags;
  ti->hwnd = tl->hwnd;
  ti->uId = tl->id;
  ti->rect = tl->rect;
  ti->hinst = tl->hinst;
  if (cb >= (int)TTTOOLINFOA_V2_SIZE) {
    ti->lParam = tl->lparam;
  }
  if (ti->lpszText != 0 && ti->lpszText != LPSTR_TEXTCALLBACKA && ((uintptr_t)ti->lpszText >> 16) != 0 && tl->kind == 0) {
    rt_copy(ti->lpszText, tl->text != 0 ? tl->text : "", 80);    // (the caller's buffer: the size is not given; 80 is what comctl32 documents)
  }
}

// The rectangle of a tool in its own window's client coordinates.
static void tool_rect(const tool_t *tl, RECT *r) {
  if (tl->flags & TTF_IDISHWND) {
    GetClientRect((HWND)tl->id, r);
  } else {
    *r = tl->rect;
  }
}

static HWND tool_window(const tool_t *tl) { return (tl->flags & TTF_IDISHWND) ? (HWND)tl->id : tl->hwnd; }

// The tool under (x, y) in window `w`'s client coordinates.
static int tool_at(tip_t *t, HWND w, int x, int y) {
  int i;

  for (i = 0; i < t->n; ++i) {
    RECT r;

    if (tool_window(&t->tool[i]) != w || (t->tool[i].flags & TTF_TRACK)) {
      continue;
    }
    tool_rect(&t->tool[i], &r);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return i;
    }
  }
  return -1;
}

// The text of a tool; for a callback the owner is asked.
static void tool_text(HWND h, tip_t *t, tool_t *tl, char *out, int max) {
  out[0] = 0;
  if (tl->kind == 0) {
    rt_copy(out, tl->text != 0 ? tl->text : "", (size_t)max);
  } else if (tl->kind == 2) {
    LoadStringA(tl->hinst, tl->res, out, max);
  } else {
    NMTTDISPINFOA nm;

    rt_memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = h;
    nm.hdr.idFrom = tl->id;
    nm.hdr.code = TTN_GETDISPINFOA;
    nm.uFlags = tl->flags;
    nm.lParam = tl->lparam;
    SendMessageA(tl->hwnd, WM_NOTIFY, tl->id, (LPARAM)&nm);
    if (nm.lpszText != 0 && ((uintptr_t)nm.lpszText >> 16) == 0) {
      LoadStringA(nm.hinst, (UINT)(uintptr_t)nm.lpszText, out, max);
    } else if (nm.lpszText != 0) {
      rt_copy(out, nm.lpszText, (size_t)max);
    }
    if ((nm.uFlags & TTF_DI_SETITEM) && nm.lpszText != 0) {
      tool_free(tl);
      tl->kind = 0;
      tl->text = rt_alloc((size_t)slen_(out) + 1);
      if (tl->text != 0) rt_copy(tl->text, out, (size_t)slen_(out) + 1);
    }
  }
  (void)t;
}

// Where the lines of `s` break: line k starts at off[k]. Returns the number of lines and the widest in characters.
static int layout(const tip_t *t, const char *s, int off[16], int len[16], int *widest) {
  int n = 0, i = 0, maxc = t->maxw > 0 ? (t->maxw - t->margin.left - t->margin.right - 2) / 8 : 1000;
  int total = slen_(s);

  if (maxc < 1) maxc = 1;
  *widest = 0;
  while (n < 16 && i <= total) {
    int start = i, end = i, brk = -1;

    while (end < total && s[end] != '\n' && end - start < maxc) {
      if (s[end] == ' ') brk = end;
      ++end;
    }
    if (end < total && s[end] != '\n' && brk > start) {         // too long: break at the last space
      end = brk;
    }
    off[n] = start;
    len[n] = end - start;
    if (len[n] > *widest) *widest = len[n];
    ++n;
    i = end;
    if (i < total && (s[i] == '\n' || s[i] == ' ')) ++i;
    if (i >= total) break;
  }
  return n;
}

static void tip_size(const tip_t *t, int *w, int *h) {
  int off[16], len[16], widest, n = layout(t, t->text, off, len, &widest);

  *w = widest * 8 + t->margin.left + t->margin.right + 2;
  *h = n * LINE_H + t->margin.top + t->margin.bottom + 2;
}

static void tip_hide(HWND h, tip_t *t) {
  KillTimer(h, T_SHOW);
  KillTimer(h, T_POP);
  if (t->state == 2 || t->tracking) {
    if (IsWindowVisible(h)) {
      ShowWindow(h, SW_HIDE);                                   // the parent paints over the place again
    }
    {
      NMHDR nm;
      HWND own = t->cur >= 0 && t->cur < t->n ? t->tool[t->cur].hwnd : 0;

      nm.hwndFrom = h;
      nm.idFrom = t->cur >= 0 && t->cur < t->n ? t->tool[t->cur].id : 0;
      nm.code = TTN_POP;
      if (own != 0) SendMessageA(own, WM_NOTIFY, nm.idFrom, (LPARAM)&nm);
    }
    t->shown_at = (int)GetTickCount();
  }
  t->state = 0;
  t->tracking = 0;
  t->cur = -1;
}

// Show the tip for tool `i` with its top left at (x, y) in the parent's client coordinates.
static void tip_show(HWND h, tip_t *t, int i, int x, int y, int exact) {
  tool_t *tl = &t->tool[i];
  HWND p = GetParent(h);
  RECT pc;
  int w, hh;

  tool_text(h, t, tl, t->text, sizeof(t->text));
  if (t->text[0] == 0) {
    t->state = 0;
    return;
  }
  tip_size(t, &w, &hh);
  GetClientRect(p, &pc);
  if (!exact) {
    if (x + w > pc.right) x = pc.right - w;
    if (y + hh > pc.bottom) y = t->py - hh - 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
  }
  t->cur = i;
  t->state = 2;
  t->last.left = x;
  t->last.top = y;
  t->last.right = x + w;
  t->last.bottom = y + hh;
  SetWindowPos(h, HWND_TOP, x, y, w, hh, SWP_SHOWWINDOW);
  InvalidateRect(h, 0, TRUE);
  if (!t->tracking && t->autopop > 0) {
    SetTimer(h, T_POP, (UINT)t->autopop, 0);
  }
}

static void tip_paint(HWND h, tip_t *t) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT c, r;
  int off[16], len[16], widest, n, k;
  HBRUSH br;

  GetClientRect(h, &c);
  r = c;
  br = CreateSolidBrush(t->bk);
  FillRect(dc, &r, br);
  DeleteObject(br);
  br = CreateSolidBrush(RGB(0, 0, 0));
  FrameRect(dc, &r, br);
  DeleteObject(br);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, t->tx);
  n = layout(t, t->text, off, len, &widest);
  for (k = 0; k < n; ++k) {
    TextOutA(dc, 1 + t->margin.left, 1 + t->margin.top + k * LINE_H + 1, t->text + off[k], len[k]);
  }
  EndPaint(h, &ps);
}

// A mouse or key message the tool's window got (relayed or from the subclass).
static void relay(HWND h, tip_t *t, HWND src, UINT msg, LPARAM lp) {
  POINT pt;
  int i;

  switch (msg) {
    case WM_MOUSEMOVE: {
      HWND p = GetParent(h);

      pt.x = (short)(lp & 0xFFFF);
      pt.y = (short)((lp >> 16) & 0xFFFF);
      i = tool_at(t, src, pt.x, pt.y);
      MapWindowPoints(src, p, &pt, 1);
      t->px = pt.x;
      t->py = pt.y;
      if (!t->active) {
        return;
      }
      if (i < 0) {
        tip_hide(h, t);
        return;
      }
      if (i != t->cur || t->state == 0) {
        int fast = t->state == 2 || (int)(GetTickCount() - (DWORD)t->shown_at) < t->reshow + 50;

        tip_hide(h, t);
        t->cur = i;
        t->state = 1;
        {
          TRACKMOUSEEVENT te;

          te.cbSize = sizeof(te);
          te.dwFlags = TME_LEAVE;
          te.hwndTrack = src;
          te.dwHoverTime = 0;
          TrackMouseEvent(&te);
        }
        SetTimer(h, T_SHOW, (UINT)(fast && t->shown_at != 0 ? t->reshow : t->initial), 0);
      } else if (t->state == 1) {
        SetTimer(h, T_SHOW, (UINT)t->initial, 0);               // (moving inside the tool: wait again)
      }
      return;
    }
    case WM_MOUSELEAVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_CHAR:
      if (!t->tracking) {
        tip_hide(h, t);
      }
      return;
    default:
      return;
  }
}

// Subclassing of tool windows (TTF_SUBCLASS): one entry per window, however many tips it has.
#define SUB_MAX 32
static struct { HWND w; WNDPROC orig; } g_sub[SUB_MAX];
static HWND g_tips[16];

static LRESULT CALLBACK sub_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  WNDPROC orig = 0;
  int i;

  for (i = 0; i < SUB_MAX; ++i) {
    if (g_sub[i].w == w) {
      orig = g_sub[i].orig;
    }
  }
  if (m == WM_MOUSEMOVE || m == WM_MOUSELEAVE || (m >= WM_LBUTTONDOWN && m <= WM_MBUTTONUP) || m == WM_MOUSEWHEEL || m == WM_KEYDOWN) {
    for (i = 0; i < 16; ++i) {
      if (g_tips[i] != 0 && IsWindow(g_tips[i])) {
        tip_t *t = (tip_t *)rt_wnd_ctl(g_tips[i]);
        int k, mine = 0;

        for (k = 0; t != 0 && k < t->n; ++k) {
          if ((t->tool[k].flags & TTF_SUBCLASS) && tool_window(&t->tool[k]) == w) mine = 1;
        }
        if (mine) {
          relay(g_tips[i], t, w, m, lp);
        }
      }
    }
  }
  return orig != 0 ? CallWindowProcA(orig, w, m, wp, lp) : DefWindowProcA(w, m, wp, lp);
}

static void subclass(HWND w) {
  int i, free_ = -1;

  for (i = 0; i < SUB_MAX; ++i) {
    if (g_sub[i].w == w) {
      return;
    }
    if (g_sub[i].w == 0 && free_ < 0) free_ = i;
  }
  if (free_ >= 0) {
    g_sub[free_].w = w;
    g_sub[free_].orig = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)sub_proc);
  }
}

static void remember(HWND h, int add) {
  int i;

  for (i = 0; i < 16; ++i) {
    if (g_tips[i] == h) {
      g_tips[i] = add ? h : 0;
      return;
    }
  }
  if (add) {
    for (i = 0; i < 16; ++i) {
      if (g_tips[i] == 0) {
        g_tips[i] = h;
        return;
      }
    }
  }
}

LRESULT CALLBACK rt_tooltip_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  tip_t *t = (tip_t *)rt_wnd_ctl(h);
  int i;

  switch (m) {
    case WM_NCCREATE:
      t = rt_alloc(sizeof(*t));
      if (t == 0) {
        return 0;
      }
      rt_memset(t, 0, sizeof(*t));
      t->active = 1;
      t->initial = 500;
      t->reshow = 100;
      t->autopop = 5000;
      t->cur = -1;
      t->margin.left = t->margin.right = t->margin.top = t->margin.bottom = 2;
      t->bk = GetSysColor(COLOR_INFOBK);
      t->tx = GetSysColor(COLOR_INFOTEXT);
      rt_wnd_set_ctl(h, t);
      remember(h, 1);
      return 1;
    case WM_NCDESTROY:
      remember(h, 0);
      if (t != 0) {
        for (i = 0; i < t->n; ++i) tool_free(&t->tool[i]);
        rt_free(t);
      }
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (t != 0) tip_paint(h, t);
      return 0;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_TIMER:
      if (t == 0) return 0;
      if (wp == T_SHOW) {
        KillTimer(h, T_SHOW);
        if (t->state == 1 && t->cur >= 0 && t->cur < t->n) {
          tool_t *tl = &t->tool[t->cur];

          if (tl->flags & TTF_CENTERTIP) {
            RECT r;
            POINT p = {0, 0};
            int w, hh;

            tool_rect(tl, &r);
            p.x = (r.left + r.right) / 2;
            p.y = r.bottom;
            MapWindowPoints(tool_window(tl), GetParent(h), &p, 1);
            tool_text(h, t, tl, t->text, sizeof(t->text));
            tip_size(t, &w, &hh);
            tip_show(h, t, t->cur, p.x - w / 2, p.y + 2, 0);
          } else {
            tip_show(h, t, t->cur, t->px, t->py + 20, 0);
          }
        }
      } else if (wp == T_POP) {
        KillTimer(h, T_POP);
        tip_hide(h, t);
      }
      return 0;
    case TTM_ACTIVATE:
      if (t != 0) {
        t->active = wp != 0;
        if (!t->active) tip_hide(h, t);
      }
      return 0;
    case TTM_SETDELAYTIME:
      if (t == 0) return 0;
      switch ((int)wp) {
        case TTDT_AUTOMATIC: t->initial = (short)lp > 0 ? (short)lp : 500; t->reshow = t->initial / 5; t->autopop = t->initial * 10; break;
        case TTDT_RESHOW: t->reshow = (short)lp; break;
        case TTDT_AUTOPOP: t->autopop = (short)lp; break;
        case TTDT_INITIAL: t->initial = (short)lp; break;
        default: break;
      }
      return 0;
    case TTM_GETDELAYTIME:
      if (t == 0) return 0;
      return wp == TTDT_RESHOW ? t->reshow : wp == TTDT_AUTOPOP ? t->autopop : t->initial;
    case TTM_ADDTOOLA: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;

      if (t == 0 || ti == 0 || ti->cbSize < TTTOOLINFOA_V1_SIZE || t->n >= TIP_MAXTOOLS) return FALSE;
      rt_memset(&t->tool[t->n], 0, sizeof(t->tool[0]));
      tool_set(&t->tool[t->n], ti, (int)ti->cbSize);
      if (ti->uFlags & TTF_SUBCLASS) {
        subclass(tool_window(&t->tool[t->n]));
      }
      ++t->n;
      return TRUE;
    }
    case TTM_DELTOOLA: {
      tool_t *tl = t != 0 && lp != 0 ? tool_find(t, (TTTOOLINFOA *)lp) : 0;

      if (tl == 0) return 0;
      i = (int)(tl - t->tool);
      if (i == t->cur) tip_hide(h, t);
      tool_free(tl);
      {
        int at = i;

        for (; i + 1 < t->n; ++i) t->tool[i] = t->tool[i + 1];
        --t->n;
        if (t->cur > at) --t->cur;
      }
      return 0;
    }
    case TTM_NEWTOOLRECTA: {
      tool_t *tl = t != 0 && lp != 0 ? tool_find(t, (TTTOOLINFOA *)lp) : 0;

      if (tl != 0) tl->rect = ((TTTOOLINFOA *)lp)->rect;
      return 0;
    }
    case TTM_GETTOOLINFOA: case TTM_SETTOOLINFOA: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;
      tool_t *tl = t != 0 && ti != 0 ? tool_find(t, ti) : 0;

      if (tl == 0) return FALSE;
      if (m == TTM_GETTOOLINFOA) tool_get(tl, ti, (int)ti->cbSize);
      else tool_set(tl, ti, (int)ti->cbSize);
      return TRUE;
    }
    case TTM_UPDATETIPTEXTA: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;
      tool_t *tl = t != 0 && ti != 0 ? tool_find(t, ti) : 0;

      if (tl != 0) {
        TTTOOLINFOA keep = *ti;

        keep.uFlags = tl->flags;
        keep.rect = tl->rect;
        keep.hinst = ti->hinst != 0 ? ti->hinst : tl->hinst;
        tool_set(tl, &keep, (int)ti->cbSize);
      }
      return 0;
    }
    case TTM_GETTEXTA: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;
      tool_t *tl = t != 0 && ti != 0 ? tool_find(t, ti) : 0;

      if (tl != 0 && ti->lpszText != 0 && tl->kind == 0) rt_copy(ti->lpszText, tl->text != 0 ? tl->text : "", 80);
      return 0;
    }
    case TTM_GETTOOLCOUNT:
      return t != 0 ? t->n : 0;
    case TTM_ENUMTOOLSA: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;

      if (t == 0 || ti == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      tool_get(&t->tool[wp], ti, (int)ti->cbSize);
      return TRUE;
    }
    case TTM_GETCURRENTTOOLA: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;

      if (t == 0 || t->state != 2 || t->cur < 0) return FALSE;
      if (ti != 0) tool_get(&t->tool[t->cur], ti, (int)ti->cbSize);
      return TRUE;
    }
    case TTM_HITTESTA: {
      TTHITTESTINFOA *hi = (TTHITTESTINFOA *)lp;

      if (t == 0 || hi == 0) return FALSE;
      i = tool_at(t, hi->hwnd, hi->pt.x, hi->pt.y);
      if (i < 0) return FALSE;
      tool_get(&t->tool[i], &hi->ti, (int)hi->ti.cbSize);
      return TRUE;
    }
    case TTM_RELAYEVENT: {
      MSG *msg = (MSG *)lp;

      if (t != 0 && msg != 0) relay(h, t, msg->hwnd, msg->message, msg->lParam);
      return 0;
    }
    case TTM_TRACKACTIVATE: {
      TTTOOLINFOA *ti = (TTTOOLINFOA *)lp;
      tool_t *tl = t != 0 && ti != 0 ? tool_find(t, ti) : 0;

      if (tl == 0) return 0;
      if (wp) {
        tip_hide(h, t);
        t->tracking = 1;
        t->cur = (int)(tl - t->tool);
        tip_show(h, t, t->cur, t->tx_, t->ty_, 1);
        t->tracking = t->state == 2;
      } else {
        t->tracking = 1;
        tip_hide(h, t);
      }
      return 0;
    }
    case TTM_TRACKPOSITION:
      if (t != 0) {
        t->tx_ = (short)(lp & 0xFFFF);
        t->ty_ = (short)((lp >> 16) & 0xFFFF);
        if (t->tracking && t->state == 2) {
          SetWindowPos(h, HWND_TOP, t->tx_, t->ty_, 0, 0, SWP_NOSIZE);
        }
      }
      return 0;
    case TTM_POP:
      if (t != 0) tip_hide(h, t);
      return 0;
    case TTM_POPUP:
      if (t != 0 && t->cur >= 0) tip_show(h, t, t->cur, t->px, t->py + 20, 0);
      return 0;
    case TTM_UPDATE:
      if (t != 0 && t->state == 2) InvalidateRect(h, 0, TRUE);
      return 0;
    case TTM_SETTIPBKCOLOR:
      if (t != 0) t->bk = (COLORREF)wp;
      return 0;
    case TTM_SETTIPTEXTCOLOR:
      if (t != 0) t->tx = (COLORREF)wp;
      return 0;
    case TTM_GETTIPBKCOLOR:
      return t != 0 ? (LRESULT)t->bk : 0;
    case TTM_GETTIPTEXTCOLOR:
      return t != 0 ? (LRESULT)t->tx : 0;
    case TTM_SETMAXTIPWIDTH: {
      int old = t != 0 ? t->maxw : 0;

      if (t != 0) t->maxw = (int)lp < 0 ? 0 : (int)lp;
      return old;
    }
    case TTM_GETMAXTIPWIDTH:
      return t != 0 ? t->maxw : 0;
    case TTM_SETMARGIN:
      if (t != 0 && lp != 0) t->margin = *(RECT *)lp;
      return 0;
    case TTM_GETMARGIN:
      if (t != 0 && lp != 0) *(RECT *)lp = t->margin;
      return 0;
    case TTM_WINDOWFROMPOINT:
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}
