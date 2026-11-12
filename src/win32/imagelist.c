// Image lists (comctl32's ImageList_*) for the Win32 runtime. An image list is a strip of equally sized images
// kept as pixels: opaque ones are 0xFFrrggbb, transparent ones 0. Images come from bitmaps, with the mask
// given as a second bitmap (white = transparent) or as a colour (AddMasked). Drawing builds the image in a
// scratch DIB (normal, mask only, image only, blended with the highlight for ILD_FOCUS / ILD_SELECTED, with
// an overlay image on top, over the list's background colour or transparent) and puts it on the DC with
// AlphaBlend (BitBlt when nothing is transparent), so it works on window DCs and memory DCs alike.
//
// Not here: icons (the runtime has none: ImageList_ReplaceIcon fails), ImageList_Merge, drag images, ILD_ROP,
// ILC_COLORnn depths (everything is 32 bits), ImageList_Read / Write.
#define WINCOMMCTRLAPI __declspec(dllexport)
#include "internal.h"
#include <commctrl.h>

#define IL_MAGIC 0x494C4953u
#define IL_SILH 0x40000000u               // private: draw the opaque pixels in the foreground colour (a disabled button's image)

struct _IMAGELIST {
  unsigned magic;
  int cx, cy, n, cap, grow;
  UINT flags;
  uint32_t *pix;                          // n images of cx*cy pixels
  COLORREF bk;
  int overlay[15];                        // the image shown for overlay 1..15 (-1: none)
};

static uint32_t rgb_of(COLORREF c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

static struct _IMAGELIST *il_of(HIMAGELIST h) {
  struct _IMAGELIST *l = (struct _IMAGELIST *)h;

  return l != 0 && l->magic == IL_MAGIC ? l : 0;
}

static size_t il_size(const struct _IMAGELIST *l) { return (size_t)l->cx * (size_t)l->cy; }

static int il_reserve(struct _IMAGELIST *l, int need) {
  uint32_t *p;
  int cap;

  if (need <= l->cap) {
    return 1;
  }
  cap = l->cap;
  while (cap < need) {
    cap += l->grow > 0 ? l->grow : 4;
  }
  p = rt_alloc((size_t)cap * il_size(l) * 4);
  if (p == 0) {
    return 0;
  }
  rt_memset(p, 0, (size_t)cap * il_size(l) * 4);
  if (l->pix != 0) {
    rt_memcpy(p, l->pix, (size_t)l->n * il_size(l) * 4);
    rt_free(l->pix);
  }
  l->pix = p;
  l->cap = cap;
  return 1;
}

EXPORT HIMAGELIST WINAPI ImageList_Create(int cx, int cy, UINT flags, int cInitial, int cGrow) {
  struct _IMAGELIST *l;
  int i;

  if (cx <= 0 || cy <= 0 || cx > 256 || cy > 256) {
    return 0;
  }
  l = rt_alloc(sizeof(*l));
  if (l == 0) {
    return 0;
  }
  rt_memset(l, 0, sizeof(*l));
  l->magic = IL_MAGIC;
  l->cx = cx;
  l->cy = cy;
  l->flags = flags;
  l->grow = cGrow > 0 ? cGrow : 4;
  l->bk = CLR_NONE;
  for (i = 0; i < 15; ++i) {
    l->overlay[i] = -1;
  }
  if (cInitial > 0 && !il_reserve(l, cInitial)) {
    rt_free(l);
    return 0;
  }
  return (HIMAGELIST)l;
}

EXPORT BOOL WINAPI ImageList_Destroy(HIMAGELIST h) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0) {
    return FALSE;
  }
  l->magic = 0;
  rt_free(l->pix);
  rt_free(l);
  return TRUE;
}

EXPORT int WINAPI ImageList_GetImageCount(HIMAGELIST h) { struct _IMAGELIST *l = il_of(h); return l != 0 ? l->n : 0; }
EXPORT UINT WINAPI ImageList_GetFlags(HIMAGELIST h) { struct _IMAGELIST *l = il_of(h); return l != 0 ? l->flags : 0; }

EXPORT BOOL WINAPI ImageList_SetImageCount(HIMAGELIST h, UINT n) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0 || n > 65536) {
    return FALSE;
  }
  if ((int)n > l->n) {
    if (!il_reserve(l, (int)n)) {
      return FALSE;
    }
    rt_memset(l->pix + (size_t)l->n * il_size(l), 0, (size_t)((int)n - l->n) * il_size(l) * 4);
  }
  l->n = (int)n;
  return TRUE;
}

EXPORT COLORREF WINAPI ImageList_SetBkColor(HIMAGELIST h, COLORREF c) {
  struct _IMAGELIST *l = il_of(h);
  COLORREF o;

  if (l == 0) {
    return CLR_NONE;
  }
  o = l->bk;
  l->bk = c;
  return o;
}

EXPORT COLORREF WINAPI ImageList_GetBkColor(HIMAGELIST h) { struct _IMAGELIST *l = il_of(h); return l != 0 ? l->bk : CLR_NONE; }

EXPORT BOOL WINAPI ImageList_GetIconSize(HIMAGELIST h, int *cx, int *cy) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0) {
    return FALSE;
  }
  if (cx != 0) *cx = l->cx;
  if (cy != 0) *cy = l->cy;
  return TRUE;
}

EXPORT BOOL WINAPI ImageList_SetIconSize(HIMAGELIST h, int cx, int cy) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0 || cx <= 0 || cy <= 0 || cx > 256 || cy > 256) {
    return FALSE;
  }
  rt_free(l->pix);                                        // all the images go
  l->pix = 0;
  l->n = 0;
  l->cap = 0;
  l->cx = cx;
  l->cy = cy;
  return TRUE;
}

EXPORT BOOL WINAPI ImageList_SetOverlayImage(HIMAGELIST h, int image, int overlay) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0 || overlay < 1 || overlay > 15 || image < 0 || image >= l->n) {
    return FALSE;
  }
  l->overlay[overlay - 1] = image;
  return TRUE;
}

// The pixels of a bitmap, top row first, as 0x00RRGGBB; the caller frees them.
static uint32_t *bitmap_pixels(HBITMAP bm, int *w, int *hh) {
  BITMAPINFO bi;
  uint32_t *p;
  int n;

  rt_memset(&bi, 0, sizeof(bi));
  if (!GetDIBits(0, bm, 0, 0, 0, &bi, DIB_RGB_COLORS)) {
    return 0;
  }
  *w = bi.bmiHeader.biWidth;
  *hh = bi.bmiHeader.biHeight;
  if (*w <= 0 || *hh <= 0) {
    return 0;
  }
  p = rt_alloc((size_t)*w * (size_t)*hh * 4);
  if (p == 0) {
    return 0;
  }
  bi.bmiHeader.biHeight = -*hh;
  bi.bmiHeader.biBitCount = 32;
  n = GetDIBits(0, bm, 0, (UINT)*hh, p, &bi, DIB_RGB_COLORS);
  if (n != *hh) {
    rt_free(p);
    return 0;
  }
  return p;
}

// Put the images of a bitmap strip (cx wide each) into the list from index `at` on (replacing, or appending when at == n).
// The mask is a second bitmap (white = transparent), or the colour `key` (when use_key), or none.
static int il_put(struct _IMAGELIST *l, int at, HBITMAP bm, HBITMAP mask, int use_key, COLORREF key) {
  int w, h, mw = 0, mh = 0, count, k, x, y;
  uint32_t *p, *m = 0;
  uint32_t kr = rgb_of(key);

  p = bitmap_pixels(bm, &w, &h);
  if (p == 0) {
    return -1;
  }
  if (mask != 0) {
    m = bitmap_pixels(mask, &mw, &mh);
  }
  count = w / l->cx;
  if (count < 1 || !il_reserve(l, at + count)) {
    rt_free(p);
    rt_free(m);
    return -1;
  }
  for (k = 0; k < count; ++k) {
    uint32_t *d = l->pix + (size_t)(at + k) * il_size(l);

    for (y = 0; y < l->cy; ++y) {
      for (x = 0; x < l->cx; ++x) {
        uint32_t v = 0;

        if (y < h) {
          uint32_t c = p[(size_t)y * (size_t)w + (size_t)(k * l->cx + x)] & 0x00FFFFFFu;
          int transparent = 0;

          if (m != 0) {
            transparent = k * l->cx + x < mw && y < mh && (m[(size_t)y * (size_t)mw + (size_t)(k * l->cx + x)] & 0x00FFFFFFu) != 0;
          } else if (use_key) {
            transparent = c == kr;
          }
          v = transparent ? 0 : (0xFF000000u | c);
        }
        d[(size_t)y * (size_t)l->cx + (size_t)x] = v;
      }
    }
  }
  if (at + count > l->n) {
    l->n = at + count;
  }
  rt_free(p);
  rt_free(m);
  return at;
}

EXPORT int WINAPI ImageList_Add(HIMAGELIST h, HBITMAP image, HBITMAP mask) {
  struct _IMAGELIST *l = il_of(h);
  int at;

  if (l == 0 || image == 0) {
    return -1;
  }
  at = l->n;
  return il_put(l, at, image, mask, 0, 0) < 0 ? -1 : at;
}

EXPORT int WINAPI ImageList_AddMasked(HIMAGELIST h, HBITMAP image, COLORREF mask) {
  struct _IMAGELIST *l = il_of(h);
  int at;

  if (l == 0 || image == 0) {
    return -1;
  }
  at = l->n;
  return il_put(l, at, image, 0, mask != CLR_NONE, mask) < 0 ? -1 : at;
}

EXPORT BOOL WINAPI ImageList_Replace(HIMAGELIST h, int i, HBITMAP image, HBITMAP mask) {
  struct _IMAGELIST *l = il_of(h);
  int w, hh;
  uint32_t *p;

  if (l == 0 || i < 0 || i >= l->n || image == 0) {
    return FALSE;
  }
  p = bitmap_pixels(image, &w, &hh);                      // (only the first image of the bitmap is used)
  if (p == 0) {
    return FALSE;
  }
  rt_free(p);
  {
    int keep = l->n;
    int r = il_put(l, i, image, mask, 0, 0);

    l->n = keep;
    return r >= 0;
  }
}

EXPORT BOOL WINAPI ImageList_Remove(HIMAGELIST h, int i) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0) {
    return FALSE;
  }
  if (i == -1) {
    l->n = 0;
    return TRUE;
  }
  if (i < 0 || i >= l->n) {
    return FALSE;
  }
  rt_memcpy(l->pix + (size_t)i * il_size(l), l->pix + (size_t)(i + 1) * il_size(l), (size_t)(l->n - i - 1) * il_size(l) * 4);
  --l->n;
  return TRUE;
}

EXPORT HIMAGELIST WINAPI ImageList_Duplicate(HIMAGELIST h) {
  struct _IMAGELIST *l = il_of(h), *d;

  if (l == 0) {
    return 0;
  }
  d = (struct _IMAGELIST *)ImageList_Create(l->cx, l->cy, l->flags, l->n, l->grow);
  if (d == 0) {
    return 0;
  }
  if (l->n > 0) {
    rt_memcpy(d->pix, l->pix, (size_t)l->n * il_size(l) * 4);
  }
  d->n = l->n;
  d->bk = l->bk;
  rt_memcpy(d->overlay, l->overlay, sizeof(d->overlay));
  return (HIMAGELIST)d;
}

EXPORT BOOL WINAPI ImageList_Copy(HIMAGELIST hd, int id, HIMAGELIST hs, int is, UINT flags) {
  struct _IMAGELIST *d = il_of(hd), *s = il_of(hs);
  size_t n;
  uint32_t *tmp;

  if (d == 0 || s == 0 || d->cx != s->cx || d->cy != s->cy || id < 0 || id >= d->n || is < 0 || is >= s->n) {
    return FALSE;
  }
  n = il_size(d);
  if (flags & ILCF_SWAP) {
    tmp = rt_alloc(n * 4);
    if (tmp == 0) {
      return FALSE;
    }
    rt_memcpy(tmp, d->pix + (size_t)id * n, n * 4);
    rt_memcpy(d->pix + (size_t)id * n, s->pix + (size_t)is * n, n * 4);
    rt_memcpy(s->pix + (size_t)is * n, tmp, n * 4);
    rt_free(tmp);
    return TRUE;
  }
  rt_memcpy(d->pix + (size_t)id * n, s->pix + (size_t)is * n, n * 4);
  if (flags & ILCF_MOVE) {
    rt_memset(s->pix + (size_t)is * n, 0, n * 4);       // (the source image goes; this one keeps its place in the list)
  }
  return TRUE;
}

EXPORT BOOL WINAPI ImageList_GetImageInfo(HIMAGELIST h, int i, IMAGEINFO *info) {
  struct _IMAGELIST *l = il_of(h);

  if (l == 0 || info == 0 || i < 0 || i >= l->n) {
    return FALSE;
  }
  rt_memset(info, 0, sizeof(*info));
  info->rcImage.right = l->cx;                            // (no bitmaps: the pixels are kept in the list itself)
  info->rcImage.bottom = l->cy;
  return TRUE;
}

EXPORT int WINAPI ImageList_ReplaceIcon(HIMAGELIST h, int i, HICON icon) { (void)h; (void)i; (void)icon; return -1; }
EXPORT HICON WINAPI ImageList_GetIcon(HIMAGELIST h, int i, UINT flags) { (void)h; (void)i; (void)flags; return 0; }

EXPORT HIMAGELIST WINAPI ImageList_LoadImageA(HINSTANCE hi, LPCSTR name, int cx, int grow, COLORREF mask, UINT type, UINT flags) {
  HBITMAP bm;
  HIMAGELIST l;

  if (type != IMAGE_BITMAP) {
    return 0;
  }
  bm = (HBITMAP)LoadImageA(hi, name, IMAGE_BITMAP, 0, 0, flags);
  if (bm == 0) {
    return 0;
  }
  l = ImageList_Create(cx, 16, ILC_COLOR32 | (mask != CLR_NONE ? ILC_MASK : 0), 4, grow);
  if (l != 0) {
    BITMAP b;

    GetObjectA(bm, sizeof(b), &b);
    ImageList_SetIconSize(l, cx, b.bmHeight);
    ImageList_AddMasked(l, bm, mask);
  }
  DeleteObject(bm);
  return l;
}

// ---- Drawing -------------------------------------------------------------------------------------------------

static uint32_t mixc(uint32_t a, uint32_t b, int pct) {      // a * (100 - pct) % + b * pct %, 0x00RRGGBB
  uint32_t r = 0;
  int s;

  for (s = 0; s < 24; s += 8) {
    uint32_t ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;

    r |= ((ca * (uint32_t)(100 - pct) + cb * (uint32_t)pct + 50) / 100) << s;
  }
  return r;
}

static HDC g_tdc;
static HBITMAP g_tbmp, g_told;
static uint32_t *g_tpix;
static int g_tw, g_th;

static int scratch(int w, int h) {
  BITMAPINFO bi;
  void *bits = 0;

  if (g_tdc != 0 && w <= g_tw && h <= g_th) {
    return 1;
  }
  if (g_tdc == 0) {
    g_tdc = CreateCompatibleDC(0);
    if (g_tdc == 0) {
      return 0;
    }
  }
  if (g_tbmp != 0) {
    SelectObject(g_tdc, g_told);
    DeleteObject(g_tbmp);
    g_tbmp = 0;
  }
  if (w < g_tw) w = g_tw;
  if (h < g_th) h = g_th;
  rt_memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  g_tbmp = CreateDIBSection(g_tdc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
  if (g_tbmp == 0 || bits == 0) {
    return 0;
  }
  g_told = (HBITMAP)SelectObject(g_tdc, g_tbmp);
  g_tpix = bits;
  g_tw = w;
  g_th = h;
  return 1;
}

EXPORT BOOL WINAPI ImageList_DrawEx(HIMAGELIST h, int i, HDC dc, int x, int y, int dx, int dy, COLORREF rgbBk, COLORREF rgbFg, UINT style) {
  struct _IMAGELIST *l = il_of(h);
  int w, hh, px, py, ov, opaque = 1;
  uint32_t bk = 0, fg = 0;
  int have_bk, pct = 0;
  const uint32_t *src;
  const uint32_t *over = 0;

  if (l == 0 || dc == 0 || i < 0 || i >= l->n) {
    return FALSE;
  }
  w = dx > 0 && dx < l->cx ? dx : l->cx;
  hh = dy > 0 && dy < l->cy ? dy : l->cy;
  if (!scratch(w, hh)) {
    return FALSE;
  }
  if (rgbBk == CLR_DEFAULT) {
    rgbBk = l->bk;
  }
  have_bk = rgbBk != CLR_NONE && !(style & (ILD_TRANSPARENT | ILD_MASK | IL_SILH));
  bk = rgb_of(rgbBk);
  if (style & ILD_SELECTED) pct = 50;
  else if (style & ILD_FOCUS) pct = 25;
  if (pct != 0 || (style & IL_SILH)) {
    COLORREF f = rgbFg == CLR_NONE || rgbFg == CLR_DEFAULT ? GetSysColor(COLOR_HIGHLIGHT) : rgbFg;

    fg = rgb_of(f);
  }
  ov = (int)((style & ILD_OVERLAYMASK) >> 8);
  if (ov >= 1 && ov <= 15 && l->overlay[ov - 1] >= 0 && l->overlay[ov - 1] < l->n) {
    over = l->pix + (size_t)l->overlay[ov - 1] * il_size(l);
  }
  src = l->pix + (size_t)i * il_size(l);
  for (py = 0; py < hh; ++py) {
    for (px = 0; px < w; ++px) {
      uint32_t v = src[(size_t)py * (size_t)l->cx + (size_t)px];
      uint32_t o = over != 0 ? over[(size_t)py * (size_t)l->cx + (size_t)px] : 0;
      uint32_t r;

      if (style & ILD_MASK) {
        r = v != 0 ? 0xFF000000u : 0xFFFFFFFFu;
      } else if (style & ILD_IMAGE) {
        r = v != 0 ? v : 0xFF000000u;
      } else if (style & IL_SILH) {
        r = v != 0 ? (0xFF000000u | fg) : 0;
      } else {
        r = v;
        if (v != 0 && pct != 0) {
          r = 0xFF000000u | mixc(v & 0x00FFFFFFu, fg, pct);
        }
        if (o != 0) {
          r = o;
        }
        if (r == 0 && have_bk) {
          r = 0xFF000000u | bk;
        }
      }
      if (r == 0) {
        opaque = 0;
      }
      g_tpix[(size_t)py * (size_t)g_tw + (size_t)px] = r;
    }
  }
  if (g_tw != w) {                                         // the scratch bitmap is wider than the image: compact the rows
    for (py = 1; py < hh; ++py) {
      rt_memcpy(g_tpix + (size_t)py * (size_t)w, g_tpix + (size_t)py * (size_t)g_tw, (size_t)w * 4);
    }
  }
  if (opaque) {
    return BitBlt(dc, x, y, w, hh, g_tdc, 0, 0, SRCCOPY);
  }
  {
    BLENDFUNCTION bf;

    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    return AlphaBlend(dc, x, y, w, hh, g_tdc, 0, 0, w, hh, bf);
  }
}

EXPORT BOOL WINAPI ImageList_Draw(HIMAGELIST h, int i, HDC dc, int x, int y, UINT style) {
  return ImageList_DrawEx(h, i, dc, x, y, 0, 0, CLR_DEFAULT, CLR_DEFAULT, style);
}

EXPORT BOOL WINAPI ImageList_DrawIndirect(IMAGELISTDRAWPARAMS *p) {
  if (p == 0 || p->cbSize < 0x2C) {
    return FALSE;
  }
  return ImageList_DrawEx(p->himl, p->i, p->hdcDst, p->x, p->y, p->cx, p->cy, p->rgbBk, p->rgbFg, p->fStyle);
}

// What the controls use: the size of an image, and a disabled image (a silhouette in `color`).
void rt_il_silhouette(HIMAGELIST h, int i, HDC dc, int x, int y, COLORREF color) {
  ImageList_DrawEx(h, i, dc, x, y, 0, 0, CLR_NONE, color, IL_SILH);
}
