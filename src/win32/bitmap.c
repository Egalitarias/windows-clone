// Bitmaps for the Win32 runtime: HBITMAP objects, memory DCs, BitBlt / StretchBlt / PatBlt with the
// ternary raster operations, DIBs (CreateDIBSection, CreateDIBitmap, Get/SetDIBits, StretchDIBits)
// and bitmap resources (LoadBitmap, LoadImage). Bitmaps are kept as 32-bit 0x00RRGGBB pixels. A
// memory DC (CreateCompatibleDC) has sysid -1; everything gdi32.c draws goes to the selected bitmap
// through rt_mem_fill / rt_mem_text. A window's surface can be drawn to by a blit (KS_WIN_BLIT) but not read back,
// so a window DC can only be the destination of an operation that does not use the old pixels.
#include "internal.h"
#include "../kernel/font8x8.h"

#define MAX_BMPS 64
#define MAX_DIM 8192

static bmp_t g_bmp[MAX_BMPS];
static uint32_t g_defpix[1];
static bmp_t g_def = {1, 1, 1, 0, 1, 0, g_defpix};      // the 1x1 monochrome bitmap a new memory DC starts with
static int g_sel[MAX_BMPS];                                // memory DCs each bitmap is selected into

bmp_t *rt_bmp(HGDIOBJ h) {
  bmp_t *b = (bmp_t *)h;

  if (b == &g_def) {
    return b;
  }
  return (b >= g_bmp && b < g_bmp + MAX_BMPS && b->used) ? b : 0;
}

static bmp_t *bmp_alloc(int w, int h) {
  int i;

  if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM || (long long)w * h > 16 * 1024 * 1024) {
    return 0;
  }
  for (i = 0; i < MAX_BMPS; ++i) {
    if (!g_bmp[i].used) {
      uint32_t *p = rt_alloc((size_t)w * (size_t)h * 4);

      if (p == 0) {
        return 0;
      }
      rt_memset(p, 0, (size_t)w * (size_t)h * 4);
      rt_memset(&g_bmp[i], 0, sizeof(bmp_t));
      g_bmp[i].used = 1;
      g_bmp[i].w = w;
      g_bmp[i].h = h;
      g_bmp[i].pix = p;
      g_sel[i] = 0;
      return &g_bmp[i];
    }
  }
  return 0;
}

static uint32_t *px_at(bmp_t *b, int x, int y) { return b->pix + (size_t)(b->flip ? b->h - 1 - y : y) * (size_t)b->w + x; }

int rt_bmp_delete(HGDIOBJ h) {
  bmp_t *b = rt_bmp(h);

  if (b == 0) {
    return 0;
  }
  if (b == &g_def || g_sel[b - g_bmp] > 0) {
    return 2;                                     // still selected into a DC (or the default bitmap)
  }
  rt_free(b->pix);
  b->used = 0;
  return 1;
}

HGDIOBJ rt_bmp_select(dc_t *d, HGDIOBJ h) {
  bmp_t *b = rt_bmp(h), *old = d->bmp;

  if (d->sysid >= 0 || b == 0) {
    return 0;
  }
  if (old != 0 && old != &g_def) {
    --g_sel[old - g_bmp];
  }
  if (b != &g_def) {
    ++g_sel[b - g_bmp];
  }
  d->bmp = b;
  d->cl = 0;
  d->ct = 0;
  d->cr = b->w;
  d->cb = b->h;
  return (HGDIOBJ)old;
}

// Called by DeleteDC for a memory DC.
void rt_bmp_dc_gone(dc_t *d) {
  if (d->bmp != 0 && d->bmp != &g_def) {
    --g_sel[d->bmp - g_bmp];
  }
  d->bmp = 0;
}

int rt_bmp_getobject(HGDIOBJ h, int n, void *p) {
  bmp_t *b = rt_bmp(h);
  BITMAP *bm = (BITMAP *)p;

  if (b == 0) {
    return -1;
  }
  if (p == 0 || n < (int)sizeof(BITMAP)) {
    return 0;
  }
  rt_memset(p, 0, (size_t)(n < (int)sizeof(DIBSECTION) ? n : (int)sizeof(DIBSECTION)));
  bm->bmWidth = b->w;
  bm->bmHeight = b->h;
  bm->bmWidthBytes = b->mono ? ((b->w + 15) / 16) * 2 : b->w * 4;
  bm->bmPlanes = 1;
  bm->bmBitsPixel = b->mono ? 1 : 32;
  if (b->dib) {
    DIBSECTION *ds = (DIBSECTION *)p;

    bm->bmBits = b->pix;
    if (n >= (int)sizeof(DIBSECTION)) {
      ds->dsBmih.biSize = sizeof(BITMAPINFOHEADER);
      ds->dsBmih.biWidth = b->w;
      ds->dsBmih.biHeight = b->flip ? b->h : -b->h;
      ds->dsBmih.biPlanes = 1;
      ds->dsBmih.biBitCount = 32;
      ds->dsBmih.biSizeImage = (DWORD)(b->w * b->h * 4);
      return (int)sizeof(DIBSECTION);
    }
  }
  return (int)sizeof(BITMAP);
}

// ---- Drawing into a memory DC ---------------------------------------------------------------------

void rt_mem_fill(dc_t *d, int x, int y, int x1, int y1, uint32_t rgb) {
  bmp_t *b = d->bmp;
  int xx, yy;

  if (b == 0) {
    return;
  }
  if (b->mono) {
    rgb = rgb != 0 ? 0x00FFFFFFu : 0;
  }
  for (yy = y; yy < y1; ++yy) {
    uint32_t *row = px_at(b, 0, yy);

    for (xx = x; xx < x1; ++xx) {
      row[xx] = rgb;
    }
  }
}

void rt_mem_text(dc_t *d, int x, int y, const char *s, int n, uint32_t rgb) {
  bmp_t *b = d->bmp;
  int i, row, col;

  if (b == 0) {
    return;
  }
  for (i = 0; i < n; ++i, x += 8) {
    unsigned char c = (unsigned char)s[i];
    const unsigned char *g;

    if (c < 0x20 || c > 0x7E) {
      c = '?';
    }
    g = g_font8x8[c - 0x20];
    for (row = 0; row < 8; ++row) {
      for (col = 0; col < 8; ++col) {
        int px = x + col, py = y + row;

        if ((g[row] & (1u << col)) && px >= d->cl && px < d->cr && py >= d->ct && py < d->cb) {
          *px_at(b, px, py) = b->mono ? (rgb != 0 ? 0x00FFFFFFu : 0) : rgb;
        }
      }
    }
  }
}

int rt_mem_getpixel(dc_t *d, int x, int y, COLORREF *c) {
  bmp_t *b = d->bmp;

  if (b == 0 || x < 0 || y < 0 || x >= b->w || y >= b->h) {
    return 0;
  }
  *c = (COLORREF)rt_colorref_to_rgb((COLORREF)(*px_at(b, x, y) & 0x00FFFFFFu));      // the swap of red and blue is its own inverse
  return 1;
}

// ---- Creating bitmaps and memory DCs --------------------------------------------------------------

EXPORT HDC WINAPI CreateCompatibleDC(HDC dc) {
  RECT clip = {0, 0, 1, 1};
  dc_t *d;

  (void)dc;
  d = gdi_dc_alloc(-1, 0, 0, &clip);
  if (d != 0) {
    d->bmp = &g_def;
  }
  return (HDC)d;
}

EXPORT HBITMAP WINAPI CreateCompatibleBitmap(HDC dc, int w, int h) {
  dc_t *d = gdi_dc(dc);
  bmp_t *b = bmp_alloc(w, h);

  if (b != 0 && d != 0 && d->sysid < 0 && d->bmp != 0 && d->bmp != &g_def && d->bmp->mono) {
    b->mono = 1;                                  // compatible with a memory DC holding a monochrome bitmap (a fresh one is treated as colour)
  }
  return (HBITMAP)b;
}

EXPORT HBITMAP WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits) {
  bmp_t *b = bmp_alloc(w, h);
  int x, y;

  (void)planes;
  if (b == 0) {
    return 0;
  }
  b->mono = (bpp == 1);
  if (bits != 0 && bpp == 1) {
    int stride = ((w + 15) / 16) * 2;

    for (y = 0; y < h; ++y) {
      const uint8_t *row = (const uint8_t *)bits + (size_t)y * (size_t)stride;

      for (x = 0; x < w; ++x) {
        px_at(b, x, y)[0] = (row[x >> 3] & (0x80 >> (x & 7))) ? 0x00FFFFFFu : 0;
      }
    }
  } else if (bits != 0 && bpp == 32) {
    rt_memcpy(b->pix, bits, (size_t)w * (size_t)h * 4);
  }
  return (HBITMAP)b;
}

EXPORT HBITMAP WINAPI CreateBitmapIndirect(const BITMAP *bm) {
  return bm == 0 ? 0 : CreateBitmap(bm->bmWidth, bm->bmHeight, bm->bmPlanes, bm->bmBitsPixel, bm->bmBits);
}

// ---- DIBs: the formats read are 1, 4, 8, 16 (5-5-5), 24 and 32 bits, uncompressed -----------------------

typedef struct {
  int w, h, topdown, bpp, stride, npal;
  const RGBQUAD *pal;
  const uint8_t *bits;
} dibinfo_t;

static int dib_parse(const BITMAPINFO *bi, const void *bits, dibinfo_t *di) {
  const BITMAPINFOHEADER *h = &bi->bmiHeader;
  int clr;

  if (bi == 0 || h->biSize < sizeof(BITMAPINFOHEADER) || h->biPlanes != 1 || (h->biCompression != BI_RGB && h->biCompression != BI_BITFIELDS)) {
    return 0;
  }
  di->bpp = h->biBitCount;
  if (di->bpp != 1 && di->bpp != 4 && di->bpp != 8 && di->bpp != 16 && di->bpp != 24 && di->bpp != 32) {
    return 0;
  }
  di->w = h->biWidth;
  di->topdown = h->biHeight < 0;
  di->h = h->biHeight < 0 ? -h->biHeight : h->biHeight;
  if (di->w <= 0 || di->h <= 0 || di->w > MAX_DIM || di->h > MAX_DIM) {
    return 0;
  }
  di->stride = ((di->w * di->bpp + 31) / 32) * 4;
  clr = di->bpp <= 8 ? (h->biClrUsed != 0 ? (int)h->biClrUsed : 1 << di->bpp) : 0;
  di->npal = clr;
  di->pal = (const RGBQUAD *)((const uint8_t *)bi + h->biSize + (h->biCompression == BI_BITFIELDS && h->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0));
  di->bits = bits != 0 ? (const uint8_t *)bits : (const uint8_t *)(di->pal + clr);
  return 1;
}

// The colour of pixel (x, y) counting from the top of the picture.
static uint32_t dib_px(const dibinfo_t *di, int x, int y) {
  const uint8_t *row = di->bits + (size_t)(di->topdown ? y : di->h - 1 - y) * (size_t)di->stride;
  int v;

  switch (di->bpp) {
    case 1: v = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
    case 4: v = (x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4); break;
    case 8: v = row[x]; break;
    case 16: {
      unsigned w16 = (unsigned)row[2 * x] | ((unsigned)row[2 * x + 1] << 8);
      unsigned r = (w16 >> 10) & 31, g = (w16 >> 5) & 31, b = w16 & 31;

      return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
    case 24: return ((uint32_t)row[3 * x + 2] << 16) | ((uint32_t)row[3 * x + 1] << 8) | row[3 * x];
    default: return ((uint32_t)row[4 * x + 2] << 16) | ((uint32_t)row[4 * x + 1] << 8) | row[4 * x];
  }
  if (v >= di->npal) {
    return 0;
  }
  return ((uint32_t)di->pal[v].rgbRed << 16) | ((uint32_t)di->pal[v].rgbGreen << 8) | di->pal[v].rgbBlue;
}

static void dib_load(bmp_t *b, const dibinfo_t *di) {
  int x, y, w = b->w < di->w ? b->w : di->w, h = b->h < di->h ? b->h : di->h;

  for (y = 0; y < h; ++y) {
    for (x = 0; x < w; ++x) {
      *px_at(b, x, y) = dib_px(di, x, y);
    }
  }
}

HBITMAP rt_dib_to_bitmap(const BITMAPINFO *bi, const void *bits) {
  dibinfo_t di;
  bmp_t *b;

  if (!dib_parse(bi, bits, &di) || (b = bmp_alloc(di.w, di.h)) == 0) {
    return 0;
  }
  dib_load(b, &di);
  return (HBITMAP)b;
}

EXPORT HBITMAP WINAPI CreateDIBitmap(HDC dc, const BITMAPINFOHEADER *hdr, DWORD init, const void *bits, const BITMAPINFO *bi, UINT usage) {
  bmp_t *b;

  (void)dc;
  (void)usage;
  if (hdr == 0 || hdr->biWidth <= 0 || hdr->biHeight == 0) {
    return 0;
  }
  b = bmp_alloc(hdr->biWidth, hdr->biHeight < 0 ? -hdr->biHeight : hdr->biHeight);
  if (b != 0 && (init & CBM_INIT) && bits != 0 && bi != 0) {
    dibinfo_t di;

    if (dib_parse(bi, bits, &di)) {
      dib_load(b, &di);
    }
  }
  return (HBITMAP)b;
}

EXPORT HBITMAP WINAPI CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, VOID **ppv, HANDLE section, DWORD off) {
  bmp_t *b;
  int h;

  (void)dc;
  (void)usage;
  (void)section;
  (void)off;
  if (bi == 0 || bi->bmiHeader.biBitCount != 32 || bi->bmiHeader.biWidth <= 0 || bi->bmiHeader.biHeight == 0) {
    SetLastError(ERROR_NOT_SUPPORTED);              // only 32-bit sections: their memory is the bitmap
    return 0;
  }
  h = bi->bmiHeader.biHeight;
  b = bmp_alloc(bi->bmiHeader.biWidth, h < 0 ? -h : h);
  if (b == 0) {
    return 0;
  }
  b->dib = 1;
  b->flip = h > 0;                                  // bottom-up: the first row in memory is the bottom one
  if (ppv != 0) {
    *ppv = b->pix;
  }
  return (HBITMAP)b;
}

// Scan lines start .. start+lines of the bitmap (counted from the bottom for a bottom-up DIB).
EXPORT int WINAPI SetDIBits(HDC dc, HBITMAP hb, UINT start, UINT lines, const void *bits, const BITMAPINFO *bi, UINT usage) {
  bmp_t *b = rt_bmp(hb);
  dibinfo_t di;
  UINT k;
  int x;

  (void)dc;
  (void)usage;
  if (b == 0 || bits == 0 || !dib_parse(bi, bits, &di)) {
    return 0;
  }
  for (k = 0; k < lines && (int)k < di.h; ++k) {
    int y = di.topdown ? (int)(start + k) : b->h - 1 - (int)(start + k);

    if (y < 0 || y >= b->h) {
      continue;
    }
    for (x = 0; x < b->w && x < di.w; ++x) {
      *px_at(b, x, y) = dib_px(&di, x, di.topdown ? (int)k : di.h - 1 - (int)k);
    }
  }
  return (int)lines;
}

// Output in 24 or 32 bits (the format asked for in bi); with bits == 0 the header is filled in.
EXPORT int WINAPI GetDIBits(HDC dc, HBITMAP hb, UINT start, UINT lines, LPVOID bits, LPBITMAPINFO bi, UINT usage) {
  bmp_t *b = rt_bmp(hb);
  BITMAPINFOHEADER *h;
  int bpp, topdown, stride;
  UINT k;
  int x;

  (void)dc;
  (void)usage;
  if (b == 0 || bi == 0) {
    return 0;
  }
  h = &bi->bmiHeader;
  if (bits == 0) {
    h->biSize = sizeof(BITMAPINFOHEADER);
    h->biWidth = b->w;
    h->biHeight = b->h;
    h->biPlanes = 1;
    h->biBitCount = 32;
    h->biCompression = BI_RGB;
    h->biSizeImage = (DWORD)(b->w * b->h * 4);
    return 1;
  }
  bpp = h->biBitCount;
  if (bpp != 24 && bpp != 32) {
    return 0;
  }
  topdown = h->biHeight < 0;
  stride = ((b->w * bpp + 31) / 32) * 4;
  for (k = 0; k < lines; ++k) {
    int y = topdown ? (int)(start + k) : b->h - 1 - (int)(start + k);
    uint8_t *row = (uint8_t *)bits + (size_t)k * (size_t)stride;

    if (y < 0 || y >= b->h) {
      continue;
    }
    for (x = 0; x < b->w; ++x) {
      uint32_t v = *px_at(b, x, y);

      if (bpp == 32) {
        ((uint32_t *)row)[x] = v;
      } else {
        row[3 * x] = (uint8_t)v;
        row[3 * x + 1] = (uint8_t)(v >> 8);
        row[3 * x + 2] = (uint8_t)(v >> 16);
      }
    }
  }
  return (int)lines;
}

// ---- The blit engine --------------------------------------------------------------------------------

// dst (x, y, w, h) <- rop(pattern, src (sx, sy, sw, sh) scaled, dst). `src` is 0 when the operation does not use a source.
static BOOL blit(dc_t *d, int dx, int dy, int dw, int dh, bmp_t *src, int sx, int sy, int sw, int sh, DWORD rop) {
  unsigned t = (rop >> 16) & 0xFF;
  int needs = ((t ^ (t >> 2)) & 0x33) != 0, needd = ((t ^ (t >> 1)) & 0x55) != 0;
  int flipx = 0, flipy = 0, lx0, lx1, ly0, ly1, lx, ly;
  unsigned long prgb = 0;
  uint32_t textc, bkc, *buf = 0;
  int band = 64;

  if (needs && src == 0) {
    return FALSE;
  }
  if (needd && d->sysid >= 0) {
    return FALSE;                                     // the old pixels of a window cannot be read
  }
  if (dw == 0 || dh == 0) {
    return TRUE;
  }
  if (dw < 0) { dx += dw; dw = -dw; flipx ^= 1; }
  if (dh < 0) { dy += dh; dh = -dh; flipy ^= 1; }
  if (!needs) {
    sw = dw;
    sh = dh;
  } else {
    if (sw < 0) { sx += sw; sw = -sw; flipx ^= 1; }
    if (sh < 0) { sy += sh; sh = -sh; flipy ^= 1; }
    if (sw == 0 || sh == 0) {
      return TRUE;
    }
  }
  gdi_brush_rgb((HBRUSH)d->brush, &prgb);
  textc = (uint32_t)rt_colorref_to_rgb(d->text);
  bkc = (uint32_t)rt_colorref_to_rgb(d->bk);
  // The part of the destination inside the clip rectangle (surface coordinates).
  lx0 = d->cl - (dx + d->ox); if (lx0 < 0) lx0 = 0;
  ly0 = d->ct - (dy + d->oy); if (ly0 < 0) ly0 = 0;
  lx1 = d->cr - (dx + d->ox); if (lx1 > dw) lx1 = dw;
  ly1 = d->cb - (dy + d->oy); if (ly1 > dh) ly1 = dh;
  if (lx1 <= lx0 || ly1 <= ly0) {
    return TRUE;
  }
  if (d->sysid >= 0) {
    buf = rt_alloc((size_t)(lx1 - lx0) * (size_t)band * 4);
    if (buf == 0) {
      return FALSE;
    }
  }
  for (ly = ly0; ly < ly1; ly += band) {
    int rows = ly1 - ly < band ? ly1 - ly : band, r;

    for (r = 0; r < rows; ++r) {
      int yy = ly + r, sy2 = 0, sxx;
      uint32_t *drow = 0;

      if (d->sysid < 0) {
        drow = px_at(d->bmp, 0, dy + yy);
      }
      if (needs) {
        sy2 = sy + (int)(((long long)(flipy ? dh - 1 - yy : yy) * sh) / dh);
      }
      for (lx = lx0; lx < lx1; ++lx) {
        uint32_t s = 0, dv = 0, p = (uint32_t)prgb, res;

        if (needs) {
          sxx = sx + (int)(((long long)(flipx ? dw - 1 - lx : lx) * sw) / dw);
          if (sxx < 0 || sy2 < 0 || sxx >= src->w || sy2 >= src->h) {
            if (buf != 0) {
              buf[(size_t)r * (size_t)(lx1 - lx0) + (size_t)(lx - lx0)] = 0;
            }
            continue;                                 // outside the source: left alone
          }
          s = *px_at(src, sxx, sy2) & 0x00FFFFFFu;
          if (src->mono) {
            s = s != 0 ? bkc : textc;
          }
        }
        if (drow != 0) {
          dv = drow[dx + lx] & 0x00FFFFFFu;
        }
        switch (t) {
          case 0xCC: res = s; break;
          case 0xF0: res = p; break;
          case 0x00: res = 0; break;
          case 0xFF: res = 0x00FFFFFFu; break;
          default: {
            int i;

            res = 0;
            for (i = 0; i < 8; ++i) {
              if (t & (1u << i)) {
                res |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? dv : ~dv);
              }
            }
            res &= 0x00FFFFFFu;
            break;
          }
        }
        if (drow != 0) {
          drow[dx + lx] = d->bmp->mono ? (res != 0 ? 0x00FFFFFFu : 0) : res;
        } else {
          buf[(size_t)r * (size_t)(lx1 - lx0) + (size_t)(lx - lx0)] = res;
        }
      }
    }
    if (buf != 0) {
      sc_winop_t op;

      op.x = dx + d->ox + lx0;
      op.y = dy + d->oy + ly;
      op.w = lx1 - lx0;
      op.h = rows;
      op.color = 0;
      op.scale = 0;
      op.ptr = (uint64_t)(uintptr_t)buf;
      sc3(KS_WIN_BLIT, d->sysid, (sl_t)&op, 0);
    }
  }
  rt_free(buf);
  return TRUE;
}

EXPORT BOOL WINAPI StretchBlt(HDC hd, int dx, int dy, int dw, int dh, HDC hs, int sx, int sy, int sw, int sh, DWORD rop) {
  dc_t *d = gdi_dc(hd), *s = gdi_dc(hs);
  unsigned t = (rop >> 16) & 0xFF;
  int needs = ((t ^ (t >> 2)) & 0x33) != 0;

  if (d == 0 || (d->sysid < 0 && d->bmp == 0)) {
    return FALSE;
  }
  if (needs && (s == 0 || s->sysid >= 0 || s->bmp == 0)) {
    return FALSE;                                     // a window's surface cannot be a source
  }
  return blit(d, dx, dy, dw, dh, needs ? s->bmp : 0, sx, sy, sw, sh, rop);
}

EXPORT BOOL WINAPI BitBlt(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, DWORD rop) {
  return StretchBlt(hd, x, y, w, h, hs, sx, sy, w, h, rop);
}

// MaskBlt-less masking: a monochrome mask drawn first with SRCAND and the image with SRCPAINT works through the ordinary rops.

// A DIB drawn on a DC: the picture is converted to a temporary bitmap first. For a bottom-up DIB ys counts from the bottom.
EXPORT int WINAPI StretchDIBits(HDC hdc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const VOID *bits, const BITMAPINFO *bi, UINT usage, DWORD rop) {
  dc_t *d = gdi_dc(hdc);
  dibinfo_t di;
  bmp_t tmp;
  int ok, y0;

  (void)usage;
  if (d == 0 || bits == 0 || !dib_parse(bi, bits, &di)) {
    return GDI_ERROR;
  }
  rt_memset(&tmp, 0, sizeof(tmp));
  tmp.used = 1;
  tmp.w = di.w;
  tmp.h = di.h;
  tmp.pix = rt_alloc((size_t)di.w * (size_t)di.h * 4);
  if (tmp.pix == 0) {
    return GDI_ERROR;
  }
  dib_load(&tmp, &di);
  y0 = di.topdown ? ys : di.h - ys - hs;
  ok = blit(d, xd, yd, wd, hd, &tmp, xs, y0, ws, hs, rop);
  rt_free(tmp.pix);
  return ok ? hd : GDI_ERROR;
}

EXPORT int WINAPI SetDIBitsToDevice(HDC hdc, int xd, int yd, DWORD w, DWORD h, int xs, int ys, UINT start, UINT lines, const VOID *bits, const BITMAPINFO *bi, UINT usage) {
  (void)start;
  (void)lines;
  return StretchDIBits(hdc, xd, yd, (int)w, (int)h, xs, ys, (int)w, (int)h, bits, bi, usage, SRCCOPY) == GDI_ERROR ? 0 : (int)h;
}

// ---- Bitmap resources and files ---------------------------------------------------------------------

EXPORT HBITMAP WINAPI LoadBitmapA(HINSTANCE hi, LPCSTR name) {
  HRSRC r = FindResourceA(hi, name, RT_BITMAP);
  const void *p = r != 0 ? LockResource(LoadResource(hi, r)) : 0;

  return p != 0 ? rt_dib_to_bitmap((const BITMAPINFO *)p, 0) : 0;
}

static HBITMAP load_bmp_file(LPCSTR path) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  DWORD size, got = 0;
  uint8_t *buf;
  HBITMAP r = 0;

  if (f == INVALID_HANDLE_VALUE) {
    return 0;
  }
  size = GetFileSize(f, 0);
  if (size > sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) && size < 32u * 1024 * 1024 && (buf = rt_alloc(size)) != 0) {
    if (ReadFile(f, buf, size, &got, 0) && got == size && buf[0] == 'B' && buf[1] == 'M') {
      const BITMAPFILEHEADER *fh = (const BITMAPFILEHEADER *)buf;

      r = rt_dib_to_bitmap((const BITMAPINFO *)(buf + sizeof(BITMAPFILEHEADER)), fh->bfOffBits < size ? buf + fh->bfOffBits : 0);
    }
    rt_free(buf);
  }
  CloseHandle(f);
  return r;
}

EXPORT HANDLE WINAPI LoadImageA(HINSTANCE hi, LPCSTR name, UINT type, int cx, int cy, UINT flags) {
  (void)cx;
  (void)cy;
  if (type != IMAGE_BITMAP) {
    return 0;                                         // no icons or cursors
  }
  return (HANDLE)((flags & LR_LOADFROMFILE) ? load_bmp_file(name) : LoadBitmapA(hi, name));
}
