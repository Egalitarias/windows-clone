// The tool bar control (ToolbarWindow32) for the Win32 runtime. Buttons (push, check, group / radio, drop-down,
// separators) with images from an image list (TB_SETIMAGELIST, or a bitmap strip with TB_ADDBITMAP) and text,
// below the image or beside it (TBSTYLE_LIST); enabled, checked, pressed, hidden, indeterminate and wrapping
// buttons; flat (hot tracking) or raised buttons; hot and disabled image lists, or a gray silhouette for a
// disabled image; wrapping into rows (TBSTYLE_WRAPABLE); placement at the top or bottom of the parent
// (CCS_TOP / CCS_BOTTOM, on TB_AUTOSIZE and WM_SIZE, as comctl32 does). A click sends WM_COMMAND to the
// parent; the drop-down arrow sends TBN_DROPDOWN; NM_CLICK and NM_RCLICK as WM_NOTIFY. With TBSTYLE_TOOLTIPS
// the control makes a tip control (or uses the one of TB_SETTOOLTIPS) that asks the parent for the text with
// TTN_GETDISPINFO, whose idFrom is the button's command id.
//
// Not here: customizing, drag and drop of buttons, the standard bitmaps (HINST_COMMCTRL), accelerators,
// vertical tool bars, multiple text rows, ellipses, owner / custom draw.
#include "internal.h"
#include <commctrl.h>

void rt_il_silhouette(HIMAGELIST h, int i, HDC dc, int x, int y, COLORREF color);

#define HI RGB(255, 255, 255)
#define LO RGB(128, 128, 128)
#define DK RGB(105, 105, 105)
#define TB_MAXB 256
#define TB_MAXSTR 64
#define DD_W 14                 // the drop-down arrow's part of a button

typedef struct {
  int image, cmd;
  BYTE state, style;
  DWORD_PTR data;
  char *text;                   // an owned copy, or 0
  int cx;                       // an own width (TBIF_SIZE), 0 = automatic
  RECT r;                       // in the control's client area; empty when hidden
} btn_t;

typedef struct {
  btn_t *b;
  int n, cap;
  HIMAGELIST il, hot, dis;
  int own_il;                   // il was made by TB_ADDBITMAP
  int bmw, bmh;                 // image size (TB_SETBITMAPSIZE)
  int bw, bh;                   // TB_SETBUTTONSIZE, 0 = automatic
  int padx, pady;
  int cbsize;
  char *str[TB_MAXSTR];         // TB_ADDSTRING's pool
  int nstr;
  UINT ex;
  int hot_item, press, press_in, press_dd;
  HWND tip;
  int own_tip;
  int rows;
  int indent;
  int tracking;
} tb_t;

static DWORD style_of(HWND h) { return (DWORD)GetWindowLongPtrA(h, GWL_STYLE); }

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  RECT rc = {l, t, r, b};

  if (r <= l || b <= t) {
    return;
  }
  SetBkColor(dc, c);
  ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &rc, 0, 0, 0);
}

static void bevel(HDC dc, const RECT *r, int down, COLORREF face) {
  fill(dc, r->left, r->top, r->right, r->bottom, face);
  fill(dc, r->left, r->top, r->right - 1, r->top + 1, down ? DK : HI);
  fill(dc, r->left, r->top, r->left + 1, r->bottom - 1, down ? DK : HI);
  fill(dc, r->left, r->bottom - 1, r->right, r->bottom, down ? HI : DK);
  fill(dc, r->right - 1, r->top, r->right, r->bottom, down ? HI : DK);
}

static int slen(const char *s) { return (int)rt_strlen(s); }

static char *sdup(const char *s) {
  char *d = rt_alloc((size_t)slen(s) + 1);

  if (d != 0) {
    rt_copy(d, s, (size_t)slen(s) + 1);
  }
  return d;
}

static int is_sep(const btn_t *b) { return (b->style & BTNS_SEP) != 0; }
static int hidden(const btn_t *b) { return (b->state & TBSTATE_HIDDEN) != 0; }
static int enabled(const btn_t *b) { return (b->state & TBSTATE_ENABLED) != 0; }
static int shows_text(const tb_t *t, const btn_t *b, DWORD st) {
  (void)st;
  return b->text != 0 && b->text[0] != 0 && (!(t->ex & TBSTYLE_EX_MIXEDBUTTONS) || (b->style & BTNS_SHOWTEXT));
}

static int has_image(const tb_t *t, const btn_t *b) { return t->il != 0 && b->image >= 0; }

// ---- Layout ----------------------------------------------------------------------------------------------

static void notify_tip(HWND h, tb_t *t);

// Where the buttons go; returns the height the control needs.
static int layout(HWND h, tb_t *t) {
  DWORD st = style_of(h);
  RECT c;
  int i, x, y, row = 0, uw = 0, uh, anytext = 0, y0 = (st & CCS_NODIVIDER) ? 0 : 2, list = (st & TBSTYLE_LIST) != 0;
  int iw = t->il != 0 ? t->bmw : 0, ih = t->il != 0 ? t->bmh : 0;
  int base_h, rowh;

  GetClientRect(h, &c);
  for (i = 0; i < t->n; ++i) {
    if (!is_sep(&t->b[i]) && !hidden(&t->b[i]) && shows_text(t, &t->b[i], st)) anytext = 1;
  }
  base_h = ih + t->pady + 1;
  if (anytext && !list) {
    base_h = (ih > 0 ? ih + 2 : 0) + 8 + t->pady + 1;
    if (ih == 0) base_h = 8 + t->pady + 1;
  } else if (anytext && list && ih < 8) {
    base_h = 8 + t->pady + 1;
  }
  if (base_h < 8 + t->pady + 1 && (ih == 0 || anytext)) base_h = 8 + t->pady + 1;
  uh = t->bh > 0 ? t->bh : base_h;
  // a uniform width for the buttons that do not size themselves to their text
  for (i = 0; i < t->n; ++i) {
    btn_t *b = &t->b[i];
    int w;

    if (is_sep(b) || hidden(b) || (b->style & BTNS_AUTOSIZE) || b->cx > 0) continue;
    w = iw + t->padx;
    if (shows_text(t, b, st)) {
      int tw = slen(b->text) * 8;

      if (list) w = (has_image(t, b) ? iw + 4 : 0) + tw + t->padx;
      else if (tw + t->padx > w) w = tw + t->padx;
    }
    if (w > uw) uw = w;
  }
  if (t->bw > 0) uw = t->bw;
  if (uw == 0) uw = iw + t->padx;
  x = t->indent;
  y = y0;
  rowh = uh;
  for (i = 0; i < t->n; ++i) {
    btn_t *b = &t->b[i];
    int w;

    if (hidden(b)) {
      b->r.left = b->r.top = b->r.right = b->r.bottom = 0;
      continue;
    }
    if (is_sep(b)) {
      w = b->image > 0 ? b->image : 8;
    } else if (b->cx > 0) {
      w = b->cx;
    } else if ((b->style & BTNS_AUTOSIZE) && t->bw == 0) {
      w = iw + t->padx;
      if (shows_text(t, b, st)) {
        int tw = slen(b->text) * 8;

        w = list ? (has_image(t, b) ? iw + 4 : 0) + tw + t->padx : (tw + t->padx > w ? tw + t->padx : w);
      }
    } else {
      w = uw;
    }
    if (!is_sep(b) && (b->style & BTNS_DROPDOWN) && !(b->style & BTNS_WHOLEDROPDOWN) && b->cx == 0) {
      w += DD_W;
    } else if (!is_sep(b) && (b->style & BTNS_WHOLEDROPDOWN) && b->cx == 0) {
      w += DD_W;
    }
    if ((st & TBSTYLE_WRAPABLE) && x + w > c.right && x > t->indent) {
      x = t->indent;
      y += rowh;
      ++row;
    }
    b->r.left = x;
    b->r.top = y;
    b->r.right = x + w;
    b->r.bottom = y + (is_sep(b) ? uh : uh);
    x += w;
    if ((b->state & TBSTATE_WRAP) && (st & TBSTYLE_WRAPABLE)) {
      x = t->indent;
      y += rowh;
      ++row;
    }
  }
  t->rows = row + 1;
  return y0 + t->rows * rowh + 2;
}

// Resize and place the control against its parent (CCS_TOP, CCS_BOTTOM), the way comctl32 does on TB_AUTOSIZE.
static void autosize(HWND h, tb_t *t) {
  DWORD st = style_of(h);
  HWND p = GetParent(h);
  RECT pc, me;
  int need, w, x, y;

  if (p == 0) {
    return;
  }
  GetClientRect(p, &pc);
  GetClientRect(h, &me);
  need = layout(h, t);
  w = me.right;
  x = 0;
  y = 0;
  if (!(st & CCS_NORESIZE)) {
    w = pc.right;
  }
  if (!(st & CCS_NOPARENTALIGN)) {
    y = (st & CCS_BOTTOM) == CCS_BOTTOM ? pc.bottom - need : 0;
    SetWindowPos(h, 0, x, y, w, (st & CCS_NORESIZE) ? me.bottom : need, SWP_NOZORDER);
  } else if (!(st & CCS_NORESIZE)) {
    SetWindowPos(h, 0, 0, 0, w, need, SWP_NOMOVE | SWP_NOZORDER);
  }
  layout(h, t);                                                  // (the width may have changed the wrapping)
}

static void relayout(HWND h, tb_t *t) {
  layout(h, t);
  notify_tip(h, t);
  InvalidateRect(h, 0, TRUE);
}

// ---- Tool tips ---------------------------------------------------------------------------------------------

static void notify_tip(HWND h, tb_t *t) {
  int i;

  if (t->tip == 0 || !IsWindow(t->tip)) {
    return;
  }
  for (;;) {
    TTTOOLINFOA ti;

    rt_memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    if ((int)SendMessageA(t->tip, TTM_GETTOOLCOUNT, 0, 0) == 0) break;
    SendMessageA(t->tip, TTM_ENUMTOOLSA, 0, (LPARAM)&ti);
    if (ti.hwnd != h) break;                                       // (a tool somebody else added)
    SendMessageA(t->tip, TTM_DELTOOLA, 0, (LPARAM)&ti);
  }
  for (i = 0; i < t->n; ++i) {
    btn_t *b = &t->b[i];
    TTTOOLINFOA ti;

    if (is_sep(b) || hidden(b)) continue;
    rt_memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = 0;
    ti.hwnd = h;
    ti.uId = (UINT_PTR)b->cmd;
    ti.rect = b->r;
    ti.lpszText = LPSTR_TEXTCALLBACKA;
    SendMessageA(t->tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
  }
}

static void ensure_tip(HWND h, tb_t *t) {
  if (t->tip != 0 && IsWindow(t->tip)) {
    return;
  }
  t->tip = 0;
  if ((style_of(h) & TBSTYLE_TOOLTIPS) && GetParent(h) != 0) {
    t->tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, 0, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, GetParent(h), 0, GetModuleHandleA(0), 0);
    t->own_tip = 1;
    notify_tip(h, t);
  }
}

static void relay(HWND h, tb_t *t, UINT msg, LPARAM lp) {
  MSG m;

  if (t->tip == 0 || !IsWindow(t->tip)) {
    return;
  }
  rt_memset(&m, 0, sizeof(m));
  m.hwnd = h;
  m.message = msg;
  m.lParam = lp;
  SendMessageA(t->tip, TTM_RELAYEVENT, 0, (LPARAM)&m);
}

// ---- Items -------------------------------------------------------------------------------------------------

static int reserve(tb_t *t, int need) {
  btn_t *p;
  int cap;

  if (need <= t->cap) {
    return 1;
  }
  cap = t->cap == 0 ? 16 : t->cap * 2;
  while (cap < need) cap *= 2;
  if (cap > TB_MAXB) cap = TB_MAXB;
  if (need > cap) return 0;
  p = rt_alloc((size_t)cap * sizeof(btn_t));
  if (p == 0) return 0;
  if (t->b != 0) {
    rt_memcpy(p, t->b, (size_t)t->n * sizeof(btn_t));
    rt_free(t->b);
  }
  t->b = p;
  t->cap = cap;
  return 1;
}

static int index_of_cmd(const tb_t *t, int cmd) {
  int i;

  for (i = 0; i < t->n; ++i) {
    if (!is_sep(&t->b[i]) && t->b[i].cmd == cmd) return i;
  }
  return -1;
}

static void set_text_of(tb_t *t, btn_t *b, INT_PTR istr) {
  rt_free(b->text);
  b->text = 0;
  if (istr >= 0 && (uintptr_t)istr < 0x10000) {
    if (istr < t->nstr) b->text = sdup(t->str[istr]);
  } else if (istr != 0 && istr != -1) {
    b->text = sdup((const char *)istr);
  }
}

static int insert_one(tb_t *t, int at, const TBBUTTON *s) {
  btn_t *b;

  if (at < 0 || at > t->n) at = t->n;
  if (!reserve(t, t->n + 1)) return 0;
  rt_memcpy(&t->b[at + 1], &t->b[at], (size_t)(t->n - at) * sizeof(btn_t));
  b = &t->b[at];
  rt_memset(b, 0, sizeof(*b));
  b->image = s->iBitmap;
  b->cmd = s->idCommand;
  b->state = s->fsState;
  b->style = s->fsStyle;
  b->data = s->dwData;
  set_text_of(t, b, s->iString);
  ++t->n;
  if (t->hot_item >= at) ++t->hot_item;
  return 1;
}

static void to_struct(const btn_t *b, TBBUTTON *s) {
  rt_memset(s, 0, sizeof(*s));
  s->iBitmap = b->image;
  s->idCommand = b->cmd;
  s->fsState = b->state;
  s->fsStyle = b->style;
  s->dwData = b->data;
  s->iString = 0;
}

static void free_btn(btn_t *b) { rt_free(b->text); b->text = 0; }

// Checking a button of a group unchecks the others in its run.
static void set_checked(tb_t *t, int i, int on) {
  btn_t *b = &t->b[i];

  if (on) {
    b->state |= TBSTATE_CHECKED;
    if (b->style & BTNS_GROUP) {
      int k;

      for (k = i - 1; k >= 0 && (t->b[k].style & BTNS_GROUP) && !is_sep(&t->b[k]); --k) t->b[k].state &= (BYTE)~TBSTATE_CHECKED;
      for (k = i + 1; k < t->n && (t->b[k].style & BTNS_GROUP) && !is_sep(&t->b[k]); ++k) t->b[k].state &= (BYTE)~TBSTATE_CHECKED;
    }
  } else {
    b->state &= (BYTE)~TBSTATE_CHECKED;
  }
}

static int hit(const tb_t *t, int x, int y) {
  int i;

  for (i = 0; i < t->n; ++i) {
    const btn_t *b = &t->b[i];

    if (!hidden(b) && x >= b->r.left && x < b->r.right && y >= b->r.top && y < b->r.bottom) return i;
  }
  return -1;
}

// ---- Drawing -----------------------------------------------------------------------------------------------

static void arrow(HDC dc, int cx, int cy, COLORREF c) {
  fill(dc, cx - 2, cy - 1, cx + 3, cy, c);
  fill(dc, cx - 1, cy, cx + 2, cy + 1, c);
  fill(dc, cx, cy + 1, cx + 1, cy + 2, c);
}

static void draw_button(HWND h, tb_t *t, HDC dc, int i) {
  btn_t *b = &t->b[i];
  DWORD st = style_of(h);
  RECT r = b->r;
  int flat = (st & TBSTYLE_FLAT) != 0, down, on = enabled(b), checked = (b->state & TBSTATE_CHECKED) != 0, dd = 0;
  int pressed = (b->state & TBSTATE_PRESSED) != 0 || (t->press == i && t->press_in && !t->press_dd);
  int hot = t->hot_item == i && on;
  int ox, oy, iw = t->il != 0 ? t->bmw : 0, ih = t->il != 0 ? t->bmh : 0, tw = 0, th = 8, cw, ch;
  COLORREF face = GetSysColor(COLOR_BTNFACE);
  HIMAGELIST il = t->il;
  int img = b->image;

  if (is_sep(b)) {
    if (flat) {
      int mid = (r.left + r.right) / 2;

      fill(dc, mid - 1, r.top + 2, mid, r.bottom - 2, LO);
      fill(dc, mid, r.top + 2, mid + 1, r.bottom - 2, HI);
    }
    return;
  }
  if ((b->style & BTNS_DROPDOWN) || (b->style & BTNS_WHOLEDROPDOWN)) {
    dd = DD_W;
  }
  down = pressed || (checked && !(b->state & TBSTATE_INDETERMINATE));
  if (!flat || down || hot) {
    if (checked && !pressed) face = RGB(248, 248, 248);
    bevel(dc, &r, down, face);
  }
  ox = oy = down ? 1 : 0;
  cw = r.right - r.left - dd;
  ch = r.bottom - r.top;
  if (shows_text(t, b, st)) tw = slen(b->text) * 8;
  if (has_image(t, b) && hot && t->hot != 0) il = t->hot;
  if (!on && t->dis != 0) il = t->dis;
  {
    int list = (st & TBSTYLE_LIST) != 0;
    int ix, iy, tx = 0, ty = 0, have_i = il != 0 && img >= 0;

    if (list) {
      int total = (have_i ? iw + (tw > 0 ? 4 : 0) : 0) + tw;

      ix = r.left + (cw - total) / 2 + ox;
      iy = r.top + (ch - ih) / 2 + oy;
      tx = ix + (have_i ? iw + 4 : 0);
      ty = r.top + (ch - th) / 2 + oy;
    } else {
      int total = (have_i ? ih + (tw > 0 ? 2 : 0) : 0) + (tw > 0 ? th : 0);

      ix = r.left + (cw - iw) / 2 + ox;
      iy = r.top + (ch - total) / 2 + oy;
      tx = r.left + (cw - tw) / 2 + ox;
      ty = iy + (have_i ? ih + 2 : 0);
    }
    if (have_i) {
      if (!on && il == t->il) {
        rt_il_silhouette(il, img, dc, ix + 1, iy + 1, HI);
        rt_il_silhouette(il, img, dc, ix, iy, LO);
      } else if (b->state & TBSTATE_INDETERMINATE) {
        rt_il_silhouette(il, img, dc, ix, iy, LO);
      } else {
        ImageList_Draw(il, img, dc, ix, iy, ILD_TRANSPARENT);
      }
    }
    if (tw > 0) {
      SetBkMode(dc, TRANSPARENT);
      if (!on) {
        SetTextColor(dc, HI);
        TextOutA(dc, tx + 1, ty + 1, b->text, slen(b->text));
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      } else {
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
      }
      TextOutA(dc, tx, ty, b->text, slen(b->text));
    }
  }
  if (dd != 0) {
    int ax = r.right - dd / 2 - 1 + ox;
    int ay = (r.top + r.bottom) / 2 + oy;

    if (!(b->style & BTNS_WHOLEDROPDOWN) && (!flat || hot || down)) {
      fill(dc, r.right - dd, r.top + 2, r.right - dd + 1, r.bottom - 2, LO);
      fill(dc, r.right - dd + 1, r.top + 2, r.right - dd + 2, r.bottom - 2, HI);
    }
    arrow(dc, ax, ay - 1, on ? GetSysColor(COLOR_BTNTEXT) : LO);
  }
}

static void paint(HWND h, tb_t *t) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT c;
  int i;

  GetClientRect(h, &c);
  fill(dc, 0, 0, c.right, c.bottom, GetSysColor(COLOR_BTNFACE));
  if (!(style_of(h) & CCS_NODIVIDER)) {
    fill(dc, 0, 0, c.right, 1, RGB(160, 160, 160));
    fill(dc, 0, 1, c.right, 2, HI);
  }
  for (i = 0; i < t->n; ++i) {
    if (!hidden(&t->b[i])) draw_button(h, t, dc, i);
  }
  EndPaint(h, &ps);
}

static void invalidate_btn(HWND h, tb_t *t, int i) {
  if (i >= 0 && i < t->n) {
    RECT r = t->b[i].r;

    InflateRect(&r, 1, 1);
    InvalidateRect(h, &r, TRUE);
  }
}

// ---- Notifications -------------------------------------------------------------------------------------------

static LRESULT notify(HWND h, UINT code, void *nm_in) {
  NMHDR *nm = (NMHDR *)nm_in;
  HWND p = GetParent(h);

  nm->hwndFrom = h;
  nm->idFrom = (UINT_PTR)GetDlgCtrlID(h);
  nm->code = code;
  return p != 0 ? SendMessageA(p, WM_NOTIFY, nm->idFrom, (LPARAM)nm) : 0;
}

static void send_command(HWND h, int cmd) {
  HWND p = GetParent(h);

  if (p != 0) {
    SendMessageA(p, WM_COMMAND, MAKEWPARAM(cmd, BN_CLICKED), (LPARAM)h);
  }
}

static void dropdown(HWND h, tb_t *t, int i) {
  NMTOOLBARA nm;

  rt_memset(&nm, 0, sizeof(nm));
  nm.iItem = t->b[i].cmd;
  to_struct(&t->b[i], &nm.tbButton);
  nm.rcButton = t->b[i].r;
  notify(h, TBN_DROPDOWN, &nm);
}

static void mouse_note(HWND h, UINT code, int i, int x, int y) {
  NMMOUSE nm;

  rt_memset(&nm, 0, sizeof(nm));
  nm.dwItemSpec = (DWORD_PTR)i;
  nm.pt.x = x;
  nm.pt.y = y;
  notify(h, code, &nm);
}

static void set_hot(HWND h, tb_t *t, int i) {
  if (i == t->hot_item) {
    return;
  }
  invalidate_btn(h, t, t->hot_item);
  t->hot_item = i;
  invalidate_btn(h, t, i);
}

static void clear_press(HWND h, tb_t *t) {
  int i = t->press;

  t->press = -1;
  t->press_in = 0;
  invalidate_btn(h, t, i);
}

// ---- Window procedure -----------------------------------------------------------------------------------------

static int find_by(tb_t *t, WPARAM wp, int by_index) {
  if (by_index) return (int)wp >= 0 && (int)wp < t->n ? (int)wp : -1;
  return index_of_cmd(t, (int)wp);
}

LRESULT CALLBACK rt_toolbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  tb_t *t = (tb_t *)rt_wnd_ctl(h);
  int i;

  switch (m) {
    case WM_NCCREATE:
      t = rt_alloc(sizeof(*t));
      if (t == 0) {
        return 0;
      }
      rt_memset(t, 0, sizeof(*t));
      t->bmw = 16;
      t->bmh = 15;
      t->padx = 8;
      t->pady = 7;
      t->cbsize = sizeof(TBBUTTON);
      t->hot_item = -1;
      t->press = -1;
      rt_wnd_set_ctl(h, t);
      return 1;
    case WM_CREATE:
      return 0;
    case WM_NCDESTROY:
      if (t != 0) {
        for (i = 0; i < t->n; ++i) free_btn(&t->b[i]);
        for (i = 0; i < t->nstr; ++i) rt_free(t->str[i]);
        rt_free(t->b);
        if (t->own_il && t->il != 0) ImageList_Destroy(t->il);
        rt_free(t);
      }
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (t != 0) paint(h, t);
      return 0;
    case WM_SIZE:
      if (t != 0) {
        autosize(h, t);
        InvalidateRect(h, 0, TRUE);
      }
      return 0;
    case WM_ENABLE:
      InvalidateRect(h, 0, TRUE);
      return 0;
    case WM_NOTIFY:                                                    // the tip's TTN_GETDISPINFO: the parent knows the text
      return GetParent(h) != 0 ? SendMessageA(GetParent(h), WM_NOTIFY, wp, lp) : 0;
    case WM_MOUSEMOVE: {
      int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF);

      if (t == 0) return 0;
      ensure_tip(h, t);
      if (t->press >= 0) {
        int in = hit(t, x, y) == t->press;

        if (in != t->press_in) {
          t->press_in = in;
          invalidate_btn(h, t, t->press);
        }
        return 0;
      }
      i = hit(t, x, y);
      set_hot(h, t, i >= 0 && (is_sep(&t->b[i]) || !enabled(&t->b[i])) ? -1 : i);
      if (!t->tracking) {
        TRACKMOUSEEVENT te;

        te.cbSize = sizeof(te);
        te.dwFlags = TME_LEAVE;
        te.hwndTrack = h;
        te.dwHoverTime = 0;
        if (TrackMouseEvent(&te)) t->tracking = 1;
      }
      relay(h, t, WM_MOUSEMOVE, lp);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (t != 0) {
        t->tracking = 0;
        if (t->press < 0) set_hot(h, t, -1);
        relay(h, t, WM_MOUSELEAVE, 0);
      }
      return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
      int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF);

      if (t == 0) return 0;
      relay(h, t, m, lp);
      i = hit(t, x, y);
      if (i < 0 || is_sep(&t->b[i]) || !enabled(&t->b[i])) return 0;
      if ((t->b[i].style & BTNS_WHOLEDROPDOWN) || ((t->b[i].style & BTNS_DROPDOWN) && x >= t->b[i].r.right - DD_W)) {
        t->press = i;
        t->press_in = 1;
        t->press_dd = 1;
        invalidate_btn(h, t, i);
        UpdateWindow(h);
        dropdown(h, t, i);
        if (t->press >= 0) clear_press(h, t);
        t->press_dd = 0;
        return 0;
      }
      t->press = i;
      t->press_in = 1;
      t->press_dd = 0;
      SetCapture(h);
      invalidate_btn(h, t, i);
      return 0;
    }
    case WM_LBUTTONUP: {
      int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF);

      if (t == 0) return 0;
      relay(h, t, m, lp);
      if (t->press >= 0) {
        int p = t->press, cmd = t->b[p].cmd, inside = hit(t, x, y) == p;

        ReleaseCapture();
        clear_press(h, t);
        if (inside) {
          btn_t *b = &t->b[p];

          if (b->style & BTNS_CHECK) {
            if (b->style & BTNS_GROUP) set_checked(t, p, 1);
            else set_checked(t, p, (b->state & TBSTATE_CHECKED) == 0);
          }
          invalidate_btn(h, t, p);
          InvalidateRect(h, 0, TRUE);
          mouse_note(h, NM_CLICK, p, x, y);
          send_command(h, cmd);
        }
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      int x = (short)(lp & 0xFFFF), y = (short)((lp >> 16) & 0xFFFF);

      if (t != 0) {
        relay(h, t, m, lp);
        mouse_note(h, NM_RCLICK, hit(t, x, y), x, y);
      }
      return 0;
    }
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
      if (t != 0) relay(h, t, m, lp);
      return 0;
    case TB_BUTTONSTRUCTSIZE:
      if (t != 0) t->cbsize = (int)wp;
      return 0;
    case TB_ADDBUTTONSA: case TB_INSERTBUTTONA: {
      const char *src = (const char *)lp;
      int n = m == TB_INSERTBUTTONA ? 1 : (int)wp, at = m == TB_INSERTBUTTONA ? (int)wp : t != 0 ? t->n : 0, k;

      if (t == 0 || src == 0 || n <= 0) return FALSE;
      for (k = 0; k < n; ++k) {
        TBBUTTON s;

        rt_memset(&s, 0, sizeof(s));
        rt_memcpy(&s, src + (size_t)k * (size_t)t->cbsize, (size_t)t->cbsize < sizeof(s) ? (size_t)t->cbsize : sizeof(s));
        if (!insert_one(t, at + k, &s)) return FALSE;
      }
      relayout(h, t);
      return TRUE;
    }
    case TB_DELETEBUTTON:
      if (t == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      free_btn(&t->b[wp]);
      rt_memcpy(&t->b[wp], &t->b[wp + 1], (size_t)(t->n - (int)wp - 1) * sizeof(btn_t));
      --t->n;
      t->hot_item = -1;
      t->press = -1;
      relayout(h, t);
      return TRUE;
    case TB_BUTTONCOUNT:
      return t != 0 ? t->n : 0;
    case TB_GETBUTTON:
      if (t == 0 || lp == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      to_struct(&t->b[wp], (TBBUTTON *)lp);
      return TRUE;
    case TB_COMMANDTOINDEX:
      return t != 0 ? index_of_cmd(t, (int)wp) : -1;
    case TB_ADDSTRINGA: {
      const char *s = (const char *)lp;
      int first;

      if (t == 0 || s == 0 || wp != 0) return -1;                       // (string resources are not supported)
      first = t->nstr;
      while (*s != 0 && t->nstr < TB_MAXSTR) {
        t->str[t->nstr++] = sdup(s);
        s += slen(s) + 1;
      }
      return first;
    }
    case TB_GETBUTTONTEXTA:
      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      if (i < 0) return -1;
      if (lp != 0) rt_copy((char *)lp, t->b[i].text != 0 ? t->b[i].text : "", 256);
      return t->b[i].text != 0 ? slen(t->b[i].text) : 0;
    case TB_ENABLEBUTTON: case TB_CHECKBUTTON: case TB_PRESSBUTTON: case TB_HIDEBUTTON: case TB_INDETERMINATE: {
      int on = LOWORD(lp) != 0;
      BYTE bit = m == TB_ENABLEBUTTON ? TBSTATE_ENABLED : m == TB_CHECKBUTTON ? TBSTATE_CHECKED : m == TB_PRESSBUTTON ? TBSTATE_PRESSED
                 : m == TB_HIDEBUTTON ? TBSTATE_HIDDEN : TBSTATE_INDETERMINATE;

      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      if (i < 0) return FALSE;
      if (m == TB_CHECKBUTTON) {
        set_checked(t, i, on);
      } else if (on) {
        t->b[i].state |= bit;
      } else {
        t->b[i].state &= (BYTE)~bit;
      }
      if (m == TB_HIDEBUTTON) relayout(h, t);
      else InvalidateRect(h, 0, TRUE);
      return TRUE;
    }
    case TB_ISBUTTONENABLED: case TB_ISBUTTONCHECKED: case TB_ISBUTTONPRESSED: case TB_ISBUTTONHIDDEN: case TB_ISBUTTONINDETERMINATE:
    case TB_ISBUTTONHIGHLIGHTED: {
      BYTE bit = m == TB_ISBUTTONENABLED ? TBSTATE_ENABLED : m == TB_ISBUTTONCHECKED ? TBSTATE_CHECKED : m == TB_ISBUTTONPRESSED ? TBSTATE_PRESSED
                 : m == TB_ISBUTTONHIDDEN ? TBSTATE_HIDDEN : m == TB_ISBUTTONHIGHLIGHTED ? TBSTATE_MARKED : TBSTATE_INDETERMINATE;

      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      if (i < 0) return -1;
      return (t->b[i].state & bit) != 0;
    }
    case TB_GETSTATE:
      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      return i < 0 ? -1 : t->b[i].state;
    case TB_SETSTATE:
      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      if (i < 0) return FALSE;
      t->b[i].state = (BYTE)LOWORD(lp);
      relayout(h, t);
      return TRUE;
    case TB_GETITEMRECT:
      if (t == 0 || lp == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      *(RECT *)lp = t->b[wp].r;
      return TRUE;
    case TB_GETRECT:
      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      if (i < 0 || lp == 0) return FALSE;
      *(RECT *)lp = t->b[i].r;
      return TRUE;
    case TB_HITTEST: {
      POINT *p = (POINT *)lp;

      if (t == 0 || p == 0) return -1;
      i = hit(t, p->x, p->y);
      if (i < 0) return -(t->n + 1);
      return is_sep(&t->b[i]) ? -(i + 1) : i;
    }
    case TB_SETBITMAPSIZE:
      if (t == 0 || (short)LOWORD(lp) <= 0 || (short)HIWORD(lp) <= 0) return FALSE;
      t->bmw = (short)LOWORD(lp);
      t->bmh = (short)HIWORD(lp);
      relayout(h, t);
      return TRUE;
    case TB_SETBUTTONSIZE:
      if (t == 0) return FALSE;
      t->bw = (short)LOWORD(lp);
      t->bh = (short)HIWORD(lp);
      relayout(h, t);
      return TRUE;
    case TB_GETBUTTONSIZE:
      if (t == 0) return 0;
      if (t->n > 0) {
        for (i = 0; i < t->n; ++i) {
          if (!is_sep(&t->b[i]) && !hidden(&t->b[i])) return MAKELONG(t->b[i].r.right - t->b[i].r.left, t->b[i].r.bottom - t->b[i].r.top);
        }
      }
      return MAKELONG(t->bmw + t->padx, t->bmh + t->pady + 1);
    case TB_SETBUTTONWIDTH:
      if (t == 0) return FALSE;
      t->bw = (short)LOWORD(lp) > 0 ? (short)LOWORD(lp) : 0;
      relayout(h, t);
      return TRUE;
    case TB_SETPADDING: {
      DWORD old;

      if (t == 0) return 0;
      old = MAKELONG(t->padx, t->pady);
      t->padx = (short)LOWORD(lp);
      t->pady = (short)HIWORD(lp);
      relayout(h, t);
      return (LRESULT)old;
    }
    case TB_GETPADDING:
      return t != 0 ? MAKELONG(t->padx, t->pady) : 0;
    case TB_SETINDENT:
      if (t != 0) {
        t->indent = (int)wp;
        relayout(h, t);
      }
      return TRUE;
    case TB_AUTOSIZE:
      if (t != 0) {
        autosize(h, t);
        InvalidateRect(h, 0, TRUE);
      }
      return 0;
    case TB_GETROWS:
      if (t == 0) return 0;
      layout(h, t);
      return t->rows;
    case TB_SETIMAGELIST: case TB_SETHOTIMAGELIST: case TB_SETDISABLEDIMAGELIST: {
      HIMAGELIST *slot = m == TB_SETIMAGELIST ? &t->il : m == TB_SETHOTIMAGELIST ? &t->hot : &t->dis;
      HIMAGELIST old;

      if (t == 0) return 0;
      old = *slot;
      if (m == TB_SETIMAGELIST && t->own_il && t->il != 0) {
        ImageList_Destroy(t->il);
        old = 0;
        t->own_il = 0;
      }
      *slot = (HIMAGELIST)lp;
      if (m == TB_SETIMAGELIST && t->il != 0) {
        ImageList_GetIconSize(t->il, &t->bmw, &t->bmh);
      }
      relayout(h, t);
      return (LRESULT)old;
    }
    case TB_GETIMAGELIST:
      return t != 0 ? (LRESULT)t->il : 0;
    case TB_GETHOTIMAGELIST:
      return t != 0 ? (LRESULT)t->hot : 0;
    case TB_GETDISABLEDIMAGELIST:
      return t != 0 ? (LRESULT)t->dis : 0;
    case TB_ADDBITMAP: {
      TBADDBITMAP *ab = (TBADDBITMAP *)lp;
      BITMAP bm;
      int first;

      if (t == 0 || ab == 0 || ab->hInst != 0 || (int)wp <= 0) return -1;      // (the standard bitmaps of comctl32 are not here)
      if (GetObjectA((HBITMAP)ab->nID, sizeof(bm), &bm) == 0) return -1;
      if (t->il == 0) {
        t->bmw = bm.bmWidth / (int)wp;
        t->bmh = bm.bmHeight;
        t->il = ImageList_Create(t->bmw, t->bmh, ILC_COLOR32 | ILC_MASK, (int)wp, 4);
        t->own_il = 1;
      }
      if (t->il == 0) return -1;
      first = ImageList_AddMasked(t->il, (HBITMAP)ab->nID, RGB(192, 192, 192));
      relayout(h, t);
      return first;
    }
    case TB_CHANGEBITMAP:
      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      if (i < 0) return FALSE;
      t->b[i].image = LOWORD(lp);
      InvalidateRect(h, 0, TRUE);
      return TRUE;
    case TB_GETBITMAP:
      i = t != 0 ? index_of_cmd(t, (int)wp) : -1;
      return i < 0 ? -1 : t->b[i].image;
    case TB_SETCMDID:
      if (t == 0 || (int)wp < 0 || (int)wp >= t->n) return FALSE;
      t->b[wp].cmd = (int)lp;
      return TRUE;
    case TB_SETEXTENDEDSTYLE: {
      DWORD old;

      if (t == 0) return 0;
      old = t->ex;
      t->ex = wp != 0 ? (t->ex & ~(UINT)wp) | ((UINT)lp & (UINT)wp) : (UINT)lp;
      relayout(h, t);
      return old;
    }
    case TB_GETEXTENDEDSTYLE:
      return t != 0 ? t->ex : 0;
    case TB_SETSTYLE:
      SetWindowLongPtrA(h, GWL_STYLE, (LONG_PTR)lp);
      if (t != 0) relayout(h, t);
      return 0;
    case TB_GETSTYLE:
      return (LRESULT)style_of(h);
    case TB_GETHOTITEM:
      return t != 0 ? t->hot_item : -1;
    case TB_SETHOTITEM: {
      int old = t != 0 ? t->hot_item : -1;

      if (t != 0) set_hot(h, t, (int)wp >= 0 && (int)wp < t->n ? (int)wp : -1);
      return old;
    }
    case TB_SETPARENT: {
      HWND old = GetParent(h);

      return (LRESULT)old;
    }
    case TB_GETTOOLTIPS:
      if (t == 0) return 0;
      ensure_tip(h, t);
      return (LRESULT)t->tip;
    case TB_SETTOOLTIPS:
      if (t == 0) return 0;
      if (t->tip != 0 && t->own_tip && IsWindow(t->tip)) DestroyWindow(t->tip);
      t->tip = (HWND)wp;
      t->own_tip = 0;
      notify_tip(h, t);
      return 0;
    case TB_SETBUTTONINFOA: case TB_GETBUTTONINFOA: {
      TBBUTTONINFOA *bi = (TBBUTTONINFOA *)lp;
      btn_t *b;

      if (t == 0 || bi == 0 || bi->cbSize < sizeof(TBBUTTONINFOA)) return m == TB_GETBUTTONINFOA ? -1 : FALSE;
      i = find_by(t, wp, (bi->dwMask & TBIF_BYINDEX) != 0);
      if (i < 0) return m == TB_GETBUTTONINFOA ? -1 : FALSE;
      b = &t->b[i];
      if (m == TB_SETBUTTONINFOA) {
        if (bi->dwMask & TBIF_IMAGE) b->image = bi->iImage;
        if (bi->dwMask & TBIF_STATE) b->state = bi->fsState;
        if (bi->dwMask & TBIF_STYLE) b->style = bi->fsStyle;
        if (bi->dwMask & TBIF_LPARAM) b->data = bi->lParam;
        if (bi->dwMask & TBIF_COMMAND) b->cmd = bi->idCommand;
        if (bi->dwMask & TBIF_SIZE) b->cx = bi->cx;
        if ((bi->dwMask & TBIF_TEXT) && bi->pszText != 0) {
          rt_free(b->text);
          b->text = sdup(bi->pszText);
        }
        relayout(h, t);
        return TRUE;
      }
      if (bi->dwMask & TBIF_IMAGE) bi->iImage = b->image;
      if (bi->dwMask & TBIF_STATE) bi->fsState = b->state;
      if (bi->dwMask & TBIF_STYLE) bi->fsStyle = b->style;
      if (bi->dwMask & TBIF_LPARAM) bi->lParam = b->data;
      if (bi->dwMask & TBIF_COMMAND) bi->idCommand = b->cmd;
      if (bi->dwMask & TBIF_SIZE) bi->cx = (WORD)(b->r.right - b->r.left);
      if ((bi->dwMask & TBIF_TEXT) && bi->pszText != 0 && bi->cchText > 0) {
        rt_copy(bi->pszText, b->text != 0 ? b->text : "", (size_t)bi->cchText);
      }
      return i;
    }
    case TB_MOVEBUTTON: {
      btn_t tmp;
      int from = (int)wp, to = (int)lp;

      if (t == 0 || from < 0 || from >= t->n || to < 0 || to >= t->n) return FALSE;
      tmp = t->b[from];
      if (from < to) rt_memcpy(&t->b[from], &t->b[from + 1], (size_t)(to - from) * sizeof(btn_t));
      else rt_memcpy(&t->b[to + 1], &t->b[to], (size_t)(from - to) * sizeof(btn_t));
      t->b[to] = tmp;
      relayout(h, t);
      return TRUE;
    }
    case TB_SETMAXTEXTROWS: case TB_GETTEXTROWS:
      return 1;
    case TB_GETMAXSIZE:
      if (t == 0 || lp == 0) return FALSE;
      layout(h, t);
      ((SIZE *)lp)->cx = 0;
      ((SIZE *)lp)->cy = 0;
      for (i = 0; i < t->n; ++i) {
        if (hidden(&t->b[i])) continue;
        if (t->b[i].r.right > ((SIZE *)lp)->cx) ((SIZE *)lp)->cx = t->b[i].r.right;
        if (t->b[i].r.bottom > ((SIZE *)lp)->cy) ((SIZE *)lp)->cy = t->b[i].r.bottom;
      }
      return TRUE;
    case TB_GETUNICODEFORMAT:
      return FALSE;
    case TB_SETUNICODEFORMAT:
      return FALSE;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}
