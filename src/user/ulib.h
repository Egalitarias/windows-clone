#ifndef ULIB_H
#define ULIB_H

// Small shared helpers for WinClone programs: strings, paths, simple drawing and
// input decoding. Header only (every function is static inline), so a program just
// includes it; nothing extra is linked.

#include "user.h"

// ---- strings ------------------------------------------------------------------------

// Copy src into dst (at most max-1 characters, always NUL-terminated). Returns the length.
static inline int ulib_copy(char *dst, const char *src, int max) {
  int n = 0;

  while (src[n] != 0 && n < max - 1) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = 0;
  return n;
}

// dst = a followed by b (b may be 0). dst must not overlap b. Returns the length.
static inline int ulib_cat(char *dst, int max, const char *a, const char *b) {
  int n = ulib_copy(dst, a, max);

  if (b != 0) {
    n += ulib_copy(dst + n, b, max - n);
  }
  return n;
}

static inline char ulib_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Equal, ignoring case.
static inline int ulib_streq_nocase(const char *a, const char *b) {
  while (*a != 0 && *b != 0) {
    if (ulib_upper(*a) != ulib_upper(*b)) {
      return 0;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

// The name ends in ".ext" (ext given without the dot), ignoring case.
static inline int ulib_has_ext(const char *name, const char *ext) {
  int n = (int)strlen(name);
  int e = (int)strlen(ext);

  return n > e && name[n - e - 1] == '.' && ulib_streq_nocase(name + n - e, ext);
}

// Decimal text of v into out (needs 21 bytes). Returns the length.
static inline int ulib_utoa(unsigned long v, char *out) {
  char tmp[24];
  int n = 0;
  int i;

  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  for (i = 0; i < n; ++i) {
    out[i] = tmp[n - 1 - i];
  }
  out[n] = 0;
  return n;
}

// "12 B", "3 KB" or "5 MB" (rounded up); out needs 24 bytes.
static inline void ulib_size_text(unsigned long size, char *out) {
  int n;

  if (size < 1024UL) {
    n = ulib_utoa(size, out);
    ulib_copy(out + n, " B", 4);
  } else if (size < 1024UL * 1024UL) {
    n = ulib_utoa((size + 1023UL) / 1024UL, out);
    ulib_copy(out + n, " KB", 4);
  } else {
    n = ulib_utoa((size + 1048575UL) / 1048576UL, out);
    ulib_copy(out + n, " MB", 4);
  }
}

// The part of a path after its last '/'.
static inline const char *ulib_basename(const char *path) {
  const char *name = path;
  int i;

  for (i = 0; path[i] != 0; ++i) {
    if (path[i] == '/') {
      name = path + i + 1;
    }
  }
  return name;
}

// ---- input ----------------------------------------------------------------------------

// The modifier bits (MOD_*) that were down for a key or mouse event, else 0.
static inline unsigned int ulib_mods(const win_event_t *ev) {
  if (ev->type == EV_KEY) {
    return (unsigned int)ev->x;
  }
  if (ev->type == EV_MOUSE_DOWN || ev->type == EV_MOUSE_MOVE || ev->type == EV_MOUSE_UP ||
      ev->type == EV_MOUSE_DOUBLE) {
    return ev->ch;
  }
  return 0;
}

static inline int ulib_in_rect(int px, int py, int x, int y, int w, int h) {
  return px >= x && px < x + w && py >= y && py < y + h;
}

// ---- drawing (8x8 font, so a character is 8 pixels wide) -------------------------------------

static inline int ulib_text_width(const char *s) {
  return 8 * (int)strlen(s);
}

// A one pixel frame.
static inline void ulib_outline(int win, int x, int y, int w, int h, uint32_t color) {
  win_fill(win, x, y, w, 1, color);
  win_fill(win, x, y + h - 1, w, 1, color);
  win_fill(win, x, y, 1, h, color);
  win_fill(win, x + w - 1, y, 1, h, color);
}

// Text cut to at most max_chars characters.
static inline void ulib_text_fit(int win, int x, int y, const char *s, uint32_t color, int max_chars) {
  char buf[128];

  if (max_chars > (int)sizeof(buf) - 1) {
    max_chars = (int)sizeof(buf) - 1;
  }
  if (max_chars <= 0) {
    return;
  }
  ulib_copy(buf, s, max_chars + 1);
  win_text(win, x, y, buf, color, 1);
}

typedef struct {
  uint32_t face;          // enabled background
  uint32_t face_off;      // disabled background
  uint32_t line;          // frame
  uint32_t text;
  uint32_t text_off;
} ulib_button_style_t;

// A push button with a centred label. (Hit testing is up to the program: ulib_in_rect.)
static inline void ulib_button(int win, int x, int y, int w, int h, const char *label, int enabled,
                               const ulib_button_style_t *st) {
  win_fill(win, x, y, w, h, enabled ? st->face : st->face_off);
  ulib_outline(win, x, y, w, h, st->line);
  win_text(win, x + (w - ulib_text_width(label)) / 2, y + (h - 8) / 2, label,
           enabled ? st->text : st->text_off, 1);
}

#endif // ULIB_H
