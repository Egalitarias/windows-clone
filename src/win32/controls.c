// The standard controls of user32 for the Win32 runtime: BUTTON (push buttons, check boxes, radio
// buttons, group boxes), STATIC (text and rectangles), EDIT (single-line and multi-line text with
// selection, caret and clipboard) and LISTBOX (a scrolling list with a scroll bar). Each is a
// window class registered at startup with its own window procedure, which draws on the control's
// child window and sends WM_COMMAND notifications to the parent the way Windows does.
//
// Drawing uses the built-in 8x8 font, so a character cell is 8 pixels wide and lines are 10
// apart. WM_CTLCOLORxxx is sent to the parent before painting, so applications can choose colors.
// Not here: COMBOBOX, SCROLLBAR as a control, owner draw, tooltips, accelerators, undo.
#include "internal.h"

#define CH_W 8
#define CH_H 10

static COLORREF face(void) { return GetSysColor(COLOR_BTNFACE); }

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  RECT rc = {l, t, r, b};

  if (r <= l || b <= t) {
    return;
  }
  SetBkColor(dc, c);
  ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &rc, 0, 0, 0);
}

static void frame(HDC dc, int l, int t, int r, int b, COLORREF c) {
  fill(dc, l, t, r, t + 1, c);
  fill(dc, l, b - 1, r, b, c);
  fill(dc, l, t, l + 1, b, c);
  fill(dc, r - 1, t, r, b, c);
}

static void dotted(HDC dc, int l, int t, int r, int b) {
  int i;

  for (i = l; i < r; i += 2) {
    fill(dc, i, t, i + 1, t + 1, RGB(0, 0, 0));
    fill(dc, i, b - 1, i + 1, b, RGB(0, 0, 0));
  }
  for (i = t; i < b; i += 2) {
    fill(dc, l, i, l + 1, i + 1, RGB(0, 0, 0));
    fill(dc, r - 1, i, r, i + 1, RGB(0, 0, 0));
  }
}

static void text(HDC dc, int x, int y, const char *s, int n, COLORREF c) {
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, c);
  TextOutA(dc, x, y, s, n);
}

static DWORD style_of(HWND h) { return (DWORD)GetWindowLongPtrA(h, GWL_STYLE); }

static void notify(HWND h, int code) {
  HWND p = GetParent(h);

  if (p != 0) {
    SendMessageA(p, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), code), (LPARAM)h);
  }
}

// The caption without its '&' mnemonic markers ("&&" stays one '&').
static int plain(const char *s, char *out, int max, int keep_amp) {
  int n = 0;

  for (; *s != 0 && n < max - 1; ++s) {
    if (*s == '&' && !keep_amp) {
      if (s[1] == '&') {
        ++s;
      } else {
        continue;
      }
    }
    out[n++] = *s;
  }
  out[n] = 0;
  return n;
}

static void mv(char *d, const char *s, size_t n) {
  size_t i;

  if (d < s) {
    for (i = 0; i < n; ++i) d[i] = s[i];
  } else {
    for (i = n; i > 0; --i) d[i - 1] = s[i - 1];
  }
}

static int in_client(HWND h, LPARAM lp) {
  RECT r;
  int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF);

  GetClientRect(h, &r);
  return x >= 0 && y >= 0 && x < r.right && y < r.bottom;
}

// ---- BUTTON ----------------------------------------------------------------------------------------------

typedef struct {
  int check;                                  // BST_UNCHECKED, BST_CHECKED, BST_INDETERMINATE
  int pressed;                                // the mouse is down on it
} btn_t;

static LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

static btn_t *btn(HWND h) { return (btn_t *)rt_wnd_ctl(h); }

static void radio_select(HWND h) {
  HWND s = h, c;
  int first = 1;

  while (!(style_of(s) & WS_GROUP)) {                    // the group starts at the nearest WS_GROUP before it
    HWND p = GetWindow(s, GW_HWNDPREV);

    if (p == 0) {
      break;
    }
    s = p;
  }
  for (c = s; c != 0; c = GetWindow(c, GW_HWNDNEXT)) {
    if (!first && (style_of(c) & WS_GROUP)) {
      break;
    }
    first = 0;
    if (GetWindowLongPtrA(c, GWLP_WNDPROC) == (LONG_PTR)button_proc && (style_of(c) & 0xF) == BS_AUTORADIOBUTTON) {
      SendMessageA(c, BM_SETCHECK, c == h ? BST_CHECKED : BST_UNCHECKED, 0);
    }
  }
}

static void button_click(HWND h) {
  btn_t *b = btn(h);
  int type = (int)(style_of(h) & 0xF);

  if (b == 0) {
    return;
  }
  if (type == BS_AUTOCHECKBOX) {
    b->check = !b->check;
  } else if (type == BS_AUTO3STATE) {
    b->check = (b->check + 1) % 3;
  } else if (type == BS_AUTORADIOBUTTON) {
    radio_select(h);
  }
  InvalidateRect(h, 0, FALSE);
  notify(h, BN_CLICKED);
}

static void push_face(HDC dc, int w, int hgt, int down, int deflt) {
  COLORREF hi = RGB(255, 255, 255), sh = RGB(160, 160, 160), dk = RGB(64, 64, 64);

  fill(dc, 0, 0, w, hgt, face());
  if (deflt) {
    frame(dc, 0, 0, w, hgt, RGB(0, 0, 0));
  }
  {
    int o = deflt ? 1 : 0;

    if (down) {
      frame(dc, o, o, w - o, hgt - o, dk);
      frame(dc, o + 1, o + 1, w - o - 1, hgt - o - 1, sh);
    } else {
      fill(dc, o, o, w - o, o + 1, hi);
      fill(dc, o, o, o + 1, hgt - o, hi);
      fill(dc, o, hgt - o - 1, w - o, hgt - o, dk);
      fill(dc, w - o - 1, o, w - o, hgt - o, dk);
      fill(dc, o + 1, hgt - o - 2, w - o - 1, hgt - o - 1, sh);
      fill(dc, w - o - 2, o + 1, w - o - 1, hgt - o - 1, sh);
    }
  }
}

static void check_mark(HDC dc, int x, int y, COLORREF c) {
  int i;

  for (i = 0; i < 3; ++i) fill(dc, x + 3 + i, y + 5 + i, x + 4 + i, y + 8 + i, c);
  for (i = 0; i < 5; ++i) fill(dc, x + 6 + i, y + 7 - i, x + 7 + i, y + 10 - i, c);
}

static void button_paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT rc;
  btn_t *b = btn(h);
  DWORD st = style_of(h);
  int type = (int)(st & 0xF), w, hg, enabled = IsWindowEnabled(h) != 0, focused = GetFocus() == h;
  char cap[128];
  int n;
  COLORREF fg = enabled ? RGB(0, 0, 0) : RGB(128, 128, 128);

  if (dc == 0) {
    return;
  }
  GetClientRect(h, &rc);
  w = rc.right;
  hg = rc.bottom;
  n = plain(rt_wnd_text(h), cap, sizeof(cap), 0);
  if (type == BS_GROUPBOX) {
    HBRUSH br = (HBRUSH)SendMessageA(GetParent(h), WM_CTLCOLORBTN, (WPARAM)dc, (LPARAM)h);
    COLORREF bg = GetBkColor(dc);

    (void)br;                                             // only the title strip is erased: the controls inside are children
    fill(dc, 0, 4, w, 5, RGB(160, 160, 160));
    fill(dc, 0, hg - 1, w, hg, RGB(160, 160, 160));
    fill(dc, 0, 4, 1, hg, RGB(160, 160, 160));
    fill(dc, w - 1, 4, w, hg, RGB(160, 160, 160));
    fill(dc, 1, 5, w - 1, 6, RGB(255, 255, 255));
    fill(dc, 1, hg - 2, w - 1, hg - 1, RGB(255, 255, 255));
    fill(dc, 1, 5, 2, hg - 1, RGB(255, 255, 255));
    fill(dc, w - 2, 5, w - 1, hg - 1, RGB(255, 255, 255));
    if (n > 0) {
      fill(dc, 6, 0, 6 + n * CH_W + 4, 10, bg);
      text(dc, 8, 0, cap, n, fg);
    }
  } else if (type <= BS_DEFPUSHBUTTON || (st & BS_PUSHLIKE) || type == BS_PUSHBOX) {
    int down = (b != 0 && b->pressed) || (b != 0 && b->check == BST_CHECKED && (st & BS_PUSHLIKE));
    int tw = n * CH_W;

    push_face(dc, w, hg, down, type == BS_DEFPUSHBUTTON);
    if (!enabled) {
      text(dc, (w - tw) / 2 + 1, (hg - 8) / 2 + 1, cap, n, RGB(255, 255, 255));
    }
    text(dc, (w - tw) / 2 + (down ? 1 : 0), (hg - 8) / 2 + (down ? 1 : 0), cap, n, fg);
    if (focused) {
      dotted(dc, 4, 4, w - 4, hg - 4);
    }
  } else {                                                // check box or radio button
    int bx = 0, by = (hg - 13) / 2, tw = n * CH_W;
    HBRUSH br = (HBRUSH)SendMessageA(GetParent(h), WM_CTLCOLORBTN, (WPARAM)dc, (LPARAM)h);
    COLORREF bg = GetBkColor(dc);
    int radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    COLORREF inner = (b != 0 && b->pressed) || !enabled ? face() : RGB(255, 255, 255);

    if (br != 0) {
      FillRect(dc, &rc, br);
    } else {
      fill(dc, 0, 0, w, hg, face());
    }
    if (radio) {
      static const unsigned char ex[13] = {4, 2, 1, 1, 0, 0, 0, 0, 0, 1, 1, 2, 4};
      int y;

      for (y = 0; y < 13; ++y) {
        fill(dc, bx + ex[y], by + y, bx + 13 - ex[y], by + y + 1, RGB(105, 105, 105));
        if (y > 0 && y < 12) {
          fill(dc, bx + ex[y] + 1, by + y, bx + 12 - ex[y], by + y + 1, inner);
        }
      }
      if (b != 0 && b->check == BST_CHECKED) {
        static const unsigned char dot[5][2] = {{5, 8}, {4, 9}, {4, 9}, {4, 9}, {5, 8}};

        for (y = 0; y < 5; ++y) {
          fill(dc, bx + dot[y][0], by + 4 + y, bx + dot[y][1], by + 5 + y, enabled ? RGB(0, 0, 0) : RGB(128, 128, 128));
        }
      }
    } else {
      frame(dc, bx, by, bx + 13, by + 13, RGB(105, 105, 105));
      fill(dc, bx + 1, by + 1, bx + 12, by + 12, inner);
      if (b != 0 && b->check == BST_CHECKED) {
        check_mark(dc, bx, by, enabled ? RGB(0, 0, 0) : RGB(128, 128, 128));
      } else if (b != 0 && b->check == BST_INDETERMINATE) {
        fill(dc, bx + 3, by + 3, bx + 10, by + 10, RGB(160, 160, 160));
      }
    }
    SetBkColor(dc, bg);
    if (!enabled) {
      text(dc, 19, (hg - 8) / 2 + 1, cap, n, RGB(255, 255, 255));
    }
    text(dc, 18, (hg - 8) / 2, cap, n, fg);
    if (focused && n > 0) {
      dotted(dc, 16, (hg - 12) / 2, 18 + tw + 3, (hg - 12) / 2 + 12);
    }
  }
  EndPaint(h, &ps);
}

static LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  btn_t *b = btn(h);

  switch (m) {
    case WM_NCCREATE:
      b = rt_alloc(sizeof(*b));
      if (b == 0) {
        return 0;
      }
      rt_memset(b, 0, sizeof(*b));
      rt_wnd_set_ctl(h, b);
      return 1;
    case WM_NCDESTROY:
      rt_free(b);
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      button_paint(h);
      return 0;
    case WM_ENABLE: case WM_SETFONT: case WM_KILLFOCUS:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_SETFOCUS:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_SETTEXT:
      DefWindowProcA(h, m, wp, lp);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case WM_GETDLGCODE:
      return (style_of(h) & 0xF) == BS_DEFPUSHBUTTON ? DLGC_DEFPUSHBUTTON : (style_of(h) & 0xF) == BS_PUSHBUTTON ? DLGC_UNDEFPUSHBUTTON : DLGC_BUTTON;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
      if (b == 0 || (style_of(h) & 0xF) == BS_GROUPBOX) {
        return 0;
      }
      SetFocus(h);
      SetCapture(h);
      b->pressed = 1;
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_MOUSEMOVE:
      if (b != 0 && GetCapture() == h) {
        int inside = in_client(h, lp);

        if (inside != b->pressed) {
          b->pressed = inside;
          InvalidateRect(h, 0, FALSE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (b != 0 && GetCapture() == h) {
        int was = b->pressed;

        ReleaseCapture();
        b->pressed = 0;
        InvalidateRect(h, 0, FALSE);
        if (was && in_client(h, lp)) {
          button_click(h);
        }
      }
      return 0;
    case WM_CHAR:
      if (wp == ' ' && b != 0) {
        button_click(h);
      }
      return 0;
    case BM_CLICK:
      button_click(h);
      return 0;
    case BM_GETCHECK:
      return b != 0 ? b->check : 0;
    case BM_SETCHECK:
      if (b != 0 && b->check != (int)wp) {
        b->check = (int)wp;
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    case BM_GETSTATE:
      return (b != 0 ? b->check : 0) | (b != 0 && b->pressed ? BST_PUSHED : 0) | (GetFocus() == h ? BST_FOCUS : 0);
    case BM_SETSTATE:
      if (b != 0) {
        b->pressed = wp != 0;
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    case BM_SETSTYLE:
      SetWindowLongPtrA(h, GWL_STYLE, (LONG_PTR)(wp));
      if (lp) {
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- STATIC ----------------------------------------------------------------------------------------------

static void static_paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT rc;
  DWORD st = style_of(h);
  int type = (int)(st & 0x1F);
  HBRUSH br;

  if (dc == 0) {
    return;
  }
  GetClientRect(h, &rc);
  br = (HBRUSH)SendMessageA(GetParent(h), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h);
  switch (type) {
    case SS_BLACKRECT: fill(dc, 0, 0, rc.right, rc.bottom, RGB(0, 0, 0)); break;
    case SS_GRAYRECT: fill(dc, 0, 0, rc.right, rc.bottom, RGB(128, 128, 128)); break;
    case SS_WHITERECT: fill(dc, 0, 0, rc.right, rc.bottom, RGB(255, 255, 255)); break;
    case SS_BLACKFRAME: frame(dc, 0, 0, rc.right, rc.bottom, RGB(0, 0, 0)); break;
    case SS_GRAYFRAME: frame(dc, 0, 0, rc.right, rc.bottom, RGB(128, 128, 128)); break;
    case SS_WHITEFRAME: frame(dc, 0, 0, rc.right, rc.bottom, RGB(255, 255, 255)); break;
    case SS_ETCHEDHORZ:
      fill(dc, 0, rc.bottom / 2 - 1, rc.right, rc.bottom / 2, RGB(160, 160, 160));
      fill(dc, 0, rc.bottom / 2, rc.right, rc.bottom / 2 + 1, RGB(255, 255, 255));
      break;
    case SS_ETCHEDVERT:
      fill(dc, rc.right / 2 - 1, 0, rc.right / 2, rc.bottom, RGB(160, 160, 160));
      fill(dc, rc.right / 2, 0, rc.right / 2 + 1, rc.bottom, RGB(255, 255, 255));
      break;
    case SS_ETCHEDFRAME:
      frame(dc, 0, 0, rc.right, rc.bottom, RGB(160, 160, 160));
      frame(dc, 1, 1, rc.right - 1, rc.bottom - 1, RGB(255, 255, 255));
      break;
    default: {
      char cap[256];
      UINT fmt = type == SS_CENTER ? DT_CENTER : type == SS_RIGHT ? DT_RIGHT : DT_LEFT;

      plain(rt_wnd_text(h), cap, sizeof(cap), (st & SS_NOPREFIX) != 0);
      if (br != 0) {
        FillRect(dc, &rc, br);
      } else {
        fill(dc, 0, 0, rc.right, rc.bottom, face());
      }
      fmt |= (type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP || (st & SS_CENTERIMAGE)) ? DT_SINGLELINE : DT_WORDBREAK;
      if (st & SS_CENTERIMAGE) fmt |= DT_VCENTER;
      SetBkMode(dc, TRANSPARENT);
      if (!IsWindowEnabled(h)) SetTextColor(dc, RGB(128, 128, 128));
      DrawTextA(dc, cap, -1, &rc, fmt);
    }
  }
  EndPaint(h, &ps);
}

static LRESULT CALLBACK static_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      static_paint(h);
      return 0;
    case WM_SETTEXT:
      DefWindowProcA(h, m, wp, lp);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case WM_ENABLE: case WM_SETFONT:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
      if (style_of(h) & SS_NOTIFY) {
        notify(h, STN_CLICKED);
      }
      return 0;
    case WM_LBUTTONDBLCLK:
      if (style_of(h) & SS_NOTIFY) {
        notify(h, STN_DBLCLK);
      }
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- EDIT ------------------------------------------------------------------------------------------------
// The text is kept with "\n" line ends (a multi-line control hands out "\r\n" as Windows does);
// selection positions count a line end as one character.

typedef struct {
  char *buf;
  int len, cap;
  int anchor, caret;                          // the selection is between them
  int top, left;                              // first visible line (multi-line) and first visible column
  int limit;
  int readonly, modified, dragging;
  char pw;
} edit_t;

static edit_t *ed(HWND h) { return (edit_t *)rt_wnd_ctl(h); }

static int ed_multi(HWND h) { return (style_of(h) & ES_MULTILINE) != 0; }

static int ed_reserve(edit_t *e, int need) {
  char *nb;
  int cap;

  if (need + 1 <= e->cap) {
    return 1;
  }
  cap = e->cap == 0 ? 64 : e->cap;
  while (cap < need + 1) {
    cap *= 2;
  }
  nb = rt_alloc((size_t)cap);
  if (nb == 0) {
    return 0;
  }
  if (e->buf != 0) {
    rt_memcpy(nb, e->buf, (size_t)e->len);
    rt_free(e->buf);
  }
  e->buf = nb;
  e->cap = cap;
  e->buf[e->len] = 0;
  return 1;
}

static int sel_lo(const edit_t *e) { return e->anchor < e->caret ? e->anchor : e->caret; }
static int sel_hi(const edit_t *e) { return e->anchor < e->caret ? e->caret : e->anchor; }

static int ed_lines(const edit_t *e) {
  int i, n = 1;

  for (i = 0; i < e->len; ++i) {
    n += e->buf[i] == '\n';
  }
  return n;
}

static int line_start(const edit_t *e, int line) {
  int i = 0;

  while (line > 0 && i < e->len) {
    if (e->buf[i++] == '\n') {
      --line;
    }
  }
  return i;
}

static int line_of(const edit_t *e, int pos) {
  int i, n = 0;

  for (i = 0; i < pos && i < e->len; ++i) {
    n += e->buf[i] == '\n';
  }
  return n;
}

static int line_len(const edit_t *e, int line) {
  int s = line_start(e, line), i = s;

  while (i < e->len && e->buf[i] != '\n') {
    ++i;
  }
  return i - s;
}

static void ed_geometry(HWND h, int *cols, int *rows) {
  RECT rc;

  GetClientRect(h, &rc);
  *cols = (rc.right - 8) / CH_W;
  *rows = ed_multi(h) ? (rc.bottom - 4) / CH_H : 1;
  if (*cols < 1) *cols = 1;
  if (*rows < 1) *rows = 1;
}

static void ed_scroll_caret(HWND h, edit_t *e) {
  int cols, rows, ln = ed_multi(h) ? line_of(e, e->caret) : 0;
  int col = e->caret - line_start(e, ln);

  ed_geometry(h, &cols, &rows);
  if (ed_multi(h)) {
    if (ln < e->top) e->top = ln;
    if (ln >= e->top + rows) e->top = ln - rows + 1;
  }
  if (col < e->left) e->left = col;
  if (col > e->left + cols - 1) e->left = col - cols + 1;
}

static void ed_changed(HWND h, edit_t *e) {
  e->modified = 1;
  ed_scroll_caret(h, e);
  InvalidateRect(h, 0, FALSE);
  notify(h, EN_CHANGE);
}

#define ED_NOTIFY 1                                      // send EN_CHANGE
#define ED_USER   2                                      // typing or pasting: ES_NUMBER and the case styles apply

// Replace the selection with `s` (n characters): what typing, pasting and EM_REPLACESEL do.
static void ed_replace(HWND h, edit_t *e, const char *s, int n, int flags) {
  DWORD st = style_of(h);
  char *tmp;
  int lo = sel_lo(e), hi = sel_hi(e), i, k = 0;

  tmp = rt_alloc((size_t)n + 1);
  if (tmp == 0) {
    return;
  }
  for (i = 0; i < n; ++i) {                              // what the control accepts
    char c = s[i];

    if (c == '\r') {
      if (i + 1 < n && s[i + 1] == '\n') continue;
      c = '\n';
    }
    if (c == '\n' && !(st & ES_MULTILINE)) continue;
    if ((unsigned char)c < 32 && c != '\n') continue;
    if ((flags & ED_USER) && (st & ES_NUMBER) && (c < '0' || c > '9')) continue;
    if ((flags & ED_USER) && (st & ES_UPPERCASE) && c >= 'a' && c <= 'z') c = (char)(c - 32);
    if ((flags & ED_USER) && (st & ES_LOWERCASE) && c >= 'A' && c <= 'Z') c = (char)(c + 32);
    tmp[k++] = c;
  }
  if (e->limit > 0 && e->len - (hi - lo) + k > e->limit) {
    k = e->limit - (e->len - (hi - lo));
    if (k < 0) k = 0;
  }
  if (k == 0 && hi == lo) {
    rt_free(tmp);
    return;
  }
  if (!ed_reserve(e, e->len - (hi - lo) + k)) {
    rt_free(tmp);
    return;
  }
  mv(e->buf + lo + k, e->buf + hi, (size_t)(e->len - hi));
  rt_memcpy(e->buf + lo, tmp, (size_t)k);
  e->len += k - (hi - lo);
  e->buf[e->len] = 0;
  e->anchor = e->caret = lo + k;
  rt_free(tmp);
  if (flags & ED_NOTIFY) {
    ed_changed(h, e);
  } else {
    ed_scroll_caret(h, e);
    InvalidateRect(h, 0, FALSE);
  }
}

static void ed_set_sel(HWND h, edit_t *e, int a, int c) {
  if (a < 0 || a > e->len) a = e->len;
  if (c < 0 || c > e->len) c = e->len;
  e->anchor = a;
  e->caret = c;
  ed_scroll_caret(h, e);
  InvalidateRect(h, 0, FALSE);
}

static void ed_move(HWND h, edit_t *e, int pos, int extend) {
  if (pos < 0) pos = 0;
  if (pos > e->len) pos = e->len;
  e->caret = pos;
  if (!extend) {
    e->anchor = pos;
  }
  ed_scroll_caret(h, e);
  InvalidateRect(h, 0, FALSE);
}

static int ed_hit(HWND h, edit_t *e, LPARAM lp) {
  int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF), col = e->left + (x - 3 + CH_W / 2) / CH_W, ln = 0;

  if (x < 3) col = e->left + (x - 3 - CH_W / 2) / CH_W;
  if (ed_multi(h)) {
    ln = e->top + (y - 2) / CH_H;
    if (y < 2) ln = e->top - 1;
    if (ln < 0) ln = 0;
    if (ln >= ed_lines(e)) ln = ed_lines(e) - 1;
  }
  if (col < 0) col = 0;
  if (col > line_len(e, ln)) col = line_len(e, ln);
  return line_start(e, ln) + col;
}

static int is_word(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static void ed_copy(HWND h, edit_t *e) {
  int lo = sel_lo(e), hi = sel_hi(e);

  (void)h;
  if (hi > lo && (style_of(h) & ES_PASSWORD) == 0) {
    sc3(KS_CLIP_SET, (sl_t)(e->buf + lo), hi - lo, 0);
  }
}

static void ed_paste(HWND h, edit_t *e) {
  static char clip[4097];
  sl_t n;

  if (e->readonly) {
    return;
  }
  n = sc3(KS_CLIP_GET, (sl_t)clip, 4096, 0);
  if (n > 0) {
    ed_replace(h, e, clip, (int)n, ED_NOTIFY | ED_USER);
  }
}

static void ed_paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  edit_t *e = ed(h);
  RECT rc;
  HBRUSH br;
  int cols, rows, r, lo, hi, focused = GetFocus() == h, enabled = IsWindowEnabled(h) != 0, multi = ed_multi(h);
  COLORREF bg, fg, hbg = RGB(0, 120, 215), hfg = RGB(255, 255, 255);

  if (dc == 0 || e == 0) {
    if (dc != 0) EndPaint(h, &ps);
    return;
  }
  GetClientRect(h, &rc);
  br = (HBRUSH)SendMessageA(GetParent(h), enabled && !e->readonly ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h);
  (void)br;
  bg = GetBkColor(dc);
  fg = enabled ? GetTextColor(dc) : RGB(128, 128, 128);
  fill(dc, 0, 0, rc.right, rc.bottom, bg);
  frame(dc, 0, 0, rc.right, rc.bottom, RGB(122, 122, 122));
  ed_geometry(h, &cols, &rows);
  lo = sel_lo(e);
  hi = sel_hi(e);
  for (r = 0; r < rows; ++r) {
    int ln = (multi ? e->top : 0) + r, s, n, i, y = multi ? 2 + r * CH_H : (rc.bottom - 8) / 2;
    char line[256];
    int vis_from, vis_n, k;

    if (ln >= ed_lines(e)) {
      break;
    }
    s = line_start(e, ln);
    n = line_len(e, ln);
    vis_from = e->left < n ? e->left : n;
    vis_n = n - vis_from;
    if (vis_n > cols + 1) vis_n = cols + 1;
    for (k = 0, i = 0; i < vis_n && k < 255; ++i, ++k) {
      line[k] = (style_of(h) & ES_PASSWORD) ? (e->pw != 0 ? e->pw : '*') : e->buf[s + vis_from + i];
    }
    line[k] = 0;
    // segments: before the selection, the selection, after it
    {
      int a = s + vis_from, b = a + vis_n;             // buffer range shown
      int x = 3, p = a;

      while (p < b) {
        int in_sel = enabled && p >= lo && p < hi;
        int q = p;

        while (q < b && (enabled && q >= lo && q < hi) == in_sel) {
          ++q;
        }
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, in_sel ? hbg : bg);
        SetTextColor(dc, in_sel ? hfg : fg);
        TextOutA(dc, x, y, line + (p - a), q - p);
        x += (q - p) * CH_W;
        p = q;
      }
      if (enabled && hi > lo && lo <= s + n && hi > s + n && ln < ed_lines(e) - 1 && multi) {   // selected line end: a little highlight
        fill(dc, x, y, x + 4, y + 8, hbg);
      }
    }
  }
  if (focused && enabled) {
    int ln = multi ? line_of(e, e->caret) : 0, col = e->caret - line_start(e, ln);
    int cx = 3 + (col - e->left) * CH_W, cy = multi ? 2 + (ln - e->top) * CH_H : (rc.bottom - 10) / 2;

    if (cx >= 2 && cx < rc.right - 2 && cy >= 0) {
      fill(dc, cx, cy, cx + 1, cy + 10, RGB(0, 0, 0));
    }
  }
  EndPaint(h, &ps);
}

static void ed_key(HWND h, edit_t *e, UINT vk) {
  int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  int multi = ed_multi(h), ln = multi ? line_of(e, e->caret) : 0, col = e->caret - line_start(e, ln), cols, rows;

  ed_geometry(h, &cols, &rows);
  switch (vk) {
    case VK_LEFT:
      if (!shift && e->anchor != e->caret) ed_move(h, e, sel_lo(e), 0);
      else ed_move(h, e, e->caret - 1, shift);
      break;
    case VK_RIGHT:
      if (!shift && e->anchor != e->caret) ed_move(h, e, sel_hi(e), 0);
      else ed_move(h, e, e->caret + 1, shift);
      break;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
      int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -rows : rows, nl;

      if (!multi) {
        ed_move(h, e, vk == VK_UP || vk == VK_PRIOR ? 0 : e->len, shift);
        break;
      }
      nl = ln + d;
      if (nl < 0) nl = 0;
      if (nl >= ed_lines(e)) nl = ed_lines(e) - 1;
      ed_move(h, e, line_start(e, nl) + (col < line_len(e, nl) ? col : line_len(e, nl)), shift);
      break;
    }
    case VK_HOME:
      ed_move(h, e, ctrl ? 0 : line_start(e, ln), shift);
      break;
    case VK_END:
      ed_move(h, e, ctrl ? e->len : line_start(e, ln) + line_len(e, ln), shift);
      break;
    case VK_DELETE:
      if (e->readonly) break;
      if (e->anchor == e->caret && e->caret < e->len) {
        e->anchor = e->caret + 1;
        if (shift) { ed_copy(h, e); }
        ed_replace(h, e, "", 0, ED_NOTIFY);
      } else if (e->anchor != e->caret) {
        if (shift) { ed_copy(h, e); }
        ed_replace(h, e, "", 0, ED_NOTIFY);
      }
      break;
    case VK_INSERT:
      if (shift) ed_paste(h, e);
      else if (ctrl) ed_copy(h, e);
      break;
    default:
      break;
  }
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  edit_t *e = ed(h);

  switch (m) {
    case WM_NCCREATE:
      e = rt_alloc(sizeof(*e));
      if (e == 0) {
        return 0;
      }
      rt_memset(e, 0, sizeof(*e));
      e->readonly = (style_of(h) & ES_READONLY) != 0;
      rt_wnd_set_ctl(h, e);
      return 1;
    case WM_CREATE: {
      const CREATESTRUCTA *cs = (const CREATESTRUCTA *)lp;

      if (e != 0 && cs->lpszName != 0 && ((uintptr_t)cs->lpszName >> 16) != 0) {
        ed_replace(h, e, cs->lpszName, (int)rt_strlen(cs->lpszName), 0);
        e->anchor = e->caret = 0;
      }
      return 0;
    }
    case WM_NCDESTROY:
      if (e != 0) {
        rt_free(e->buf);
        rt_free(e);
      }
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      ed_paint(h);
      return 0;
    case WM_ENABLE: case WM_SETFONT:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_SIZE:
      if (e != 0) ed_scroll_caret(h, e);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_HASSETSEL | DLGC_WANTCHARS | DLGC_WANTARROWS | ((style_of(h) & ES_WANTRETURN) ? DLGC_WANTALLKEYS : 0);
    case WM_SETFOCUS:
      InvalidateRect(h, 0, FALSE);
      notify(h, EN_SETFOCUS);
      return 0;
    case WM_KILLFOCUS:
      if (e != 0) e->dragging = 0;
      InvalidateRect(h, 0, FALSE);
      notify(h, EN_KILLFOCUS);
      return 0;
    case WM_SETTEXT: {
      const char *t = (const char *)lp;

      if (e == 0) return FALSE;
      e->anchor = 0;
      e->caret = e->len;
      ed_replace(h, e, t != 0 ? t : "", t != 0 ? (int)rt_strlen(t) : 0, 0);
      e->anchor = e->caret = 0;
      e->top = e->left = 0;
      e->modified = 0;
      InvalidateRect(h, 0, FALSE);
      notify(h, EN_CHANGE);
      return TRUE;
    }
    case WM_GETTEXTLENGTH: case WM_GETTEXT: {
      int i, n = 0, multi = ed_multi(h);
      char *out = (char *)lp;
      int max = m == WM_GETTEXT ? (int)wp : 0;

      if (e == 0) return 0;
      for (i = 0; i < e->len; ++i) {
        int need = (e->buf[i] == '\n' && multi) ? 2 : 1;

        if (m == WM_GETTEXT) {
          if (n + need > max - 1) break;
          if (need == 2) out[n] = '\r';
          out[n + need - 1] = e->buf[i];
        }
        n += need;
      }
      if (m == WM_GETTEXT && max > 0) out[n] = 0;
      return n;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
      if (e == 0) return 0;
      SetFocus(h);
      if (m == WM_LBUTTONDBLCLK) {                       // select the word
        int p = ed_hit(h, e, lp), a = p, b = p;

        while (a > 0 && is_word(e->buf[a - 1])) --a;
        while (b < e->len && is_word(e->buf[b])) ++b;
        e->anchor = a;
        e->caret = b;
        InvalidateRect(h, 0, FALSE);
        return 0;
      }
      SetCapture(h);
      e->dragging = 1;
      ed_move(h, e, ed_hit(h, e, lp), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
      return 0;
    case WM_MOUSEMOVE:
      if (e != 0 && e->dragging && GetCapture() == h) {
        ed_move(h, e, ed_hit(h, e, lp), 1);
      }
      return 0;
    case WM_LBUTTONUP:
      if (e != 0 && e->dragging) {
        e->dragging = 0;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (e != 0 && ed_multi(h)) {
        int d = -((short)(wp >> 16)) / 120 * 3, n = ed_lines(e);

        e->top += d;
        if (e->top > n - 1) e->top = n - 1;
        if (e->top < 0) e->top = 0;
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    case WM_KEYDOWN:
      if (e != 0) ed_key(h, e, (UINT)wp);
      return 0;
    case WM_CHAR:
      if (e == 0) return 0;
      if (wp == 1) {                                     // Ctrl+A
        ed_set_sel(h, e, 0, e->len);
      } else if (wp == 3) {                              // Ctrl+C
        ed_copy(h, e);
      } else if (wp == 22) {                             // Ctrl+V
        ed_paste(h, e);
      } else if (wp == 24) {                             // Ctrl+X
        if (!e->readonly) {
          ed_copy(h, e);
          ed_replace(h, e, "", 0, ED_NOTIFY);
        }
      } else if (e->readonly) {
        return 0;
      } else if (wp == 8) {                              // backspace
        if (e->anchor == e->caret && e->caret > 0) {
          e->anchor = e->caret - 1;
        }
        if (e->anchor != e->caret) {
          ed_replace(h, e, "", 0, ED_NOTIFY);
        }
      } else if (wp == 13) {
        if (ed_multi(h)) ed_replace(h, e, "\n", 1, ED_NOTIFY | ED_USER);
      } else if (wp >= 32 && wp < 127) {
        char c = (char)wp;

        ed_replace(h, e, &c, 1, ED_NOTIFY | ED_USER);
      }
      return 0;
    case WM_CUT:
      if (e != 0 && !e->readonly) { ed_copy(h, e); ed_replace(h, e, "", 0, ED_NOTIFY); }
      return 0;
    case WM_COPY:
      if (e != 0) ed_copy(h, e);
      return 0;
    case WM_PASTE:
      if (e != 0) ed_paste(h, e);
      return 0;
    case WM_CLEAR:
      if (e != 0 && !e->readonly) ed_replace(h, e, "", 0, ED_NOTIFY);
      return 0;
    case EM_GETSEL:
      if (e == 0) return 0;
      if (wp != 0) *(DWORD *)wp = (DWORD)sel_lo(e);
      if (lp != 0) *(DWORD *)lp = (DWORD)sel_hi(e);
      return MAKELONG(sel_lo(e), sel_hi(e));
    case EM_SETSEL:
      if (e != 0) {
        int a = (int)wp, b = (int)lp;

        if (a == -1) { a = 0; b = 0; }
        ed_set_sel(h, e, a, b);
      }
      return 0;
    case EM_REPLACESEL:
      if (e != 0) ed_replace(h, e, (const char *)lp, (int)rt_strlen((const char *)lp), ED_NOTIFY);
      return 0;
    case EM_LIMITTEXT:                                   // also EM_SETLIMITTEXT
      if (e != 0) e->limit = (int)wp;
      return 0;
    case EM_GETLIMITTEXT:
      return e != 0 ? e->limit : 0;
    case EM_GETLINECOUNT:
      return e != 0 ? ed_lines(e) : 0;
    case EM_LINEFROMCHAR:
      return e != 0 ? line_of(e, (int)wp == -1 ? sel_lo(e) : (int)wp) : 0;
    case EM_LINEINDEX:
      return e != 0 ? ((int)wp == -1 ? line_start(e, line_of(e, e->caret)) : ((int)wp < ed_lines(e) ? line_start(e, (int)wp) : -1)) : -1;
    case EM_LINELENGTH:
      if (e == 0) return 0;
      {
        int p = (int)wp, ln;

        if (p == -1) p = e->caret;
        ln = line_of(e, p);
        return line_len(e, ln);
      }
    case EM_GETLINE: {
      int ln = (int)wp, n, max;
      char *out = (char *)lp;

      if (e == 0 || ln >= ed_lines(e)) return 0;
      max = *(WORD *)out;                                // the buffer's first WORD is its size
      n = line_len(e, ln);
      if (n > max) n = max;
      rt_memcpy(out, e->buf + line_start(e, ln), (size_t)n);
      if (n < max) out[n] = 0;
      return n;
    }
    case EM_SETREADONLY:
      if (e != 0) e->readonly = wp != 0;
      return TRUE;
    case EM_GETMODIFY:
      return e != 0 ? e->modified : 0;
    case EM_SETMODIFY:
      if (e != 0) e->modified = wp != 0;
      return 0;
    case EM_SCROLLCARET:
      if (e != 0) { ed_scroll_caret(h, e); InvalidateRect(h, 0, FALSE); }
      return 0;
    case EM_SETPASSWORDCHAR:
      if (e != 0) { e->pw = (char)wp; InvalidateRect(h, 0, FALSE); }
      return 0;
    case EM_GETFIRSTVISIBLELINE:
      return e != 0 ? e->top : 0;
    case EM_LINESCROLL:
      if (e != 0) { e->top += (int)lp; if (e->top < 0) e->top = 0; InvalidateRect(h, 0, FALSE); }
      return TRUE;
    case EM_CANUNDO:
      return 0;
    case EM_EMPTYUNDOBUFFER:
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- LISTBOX ---------------------------------------------------------------------------------------------

#define ITEM_H 12
#define SB_W   14

typedef struct {
  char *s;
  LPARAM data;
  int sel;
} item_t;

typedef struct {
  item_t *it;
  int n, cap;
  int cur;                                    // the selected item (single selection) or the caret item
  int top;
  int drag;                                   // 1: dragging the thumb (offset in thumb_off), 2: tracking the mouse on the items
  int thumb_off;
} lb_t;

static lb_t *lb(HWND h) { return (lb_t *)rt_wnd_ctl(h); }

static int lb_rows(HWND h) {
  RECT rc;
  int r;

  GetClientRect(h, &rc);
  r = (rc.bottom - 4) / ITEM_H;
  return r < 1 ? 1 : r;
}

static int lb_multi(HWND h) { return (style_of(h) & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0; }

static int lb_scrollbar(HWND h, lb_t *l) { return l->n > lb_rows(h); }

static void lb_clamp_top(HWND h, lb_t *l) {
  int max = l->n - lb_rows(h);

  if (max < 0) max = 0;
  if (l->top > max) l->top = max;
  if (l->top < 0) l->top = 0;
}

static void lb_show(HWND h, lb_t *l, int i) {
  int rows = lb_rows(h);

  if (i < l->top) l->top = i;
  if (i >= l->top + rows) l->top = i - rows + 1;
  lb_clamp_top(h, l);
}

static int lb_insert(HWND h, lb_t *l, int at, const char *s) {
  item_t *ni;
  int i;

  if (l->n == l->cap) {
    int cap = l->cap == 0 ? 16 : l->cap * 2;

    ni = rt_alloc(sizeof(item_t) * (size_t)cap);
    if (ni == 0) {
      return LB_ERRSPACE;
    }
    if (l->it != 0) {
      rt_memcpy(ni, l->it, sizeof(item_t) * (size_t)l->n);
      rt_free(l->it);
    }
    l->it = ni;
    l->cap = cap;
  }
  if (at < 0 || at > l->n) {
    at = l->n;
    if (style_of(h) & LBS_SORT) {                       // sorted: before the first that is greater
      for (i = 0; i < l->n; ++i) {
        if (rt_stricmp(s, l->it[i].s) < 0) {
          break;
        }
      }
      at = i;
    }
  }
  for (i = l->n; i > at; --i) {
    l->it[i] = l->it[i - 1];
  }
  l->it[at].s = rt_alloc(rt_strlen(s) + 1);
  if (l->it[at].s == 0) {
    for (i = at; i < l->n; ++i) l->it[i] = l->it[i + 1];
    return LB_ERRSPACE;
  }
  rt_copy(l->it[at].s, s, rt_strlen(s) + 1);
  l->it[at].data = 0;
  l->it[at].sel = 0;
  ++l->n;
  if (l->cur >= at && l->cur >= 0) ++l->cur;
  return at;
}

static void lb_delete(lb_t *l, int at) {
  int i;

  rt_free(l->it[at].s);
  for (i = at; i + 1 < l->n; ++i) {
    l->it[i] = l->it[i + 1];
  }
  --l->n;
  if (l->cur == at) l->cur = -1;
  else if (l->cur > at) --l->cur;
}

static void lb_select(HWND h, lb_t *l, int i, int notify_it) {
  int j;

  if (i < 0 || i >= l->n) {
    return;
  }
  if (!lb_multi(h)) {
    for (j = 0; j < l->n; ++j) l->it[j].sel = 0;
    l->it[i].sel = 1;
  }
  l->cur = i;
  lb_show(h, l, i);
  InvalidateRect(h, 0, FALSE);
  if (notify_it && (style_of(h) & LBS_NOTIFY)) {
    notify(h, LBN_SELCHANGE);
  }
}

// The scroll bar: [up arrow][track with the thumb][down arrow], on the right.
static void lb_thumb(HWND h, lb_t *l, int *y0, int *y1) {
  RECT rc;
  int track, th, range = l->n - lb_rows(h);

  GetClientRect(h, &rc);
  track = rc.bottom - 2 - 2 * SB_W;
  th = track * lb_rows(h) / l->n;
  if (th < 12) th = 12;
  if (th > track) th = track;
  *y0 = 1 + SB_W + (range > 0 ? (track - th) * l->top / range : 0);
  *y1 = *y0 + th;
}

static void lb_paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  lb_t *l = lb(h);
  RECT rc;
  int rows, i, w, sb;
  COLORREF bg, fg;

  if (dc == 0 || l == 0) {
    if (dc != 0) EndPaint(h, &ps);
    return;
  }
  GetClientRect(h, &rc);
  SendMessageA(GetParent(h), WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)h);
  bg = GetBkColor(dc);
  fg = IsWindowEnabled(h) ? GetTextColor(dc) : RGB(128, 128, 128);
  rows = lb_rows(h);
  sb = lb_scrollbar(h, l);
  w = rc.right - 2 - (sb ? SB_W : 0);
  fill(dc, 0, 0, rc.right, rc.bottom, bg);
  frame(dc, 0, 0, rc.right, rc.bottom, RGB(122, 122, 122));
  for (i = 0; i < rows && l->top + i < l->n; ++i) {
    item_t *it = &l->it[l->top + i];
    int y = 2 + i * ITEM_H, len = (int)rt_strlen(it->s);
    int maxc = (w - 4) / CH_W;

    if (len > maxc) len = maxc;
    if (it->sel) {
      fill(dc, 1, y, 1 + w, y + ITEM_H, RGB(0, 120, 215));
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, it->sel ? RGB(255, 255, 255) : fg);
    TextOutA(dc, 4, y + 2, it->s, len);
    if (l->top + i == l->cur && GetFocus() == h) {
      dotted(dc, 1, y, 1 + w, y + ITEM_H);
    }
  }
  if (sb) {
    int t0, t1, x = rc.right - 1 - SB_W;

    lb_thumb(h, l, &t0, &t1);
    fill(dc, x, 1, x + SB_W, rc.bottom - 1, RGB(240, 240, 240));
    {
      int k;

      for (k = 0; k < 2; ++k) {                                           // the arrow buttons
        int by = k == 0 ? 1 : rc.bottom - 1 - SB_W, c = x + SB_W / 2, j;

        fill(dc, x, by, x + SB_W, by + SB_W, RGB(240, 240, 240));
        frame(dc, x, by, x + SB_W, by + SB_W, RGB(160, 160, 160));
        for (j = 0; j < 3; ++j) {
          int ty = k == 0 ? by + 5 + j : by + 8 - j;

          fill(dc, c - j, ty, c + j + 1, ty + 1, RGB(0, 0, 0));
        }
      }
    }
    fill(dc, x, t0, x + SB_W, t1, RGB(205, 205, 205));
    frame(dc, x, t0, x + SB_W, t1, RGB(150, 150, 150));
  }
  EndPaint(h, &ps);
}

static int lb_item_at(HWND h, lb_t *l, int y) {
  int i = l->top + (y - 2) / ITEM_H;

  (void)h;
  return y < 2 || i >= l->n ? -1 : i;
}

static LRESULT lb_scroll_click(HWND h, lb_t *l, int x, int y) {
  RECT rc;
  int t0, t1;

  GetClientRect(h, &rc);
  (void)x;
  lb_thumb(h, l, &t0, &t1);
  if (y < 1 + SB_W) {
    --l->top;
  } else if (y >= rc.bottom - 1 - SB_W) {
    ++l->top;
  } else if (y < t0) {
    l->top -= lb_rows(h) - 1;
  } else if (y >= t1) {
    l->top += lb_rows(h) - 1;
  } else {
    l->drag = 1;
    l->thumb_off = y - t0;
    SetCapture(h);
    return 0;
  }
  lb_clamp_top(h, l);
  InvalidateRect(h, 0, FALSE);
  return 0;
}

static LRESULT CALLBACK listbox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  lb_t *l = lb(h);

  switch (m) {
    case WM_NCCREATE:
      l = rt_alloc(sizeof(*l));
      if (l == 0) {
        return 0;
      }
      rt_memset(l, 0, sizeof(*l));
      l->cur = -1;
      rt_wnd_set_ctl(h, l);
      return 1;
    case WM_NCDESTROY:
      if (l != 0) {
        int i;

        for (i = 0; i < l->n; ++i) rt_free(l->it[i].s);
        rt_free(l->it);
        rt_free(l);
      }
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      lb_paint(h);
      return 0;
    case WM_ENABLE: case WM_SETFONT: case WM_SETFOCUS: case WM_KILLFOCUS:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_SIZE:
      if (l != 0) lb_clamp_top(h, l);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
      RECT rc;
      int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF), i;

      if (l == 0) return 0;
      SetFocus(h);
      GetClientRect(h, &rc);
      if (lb_scrollbar(h, l) && x >= rc.right - 1 - SB_W) {
        return lb_scroll_click(h, l, x, y);
      }
      i = lb_item_at(h, l, y);
      if (m == WM_LBUTTONDBLCLK) {
        if (i >= 0 && (style_of(h) & LBS_NOTIFY)) notify(h, LBN_DBLCLK);
        return 0;
      }
      if (i >= 0) {
        if (lb_multi(h)) {
          int ctrl = (style_of(h) & LBS_MULTIPLESEL) || (GetKeyState(VK_CONTROL) & 0x8000);
          int j;

          if (!ctrl) for (j = 0; j < l->n; ++j) l->it[j].sel = 0;
          l->it[i].sel = ctrl ? !l->it[i].sel : 1;
          l->cur = i;
          InvalidateRect(h, 0, FALSE);
          if (style_of(h) & LBS_NOTIFY) notify(h, LBN_SELCHANGE);
        } else {
          lb_select(h, l, i, 1);
          l->drag = 2;
          SetCapture(h);
        }
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (l != 0 && l->drag == 1) {
        RECT rc;
        int t0, t1, track, range = l->n - lb_rows(h), y = (short)((lp >> 16) & 0xFFFF);

        GetClientRect(h, &rc);
        lb_thumb(h, l, &t0, &t1);
        track = rc.bottom - 2 - 2 * SB_W - (t1 - t0);
        if (track > 0 && range > 0) {
          l->top = (y - l->thumb_off - 1 - SB_W) * range / track;
          lb_clamp_top(h, l);
          InvalidateRect(h, 0, FALSE);
        }
      } else if (l != 0 && l->drag == 2) {
        int i = lb_item_at(h, l, (short)((lp >> 16) & 0xFFFF));

        if (i >= 0 && i != l->cur) {
          lb_select(h, l, i, 1);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (l != 0 && l->drag != 0) {
        l->drag = 0;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (l != 0) {
        l->top -= (short)(wp >> 16) / 120 * 3;
        lb_clamp_top(h, l);
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    case WM_KEYDOWN: {
      int to;

      if (l == 0 || l->n == 0) return 0;
      switch (wp) {
        case VK_UP: to = l->cur - 1; break;
        case VK_DOWN: to = l->cur + 1; break;
        case VK_PRIOR: to = l->cur - (lb_rows(h) - 1); break;
        case VK_NEXT: to = l->cur + (lb_rows(h) - 1); break;
        case VK_HOME: to = 0; break;
        case VK_END: to = l->n - 1; break;
        default: return 0;
      }
      if (to < 0) to = 0;
      if (to >= l->n) to = l->n - 1;
      if (to != l->cur) {
        if (lb_multi(h)) {
          l->cur = to;
          lb_show(h, l, to);
          InvalidateRect(h, 0, FALSE);
        } else {
          lb_select(h, l, to, 1);
        }
      }
      return 0;
    }
    case WM_CHAR:
      if (l != 0 && wp > 32 && wp < 127 && !lb_multi(h)) {         // type ahead: the next item that starts with it
        int k;

        for (k = 1; k <= l->n; ++k) {
          int i = (l->cur + k) % l->n;
          char c = l->it[i].s[0];

          if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
          if (c == (char)(wp >= 'A' && wp <= 'Z' ? wp + 32 : wp)) {
            lb_select(h, l, i, 1);
            break;
          }
        }
      }
      return 0;
    case LB_ADDSTRING: case LB_INSERTSTRING: {
      int r;

      if (l == 0) return LB_ERR;
      r = lb_insert(h, l, m == LB_ADDSTRING ? -1 : (int)wp, (const char *)lp);
      if (r >= 0) {
        lb_clamp_top(h, l);
        InvalidateRect(h, 0, FALSE);
      }
      return r;
    }
    case LB_DELETESTRING:
      if (l == 0 || (int)wp < 0 || (int)wp >= l->n) return LB_ERR;
      lb_delete(l, (int)wp);
      lb_clamp_top(h, l);
      InvalidateRect(h, 0, FALSE);
      return l->n;
    case LB_RESETCONTENT:
      if (l != 0) {
        int i;

        for (i = 0; i < l->n; ++i) rt_free(l->it[i].s);
        l->n = 0;
        l->cur = -1;
        l->top = 0;
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    case LB_GETCOUNT:
      return l != 0 ? l->n : LB_ERR;
    case LB_GETCURSEL:
      return l != 0 ? l->cur : LB_ERR;
    case LB_SETCURSEL:
      if (l == 0) return LB_ERR;
      if ((int)wp < 0 || (int)wp >= l->n) {
        int j;

        for (j = 0; j < l->n; ++j) l->it[j].sel = 0;
        l->cur = -1;
        InvalidateRect(h, 0, FALSE);
        return LB_ERR;
      }
      lb_select(h, l, (int)wp, 0);
      return wp;
    case LB_GETSEL:
      return l != 0 && (int)wp >= 0 && (int)wp < l->n ? l->it[wp].sel : LB_ERR;
    case LB_SETSEL:
      if (l == 0) return LB_ERR;
      if ((int)lp == -1) {
        int j;

        for (j = 0; j < l->n; ++j) l->it[j].sel = wp != 0;
      } else if ((int)lp >= 0 && (int)lp < l->n) {
        l->it[lp].sel = wp != 0;
      }
      InvalidateRect(h, 0, FALSE);
      return 0;
    case LB_GETSELCOUNT: {
      int j, c = 0;

      if (l == 0) return LB_ERR;
      for (j = 0; j < l->n; ++j) c += l->it[j].sel != 0;
      return c;
    }
    case LB_GETSELITEMS: {
      int j, c = 0;

      if (l == 0) return LB_ERR;
      for (j = 0; j < l->n && c < (int)wp; ++j) {
        if (l->it[j].sel) ((int *)lp)[c++] = j;
      }
      return c;
    }
    case LB_GETTEXTLEN:
      return l != 0 && (int)wp >= 0 && (int)wp < l->n ? (LRESULT)rt_strlen(l->it[wp].s) : LB_ERR;
    case LB_GETTEXT:
      if (l == 0 || (int)wp < 0 || (int)wp >= l->n) return LB_ERR;
      rt_copy((char *)lp, l->it[wp].s, rt_strlen(l->it[wp].s) + 1);
      return (LRESULT)rt_strlen(l->it[wp].s);
    case LB_GETITEMDATA:
      return l != 0 && (int)wp >= 0 && (int)wp < l->n ? (LRESULT)l->it[wp].data : LB_ERR;
    case LB_SETITEMDATA:
      if (l == 0 || (int)wp < 0 || (int)wp >= l->n) return LB_ERR;
      l->it[wp].data = lp;
      return 0;
    case LB_GETTOPINDEX:
      return l != 0 ? l->top : LB_ERR;
    case LB_SETTOPINDEX:
      if (l == 0) return LB_ERR;
      l->top = (int)wp;
      lb_clamp_top(h, l);
      InvalidateRect(h, 0, FALSE);
      return 0;
    case LB_FINDSTRING: case LB_FINDSTRINGEXACT: case LB_SELECTSTRING: {
      const char *s = (const char *)lp;
      size_t sl = rt_strlen(s);
      int k, start = (int)wp + 1;

      if (l == 0) return LB_ERR;
      for (k = 0; k < l->n; ++k) {
        int i = (start + k) % l->n;
        const char *t = l->it[i].s;
        size_t j;
        int ok = 1;

        if (m == LB_FINDSTRINGEXACT ? rt_strlen(t) != sl : rt_strlen(t) < sl) continue;
        for (j = 0; j < sl && ok; ++j) {
          char a = s[j], b = t[j];

          if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
          if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
          ok = a == b;
        }
        if (ok) {
          if (m == LB_SELECTSTRING) lb_select(h, l, i, 0);
          return i;
        }
      }
      return LB_ERR;
    }
    case LB_GETITEMRECT: {
      RECT *r = (RECT *)lp;
      int i = (int)wp;

      if (l == 0 || r == 0 || i < l->top || i >= l->n || i >= l->top + lb_rows(h)) return LB_ERR;
      GetClientRect(h, r);
      r->left = 1;
      r->right -= 1 + (lb_scrollbar(h, l) ? SB_W : 0);
      r->top = 2 + (i - l->top) * ITEM_H;
      r->bottom = r->top + ITEM_H;
      return 0;
    }
    case LB_ITEMFROMPOINT: {
      int i = l != 0 ? lb_item_at(h, l, (short)((lp >> 16) & 0xFFFF)) : -1;

      return i < 0 ? 1 << 16 | (l != 0 && l->n > 0 ? l->n - 1 : 0) : i;
    }
    case LB_SETITEMHEIGHT: case LB_SETHORIZONTALEXTENT: case LB_SETCOLUMNWIDTH:
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// ---- registration -------------------------------------------------------------------------------------------

void rt_controls_register(void) {
  static const struct { const char *name; WNDPROC proc; int bg; UINT style; } kClasses[] = {
    {"BUTTON", button_proc, COLOR_BTNFACE, 0},
    {"STATIC", static_proc, COLOR_BTNFACE, 0},
    {"EDIT", edit_proc, COLOR_WINDOW, CS_DBLCLKS},
    {"LISTBOX", listbox_proc, COLOR_WINDOW, CS_DBLCLKS},
  };
  int i;

  for (i = 0; i < 4; ++i) {
    WNDCLASSEXA wc;

    rt_memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = kClasses[i].style;
    wc.lpfnWndProc = kClasses[i].proc;
    wc.hbrBackground = (HBRUSH)(uintptr_t)(kClasses[i].bg + 1);
    wc.lpszClassName = kClasses[i].name;
    RegisterClassExA(&wc);
  }
}
