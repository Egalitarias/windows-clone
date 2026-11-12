// The list view control (SysListView32) for the Win32 runtime. Report view (columns with a header you can
// click and resize), list view (items flow down in columns) and small-icon / icon views (items flow across
// in cells), with item images from image lists (LVSIL_SMALL in front of the label, LVSIL_NORMAL above it). Items with sub-item text, single and
// multiple selection with the mouse and keyboard, a focus item, optional check boxes, grid lines, full row
// selection, sorting with a callback, hit testing, scrolling by the frame scroll bars of the window (they
// appear when the content does not fit) and the usual notifications as WM_NOTIFY.
//
// Not here: label editing, groups, tiles, callback items (LVS_OWNERDATA), column
// reordering, tool tips, custom draw, LVS_SORTASCENDING at insertion (use LVM_SORTITEMS).
#include "internal.h"
#include <commctrl.h>

#define ROW_H 14
#define HDR_H 18
#define LV_MAXCOL 16
#define HI RGB(255, 255, 255)
#define LO RGB(128, 128, 128)
#define DK RGB(105, 105, 105)

typedef struct {
  char *text[LV_MAXCOL];      // text[0] is the item's label, text[k] the sub-item shown in column k
  LPARAM param;
  UINT state;
  int image;
} lvi_t;

typedef struct {
  char *title;
  int width, fmt;
} lvc_t;

typedef struct {
  lvi_t *it;
  int n, cap;
  lvc_t col[LV_MAXCOL];
  int ncol;
  int focus, anchor;
  int sx, sy;                 // scroll offsets in pixels
  UINT ex;                    // extended styles
  COLORREF bk, tx, txbk;
  HIMAGELIST il[3];           // LVSIL_NORMAL (icon view), LVSIL_SMALL (the others), LVSIL_STATE (kept, not drawn)
  int track, trackcol, trackx, trackw;     // resizing a column
  int pressed;                // the header button held down, or -1
  int inbars;                 // guards the recursion between the scroll bars and the layout
} lv_t;

static COLORREF face(void) { return GetSysColor(COLOR_BTNFACE); }
static DWORD style_of(HWND h) { return (DWORD)GetWindowLongPtrA(h, GWL_STYLE); }
static int slen(const char *s) { int n = 0; while (s != 0 && s[n] != 0) ++n; return n; }

static char *sdup(const char *s) {
  int n = slen(s), i;
  char *d = rt_alloc((size_t)n + 1);

  if (d != 0) {
    for (i = 0; i < n; ++i) d[i] = s[i];
    d[n] = 0;
  }
  return d;
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  RECT rc = {l, t, r, b};

  if (r <= l || b <= t) {
    return;
  }
  SetBkColor(dc, c);
  ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &rc, 0, 0, 0);
}

// Text clipped to `room` pixels (whole characters).
static void put(HDC dc, int x, int y, const char *s, int room, COLORREF c) {
  int n = slen(s);

  if (n > room / 8) n = room > 0 ? room / 8 : 0;
  if (n > 0) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, n);
  }
}

static int sx_(LPARAM lp) { return (short)(lp & 0xFFFF); }
static int sy_(LPARAM lp) { return (short)((lp >> 16) & 0xFFFF); }

static LRESULT notify(HWND h, UINT code, NMHDR *nm) {
  HWND p = GetParent(h);

  nm->hwndFrom = h;
  nm->idFrom = (UINT_PTR)GetDlgCtrlID(h);
  nm->code = code;
  return p != 0 ? SendMessageA(p, WM_NOTIFY, nm->idFrom, (LPARAM)nm) : 0;
}

static void item_notify(HWND h, lv_t *l, UINT code, int i, int sub, UINT newst, UINT oldst, UINT changed) {
  NMLISTVIEW nm;

  rt_memset(&nm, 0, sizeof(nm));
  nm.iItem = i;
  nm.iSubItem = sub;
  nm.uNewState = newst;
  nm.uOldState = oldst;
  nm.uChanged = changed;
  nm.lParam = i >= 0 && i < l->n ? l->it[i].param : 0;
  notify(h, code, &nm.hdr);
}

// ---- Layout ----------------------------------------------------------------------------------------------

static int view_of(HWND h) { return (int)(style_of(h) & LVS_TYPEMASK); }
static int hdr_h(HWND h) { return view_of(h) == LVS_REPORT && !(style_of(h) & LVS_NOCOLUMNHEADER) ? HDR_H : 0; }

// Image sizes and the room the small image takes in front of a label.
static int img_w(const lv_t *l, int which) { int w = 0, hh = 0; if (l->il[which] != 0) ImageList_GetIconSize(l->il[which], &w, &hh); return w; }
static int img_h(const lv_t *l, int which) { int w = 0, hh = 0; if (l->il[which] != 0) ImageList_GetIconSize(l->il[which], &w, &hh); return hh; }
static int img_gap(const lv_t *l) { return l->il[1] != 0 ? img_w(l, 1) + 3 : 0; }
static int row_h(const lv_t *l) { int ih = img_h(l, 1); return ih + 1 > ROW_H ? ih + 1 : ROW_H; }

static int col_w(const lv_t *l, int c) { return c >= 0 && c < l->ncol ? l->col[c].width : 0; }

static int total_w(const lv_t *l) {
  int c, w = 0;

  for (c = 0; c < l->ncol; ++c) w += l->col[c].width;
  return w;
}

// The label's width for the list view: the first column's width if set, else what the longest label needs.
static int list_cell_w(const lv_t *l) {
  int i, w = 0;

  if (l->ncol > 0 && l->col[0].width > 0) {
    return l->col[0].width;
  }
  for (i = 0; i < l->n; ++i) {
    int k = slen(l->it[i].text[0]) * 8 + 24 + img_gap(l);

    if (k > w) w = k;
  }
  return w > 0 ? w : 64;
}

// Cell size for the list and icon views.
static void cell_of(HWND h, const lv_t *l, int *cw, int *ch) {
  switch (view_of(h)) {
    case LVS_LIST: *cw = list_cell_w(l); *ch = row_h(l); break;
    case LVS_SMALLICON: *cw = 120 + img_gap(l); *ch = row_h(l) > 16 ? row_h(l) : 16; break;
    default:
      *cw = img_w(l, 0) + 16 > 80 ? img_w(l, 0) + 16 : 80;
      *ch = img_h(l, 0) > 0 ? img_h(l, 0) + 20 : 34;
      break;
  }
}

static void client_of(HWND h, int *w, int *hh) {
  RECT r;

  GetClientRect(h, &r);
  *w = r.right;
  *hh = r.bottom;
}

// Item i's rectangle in client coordinates (scrolled). In report view the rectangle spans all columns.
static void item_rect(HWND h, const lv_t *l, int i, RECT *r) {
  int cw, ch, w, hh, per, rows;

  client_of(h, &w, &hh);
  switch (view_of(h)) {
    case LVS_REPORT:
      SetRect(r, -l->sx, hdr_h(h) + i * row_h(l) - l->sy, -l->sx + total_w(l), hdr_h(h) + (i + 1) * row_h(l) - l->sy);
      return;
    case LVS_LIST:
      cell_of(h, l, &cw, &ch);
      rows = hh / ch > 0 ? hh / ch : 1;
      SetRect(r, (i / rows) * cw - l->sx, (i % rows) * ch, (i / rows) * cw - l->sx + cw, (i % rows) * ch + ch);
      return;
    default:
      cell_of(h, l, &cw, &ch);
      per = w / cw > 0 ? w / cw : 1;
      SetRect(r, (i % per) * cw, (i / per) * ch - l->sy, (i % per) * cw + cw, (i / per) * ch - l->sy + ch);
      return;
  }
}

// The part of the item the label occupies (selection highlight without full row select).
static void label_rect(HWND h, const lv_t *l, int i, RECT *r) {
  item_rect(h, l, i, r);
  if (view_of(h) == LVS_REPORT && !(l->ex & LVS_EX_FULLROWSELECT)) {
    r->right = r->left + col_w(l, 0);
  }
}

// Content size: report and icon views scroll vertically, report and list views horizontally.
static void content(HWND h, const lv_t *l, int *cw_, int *ch_) {
  int cw, ch, w, hh, per, rows;

  client_of(h, &w, &hh);
  *cw_ = 0;
  *ch_ = 0;
  switch (view_of(h)) {
    case LVS_REPORT:
      *cw_ = total_w(l);
      *ch_ = l->n * row_h(l);
      return;
    case LVS_LIST:
      cell_of(h, l, &cw, &ch);
      rows = hh / ch > 0 ? hh / ch : 1;
      *cw_ = ((l->n + rows - 1) / rows) * cw;
      return;
    default:
      cell_of(h, l, &cw, &ch);
      per = w / cw > 0 ? w / cw : 1;
      *ch_ = ((l->n + per - 1) / per) * ch;
      return;
  }
}

static void set_bar(HWND h, int bar, int total, int page, int pos) {
  SCROLLINFO si;

  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = total > 0 ? total - 1 : 0;
  si.nPage = (UINT)(page > 0 ? page : 1);
  si.nPos = pos;
  SetScrollInfo(h, bar, &si, TRUE);
}

// Make the frame bars match the content: they appear when it does not fit and keep the offsets inside.
static void update_bars(HWND h, lv_t *l) {
  int pass;

  if (l->inbars) {
    return;
  }
  l->inbars = 1;
  for (pass = 0; pass < 2; ++pass) {                              // a bar taking room can make the other one necessary
    int cw, ch, w, hh, maxx, maxy;

    content(h, l, &cw, &ch);
    client_of(h, &w, &hh);
    hh -= hdr_h(h);
    maxx = cw > w ? cw - w : 0;
    maxy = ch > hh ? ch - hh : 0;
    if (l->sx > maxx) l->sx = maxx;
    if (l->sy > maxy) l->sy = maxy;
    set_bar(h, SB_HORZ, cw, w, l->sx);
    set_bar(h, SB_VERT, ch, hh, l->sy);
  }
  {
    int cw, ch, w, hh;

    content(h, l, &cw, &ch);
    client_of(h, &w, &hh);
    hh -= hdr_h(h);
    if (ch <= hh) l->sy = 0;
    if (cw <= w) l->sx = 0;
  }
  l->inbars = 0;
}

static void scroll_to(HWND h, lv_t *l, int sx, int sy) {
  int cw, ch, w, hh;

  content(h, l, &cw, &ch);
  client_of(h, &w, &hh);
  hh -= hdr_h(h);
  if (sx > cw - w) sx = cw - w;
  if (sy > ch - hh) sy = ch - hh;
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  if (sx != l->sx || sy != l->sy) {
    l->sx = sx;
    l->sy = sy;
    SetScrollPos(h, SB_HORZ, sx, TRUE);
    SetScrollPos(h, SB_VERT, sy, TRUE);
    InvalidateRect(h, 0, FALSE);
  }
}

static void ensure_visible(HWND h, lv_t *l, int i) {
  RECT r;
  int w, hh, top = hdr_h(h), nx = l->sx, ny = l->sy;

  if (i < 0 || i >= l->n) {
    return;
  }
  client_of(h, &w, &hh);
  item_rect(h, l, i, &r);
  if (view_of(h) != LVS_LIST) {
    if (r.top < top) ny += r.top - top;
    else if (r.bottom > hh) ny += r.bottom - hh;
  }
  if (view_of(h) == LVS_LIST) {
    if (r.left < 0) nx += r.left;
    else if (r.right > w) nx += r.right - w;
  }
  scroll_to(h, l, nx, ny);
}

// ---- Items and selection ---------------------------------------------------------------------------------

static int items_reserve(lv_t *l, int need) {
  lvi_t *p;
  int cap, i;

  if (need <= l->cap) {
    return 1;
  }
  cap = l->cap == 0 ? 16 : l->cap;
  while (cap < need) cap *= 2;
  p = rt_alloc((size_t)cap * sizeof(lvi_t));
  if (p == 0) {
    return 0;
  }
  for (i = 0; i < l->n; ++i) p[i] = l->it[i];
  rt_free(l->it);
  l->it = p;
  l->cap = cap;
  return 1;
}

static void item_free(lvi_t *it) {
  int c;

  for (c = 0; c < LV_MAXCOL; ++c) {
    rt_free(it->text[c]);
    it->text[c] = 0;
  }
}

static void set_text(lvi_t *it, int sub, const char *s) {
  char *d;

  if (sub < 0 || sub >= LV_MAXCOL) {
    return;
  }
  d = sdup(s != 0 ? s : "");
  if (d != 0) {
    rt_free(it->text[sub]);
    it->text[sub] = d;
  }
}

// Change an item's state bits and tell the parent.
static void set_state(HWND h, lv_t *l, int i, UINT mask, UINT value) {
  UINT old, nw;

  if (i < 0 || i >= l->n) {
    return;
  }
  old = l->it[i].state;
  nw = (old & ~mask) | (value & mask);
  if (nw == old) {
    return;
  }
  l->it[i].state = nw;
  if (mask & LVIS_FOCUSED) {
    if (nw & LVIS_FOCUSED) l->focus = i;
    else if (l->focus == i) l->focus = -1;
  }
  item_notify(h, l, LVN_ITEMCHANGED, i, 0, nw, old, LVIF_STATE);
  {
    RECT r;

    item_rect(h, l, i, &r);
    InvalidateRect(h, &r, FALSE);
  }
}

static void select_only(HWND h, lv_t *l, int keep) {
  int i;

  for (i = 0; i < l->n; ++i) {
    if (i != keep && (l->it[i].state & LVIS_SELECTED)) set_state(h, l, i, LVIS_SELECTED, 0);
  }
}

static void focus_item(HWND h, lv_t *l, int i) {
  if (l->focus >= 0 && l->focus < l->n && l->focus != i) {
    set_state(h, l, l->focus, LVIS_FOCUSED, 0);
  }
  if (i >= 0 && i < l->n) {
    set_state(h, l, i, LVIS_FOCUSED, LVIS_FOCUSED);
  }
  l->focus = i;
}

static int selected_count(const lv_t *l) {
  int i, n = 0;

  for (i = 0; i < l->n; ++i) n += (l->it[i].state & LVIS_SELECTED) != 0;
  return n;
}

// A click or key movement to item i with the modifiers (MK_SHIFT / MK_CONTROL).
static void choose(HWND h, lv_t *l, int i, int shift, int ctrl) {
  int single = (style_of(h) & LVS_SINGLESEL) != 0, k, lo, hi;

  if (i < 0 || i >= l->n) {
    return;
  }
  if (single || (!shift && !ctrl)) {
    select_only(h, l, i);
    set_state(h, l, i, LVIS_SELECTED, LVIS_SELECTED);
    l->anchor = i;
  } else if (ctrl && !shift) {
    set_state(h, l, i, LVIS_SELECTED, (l->it[i].state & LVIS_SELECTED) ? 0 : LVIS_SELECTED);
    l->anchor = i;
  } else {                                                        // a range from the anchor
    int a = l->anchor >= 0 && l->anchor < l->n ? l->anchor : i;

    lo = a < i ? a : i;
    hi = a < i ? i : a;
    for (k = 0; k < l->n; ++k) {
      if (k >= lo && k <= hi) set_state(h, l, k, LVIS_SELECTED, LVIS_SELECTED);
      else if (!ctrl) set_state(h, l, k, LVIS_SELECTED, 0);
    }
  }
  focus_item(h, l, i);
  ensure_visible(h, l, i);
}

static int checkbox_gap(const lv_t *l) { return (l->ex & LVS_EX_CHECKBOXES) ? 18 : 0; }

// ---- Hit testing -----------------------------------------------------------------------------------------

// Returns the item at (x, y) or -1; *sub gets the column in report view; *flags gets LVHT_*.
static int hit(HWND h, lv_t *l, int x, int y, int *sub, UINT *flags) {
  int i, c;

  *sub = 0;
  *flags = LVHT_NOWHERE;
  for (i = 0; i < l->n; ++i) {
    RECT r;

    item_rect(h, l, i, &r);
    if (r.bottom <= hdr_h(h) || r.top >= 100000) {
      continue;
    }
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom && y >= hdr_h(h)) {
      int x0 = r.left;

      if (view_of(h) == LVS_REPORT) {
        for (c = 0; c < l->ncol; ++c) {
          if (x < x0 + l->col[c].width) break;
          x0 += l->col[c].width;
        }
        *sub = c < l->ncol ? c : 0;
        if (c >= l->ncol) {
          *flags = LVHT_NOWHERE;
          return -1;
        }
      }
      if ((l->ex & LVS_EX_CHECKBOXES) && x < x0 + checkbox_gap(l)) *flags = LVHT_ONITEMSTATEICON;
      else if (view_of(h) == LVS_REPORT && !(l->ex & LVS_EX_FULLROWSELECT) && *sub > 0) *flags = LVHT_ONITEMLABEL;
      else *flags = LVHT_ONITEMLABEL;
      return i;
    }
  }
  return -1;
}

// ---- Drawing ---------------------------------------------------------------------------------------------

static void draw_header(HDC dc, HWND h, lv_t *l, int w) {
  int c, x = -l->sx;

  fill(dc, 0, 0, w, HDR_H, face());
  for (c = 0; c < l->ncol; ++c) {
    int cw = l->col[c].width, r = x + cw, down = l->pressed == c, tx, tw = slen(l->col[c].title) * 8;

    if (r > 0 && x < w) {
      fill(dc, x, 0, r, HDR_H, face());
      fill(dc, x, 0, r - 1, 1, down ? DK : HI);
      fill(dc, x, 0, x + 1, HDR_H - 1, down ? DK : HI);
      fill(dc, x, HDR_H - 1, r, HDR_H, down ? HI : DK);
      fill(dc, r - 1, 0, r, HDR_H, down ? HI : DK);
      tx = x + 6;
      if (l->col[c].fmt & LVCFMT_RIGHT) tx = r - 6 - tw;
      else if (l->col[c].fmt & LVCFMT_CENTER) tx = x + (cw - tw) / 2;
      if (tx < x + 4) tx = x + 4;
      put(dc, tx + down, 5 + down, l->col[c].title, r - tx - 4, RGB(0, 0, 0));
    }
    x = r;
  }
  if (x < w) {
    fill(dc, x, 0, w, HDR_H, face());
    fill(dc, x, HDR_H - 1, w, HDR_H, DK);
  }
  (void)h;
}

static void draw_checkbox(HDC dc, int x, int y, int checked) {
  fill(dc, x, y, x + 12, y + 12, HI);
  fill(dc, x, y, x + 12, y + 1, LO);
  fill(dc, x, y, x + 1, y + 12, LO);
  fill(dc, x, y + 11, x + 12, y + 12, DK);
  fill(dc, x + 11, y, x + 12, y + 12, DK);
  if (checked) {
    int k;

    for (k = 0; k < 3; ++k) fill(dc, x + 2 + k, y + 5 + k, x + 3 + k, y + 8 + k, RGB(0, 0, 0));
    for (k = 0; k < 5; ++k) fill(dc, x + 5 + k, y + 7 - k, x + 6 + k, y + 10 - k, RGB(0, 0, 0));
  }
}

// The item's image from the list `which` (0 normal, 1 small); a selected item's is blended with the highlight.
static void draw_image(HDC dc, const lv_t *l, int i, int which, int x, int y, int blend) {
  UINT st = ILD_TRANSPARENT | (blend ? ILD_SELECTED : 0) | (((l->it[i].state & LVIS_OVERLAYMASK) >> 8) << 8);

  if (l->il[which] != 0 && l->it[i].image >= 0) {
    ImageList_Draw(l->il[which], l->it[i].image, dc, x, y, st);
  }
}

static void draw_item(HDC dc, HWND h, lv_t *l, int i, int w, int focused, COLORREF bk, COLORREF tx) {
  RECT r, lab;
  int c, x, sel = (l->it[i].state & LVIS_SELECTED) != 0, gap = checkbox_gap(l), ty;
  COLORREF fg = tx, bg = bk;

  item_rect(h, l, i, &r);
  if (r.bottom <= 0 || r.right <= 0 || r.left >= w) {
    return;
  }
  label_rect(h, l, i, &lab);
  if (sel && (focused || (style_of(h) & LVS_SHOWSELALWAYS))) {
    bg = focused ? GetSysColor(COLOR_HIGHLIGHT) : RGB(205, 205, 205);
    fg = focused ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 0, 0);
    fill(dc, lab.left, lab.top, lab.right, lab.bottom, bg);
  }
  ty = r.top + (r.bottom - r.top - 8) / 2;
  if (view_of(h) == LVS_REPORT) {
    x = r.left;
    for (c = 0; c < l->ncol; ++c) {
      int cw = l->col[c].width, off = 4 + (c == 0 ? gap + img_gap(l) : 0);

      if (c == 0 && gap > 0) {
        int st = (l->it[i].state & LVIS_STATEIMAGEMASK) >> 12;

        draw_checkbox(dc, x + 3, r.top + (r.bottom - r.top - 12) / 2, st == 2);
      }
      if (c == 0) draw_image(dc, l, i, 1, x + 2 + gap, r.top + (r.bottom - r.top - img_h(l, 1)) / 2, sel && focused);
      if (l->it[i].text[c] != 0 && x + cw > 0 && x < w) {
        int al = l->col[c].fmt & LVCFMT_JUSTIFYMASK, tw = slen(l->it[i].text[c]) * 8, tx2 = x + off;

        if (c > 0 && al == LVCFMT_RIGHT) tx2 = x + cw - 4 - tw;
        else if (c > 0 && al == LVCFMT_CENTER) tx2 = x + (cw - tw) / 2;
        if (tx2 < x + off) tx2 = x + off;
        put(dc, tx2, ty, l->it[i].text[c], x + cw - tx2 - 2, (c == 0 || (l->ex & LVS_EX_FULLROWSELECT)) ? fg : tx);
      }
      if (l->ex & LVS_EX_GRIDLINES) fill(dc, x + cw - 1, r.top, x + cw, r.bottom, RGB(224, 224, 224));
      x += cw;
    }
    if (l->ex & LVS_EX_GRIDLINES) fill(dc, r.left, r.bottom - 1, r.right, r.bottom, RGB(224, 224, 224));
  } else {
    const char *s = l->it[i].text[0] != 0 ? l->it[i].text[0] : "";
    int tw = slen(s) * 8;

    if (gap > 0) draw_checkbox(dc, r.left + 3, r.top + (r.bottom - r.top - 12) / 2, ((l->it[i].state & LVIS_STATEIMAGEMASK) >> 12) == 2);
    if (view_of(h) == LVS_ICON) {
      int room = r.right - r.left - 4;

      draw_image(dc, l, i, 0, r.left + (r.right - r.left - img_w(l, 0)) / 2, r.top + 2, sel && focused);
      tw = tw > room ? room / 8 * 8 : tw;
      put(dc, r.left + (r.right - r.left - tw) / 2, r.top + (r.bottom - r.top - 8) - 3, s, room, fg);
    } else {
      draw_image(dc, l, i, 1, r.left + 2 + gap, r.top + (r.bottom - r.top - img_h(l, 1)) / 2, sel && focused);
      put(dc, r.left + 4 + gap + img_gap(l), ty, s, r.right - r.left - 6 - gap - img_gap(l), fg);
    }
  }
  if ((l->it[i].state & LVIS_FOCUSED) && focused) {                // a dotted focus rectangle
    int k;

    for (k = lab.left; k < lab.right; k += 2) {
      fill(dc, k, lab.top, k + 1, lab.top + 1, sel ? RGB(255, 255, 255) : RGB(0, 0, 0));
      fill(dc, k, lab.bottom - 1, k + 1, lab.bottom, sel ? RGB(255, 255, 255) : RGB(0, 0, 0));
    }
    for (k = lab.top; k < lab.bottom; k += 2) {
      fill(dc, lab.left, k, lab.left + 1, k + 1, sel ? RGB(255, 255, 255) : RGB(0, 0, 0));
      fill(dc, lab.right - 1, k, lab.right, k + 1, sel ? RGB(255, 255, 255) : RGB(0, 0, 0));
    }
  }
  (void)bg;
}

static void paint(HWND h, lv_t *l) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  int w, hh, i, focused = GetFocus() == h;
  COLORREF bk = l->bk == CLR_DEFAULT ? GetSysColor(COLOR_WINDOW) : l->bk, tx = l->tx == CLR_DEFAULT ? GetSysColor(COLOR_WINDOWTEXT) : l->tx;

  client_of(h, &w, &hh);
  fill(dc, 0, hdr_h(h), w, hh, bk);
  for (i = 0; i < l->n; ++i) {
    draw_item(dc, h, l, i, w, focused, bk, tx);
  }
  if (hdr_h(h) > 0) {
    draw_header(dc, h, l, w);                                     // over the items that scrolled under it
  }
  EndPaint(h, &ps);
}

// ---- Columns ---------------------------------------------------------------------------------------------

static int autosize(const lv_t *l, int c, int with_header) {
  int i, w = with_header ? slen(l->col[c].title) * 8 + 16 : 0;

  for (i = 0; i < l->n; ++i) {
    int k = slen(l->it[i].text[c]) * 8 + 12 + (c == 0 ? checkbox_gap(l) + img_gap(l) : 0);

    if (k > w) w = k;
  }
  return w > 0 ? w : 16;
}

static int col_insert(lv_t *l, int at, const LVCOLUMNA *c) {
  int k, i;

  if (l->ncol >= LV_MAXCOL) {
    return -1;
  }
  if (at < 0 || at > l->ncol) at = l->ncol;
  for (k = l->ncol; k > at; --k) l->col[k] = l->col[k - 1];
  l->col[at].title = sdup((c->mask & LVCF_TEXT) && c->pszText != 0 ? c->pszText : "");
  l->col[at].width = (c->mask & LVCF_WIDTH) ? c->cx : 80;
  l->col[at].fmt = (c->mask & LVCF_FMT) ? c->fmt : LVCFMT_LEFT;
  ++l->ncol;
  for (i = 0; i < l->n; ++i) {                                    // the sub-items from `at` on move one column right (the label stays in column 0)
    int from = at > 1 ? at : 1;

    for (k = LV_MAXCOL - 1; k > from; --k) l->it[i].text[k] = l->it[i].text[k - 1];
    l->it[i].text[from] = 0;
  }
  return at;
}

static void col_delete(lv_t *l, int at) {
  int k, i;

  rt_free(l->col[at].title);
  for (k = at; k + 1 < l->ncol; ++k) l->col[k] = l->col[k + 1];
  --l->ncol;
  for (i = 0; i < l->n; ++i) {
    int from = at > 1 ? at : 1;

    rt_free(l->it[i].text[from]);
    for (k = from; k + 1 < LV_MAXCOL; ++k) l->it[i].text[k] = l->it[i].text[k + 1];
    l->it[i].text[LV_MAXCOL - 1] = 0;
  }
}

// ---- Sorting ---------------------------------------------------------------------------------------------

static void sort_items(HWND h, lv_t *l, PFNLVCOMPARE cmp, LPARAM lp) {
  int i, j;

  for (i = 1; i < l->n; ++i) {                                    // insertion sort: stable, fine for the sizes here
    lvi_t key = l->it[i];

    for (j = i - 1; j >= 0 && cmp(l->it[j].param, key.param, lp) > 0; --j) {
      l->it[j + 1] = l->it[j];
    }
    l->it[j + 1] = key;
  }
  l->focus = -1;
  for (i = 0; i < l->n; ++i) {
    if (l->it[i].state & LVIS_FOCUSED) l->focus = i;
  }
  l->anchor = l->focus;
  InvalidateRect(h, 0, FALSE);
}

// ---- Messages --------------------------------------------------------------------------------------------

static void delete_item(HWND h, lv_t *l, int i) {
  int k;

  item_notify(h, l, LVN_DELETEITEM, i, 0, 0, 0, 0);
  item_free(&l->it[i]);
  for (k = i; k + 1 < l->n; ++k) l->it[k] = l->it[k + 1];
  --l->n;
  if (l->focus == i) l->focus = -1;
  else if (l->focus > i) --l->focus;
  if (l->anchor >= l->n) l->anchor = l->n - 1;
}

static void fill_lvitem(const lv_t *l, int i, LVITEMA *it) {
  const lvi_t *s = &l->it[i];

  if (it->mask & LVIF_TEXT) {
    const char *t = it->iSubItem >= 0 && it->iSubItem < LV_MAXCOL && s->text[it->iSubItem] != 0 ? s->text[it->iSubItem] : "";
    int n = slen(t), k;

    if (it->pszText != 0 && it->cchTextMax > 0) {
      if (n > it->cchTextMax - 1) n = it->cchTextMax - 1;
      for (k = 0; k < n; ++k) it->pszText[k] = t[k];
      it->pszText[n] = 0;
    }
  }
  if (it->mask & LVIF_PARAM) it->lParam = s->param;
  if (it->mask & LVIF_STATE) it->state = s->state & it->stateMask;
  if (it->mask & LVIF_IMAGE) it->iImage = s->image;
}

static int next_item(const lv_t *l, int start, UINT flags) {
  int i;

  for (i = start + 1; i < l->n; ++i) {
    UINT st = l->it[i].state;

    if (flags == LVNI_ALL || ((flags & LVNI_FOCUSED) && (st & LVIS_FOCUSED)) || ((flags & LVNI_SELECTED) && (st & LVIS_SELECTED))) {
      return i;
    }
  }
  return -1;
}

static void key(HWND h, lv_t *l, UINT vk) {
  NMLVKEYDOWN kd;
  int cur = l->focus >= 0 ? l->focus : 0, to = cur, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  int w, hh, cw, ch, page, v = view_of(h);

  rt_memset(&kd, 0, sizeof(kd));
  kd.wVKey = (WORD)vk;
  notify(h, LVN_KEYDOWN, &kd.hdr);
  if (l->n == 0) {
    return;
  }
  client_of(h, &w, &hh);
  cell_of(h, l, &cw, &ch);
  page = v == LVS_REPORT ? (hh - hdr_h(h)) / row_h(l) : (v == LVS_LIST ? hh / ch : (w / cw > 0 ? w / cw : 1) * (hh / ch > 0 ? hh / ch : 1));
  if (page < 1) page = 1;
  switch (vk) {
    case VK_UP: to = v == LVS_ICON || v == LVS_SMALLICON ? cur - (w / cw > 0 ? w / cw : 1) : cur - 1; break;
    case VK_DOWN: to = v == LVS_ICON || v == LVS_SMALLICON ? cur + (w / cw > 0 ? w / cw : 1) : cur + 1; break;
    case VK_LEFT: to = v == LVS_LIST ? cur - (hh / ch > 0 ? hh / ch : 1) : (v == LVS_REPORT ? cur : cur - 1); break;
    case VK_RIGHT: to = v == LVS_LIST ? cur + (hh / ch > 0 ? hh / ch : 1) : (v == LVS_REPORT ? cur : cur + 1); break;
    case VK_HOME: to = 0; break;
    case VK_END: to = l->n - 1; break;
    case VK_PRIOR: to = cur - page; break;
    case VK_NEXT: to = cur + page; break;
    case VK_SPACE:
      if (l->focus >= 0) {
        if (l->ex & LVS_EX_CHECKBOXES) {
          UINT st = (l->it[l->focus].state & LVIS_STATEIMAGEMASK) >> 12;

          set_state(h, l, l->focus, LVIS_STATEIMAGEMASK, (st == 2 ? 1u : 2u) << 12);
        } else if (ctrl) {
          choose(h, l, l->focus, 0, 1);
        }
      }
      return;
    case VK_RETURN: {
      NMHDR nm;

      notify(h, NM_RETURN, &nm);
      return;
    }
    default: return;
  }
  if (to < 0) to = 0;
  if (to >= l->n) to = l->n - 1;
  if (ctrl && !shift) {                                           // Ctrl+arrow moves the focus only
    focus_item(h, l, to);
    ensure_visible(h, l, to);
  } else {
    choose(h, l, to, shift, 0);
  }
}

static void do_click(HWND h, lv_t *l, UINT m, WPARAM wp, LPARAM lp) {
  int x = sx_(lp), y = sy_(lp), sub;
  UINT flags;
  int i = hit(h, l, x, y, &sub, &flags);
  NMITEMACTIVATE nm;

  SetFocus(h);
  if (i >= 0 && flags == LVHT_ONITEMSTATEICON && m == WM_LBUTTONDOWN) {
    UINT st = (l->it[i].state & LVIS_STATEIMAGEMASK) >> 12;

    set_state(h, l, i, LVIS_STATEIMAGEMASK, (st == 2 ? 1u : 2u) << 12);
    choose(h, l, i, 0, 0);
  } else if (i >= 0) {
    choose(h, l, i, (wp & MK_SHIFT) != 0, (wp & MK_CONTROL) != 0);
  } else if (!(wp & (MK_SHIFT | MK_CONTROL))) {
    select_only(h, l, -1);
  }
  rt_memset(&nm, 0, sizeof(nm));
  nm.iItem = i;
  nm.iSubItem = sub;
  nm.ptAction.x = x;
  nm.ptAction.y = y;
  nm.uKeyFlags = (UINT)((wp & MK_SHIFT ? LVKF_SHIFT : 0) | (wp & MK_CONTROL ? LVKF_CONTROL : 0));
  notify(h, m == WM_LBUTTONDBLCLK ? NM_DBLCLK : NM_CLICK, &nm.hdr);
}

// Header: which column's button or divider is at x?
static int header_col(HWND h, lv_t *l, int x, int *divider) {
  int c, e = -l->sx;

  (void)h;
  *divider = 0;
  for (c = 0; c < l->ncol; ++c) {
    e += l->col[c].width;
    if (x >= e - 3 && x <= e + 2) {
      *divider = 1;
      return c;
    }
    if (x < e) {
      return c;
    }
  }
  return -1;
}

static LRESULT CALLBACK lv_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  lv_t *l = (lv_t *)rt_wnd_ctl(h);
  int i;

  switch (m) {
    case WM_NCCREATE:
      l = rt_alloc(sizeof(*l));
      if (l == 0) {
        return 0;
      }
      rt_memset(l, 0, sizeof(*l));
      l->focus = -1;
      l->anchor = -1;
      l->pressed = -1;
      l->bk = CLR_DEFAULT;
      l->tx = CLR_DEFAULT;
      l->txbk = CLR_NONE;
      rt_wnd_set_ctl(h, l);
      return 1;
    case WM_NCDESTROY:
      if (l != 0) {
        for (i = 0; i < l->n; ++i) item_free(&l->it[i]);
        for (i = 0; i < l->ncol; ++i) rt_free(l->col[i].title);
        rt_free(l->it);
        rt_free(l);
      }
      rt_wnd_set_ctl(h, 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (l != 0) paint(h, l);
      return 0;
    case WM_SIZE:
      if (l != 0) update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
      InvalidateRect(h, 0, FALSE);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_VSCROLL: case WM_HSCROLL: {                           // the window's own frame bars
      SCROLLINFO si;
      int bar = m == WM_VSCROLL ? SB_VERT : SB_HORZ, pos, line = bar == SB_VERT ? row_h(l) : 8;

      if (l == 0) return 0;
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(h, bar, &si);
      pos = bar == SB_VERT ? l->sy : l->sx;
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: return 0;
      }
      scroll_to(h, l, bar == SB_HORZ ? pos : l->sx, bar == SB_VERT ? pos : l->sy);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
      if (l == 0) return 0;
      if (hdr_h(h) > 0 && sy_(lp) < hdr_h(h)) {
        int div, c = header_col(h, l, sx_(lp), &div);

        SetFocus(h);
        if (c >= 0) {
          SetCapture(h);
          if (div) {
            l->track = 1;
            l->trackcol = c;
            l->trackx = sx_(lp);
            l->trackw = l->col[c].width;
          } else {
            l->pressed = c;
            InvalidateRect(h, 0, FALSE);
          }
        }
        return 0;
      }
      do_click(h, l, m, wp, lp);
      return 0;
    case WM_MOUSEMOVE:
      if (l != 0 && l->track) {
        int w = l->trackw + sx_(lp) - l->trackx;

        l->col[l->trackcol].width = w < 4 ? 4 : w;
        update_bars(h, l);
        InvalidateRect(h, 0, FALSE);
      } else if (l != 0 && l->pressed >= 0) {
        int div, c = header_col(h, l, sx_(lp), &div);
        int inside = c == l->pressed && !div && sy_(lp) < hdr_h(h);
        (void)inside;
      }
      return 0;
    case WM_LBUTTONUP:
      if (l == 0) return 0;
      if (l->track) {
        ReleaseCapture();
        l->track = 0;
      } else if (l->pressed >= 0) {
        int div, c = header_col(h, l, sx_(lp), &div), col = l->pressed;

        ReleaseCapture();
        l->pressed = -1;
        InvalidateRect(h, 0, FALSE);
        if (c == col && sy_(lp) < hdr_h(h) && sy_(lp) >= 0) {
          item_notify(h, l, LVN_COLUMNCLICK, -1, col, 0, 0, 0);
        }
      }
      return 0;
    case WM_KEYDOWN:
      if (l != 0) key(h, l, (UINT)wp);
      return 0;
    case LVM_GETITEMCOUNT:
      return l != 0 ? l->n : 0;
    case LVM_INSERTITEMA: {
      const LVITEMA *it = (const LVITEMA *)lp;
      int at;

      if (l == 0 || it == 0 || !items_reserve(l, l->n + 1)) return -1;
      at = it->iItem < 0 ? 0 : (it->iItem > l->n ? l->n : it->iItem);
      for (i = l->n; i > at; --i) l->it[i] = l->it[i - 1];
      rt_memset(&l->it[at], 0, sizeof(lvi_t));
      ++l->n;
      if ((it->mask & LVIF_TEXT) && it->pszText != 0) set_text(&l->it[at], 0, it->pszText);
      else set_text(&l->it[at], 0, "");
      if (it->mask & LVIF_PARAM) l->it[at].param = it->lParam;
      if (it->mask & LVIF_STATE) l->it[at].state = (it->state & it->stateMask) & ~(UINT)LVIS_FOCUSED;
      if (it->mask & LVIF_IMAGE) l->it[at].image = it->iImage;
      if (l->focus >= at) ++l->focus;
      if (l->anchor >= at) ++l->anchor;
      update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return at;
    }
    case LVM_DELETEITEM:
      if (l == 0 || (int)wp < 0 || (int)wp >= l->n) return FALSE;
      delete_item(h, l, (int)wp);
      update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case LVM_DELETEALLITEMS:
      if (l == 0) return FALSE;
      {
        NMLISTVIEW nm;

        rt_memset(&nm, 0, sizeof(nm));
        nm.iItem = -1;
        notify(h, LVN_DELETEALLITEMS, &nm.hdr);
      }
      for (i = 0; i < l->n; ++i) item_free(&l->it[i]);
      l->n = 0;
      l->focus = -1;
      l->anchor = -1;
      l->sx = l->sy = 0;
      update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case LVM_GETITEMA: {
      LVITEMA *it = (LVITEMA *)lp;

      if (l == 0 || it == 0 || it->iItem < 0 || it->iItem >= l->n) return FALSE;
      fill_lvitem(l, it->iItem, it);
      return TRUE;
    }
    case LVM_SETITEMA: {
      const LVITEMA *it = (const LVITEMA *)lp;

      if (l == 0 || it == 0 || it->iItem < 0 || it->iItem >= l->n) return FALSE;
      if ((it->mask & LVIF_TEXT) && it->pszText != 0) set_text(&l->it[it->iItem], it->iSubItem, it->pszText);
      if (it->mask & LVIF_PARAM) l->it[it->iItem].param = it->lParam;
      if (it->mask & LVIF_IMAGE) l->it[it->iItem].image = it->iImage;
      if (it->mask & LVIF_STATE) set_state(h, l, it->iItem, it->stateMask, it->state);
      {
        RECT r;

        item_rect(h, l, it->iItem, &r);
        InvalidateRect(h, &r, FALSE);
      }
      return TRUE;
    }
    case LVM_SETITEMTEXTA: {
      const LVITEMA *it = (const LVITEMA *)lp;

      if (l == 0 || it == 0 || (int)wp < 0 || (int)wp >= l->n) return FALSE;
      set_text(&l->it[wp], it->iSubItem, it->pszText);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    }
    case LVM_GETITEMTEXTA: {
      LVITEMA *it = (LVITEMA *)lp;
      LVITEMA q;

      if (l == 0 || it == 0 || (int)wp < 0 || (int)wp >= l->n) return 0;
      q = *it;
      q.mask = LVIF_TEXT;
      fill_lvitem(l, (int)wp, &q);
      return slen(it->pszText);
    }
    case LVM_SETITEMSTATE: {
      const LVITEMA *it = (const LVITEMA *)lp;

      if (l == 0 || it == 0) return FALSE;
      if ((int)wp == -1) {
        for (i = 0; i < l->n; ++i) set_state(h, l, i, it->stateMask, it->state);
      } else {
        set_state(h, l, (int)wp, it->stateMask, it->state);
      }
      return TRUE;
    }
    case LVM_GETITEMSTATE:
      return l != 0 && (int)wp >= 0 && (int)wp < l->n ? (LRESULT)(l->it[wp].state & (UINT)lp) : 0;
    case LVM_GETNEXTITEM:
      return l != 0 ? next_item(l, (int)wp, (UINT)lp) : -1;
    case LVM_GETSELECTEDCOUNT:
      return l != 0 ? selected_count(l) : 0;
    case LVM_FINDITEMA: {
      const LVFINDINFOA *f = (const LVFINDINFOA *)lp;

      if (l == 0 || f == 0) return -1;
      for (i = (int)wp + 1; i < l->n; ++i) {
        if ((f->flags & LVFI_PARAM) && l->it[i].param == f->lParam) return i;
        if ((f->flags & (LVFI_STRING | LVFI_PARTIAL)) && f->psz != 0 && l->it[i].text[0] != 0) {
          const char *a = l->it[i].text[0], *b = f->psz;
          int k = 0;

          while (b[k] != 0 && a[k] != 0 && (a[k] | 0x20) == (b[k] | 0x20)) ++k;      // case-insensitive
          if (b[k] == 0 && ((f->flags & LVFI_PARTIAL) || a[k] == 0)) return i;
        }
      }
      return -1;
    }
    case LVM_SORTITEMS:
      if (l != 0 && lp != 0) sort_items(h, l, (PFNLVCOMPARE)lp, (LPARAM)wp);
      return TRUE;
    case LVM_HITTEST: case LVM_SUBITEMHITTEST: {
      LVHITTESTINFO *hi = (LVHITTESTINFO *)lp;
      int sub, r;

      if (l == 0 || hi == 0) return -1;
      r = hit(h, l, hi->pt.x, hi->pt.y, &sub, &hi->flags);
      hi->iItem = r;
      hi->iSubItem = r >= 0 ? sub : 0;
      return r;
    }
    case LVM_GETITEMRECT: {
      RECT *r = (RECT *)lp;
      int code = r != 0 ? r->left : 0;

      if (l == 0 || r == 0 || (int)wp < 0 || (int)wp >= l->n) return FALSE;
      if (code == LVIR_LABEL || code == LVIR_ICON) label_rect(h, l, (int)wp, r);
      else item_rect(h, l, (int)wp, r);
      if (code == LVIR_SELECTBOUNDS) item_rect(h, l, (int)wp, r);
      return TRUE;
    }
    case LVM_GETSUBITEMRECT: {
      RECT *r = (RECT *)lp;
      int sub = r != 0 ? r->top : 0, c, x;

      if (l == 0 || r == 0 || (int)wp < 0 || (int)wp >= l->n || sub < 0 || sub >= (l->ncol > 0 ? l->ncol : 1)) return FALSE;
      item_rect(h, l, (int)wp, r);
      x = r->left;
      for (c = 0; c < sub; ++c) x += col_w(l, c);
      r->left = x;
      r->right = x + (view_of(h) == LVS_REPORT ? col_w(l, sub) : r->right - r->left);
      return TRUE;
    }
    case LVM_ENSUREVISIBLE:
      if (l != 0) ensure_visible(h, l, (int)wp);
      return TRUE;
    case LVM_GETTOPINDEX:
      if (l == 0) return 0;
      if (view_of(h) == LVS_REPORT) return l->sy / row_h(l);
      return 0;
    case LVM_GETCOUNTPERPAGE: {
      int w, hh;

      if (l == 0) return 0;
      client_of(h, &w, &hh);
      return view_of(h) == LVS_REPORT ? (hh - hdr_h(h)) / row_h(l) : hh / row_h(l);
    }
    case LVM_SCROLL:
      if (l != 0) scroll_to(h, l, l->sx + (int)wp, l->sy + (int)lp);
      return TRUE;
    case LVM_UPDATE: case LVM_REDRAWITEMS:
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case LVM_INSERTCOLUMNA: {
      int at;

      if (l == 0 || lp == 0) return -1;
      at = col_insert(l, (int)wp, (const LVCOLUMNA *)lp);
      update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return at;
    }
    case LVM_DELETECOLUMN:
      if (l == 0 || (int)wp < 0 || (int)wp >= l->ncol) return FALSE;
      col_delete(l, (int)wp);
      update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case LVM_GETCOLUMNA: case LVM_SETCOLUMNA: {
      LVCOLUMNA *c = (LVCOLUMNA *)lp;

      if (l == 0 || c == 0 || (int)wp < 0 || (int)wp >= l->ncol) return FALSE;
      if (m == LVM_SETCOLUMNA) {
        if ((c->mask & LVCF_TEXT) && c->pszText != 0) {
          char *d = sdup(c->pszText);

          if (d != 0) {
            rt_free(l->col[wp].title);
            l->col[wp].title = d;
          }
        }
        if (c->mask & LVCF_WIDTH) l->col[wp].width = c->cx;
        if (c->mask & LVCF_FMT) l->col[wp].fmt = c->fmt;
        update_bars(h, l);
        InvalidateRect(h, 0, FALSE);
      } else {
        if (c->mask & LVCF_WIDTH) c->cx = l->col[wp].width;
        if (c->mask & LVCF_FMT) c->fmt = l->col[wp].fmt;
        if ((c->mask & LVCF_TEXT) && c->pszText != 0 && c->cchTextMax > 0) {
          const char *t = l->col[wp].title;
          int n = slen(t), k;

          if (n > c->cchTextMax - 1) n = c->cchTextMax - 1;
          for (k = 0; k < n; ++k) c->pszText[k] = t[k];
          c->pszText[n] = 0;
        }
      }
      return TRUE;
    }
    case LVM_SETCOLUMNWIDTH:
      if (l == 0 || (int)wp < 0 || (int)wp >= (l->ncol > 0 ? l->ncol : 1)) return FALSE;
      if (l->ncol == 0) return TRUE;
      if ((short)LOWORD(lp) == LVSCW_AUTOSIZE) l->col[wp].width = autosize(l, (int)wp, 0);
      else if ((short)LOWORD(lp) == LVSCW_AUTOSIZE_USEHEADER) l->col[wp].width = autosize(l, (int)wp, 1);
      else l->col[wp].width = (short)LOWORD(lp);
      update_bars(h, l);
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case LVM_GETCOLUMNWIDTH:
      return l != 0 ? (view_of(h) == LVS_LIST ? list_cell_w(l) : col_w(l, (int)wp)) : 0;
    case LVM_SETEXTENDEDLISTVIEWSTYLE:
      if (l == 0) return 0;
      {
        UINT old = l->ex, mask = wp != 0 ? (UINT)wp : 0xFFFFFFFFu;

        l->ex = (l->ex & ~mask) | ((UINT)lp & mask);
        update_bars(h, l);
        InvalidateRect(h, 0, FALSE);
        return old;
      }
    case LVM_GETEXTENDEDLISTVIEWSTYLE:
      return l != 0 ? l->ex : 0;
    case LVM_SETBKCOLOR: case LVM_SETTEXTCOLOR: case LVM_SETTEXTBKCOLOR:
      if (l == 0) return FALSE;
      if (m == LVM_SETBKCOLOR) l->bk = (COLORREF)lp;
      else if (m == LVM_SETTEXTCOLOR) l->tx = (COLORREF)lp;
      else l->txbk = (COLORREF)lp;
      InvalidateRect(h, 0, FALSE);
      return TRUE;
    case LVM_GETBKCOLOR:
      return l != 0 ? (LRESULT)(l->bk == CLR_DEFAULT ? GetSysColor(COLOR_WINDOW) : l->bk) : 0;
    case LVM_GETTEXTCOLOR:
      return l != 0 ? (LRESULT)(l->tx == CLR_DEFAULT ? GetSysColor(COLOR_WINDOWTEXT) : l->tx) : 0;
    case LVM_GETTEXTBKCOLOR:
      return l != 0 ? (LRESULT)l->txbk : 0;
    case LVM_SETIMAGELIST: {
      HIMAGELIST old;

      if (l == 0 || (int)wp < 0 || (int)wp > 2) return 0;
      old = l->il[wp];
      l->il[wp] = (HIMAGELIST)lp;
      update_bars(h, l);
      InvalidateRect(h, 0, TRUE);
      return (LRESULT)old;
    }
    case LVM_GETIMAGELIST:
      return l != 0 && (int)wp >= 0 && (int)wp <= 2 ? (LRESULT)l->il[wp] : 0;
    case LVM_GETHEADER: case LVM_EDITLABELA: case LVM_GETEDITCONTROL: case LVM_SETITEMCOUNT:
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

LRESULT CALLBACK rt_listview_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) { return lv_proc(h, m, wp, lp); }
