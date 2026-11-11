// gdi32 (and the drawing functions of user32) for the Win32 runtime. Drawing goes straight to
// the window's surface through the window syscalls: filled rectangles, 8x8-font text, lines
// made of small rectangles. There are no memory DCs or bitmaps, no clipping, and a single
// built-in font (8x8, so TEXTMETRIC and GetTextExtent report 8-pixel characters).
#include "internal.h"

#define MAX_DCS 16
#define MAX_OBJS 128

typedef struct {
  int used;
  int type;                   // OBJ_PEN, OBJ_BRUSH, OBJ_FONT
  COLORREF color;
  int style;                  // pen style / brush null flag
  int width;
  int stock;
} gobj_t;

static dc_t g_dc[MAX_DCS];
static gobj_t g_obj[MAX_OBJS];
static int g_obj_init;

// A little more than the classic system colors.
COLORREF gdi_sys_color(int i) {
  switch (i) {
    case COLOR_SCROLLBAR: return RGB(200, 200, 200);
    case COLOR_BACKGROUND: return RGB(58, 110, 165);
    case COLOR_ACTIVECAPTION: return RGB(0, 120, 215);
    case COLOR_INACTIVECAPTION: return RGB(153, 180, 209);
    case COLOR_MENU: return RGB(240, 240, 240);
    case COLOR_WINDOW: return RGB(255, 255, 255);
    case COLOR_WINDOWFRAME: return RGB(100, 100, 100);
    case COLOR_MENUTEXT: case COLOR_WINDOWTEXT: case COLOR_CAPTIONTEXT: return RGB(0, 0, 0);
    case COLOR_BTNFACE: return RGB(240, 240, 240);
    case COLOR_BTNSHADOW: return RGB(160, 160, 160);
    case COLOR_BTNTEXT: return RGB(0, 0, 0);
    case COLOR_HIGHLIGHT: return RGB(0, 120, 215);
    case COLOR_HIGHLIGHTTEXT: return RGB(255, 255, 255);
    case COLOR_BTNHIGHLIGHT: return RGB(255, 255, 255);
    default: return RGB(240, 240, 240);
  }
}

unsigned long rt_colorref_to_rgb(COLORREF c) { return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu); }

static HGDIOBJ obj_make(int type, COLORREF color, int style, int width, int stock) {
  int i;

  for (i = 0; i < MAX_OBJS; ++i) {
    if (!g_obj[i].used) {
      g_obj[i].used = 1;
      g_obj[i].type = type;
      g_obj[i].color = color;
      g_obj[i].style = style;
      g_obj[i].width = width;
      g_obj[i].stock = stock;
      return (HGDIOBJ)&g_obj[i];
    }
  }
  return 0;
}

static gobj_t *obj_get(HGDIOBJ h) {
  gobj_t *o = (gobj_t *)h;

  return (o >= g_obj && o < g_obj + MAX_OBJS && o->used) ? o : 0;
}

static HGDIOBJ g_stock[20];

static void stock_init(void) {
  if (g_obj_init) {
    return;
  }
  g_obj_init = 1;
  g_stock[WHITE_BRUSH] = obj_make(OBJ_BRUSH, RGB(255, 255, 255), 0, 0, 1);
  g_stock[LTGRAY_BRUSH] = obj_make(OBJ_BRUSH, RGB(192, 192, 192), 0, 0, 1);
  g_stock[GRAY_BRUSH] = obj_make(OBJ_BRUSH, RGB(128, 128, 128), 0, 0, 1);
  g_stock[DKGRAY_BRUSH] = obj_make(OBJ_BRUSH, RGB(64, 64, 64), 0, 0, 1);
  g_stock[BLACK_BRUSH] = obj_make(OBJ_BRUSH, RGB(0, 0, 0), 0, 0, 1);
  g_stock[NULL_BRUSH] = obj_make(OBJ_BRUSH, 0, 1, 0, 1);
  g_stock[WHITE_PEN] = obj_make(OBJ_PEN, RGB(255, 255, 255), PS_SOLID, 1, 1);
  g_stock[BLACK_PEN] = obj_make(OBJ_PEN, RGB(0, 0, 0), PS_SOLID, 1, 1);
  g_stock[NULL_PEN] = obj_make(OBJ_PEN, 0, PS_NULL, 0, 1);
  g_stock[OEM_FIXED_FONT] = obj_make(OBJ_FONT, 0, 0, 0, 1);
  g_stock[ANSI_FIXED_FONT] = g_stock[OEM_FIXED_FONT];
  g_stock[ANSI_VAR_FONT] = g_stock[OEM_FIXED_FONT];
  g_stock[SYSTEM_FONT] = g_stock[OEM_FIXED_FONT];
  g_stock[DEFAULT_GUI_FONT] = g_stock[OEM_FIXED_FONT];
}

HBRUSH gdi_sys_brush(int index) { return (HBRUSH)(uintptr_t)(index + 1); }

// A brush handle is a table entry, or (COLOR_xxx + 1) as in window classes.
static int brush_color(HBRUSH b, unsigned long *rgb) {
  gobj_t *o;

  if ((uintptr_t)b < 64) {
    *rgb = rt_colorref_to_rgb(gdi_sys_color((int)(uintptr_t)b - 1));
    return 1;
  }
  o = obj_get((HGDIOBJ)b);
  if (o == 0 || o->type != OBJ_BRUSH || o->style == 1) {
    return 0;
  }
  *rgb = rt_colorref_to_rgb(o->color);
  return 1;
}

// ---- Device contexts -----------------------------------------------------------------------------

dc_t *gdi_dc_alloc(int sysid) {
  int i;

  stock_init();
  for (i = 0; i < MAX_DCS; ++i) {
    if (!g_dc[i].used) {
      dc_t *d = &g_dc[i];

      rt_memset(d, 0, sizeof(*d));
      d->used = 1;
      d->sysid = sysid;
      d->text = RGB(0, 0, 0);
      d->bk = RGB(255, 255, 255);
      d->bkmode = OPAQUE;
      d->pen = g_stock[BLACK_PEN];
      d->brush = g_stock[WHITE_BRUSH];
      d->font = g_stock[SYSTEM_FONT];
      return d;
    }
  }
  return 0;
}

void gdi_dc_free(dc_t *d) {
  if (d != 0) {
    d->used = 0;
  }
}

dc_t *gdi_dc(HDC h) {
  dc_t *d = (dc_t *)h;

  return (d >= g_dc && d < g_dc + MAX_DCS && d->used) ? d : 0;
}

void gdi_fill(int sysid, int x, int y, int w, int h, uint32_t rgb) {
  sc_winop_t op;

  if (w <= 0 || h <= 0) {
    return;
  }
  op.x = x; op.y = y; op.w = w; op.h = h; op.color = rgb; op.scale = 0; op.ptr = 0;
  sc3(KS_WIN_FILL, sysid, (sl_t)&op, 0);
}

void gdi_text(int sysid, int x, int y, const char *s, int n, uint32_t rgb) {
  char buf[256];
  sc_winop_t op;

  if (n > 255) {
    n = 255;
  }
  rt_memcpy(buf, s, (size_t)n);
  buf[n] = 0;
  op.x = x; op.y = y; op.w = 0; op.h = 0; op.color = rgb; op.scale = 1; op.ptr = (uint64_t)(uintptr_t)buf;
  sc3(KS_WIN_TEXT, sysid, (sl_t)&op, 0);
}

EXPORT int WINAPI SaveDC(HDC dc) { (void)dc; return 1; }
EXPORT BOOL WINAPI RestoreDC(HDC dc, int n) { (void)dc; (void)n; return TRUE; }
EXPORT BOOL WINAPI DeleteDC(HDC dc) { gdi_dc_free(gdi_dc(dc)); return TRUE; }

EXPORT int WINAPI GetDeviceCaps(HDC dc, int index) {
  sl_t s = sc3(KS_SCREEN, 0, 0, 0);

  (void)dc;
  switch (index) {
    case HORZRES: return (int)(s >> 16);
    case VERTRES: return (int)(s & 0xFFFF);
    case BITSPIXEL: return 32;
    case PLANES: return 1;
    case LOGPIXELSX: case LOGPIXELSY: return 96;
    case HORZSIZE: return (int)((s >> 16) * 254 / 960);
    case VERTSIZE: return (int)((s & 0xFFFF) * 254 / 960);
    case NUMCOLORS: return -1;
    default: return 0;
  }
}

// ---- Objects --------------------------------------------------------------------------------------------

EXPORT HGDIOBJ WINAPI GetStockObject(int i) {
  stock_init();
  return i >= 0 && i < 20 ? g_stock[i] : 0;
}

EXPORT HBRUSH WINAPI CreateSolidBrush(COLORREF c) { return (HBRUSH)obj_make(OBJ_BRUSH, c, 0, 0, 0); }
EXPORT HPEN WINAPI CreatePen(int style, int width, COLORREF c) { return (HPEN)obj_make(OBJ_PEN, c, style, width < 1 ? 1 : width, 0); }
EXPORT HFONT WINAPI CreateFontA(int a, int b, int c, int d, int e, DWORD f, DWORD g, DWORD h, DWORD i, DWORD j, DWORD k, DWORD l, DWORD m, LPCSTR n) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; (void)m; (void)n;
  return (HFONT)obj_make(OBJ_FONT, 0, 0, 0, 0);
}
EXPORT HFONT WINAPI CreateFontIndirectA(const LOGFONTA *lf) { (void)lf; return (HFONT)obj_make(OBJ_FONT, 0, 0, 0, 0); }

EXPORT HGDIOBJ WINAPI SelectObject(HDC h, HGDIOBJ o) {
  dc_t *d = gdi_dc(h);
  gobj_t *g = obj_get(o);
  HGDIOBJ old = 0;

  if (d == 0 || (g == 0 && (uintptr_t)o >= 64)) {
    return 0;
  }
  if ((uintptr_t)o < 64) {                               // a system-color brush
    old = d->brush;
    d->brush = o;
    return old;
  }
  switch (g->type) {
    case OBJ_PEN: old = d->pen; d->pen = o; break;
    case OBJ_BRUSH: old = d->brush; d->brush = o; break;
    case OBJ_FONT: old = d->font; d->font = o; break;
    default: break;
  }
  return old;
}

EXPORT BOOL WINAPI DeleteObject(HGDIOBJ o) {
  gobj_t *g = obj_get(o);

  if (g != 0 && !g->stock) {
    g->used = 0;
  }
  return TRUE;
}

EXPORT int WINAPI GetObjectA(HANDLE h, int n, LPVOID p) { (void)h; (void)n; (void)p; return 0; }
EXPORT DWORD WINAPI GetObjectType(HGDIOBJ h) { gobj_t *g = obj_get(h); return g != 0 ? (DWORD)g->type : 0; }

// ---- State ------------------------------------------------------------------------------------------------

EXPORT COLORREF WINAPI SetTextColor(HDC h, COLORREF c) { dc_t *d = gdi_dc(h); COLORREF o; if (d == 0) return CLR_INVALID; o = d->text; d->text = c; return o; }
EXPORT COLORREF WINAPI GetTextColor(HDC h) { dc_t *d = gdi_dc(h); return d != 0 ? d->text : CLR_INVALID; }
EXPORT COLORREF WINAPI SetBkColor(HDC h, COLORREF c) { dc_t *d = gdi_dc(h); COLORREF o; if (d == 0) return CLR_INVALID; o = d->bk; d->bk = c; return o; }
EXPORT COLORREF WINAPI GetBkColor(HDC h) { dc_t *d = gdi_dc(h); return d != 0 ? d->bk : CLR_INVALID; }
EXPORT int WINAPI SetBkMode(HDC h, int m) { dc_t *d = gdi_dc(h); int o; if (d == 0) return 0; o = d->bkmode; d->bkmode = m; return o; }
EXPORT int WINAPI GetBkMode(HDC h) { dc_t *d = gdi_dc(h); return d != 0 ? d->bkmode : 0; }
EXPORT UINT WINAPI SetTextAlign(HDC h, UINT a) { dc_t *d = gdi_dc(h); UINT o; if (d == 0) return GDI_ERROR; o = (UINT)d->align; d->align = (int)a; return o; }
EXPORT int WINAPI SetROP2(HDC h, int r) { (void)h; (void)r; return R2_COPYPEN; }
EXPORT int WINAPI SetMapMode(HDC h, int m) { (void)h; (void)m; return MM_TEXT; }
EXPORT int WINAPI SetStretchBltMode(HDC h, int m) { (void)h; (void)m; return 0; }

// ---- Text -----------------------------------------------------------------------------------------------------

EXPORT BOOL WINAPI GetTextExtentPoint32A(HDC h, LPCSTR s, int n, LPSIZE sz) {
  (void)h;
  sz->cx = n * 8;
  sz->cy = 8;
  return TRUE;
}

EXPORT BOOL WINAPI GetTextExtentPointA(HDC h, LPCSTR s, int n, LPSIZE sz) { return GetTextExtentPoint32A(h, s, n, sz); }

EXPORT BOOL WINAPI GetTextMetricsA(HDC h, LPTEXTMETRICA tm) {
  (void)h;
  rt_memset(tm, 0, sizeof(*tm));
  tm->tmHeight = 8;
  tm->tmAscent = 7;
  tm->tmDescent = 1;
  tm->tmExternalLeading = 2;
  tm->tmAveCharWidth = 8;
  tm->tmMaxCharWidth = 8;
  tm->tmWeight = FW_NORMAL;
  tm->tmFirstChar = 32;
  tm->tmLastChar = 126;
  tm->tmPitchAndFamily = TMPF_FIXED_PITCH;
  return TRUE;
}

EXPORT BOOL WINAPI TextOutA(HDC h, int x, int y, LPCSTR s, int n) {
  dc_t *d = gdi_dc(h);
  int w = n * 8;

  if (d == 0) {
    return FALSE;
  }
  if ((d->align & TA_CENTER) == TA_CENTER) {
    x -= w / 2;
  } else if (d->align & TA_RIGHT) {
    x -= w;
  }
  if (d->bkmode == OPAQUE) {
    gdi_fill(d->sysid, x, y, w, 8, (uint32_t)rt_colorref_to_rgb(d->bk));
  }
  gdi_text(d->sysid, x, y, s, n, (uint32_t)rt_colorref_to_rgb(d->text));
  return TRUE;
}

EXPORT BOOL WINAPI ExtTextOutA(HDC h, int x, int y, UINT opts, const RECT *r, LPCSTR s, UINT n, const INT *dx) {
  dc_t *d = gdi_dc(h);

  (void)dx;
  if (d == 0) {
    return FALSE;
  }
  if ((opts & ETO_OPAQUE) && r != 0) {
    gdi_fill(d->sysid, r->left, r->top, r->right - r->left, r->bottom - r->top, (uint32_t)rt_colorref_to_rgb(d->bk));
  }
  return s != 0 ? TextOutA(h, x, y, s, (int)n) : TRUE;
}

EXPORT int WINAPI DrawTextA(HDC h, LPCSTR text, int n, LPRECT r, UINT fmt) {
  dc_t *d = gdi_dc(h);
  int width = r->right - r->left, per = width / 8 > 0 ? width / 8 : 1, y = r->top, lines = 0, i, len;
  const char *p;
  int single = (fmt & DT_SINGLELINE) != 0, wrap = (fmt & DT_WORDBREAK) != 0;
  int maxw = 0;

  if (d == 0) {
    return 0;
  }
  len = n < 0 ? (int)rt_strlen(text) : n;
  // Break the text into lines first so the block can be centered vertically.
  struct { int start, len; } ln[64];
  p = text;
  i = 0;
  while (i <= len && lines < 64) {
    int s = i, e = i;

    while (e < len && (single || text[e] != '\n') && (!wrap || e - s < per)) {
      ++e;
    }
    if (wrap && e < len && text[e] != '\n') {                // back up to the last space
      int b = e;

      while (b > s && text[b] != ' ') {
        --b;
      }
      if (b > s) {
        e = b;
      }
    }
    ln[lines].start = s;
    ln[lines].len = e - s;
    if (e - s > maxw) {
      maxw = e - s;
    }
    ++lines;
    i = e;
    if (i < len && (text[i] == '\n' || text[i] == ' ')) {
      ++i;
    }
    if (i >= len) {
      break;
    }
  }
  if (fmt & DT_CALCRECT) {
    r->right = r->left + maxw * 8;
    r->bottom = r->top + lines * 10;
    return lines * 10;
  }
  if (fmt & DT_VCENTER) {
    y = r->top + (r->bottom - r->top - lines * 10) / 2;
  } else if (fmt & DT_BOTTOM) {
    y = r->bottom - lines * 10;
  }
  for (i = 0; i < lines; ++i) {
    int tw = ln[i].len * 8, x = r->left;

    if (fmt & DT_CENTER) {
      x = r->left + (width - tw) / 2;
    } else if (fmt & DT_RIGHT) {
      x = r->right - tw;
    }
    if (d->bkmode == OPAQUE) {
      gdi_fill(d->sysid, x, y, tw, 8, (uint32_t)rt_colorref_to_rgb(d->bk));
    }
    gdi_text(d->sysid, x, y, text + ln[i].start, ln[i].len, (uint32_t)rt_colorref_to_rgb(d->text));
    y += 10;
  }
  return lines * 10;
}

// ---- Shapes ----------------------------------------------------------------------------------------------------

EXPORT int WINAPI FillRect(HDC h, const RECT *r, HBRUSH b) {
  dc_t *d = gdi_dc(h);
  unsigned long rgb;

  if (d == 0 || !brush_color(b, &rgb)) {
    return 0;
  }
  gdi_fill(d->sysid, r->left, r->top, r->right - r->left, r->bottom - r->top, (uint32_t)rgb);
  return 1;
}

EXPORT int WINAPI FrameRect(HDC h, const RECT *r, HBRUSH b) {
  dc_t *d = gdi_dc(h);
  unsigned long rgb;
  int w = r->right - r->left, ht = r->bottom - r->top;

  if (d == 0 || !brush_color(b, &rgb)) {
    return 0;
  }
  gdi_fill(d->sysid, r->left, r->top, w, 1, (uint32_t)rgb);
  gdi_fill(d->sysid, r->left, r->bottom - 1, w, 1, (uint32_t)rgb);
  gdi_fill(d->sysid, r->left, r->top, 1, ht, (uint32_t)rgb);
  gdi_fill(d->sysid, r->right - 1, r->top, 1, ht, (uint32_t)rgb);
  return 1;
}

EXPORT BOOL WINAPI PatBlt(HDC h, int x, int y, int w, int ht, DWORD rop) {
  dc_t *d = gdi_dc(h);
  unsigned long rgb;

  if (d == 0) {
    return FALSE;
  }
  if (rop == BLACKNESS) {
    rgb = 0;
  } else if (rop == WHITENESS) {
    rgb = 0x00FFFFFF;
  } else if (!brush_color((HBRUSH)d->brush, &rgb)) {
    return TRUE;
  }
  gdi_fill(d->sysid, x, y, w, ht, (uint32_t)rgb);
  return TRUE;
}

static int pen_info(dc_t *d, unsigned long *rgb, int *width) {
  gobj_t *p = obj_get(d->pen);

  if (p == 0 || p->style == PS_NULL) {
    return 0;
  }
  *rgb = rt_colorref_to_rgb(p->color);
  *width = p->width < 1 ? 1 : p->width;
  return 1;
}

EXPORT BOOL WINAPI Rectangle(HDC h, int l, int t, int r, int b) {
  dc_t *d = gdi_dc(h);
  unsigned long rgb;
  int pw;

  if (d == 0) {
    return FALSE;
  }
  if (brush_color((HBRUSH)d->brush, &rgb)) {
    gdi_fill(d->sysid, l, t, r - l, b - t, (uint32_t)rgb);
  }
  if (pen_info(d, &rgb, &pw)) {
    gdi_fill(d->sysid, l, t, r - l, pw, (uint32_t)rgb);
    gdi_fill(d->sysid, l, b - pw, r - l, pw, (uint32_t)rgb);
    gdi_fill(d->sysid, l, t, pw, b - t, (uint32_t)rgb);
    gdi_fill(d->sysid, r - pw, t, pw, b - t, (uint32_t)rgb);
  }
  return TRUE;
}

EXPORT BOOL WINAPI RoundRect(HDC h, int l, int t, int r, int b, int w, int ht) { (void)w; (void)ht; return Rectangle(h, l, t, r, b); }

static unsigned long isqrt(unsigned long long v) {
  unsigned long long x = 0, bit = 1ull << 62;

  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (v >= x + bit) {
      v -= x + bit;
      x = (x >> 1) + bit;
    } else {
      x >>= 1;
    }
    bit >>= 2;
  }
  return (unsigned long)x;
}

EXPORT BOOL WINAPI Ellipse(HDC h, int l, int t, int r, int b) {
  dc_t *d = gdi_dc(h);
  long long a2, b2;
  int y, cx, cy, ry, rx, fill, pen;
  unsigned long frgb = 0, prgb = 0;
  int pw = 1;

  if (d == 0 || r <= l || b <= t) {
    return FALSE;
  }
  fill = brush_color((HBRUSH)d->brush, &frgb);
  pen = pen_info(d, &prgb, &pw);
  rx = (r - l) / 2;
  ry = (b - t) / 2;
  cx = l + rx;
  cy = t + ry;
  a2 = (long long)rx * rx;
  b2 = (long long)ry * ry;
  for (y = -ry; y < ry + ((b - t) & 1); ++y) {
    long long dy = y >= 0 ? y + 0 : y;
    unsigned long span = b2 == 0 ? 0 : isqrt((unsigned long long)(a2 * (b2 - dy * dy)) / (unsigned long long)b2);
    int x0 = cx - (int)span, x1 = cx + (int)span;

    if (fill) {
      gdi_fill(d->sysid, x0, cy + y, x1 - x0, 1, (uint32_t)frgb);
    }
    if (pen) {
      int edge = pw > (int)span ? (int)span : pw;

      gdi_fill(d->sysid, x0, cy + y, edge > 0 ? edge : 1, 1, (uint32_t)prgb);
      gdi_fill(d->sysid, x1 - (edge > 0 ? edge : 1), cy + y, edge > 0 ? edge : 1, 1, (uint32_t)prgb);
    }
  }
  return TRUE;
}

EXPORT BOOL WINAPI MoveToEx(HDC h, int x, int y, LPPOINT old) {
  dc_t *d = gdi_dc(h);

  if (d == 0) {
    return FALSE;
  }
  if (old != 0) {
    old->x = d->px;
    old->y = d->py;
  }
  d->px = x;
  d->py = y;
  return TRUE;
}

EXPORT BOOL WINAPI LineTo(HDC h, int x1, int y1) {
  dc_t *d = gdi_dc(h);
  unsigned long rgb;
  int pw, x0, y0, dx, dy, sx, sy, err;

  if (d == 0) {
    return FALSE;
  }
  x0 = d->px;
  y0 = d->py;
  d->px = x1;
  d->py = y1;
  if (!pen_info(d, &rgb, &pw)) {
    return TRUE;
  }
  if (y0 == y1) {                                        // horizontal: one rectangle
    gdi_fill(d->sysid, x0 < x1 ? x0 : x1, y0 - pw / 2, (x0 < x1 ? x1 - x0 : x0 - x1) + 1, pw, (uint32_t)rgb);
    return TRUE;
  }
  if (x0 == x1) {
    gdi_fill(d->sysid, x0 - pw / 2, y0 < y1 ? y0 : y1, pw, (y0 < y1 ? y1 - y0 : y0 - y1) + 1, (uint32_t)rgb);
    return TRUE;
  }
  dx = x1 > x0 ? x1 - x0 : x0 - x1;
  dy = y1 > y0 ? y0 - y1 : y1 - y0;                      // negative
  sx = x0 < x1 ? 1 : -1;
  sy = y0 < y1 ? 1 : -1;
  err = dx + dy;
  for (;;) {
    gdi_fill(d->sysid, x0 - pw / 2, y0 - pw / 2, pw, pw, (uint32_t)rgb);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    if (2 * err >= dy) {
      err += dy;
      x0 += sx;
    }
    if (2 * err <= dx) {
      err += dx;
      y0 += sy;
    }
  }
  return TRUE;
}

EXPORT BOOL WINAPI Polyline(HDC h, const POINT *pts, int n) {
  int i;

  if (n < 2) {
    return FALSE;
  }
  MoveToEx(h, pts[0].x, pts[0].y, 0);
  for (i = 1; i < n; ++i) {
    LineTo(h, pts[i].x, pts[i].y);
  }
  return TRUE;
}

EXPORT COLORREF WINAPI SetPixel(HDC h, int x, int y, COLORREF c) {
  dc_t *d = gdi_dc(h);

  if (d == 0) {
    return CLR_INVALID;
  }
  gdi_fill(d->sysid, x, y, 1, 1, (uint32_t)rt_colorref_to_rgb(c));
  return c;
}

EXPORT BOOL WINAPI SetPixelV(HDC h, int x, int y, COLORREF c) { return SetPixel(h, x, y, c) != CLR_INVALID; }
EXPORT COLORREF WINAPI GetPixel(HDC h, int x, int y) { (void)h; (void)x; (void)y; return CLR_INVALID; }
