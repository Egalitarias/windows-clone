#include "string.h"

void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  size_t i;
  for (i = 0; i < n; i++) {
    d[i] = s[i];
  }
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  size_t i;
  if (d == s || n == 0) {
    return dst;
  }
  if (d < s) {
    for (i = 0; i < n; i++) {
      d[i] = s[i];
    }
  } else {
    for (i = n; i > 0; i--) {
      d[i - 1] = s[i - 1];
    }
  }
  return dst;
}

void *memset(void *dst, int value, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  size_t i;
  for (i = 0; i < n; i++) {
    d[i] = (uint8_t)value;
  }
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a;
  const uint8_t *y = (const uint8_t *)b;
  size_t i;
  for (i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return (int)x[i] - (int)y[i];
    }
  }
  return 0;
}

size_t str_len(const char *s) {
  size_t n = 0;
  while (s && s[n]) {
    n++;
  }
  return n;
}

size_t str_copy(char *dst, const char *src, size_t max) {
  size_t i = 0;
  if (max == 0) {
    return 0;
  }
  while (src && src[i] && i + 1 < max) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
  return i;
}

char char_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

char char_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int str_eq_nocase(const char *a, const char *b) {
  size_t i = 0;
  while (a[i] && b[i]) {
    if (char_upper(a[i]) != char_upper(b[i])) {
      return 0;
    }
    i++;
  }
  return a[i] == b[i];
}

int u32_to_str(uint32_t v, char *out) {
  char tmp[11];
  int n = 0, len = 0;
  if (v == 0) {
    tmp[n++] = '0';
  }
  while (v > 0) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n > 0) {
    out[len++] = tmp[--n];
  }
  out[len] = 0;
  return len;
}
