// Menus for the Win32 runtime: menu objects (CreateMenu, AppendMenu, ...), the menu bar of a
// top-level window, popup menus (TrackPopupMenu and the submenus of the bar), keyboard
// accelerators. See docs/WIN32.md.
//
// A window with a menu has RT_MENU_H pixels at the top of its surface for the bar (the client area
// starts below it). Popups are not windows: they are drawn over the client area of the window's
// surface while a modal loop (track) reads the window's input events itself, and when one closes
// the window is painted again over the place it covered. A popup is clamped to the surface, so
// in a small window a long menu is cut short.
#include "internal.h"

#define MAX_MENUS 96
#define MAX_LV    5
#define ITEM_H    18
#define SEP_H     6
#define COL_FACE  0x00F0F0F0u
#define COL_DARK  0x00646464u
#define COL_GRAY  0x00A0A0A0u
#define COL_HOT   0x000078D7u

typedef struct menu menu_t;
typedef struct {
  UINT id;
  UINT flags;                               // MF_GRAYED, MF_DISABLED, MF_CHECKED, MF_SEPARATOR, MFT_RADIOCHECK (0x200)
  char *text;                               // with '&' mnemonics and an accelerator after '\t'
  menu_t *sub;
  ULONG_PTR data;
} mitem_t;

struct menu {
  int used;
  mitem_t *it;
  int n, cap;
};

static menu_t g_menu[MAX_MENUS];

static menu_t *mget(HMENU h) {
  menu_t *m = (menu_t *)h;

  return (m >= g_menu && m < g_menu + MAX_MENUS && m->used) ? m : 0;
}

int rt_menu_valid(HMENU h) { return mget(h) != 0; }

EXPORT HMENU WINAPI CreateMenu(void) {
  int i;

  for (i = 0; i < MAX_MENUS; ++i) {
    if (!g_menu[i].used) {
      rt_memset(&g_menu[i], 0, sizeof(g_menu[i]));
      g_menu[i].used = 1;
      return (HMENU)&g_menu[i];
    }
  }
  SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  return 0;
}

EXPORT HMENU WINAPI CreatePopupMenu(void) { return CreateMenu(); }
EXPORT BOOL WINAPI IsMenu(HMENU h) { return mget(h) != 0; }

static void free_item(mitem_t *it, int destroy_sub) {
  rt_free(it->text);
  it->text = 0;
  if (destroy_sub && it->sub != 0) {
    rt_menu_destroy((HMENU)it->sub);
  }
}

void rt_menu_destroy(HMENU h) {
  menu_t *m = mget(h);
  int i;

  if (m == 0) {
    return;
  }
  m->used = 0;
  for (i = 0; i < m->n; ++i) {
    free_item(&m->it[i], 1);
  }
  rt_free(m->it);
  m->it = 0;
  m->n = m->cap = 0;
}

EXPORT BOOL WINAPI DestroyMenu(HMENU h) {
  if (mget(h) == 0) {
    return FALSE;
  }
  rt_menu_destroy(h);
  return TRUE;
}

// The item `v` names (a command id, or a position with MF_BYPOSITION); submenus are searched for ids.
static mitem_t *lookup(menu_t *m, UINT v, UINT flags, menu_t **owner, int *index) {
  int i;

  if (flags & MF_BYPOSITION) {
    if ((int)v < 0 || (int)v >= m->n) {
      return 0;
    }
    if (owner) *owner = m;
    if (index) *index = (int)v;
    return &m->it[v];
  }
  for (i = 0; i < m->n; ++i) {
    mitem_t *r;

    if (m->it[i].sub == 0 && !(m->it[i].flags & MF_SEPARATOR) && m->it[i].id == v) {
      if (owner) *owner = m;
      if (index) *index = i;
      return &m->it[i];
    }
    if (m->it[i].sub != 0 && (r = lookup(m->it[i].sub, v, flags, owner, index)) != 0) {
      return r;
    }
  }
  return 0;
}

static int insert_item(menu_t *m, int pos, UINT flags, UINT_PTR id, const char *text, ULONG_PTR data) {
  mitem_t it;
  int i;

  if (m->n == m->cap) {
    int cap = m->cap == 0 ? 8 : m->cap * 2;
    mitem_t *ni = rt_alloc(sizeof(mitem_t) * (size_t)cap);

    if (ni == 0) {
      return 0;
    }
    if (m->it != 0) {
      rt_memcpy(ni, m->it, sizeof(mitem_t) * (size_t)m->n);
      rt_free(m->it);
    }
    m->it = ni;
    m->cap = cap;
  }
  rt_memset(&it, 0, sizeof(it));
  it.flags = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_SEPARATOR | 0x200);
  it.data = data;
  if (flags & MF_POPUP) {
    it.sub = mget((HMENU)id);
    it.id = (UINT)id;
  } else {
    it.id = (UINT)id;
  }
  if (!(flags & (MF_SEPARATOR | MF_OWNERDRAW | MF_BITMAP)) && text != 0) {
    it.text = rt_alloc(rt_strlen(text) + 1);
    if (it.text == 0) {
      return 0;
    }
    rt_copy(it.text, text, rt_strlen(text) + 1);
  }
  if (pos < 0 || pos > m->n) {
    pos = m->n;
  }
  for (i = m->n; i > pos; --i) {
    m->it[i] = m->it[i - 1];
  }
  m->it[pos] = it;
  ++m->n;
  return 1;
}

EXPORT BOOL WINAPI AppendMenuA(HMENU h, UINT flags, UINT_PTR id, LPCSTR text) {
  menu_t *m = mget(h);

  return m != 0 && insert_item(m, -1, flags, id, text, (ULONG_PTR)text);
}

EXPORT BOOL WINAPI InsertMenuA(HMENU h, UINT pos, UINT flags, UINT_PTR id, LPCSTR text) {
  menu_t *m = mget(h);
  int idx;

  if (m == 0) {
    return FALSE;
  }
  if (pos == (UINT)-1) {
    idx = -1;
  } else if (flags & MF_BYPOSITION) {
    idx = (int)pos;
  } else {
    int at = -1;

    idx = lookup(m, pos, 0, 0, &at) != 0 ? at : -1;
  }
  return insert_item(m, idx, flags, id, text, (ULONG_PTR)text);
}

static BOOL remove_item(HMENU h, UINT v, UINT flags, int destroy_sub) {
  menu_t *m = mget(h), *own = 0;
  int idx = 0, i;

  if (m == 0 || lookup(m, v, flags, &own, &idx) == 0) {
    return FALSE;
  }
  free_item(&own->it[idx], destroy_sub);
  for (i = idx; i + 1 < own->n; ++i) {
    own->it[i] = own->it[i + 1];
  }
  --own->n;
  return TRUE;
}

EXPORT BOOL WINAPI DeleteMenu(HMENU h, UINT v, UINT flags) { return remove_item(h, v, flags, 1); }
EXPORT BOOL WINAPI RemoveMenu(HMENU h, UINT v, UINT flags) { return remove_item(h, v, flags, 0); }

EXPORT BOOL WINAPI ModifyMenuA(HMENU h, UINT v, UINT flags, UINT_PTR id, LPCSTR text) {
  menu_t *m = mget(h), *own = 0;
  int idx = 0;

  if (m == 0 || lookup(m, v, flags, &own, &idx) == 0) {
    return FALSE;
  }
  free_item(&own->it[idx], 0);
  {
    int i;

    for (i = idx; i + 1 < own->n; ++i) {
      own->it[i] = own->it[i + 1];
    }
    --own->n;
  }
  return insert_item(own, idx, flags, id, text, (ULONG_PTR)text);
}

EXPORT int WINAPI GetMenuItemCount(HMENU h) { menu_t *m = mget(h); return m != 0 ? m->n : -1; }

EXPORT UINT WINAPI GetMenuItemID(HMENU h, int pos) {
  menu_t *m = mget(h);

  if (m == 0 || pos < 0 || pos >= m->n) {
    return (UINT)-1;
  }
  return m->it[pos].sub != 0 ? (UINT)-1 : (m->it[pos].flags & MF_SEPARATOR) ? 0 : m->it[pos].id;
}

EXPORT HMENU WINAPI GetSubMenu(HMENU h, int pos) {
  menu_t *m = mget(h);

  return m != 0 && pos >= 0 && pos < m->n ? (HMENU)m->it[pos].sub : 0;
}

EXPORT int WINAPI GetMenuStringA(HMENU h, UINT v, LPSTR buf, int n, UINT flags) {
  menu_t *m = mget(h);
  mitem_t *it = m != 0 ? lookup(m, v, flags, 0, 0) : 0;
  const char *t;

  if (it == 0) {
    return 0;
  }
  t = it->text != 0 ? it->text : "";
  if (buf == 0 || n <= 0) {
    return (int)rt_strlen(t);
  }
  rt_copy(buf, t, (size_t)n);
  return (int)rt_strlen(buf);
}

EXPORT UINT WINAPI GetMenuState(HMENU h, UINT v, UINT flags) {
  menu_t *m = mget(h);
  mitem_t *it = m != 0 ? lookup(m, v, flags, 0, 0) : 0;

  if (it == 0) {
    return (UINT)-1;
  }
  if (it->sub != 0) {
    return (it->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED)) | MF_POPUP | ((UINT)it->sub->n << 8);
  }
  return it->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_SEPARATOR);
}

EXPORT DWORD WINAPI CheckMenuItem(HMENU h, UINT v, UINT flags) {
  menu_t *m = mget(h);
  mitem_t *it = m != 0 ? lookup(m, v, flags, 0, 0) : 0;
  DWORD old;

  if (it == 0) {
    return (DWORD)-1;
  }
  old = (it->flags & MF_CHECKED) ? MF_CHECKED : MF_UNCHECKED;
  it->flags = (it->flags & ~(UINT)MF_CHECKED) | (flags & MF_CHECKED);
  return old;
}

EXPORT BOOL WINAPI EnableMenuItem(HMENU h, UINT v, UINT flags) {
  menu_t *m = mget(h);
  mitem_t *it = m != 0 ? lookup(m, v, flags, 0, 0) : 0;
  BOOL old;

  if (it == 0) {
    return (BOOL)-1;
  }
  old = (BOOL)(it->flags & (MF_GRAYED | MF_DISABLED));
  it->flags = (it->flags & ~(UINT)(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED));
  return old;
}

EXPORT BOOL WINAPI CheckMenuRadioItem(HMENU h, UINT first, UINT last, UINT check, UINT flags) {
  menu_t *m = mget(h);
  UINT i;

  if (m == 0) {
    return FALSE;
  }
  for (i = first; i <= last; ++i) {
    mitem_t *it = lookup(m, i, flags, 0, 0);

    if (it != 0) {
      it->flags = (it->flags & ~(UINT)(MF_CHECKED | 0x200)) | (i == check ? (MF_CHECKED | 0x200) : 0);
    }
  }
  return TRUE;
}

// The MENUITEMINFO forms.
static UINT info_flags(const MENUITEMINFOA *mi) {
  UINT f = 0;

  if ((mi->fMask & MIIM_STATE) && (mi->fState & MFS_DISABLED)) f |= MF_GRAYED;
  if ((mi->fMask & MIIM_STATE) && (mi->fState & MFS_CHECKED)) f |= MF_CHECKED;
  if ((mi->fMask & MIIM_FTYPE) || (mi->fMask & MIIM_TYPE)) {
    if (mi->fType & MFT_SEPARATOR) f |= MF_SEPARATOR;
    if (mi->fType & MFT_RADIOCHECK) f |= 0x200;
  }
  if ((mi->fMask & MIIM_SUBMENU) && mi->hSubMenu != 0) f |= MF_POPUP;
  return f;
}

EXPORT BOOL WINAPI InsertMenuItemA(HMENU h, UINT v, BOOL by_pos, const MENUITEMINFOA *mi) {
  menu_t *m = mget(h);
  UINT f = info_flags(mi);
  int idx = -1, at;

  if (m == 0) {
    return FALSE;
  }
  if (by_pos) {
    idx = (int)v;
  } else if (v != 0 && lookup(m, v, 0, 0, &at) != 0) {
    idx = at;
  }
  return insert_item(m, idx, f, (f & MF_POPUP) ? (UINT_PTR)mi->hSubMenu : ((mi->fMask & MIIM_ID) ? mi->wID : 0),
                     (mi->fMask & (MIIM_STRING | MIIM_TYPE)) ? mi->dwTypeData : 0, mi->dwItemData);
}

EXPORT BOOL WINAPI GetMenuItemInfoA(HMENU h, UINT v, BOOL by_pos, LPMENUITEMINFOA mi) {
  menu_t *m = mget(h);
  mitem_t *it = m != 0 ? lookup(m, v, by_pos ? MF_BYPOSITION : 0, 0, 0) : 0;

  if (it == 0) {
    return FALSE;
  }
  if (mi->fMask & MIIM_ID) mi->wID = it->sub != 0 ? 0 : it->id;
  if (mi->fMask & MIIM_SUBMENU) mi->hSubMenu = (HMENU)it->sub;
  if (mi->fMask & MIIM_STATE) mi->fState = (it->flags & (MF_GRAYED | MF_DISABLED) ? MFS_DISABLED : 0) | (it->flags & MF_CHECKED ? MFS_CHECKED : 0);
  if (mi->fMask & (MIIM_FTYPE | MIIM_TYPE)) mi->fType = ((it->flags & MF_SEPARATOR) ? MFT_SEPARATOR : 0) | ((it->flags & 0x200) ? MFT_RADIOCHECK : 0);
  if (mi->fMask & MIIM_DATA) mi->dwItemData = it->data;
  if ((mi->fMask & (MIIM_STRING | MIIM_TYPE)) && mi->dwTypeData != 0 && mi->cch > 0) {
    const char *t = it->text != 0 ? it->text : "";

    rt_copy(mi->dwTypeData, t, mi->cch);
    mi->cch = (UINT)rt_strlen(mi->dwTypeData);
  } else if (mi->fMask & (MIIM_STRING | MIIM_TYPE)) {
    mi->cch = it->text != 0 ? (UINT)rt_strlen(it->text) : 0;
  }
  return TRUE;
}

EXPORT BOOL WINAPI SetMenuItemInfoA(HMENU h, UINT v, BOOL by_pos, const MENUITEMINFOA *mi) {
  menu_t *m = mget(h);
  mitem_t *it = m != 0 ? lookup(m, v, by_pos ? MF_BYPOSITION : 0, 0, 0) : 0;

  if (it == 0) {
    return FALSE;
  }
  if (mi->fMask & MIIM_ID) it->id = mi->wID;
  if (mi->fMask & MIIM_SUBMENU) it->sub = mget(mi->hSubMenu);
  if (mi->fMask & MIIM_DATA) it->data = mi->dwItemData;
  if (mi->fMask & MIIM_STATE) {
    it->flags = (it->flags & ~(UINT)(MF_GRAYED | MF_DISABLED | MF_CHECKED)) | ((mi->fState & MFS_DISABLED) ? MF_GRAYED : 0) | ((mi->fState & MFS_CHECKED) ? MF_CHECKED : 0);
  }
  if ((mi->fMask & (MIIM_FTYPE | MIIM_TYPE))) {
    it->flags = (it->flags & ~(UINT)(MF_SEPARATOR | 0x200)) | ((mi->fType & MFT_SEPARATOR) ? MF_SEPARATOR : 0) | ((mi->fType & MFT_RADIOCHECK) ? 0x200 : 0);
  }
  if ((mi->fMask & (MIIM_STRING | MIIM_TYPE)) && mi->dwTypeData != 0) {
    char *c = rt_alloc(rt_strlen(mi->dwTypeData) + 1);

    if (c != 0) {
      rt_copy(c, mi->dwTypeData, rt_strlen(mi->dwTypeData) + 1);
      rt_free(it->text);
      it->text = c;
    }
  }
  return TRUE;
}

// ---- Layout and drawing ----------------------------------------------------------------------------------------

// The label without '&' (the mnemonic's position goes to *mn, -1 if none) and the text after the tab.
static int split(const char *text, char *left, int max, char *right, int rmax, int *mn) {
  int n = 0, r = 0;

  *mn = -1;
  right[0] = 0;
  for (; text != 0 && *text != 0 && *text != '\t' && n < max - 1; ++text) {
    if (*text == '&') {
      if (text[1] == '&') {
        ++text;
      } else {
        if (*mn < 0) *mn = n;
        continue;
      }
    }
    left[n++] = *text;
  }
  left[n] = 0;
  if (text != 0 && *text == '\t') {
    for (++text; *text != 0 && r < rmax - 1; ++text) right[r++] = *text;
    right[r] = 0;
  }
  return n;
}

static int is_sep(const mitem_t *it) { return (it->flags & MF_SEPARATOR) != 0; }
static int is_dis(const mitem_t *it) { return (it->flags & (MF_GRAYED | MF_DISABLED)) != 0; }
static int item_h(const mitem_t *it) { return is_sep(it) ? SEP_H : ITEM_H; }

typedef struct {
  menu_t *m;
  int x, y, w, h;
  int hot;
} lvl_t;

typedef struct {
  HWND hwnd;
  int sysid, sw, sh, mb;
  menu_t *root;
  int bar, bar_hot;
  lvl_t lv[MAX_LV];
  int depth;
  int moved, done, cmd, dirty;
} trk_t;

static trk_t g_t;
static int g_tracking;

static void popup_size(menu_t *m, int *w, int *h) {
  int i, mw = 80, hh = 4;
  char l[96], r[48];
  int mn;

  for (i = 0; i < m->n; ++i) {
    int lw, rw;

    hh += item_h(&m->it[i]);
    if (is_sep(&m->it[i])) continue;
    lw = split(m->it[i].text != 0 ? m->it[i].text : "", l, sizeof(l), r, sizeof(r), &mn) * 8;
    rw = (int)rt_strlen(r) * 8;
    lw += 24 + 22 + (rw > 0 ? 20 + rw : 0);
    if (lw > mw) mw = lw;
  }
  *w = mw;
  *h = hh;
}

static int item_y(const lvl_t *v, int i) {
  int y = v->y + 2, k;

  for (k = 0; k < i; ++k) y += item_h(&v->m->it[k]);
  return y;
}

static int item_at(const lvl_t *v, int x, int y) {
  int i, yy = v->y + 2;

  if (x < v->x + 1 || x >= v->x + v->w - 1) return -1;
  for (i = 0; i < v->m->n; ++i) {
    int hh = item_h(&v->m->it[i]);

    if (y >= yy && y < yy + hh) return i;
    yy += hh;
  }
  return -1;
}

static void bar_item(const menu_t *m, int i, int *x, int *w) {
  int k, xx = 2;
  char l[96], r[16];
  int mn;

  for (k = 0; k < i; ++k) {
    xx += split(m->it[k].text != 0 ? m->it[k].text : "", l, sizeof(l), r, sizeof(r), &mn) * 8 + 16;
  }
  *x = xx;
  *w = split(m->it[i].text != 0 ? m->it[i].text : "", l, sizeof(l), r, sizeof(r), &mn) * 8 + 16;
}

static int bar_at(const menu_t *m, int x) {
  int i, bx, bw;

  for (i = 0; i < m->n; ++i) {
    bar_item(m, i, &bx, &bw);
    if (x >= bx && x < bx + bw) return i;
  }
  return -1;
}

static void label(dc_t *dc, int x, int y, const char *text, int max_chars, uint32_t rgb, int underline) {
  char l[96], r[48];
  int mn, n = split(text, l, sizeof(l), r, sizeof(r), &mn);

  (void)max_chars;
  gdi_text(dc, x, y, l, n, rgb);
  if (underline && mn >= 0) {
    gdi_fill(dc, x + mn * 8, y + 9, 8, 1, rgb);
  }
}

static void draw_check(dc_t *dc, int x, int y, int radio, uint32_t rgb) {
  int i;

  if (radio) {
    gdi_fill(dc, x + 2, y + 3, 4, 6, rgb);
    gdi_fill(dc, x + 1, y + 4, 6, 4, rgb);
    return;
  }
  for (i = 0; i < 3; ++i) gdi_fill(dc, x + i, y + 4 + i, 1, 3, rgb);
  for (i = 0; i < 5; ++i) gdi_fill(dc, x + 3 + i, y + 6 - i, 1, 3, rgb);
}

static void draw_popup(dc_t *dc, const lvl_t *v) {
  int i, y = v->y + 2;

  gdi_fill(dc, v->x, v->y, v->w, v->h, COL_DARK);
  gdi_fill(dc, v->x + 1, v->y + 1, v->w - 2, v->h - 2, COL_FACE);
  for (i = 0; i < v->m->n; ++i) {
    const mitem_t *it = &v->m->it[i];
    int hh = item_h(it);

    if (is_sep(it)) {
      gdi_fill(dc, v->x + 4, y + 2, v->w - 8, 1, COL_GRAY);
      gdi_fill(dc, v->x + 4, y + 3, v->w - 8, 1, 0x00FFFFFFu);
    } else {
      int hot = v->hot == i && !is_dis(it);
      uint32_t fg = is_dis(it) ? COL_GRAY : hot ? 0x00FFFFFFu : 0;
      char l[96], r[48];
      int mn;

      if (hot) gdi_fill(dc, v->x + 2, y, v->w - 4, hh, COL_HOT);
      if (it->flags & MF_CHECKED) draw_check(dc, v->x + 7, y + 3, (it->flags & 0x200) != 0, fg);
      label(dc, v->x + 24, y + 5, it->text != 0 ? it->text : "", 0, fg, 1);
      split(it->text != 0 ? it->text : "", l, sizeof(l), r, sizeof(r), &mn);
      if (r[0] != 0) gdi_text(dc, v->x + v->w - 20 - (int)rt_strlen(r) * 8, y + 5, r, (int)rt_strlen(r), fg);
      if (it->sub != 0) {
        int k;

        for (k = 0; k < 4; ++k) gdi_fill(dc, v->x + v->w - 12 + k, y + 5 + k, 1, 8 - 2 * k, fg);
      }
    }
    y += hh;
  }
}

static void draw_bar_with(dc_t *dc, menu_t *m, int w, int hot) {
  int i;

  gdi_fill(dc, 0, 0, w, RT_MENU_H, COL_FACE);
  gdi_fill(dc, 0, RT_MENU_H - 1, w, 1, COL_GRAY);
  for (i = 0; i < m->n; ++i) {
    int bx, bw;
    uint32_t fg = is_dis(&m->it[i]) ? COL_GRAY : 0;

    bar_item(m, i, &bx, &bw);
    if (i == hot) {
      gdi_fill(dc, bx, 1, bw, RT_MENU_H - 3, COL_HOT);
      fg = 0x00FFFFFFu;
    }
    label(dc, bx + 8, 5, m->it[i].text != 0 ? m->it[i].text : "", 0, fg, 1);
  }
}

void rt_menu_draw_bar(HWND h) {
  int sysid, cw, ch, mb;
  menu_t *m = mget(rt_wnd_menu(h));
  RECT clip;
  dc_t *dc;

  if (m == 0 || !rt_wnd_surface(h, &sysid, &cw, &ch, &mb)) {
    return;
  }
  clip.left = 0; clip.top = 0; clip.right = cw; clip.bottom = RT_MENU_H;
  dc = gdi_dc_alloc(sysid, 0, 0, &clip);
  if (dc == 0) {
    return;
  }
  draw_bar_with(dc, m, cw, g_tracking && g_t.hwnd == h && g_t.bar ? g_t.bar_hot : -1);
  gdi_dc_free(dc);
}

// ---- Tracking ------------------------------------------------------------------------------------------------

static void redraw(trk_t *t) {
  RECT clip = {0, 0, t->sw, t->sh};
  dc_t *dc;
  int i;

  if (t->dirty) {                                         // a popup went away: show what was under it
    t->dirty = 0;
    InvalidateRect(t->hwnd, 0, TRUE);
    UpdateWindow(t->hwnd);
  }
  dc = gdi_dc_alloc(t->sysid, 0, 0, &clip);
  if (dc == 0) {
    return;
  }
  if (t->bar) {
    draw_bar_with(dc, t->root, t->sw, t->bar_hot);
  }
  for (i = 0; i < t->depth; ++i) draw_popup(dc, &t->lv[i]);
  gdi_dc_free(dc);
}

static int selectable(const mitem_t *it) { return !is_sep(it) && !is_dis(it); }

static int step(menu_t *m, int from, int dir) {
  int k, i = from;

  for (k = 0; k < m->n; ++k) {
    i += dir;
    if (i < 0) i = m->n - 1;
    if (i >= m->n) i = 0;
    if (selectable(&m->it[i])) return i;
  }
  return -1;
}

static void close_to(trk_t *t, int depth) {
  if (t->depth > depth) {
    t->depth = depth;
    t->dirty = 1;
  }
}

static void open_level(trk_t *t, menu_t *m, int x, int y, UINT align, int parent_x, int select_first) {
  lvl_t *v;
  int w, h;

  if (t->depth >= MAX_LV) {
    return;
  }
  SendMessageA(t->hwnd, WM_INITMENUPOPUP, (WPARAM)m, 0);
  popup_size(m, &w, &h);
  if (align & TPM_RIGHTALIGN) x -= w;
  if (align & TPM_BOTTOMALIGN) y -= h;
  if (x + w > t->sw) x = parent_x >= 0 && parent_x - w >= 0 ? parent_x - w : t->sw - w;
  if (y + h > t->sh) y = t->sh - h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  v = &t->lv[t->depth++];
  v->m = m;
  v->x = x;
  v->y = y;
  v->w = w;
  v->h = h;
  v->hot = select_first ? step(m, -1, 1) : -1;
}

static void open_sub(trk_t *t, int L, int i, int select_first) {
  lvl_t *v = &t->lv[L];

  open_level(t, v->m->it[i].sub, v->x + v->w - 2, item_y(v, i) - 2, 0, v->x + 2, select_first);
}

static void open_bar(trk_t *t, int j, int select_first) {
  int bx, bw;

  t->bar_hot = j;
  if (j >= 0 && t->root->it[j].sub != 0) {
    bar_item(t->root, j, &bx, &bw);
    open_level(t, t->root->it[j].sub, bx, t->mb, 0, -1, select_first);
  }
}

static int level_at(const trk_t *t, int x, int y) {
  int L;

  for (L = t->depth - 1; L >= 0; --L) {
    const lvl_t *v = &t->lv[L];

    if (x >= v->x && x < v->x + v->w && y >= v->y && y < v->y + v->h) return L;
  }
  return -1;
}

static void pick(trk_t *t, const mitem_t *it) {
  t->cmd = (int)it->id;
  t->done = 1;
}

static void hover(trk_t *t, int x, int y) {
  int L = level_at(t, x, y);

  if (L >= 0) {
    lvl_t *v = &t->lv[L];
    int i = item_at(v, x, y);

    if (i >= 0 && v->hot != i) {
      close_to(t, L + 1);
      v->hot = i;
      if (selectable(&v->m->it[i]) && v->m->it[i].sub != 0) open_sub(t, L, i, 0);
    }
  } else if (t->bar && y < t->mb) {
    int j = bar_at(t->root, x);

    if (j >= 0 && j != t->bar_hot) {
      close_to(t, 0);
      open_bar(t, j, 0);
    }
  }
}

static int mnemonic_item(menu_t *m, int ch) {
  int i;
  char l[96], r[16];
  int mn;

  if (ch >= 'A' && ch <= 'Z') ch += 32;
  for (i = 0; i < m->n; ++i) {
    if (m->it[i].text != 0 && selectable(&m->it[i])) {
      split(m->it[i].text, l, sizeof(l), r, sizeof(r), &mn);
      if (mn >= 0) {
        int c = l[mn];

        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == ch) return i;
      }
    }
  }
  return -1;
}

static void on_key(trk_t *t, unsigned ch) {
  lvl_t *v = t->depth > 0 ? &t->lv[t->depth - 1] : 0;

  switch (ch) {
    case SCKEY_DOWN: case SCKEY_UP:
      if (v == 0 && t->bar && t->bar_hot >= 0) {
        open_bar(t, t->bar_hot, 1);
      } else if (v != 0) {
        int i = step(v->m, v->hot < 0 ? (ch == SCKEY_DOWN ? -1 : 0) : v->hot, ch == SCKEY_DOWN ? 1 : -1);

        if (i >= 0) v->hot = i;
      }
      break;
    case SCKEY_RIGHT:
      if (v != 0 && v->hot >= 0 && v->m->it[v->hot].sub != 0 && selectable(&v->m->it[v->hot])) {
        open_sub(t, t->depth - 1, v->hot, 1);
      } else if (t->bar && t->depth <= 1) {
        close_to(t, 0);
        open_bar(t, (t->bar_hot + 1) % t->root->n, 1);
      }
      break;
    case SCKEY_LEFT:
      if (t->depth > 1) {
        close_to(t, t->depth - 1);
      } else if (t->bar) {
        close_to(t, 0);
        open_bar(t, (t->bar_hot + t->root->n - 1) % t->root->n, 1);
      }
      break;
    case 10: case 13:
      if (v != 0 && v->hot >= 0 && selectable(&v->m->it[v->hot])) {
        if (v->m->it[v->hot].sub != 0) open_sub(t, t->depth - 1, v->hot, 1);
        else pick(t, &v->m->it[v->hot]);
      }
      break;
    case 27:
      if (t->depth > 1) close_to(t, t->depth - 1);
      else t->done = 1;
      break;
    default:
      if (v != 0 && ch >= 32 && ch < 127) {
        int i = mnemonic_item(v->m, (int)ch);

        if (i >= 0) {
          v->hot = i;
          if (v->m->it[i].sub != 0) open_sub(t, t->depth - 1, i, 1);
          else pick(t, &v->m->it[i]);
        }
      }
      break;
  }
}

static int track(HWND hwnd, menu_t *root, int bar, int bar_index, int x, int y, UINT align, int select_first) {
  trk_t *t = &g_t;
  int cw, ch;

  if (g_tracking || !rt_wnd_surface(hwnd, &t->sysid, &cw, &ch, &t->mb)) {
    return 0;
  }
  g_tracking = 1;
  t->hwnd = hwnd;
  t->sw = cw;
  t->sh = ch + t->mb;
  t->root = root;
  t->bar = bar;
  t->bar_hot = -1;
  t->depth = 0;
  t->moved = t->done = t->cmd = t->dirty = 0;
  if (bar) {
    SendMessageA(hwnd, WM_INITMENU, (WPARAM)root, 0);
    open_bar(t, bar_index, select_first);
  } else {
    open_level(t, root, x, y, align, -1, 0);
  }
  redraw(t);
  while (!t->done) {
    sc_winev_t ev;
    sl_t r = sc3(KS_WIN_WAIT, t->sysid, (sl_t)&ev, 200);

    if (r < 0) {
      break;
    }
    if (r == 0) {
      continue;
    }
    switch (ev.type) {
      case SCEV_CLOSE:
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
        t->done = 1;
        break;
      case SCEV_RESIZE:
        t->done = 1;
        break;
      case SCEV_MOUSE_MOVE:
        t->moved = 1;
        hover(t, ev.x, ev.y);
        break;
      case SCEV_MOUSE_DOWN: {
        int L = level_at(t, ev.x, ev.y);

        if (L >= 0) {
          lvl_t *v = &t->lv[L];
          int i = item_at(v, ev.x, ev.y);

          if (i >= 0 && selectable(&v->m->it[i])) {
            v->hot = i;
            if (v->m->it[i].sub != 0) {
              if (t->depth == L + 1) open_sub(t, L, i, 0);
            } else {
              pick(t, &v->m->it[i]);
            }
          }
        } else if (t->bar && ev.y < t->mb) {
          int j = bar_at(t->root, ev.x);

          if (j == t->bar_hot && t->depth > 0) {
            t->done = 1;                                  // pressing the open title again closes it
          } else if (j >= 0) {
            close_to(t, 0);
            open_bar(t, j, 0);
          }
        } else {
          t->done = 1;                                    // a click elsewhere closes the menu
        }
        break;
      }
      case SCEV_MOUSE_UP: {
        int L = level_at(t, ev.x, ev.y);

        if (t->moved && L >= 0) {                         // released over an item after moving: choose it
          lvl_t *v = &t->lv[L];
          int i = item_at(v, ev.x, ev.y);

          if (i >= 0 && selectable(&v->m->it[i]) && v->m->it[i].sub == 0) pick(t, &v->m->it[i]);
        }
        break;
      }
      case SCEV_KEY:
        on_key(t, ev.ch);
        break;
      default:
        break;
    }
    if (!t->done) {
      redraw(t);
    }
  }
  g_tracking = 0;
  InvalidateRect(hwnd, 0, TRUE);                          // paint over the popups
  return t->cmd;
}

void rt_menu_track_bar(HWND h, int x) {
  menu_t *m = mget(rt_wnd_menu(h));
  int j = m != 0 ? bar_at(m, x) : -1, cmd;

  if (j < 0) {
    return;
  }
  cmd = track(h, m, 1, j, 0, 0, 0, 0);
  if (cmd != 0) {
    PostMessageA(h, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
  }
}

int rt_menu_alt_key(HWND h, unsigned ch) {
  menu_t *m = mget(rt_wnd_menu(h));
  int j, cmd;

  if (m == 0 || (j = mnemonic_item(m, (int)ch)) < 0) {
    return 0;
  }
  cmd = track(h, m, 1, j, 0, 0, 0, 1);
  if (cmd != 0) {
    PostMessageA(h, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
  }
  return 1;
}

EXPORT BOOL WINAPI TrackPopupMenu(HMENU h, UINT flags, int x, int y, int reserved, HWND hwnd, const RECT *rc) {
  menu_t *m = mget(h);
  int cmd;

  (void)reserved;
  (void)rc;
  if (m == 0) {
    return FALSE;
  }
  cmd = track(hwnd, m, 0, 0, x, y, flags, 0);
  if (flags & TPM_RETURNCMD) {
    return cmd;
  }
  if (cmd != 0 && !(flags & TPM_NONOTIFY)) {
    PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
  }
  return cmd != 0;
}

EXPORT BOOL WINAPI TrackPopupMenuEx(HMENU h, UINT flags, int x, int y, HWND hwnd, LPTPMPARAMS p) {
  (void)p;
  return TrackPopupMenu(h, flags, x, y, 0, hwnd, 0);
}

// ---- Accelerators --------------------------------------------------------------------------------------------

typedef struct {
  int used;
  ACCEL *a;
  int n;
} acc_t;

static acc_t g_acc[8];

EXPORT HACCEL WINAPI CreateAcceleratorTableA(LPACCEL a, int n) {
  int i;

  for (i = 0; i < 8; ++i) {
    if (!g_acc[i].used && n > 0) {
      g_acc[i].a = rt_alloc(sizeof(ACCEL) * (size_t)n);
      if (g_acc[i].a == 0) {
        return 0;
      }
      rt_memcpy(g_acc[i].a, a, sizeof(ACCEL) * (size_t)n);
      g_acc[i].n = n;
      g_acc[i].used = 1;
      return (HACCEL)&g_acc[i];
    }
  }
  return 0;
}

EXPORT BOOL WINAPI DestroyAcceleratorTable(HACCEL h) {
  acc_t *a = (acc_t *)h;

  if (a < g_acc || a >= g_acc + 8 || !a->used) {
    return FALSE;
  }
  rt_free(a->a);
  a->used = 0;
  return TRUE;
}

EXPORT int WINAPI TranslateAcceleratorA(HWND hwnd, HACCEL h, LPMSG m) {
  acc_t *a = (acc_t *)h;
  int i;
  int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

  if (a < g_acc || a >= g_acc + 8 || !a->used || (m->message != WM_KEYDOWN && m->message != WM_CHAR)) {
    return 0;
  }
  for (i = 0; i < a->n; ++i) {
    const ACCEL *e = &a->a[i];
    menu_t *mn = mget(rt_wnd_menu(hwnd));

    if (e->fVirt & FVIRTKEY) {
      if (m->message != WM_KEYDOWN || e->key != (WORD)m->wParam) continue;
      if (((e->fVirt & FCONTROL) != 0) != ctrl || ((e->fVirt & FSHIFT) != 0) != shift || ((e->fVirt & FALT) != 0) != alt) continue;
    } else if (m->message != WM_CHAR || e->key != (WORD)m->wParam) {
      continue;
    }
    if (mn != 0) {                                        // a disabled menu item does not fire
      mitem_t *it = lookup(mn, e->cmd, 0, 0, 0);

      if (it != 0 && is_dis(it)) return 0;
    }
    SendMessageA(hwnd, WM_COMMAND, MAKEWPARAM(e->cmd, 1), 0);
    return 1;
  }
  return 0;
}
