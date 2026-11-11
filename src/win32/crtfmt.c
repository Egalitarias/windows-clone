// printf / scanf engines and number conversions for the C runtime part of WIN32.DLL.
// Doubles are printed exactly: the value is expanded to its full decimal digits with a small
// big-number routine, then rounded (to nearest, ties to even) to what the format asks for.
#include "crt.h"

// ---- exact decimal expansion of a double --------------------------------------------------------

static char g_dig[1500];     // decimal digits, no leading or trailing zeros
static int g_ndig;           // how many (0 for the value zero)
static int g_decpt;          // value = 0.DIGITS * 10^g_decpt

static void dbl_expand(double v) {
  union { double d; uint64_t u; } c;
  uint64_t mant, ip = 0;
  int ex, exp2, nd = 0, intdigits = 0, i;
  static char tmp[1500];

  c.d = v;
  mant = c.u & 0xFFFFFFFFFFFFFull;
  ex = (int)((c.u >> 52) & 0x7FF);
  if (ex == 0) {
    exp2 = -1074;
  } else {
    mant |= 1ull << 52;
    exp2 = ex - 1075;
  }
  g_ndig = 0;
  g_decpt = 0;
  if (mant == 0) {
    return;
  }
  if (exp2 >= 0) {
    uint32_t limb[40];
    int word = exp2 / 32, bit = exp2 % 32, top, pos = 400;
    unsigned __int128 t = (unsigned __int128)mant << bit;
    static char buf[420];

    for (i = 0; i < 40; ++i) {
      limb[i] = 0;
    }
    limb[word] = (uint32_t)t;
    limb[word + 1] = (uint32_t)(t >> 32);
    limb[word + 2] = (uint32_t)(t >> 64);
    limb[word + 3] = (uint32_t)(t >> 96);
    top = word + 3;
    for (;;) {
      uint64_t rem = 0;
      uint32_t chunk;
      int k;

      while (top >= 0 && limb[top] == 0) {
        --top;
      }
      if (top < 0) {
        break;
      }
      for (i = top; i >= 0; --i) {
        uint64_t cur = (rem << 32) | limb[i];

        limb[i] = (uint32_t)(cur / 1000000000u);
        rem = cur % 1000000000u;
      }
      chunk = (uint32_t)rem;
      for (k = 0; k < 9; ++k) {
        buf[--pos] = (char)('0' + chunk % 10);
        chunk /= 10;
      }
    }
    while (buf[pos] == '0' && pos < 399) {
      ++pos;
    }
    for (i = pos; i < 400; ++i) {
      tmp[nd++] = buf[i];
    }
    intdigits = nd;
  } else {
    int k = -exp2, L, s;
    uint64_t num;
    uint32_t F[40];
    char buf[24];
    int bn = 0;

    ip = k < 64 ? (mant >> k) : 0;
    num = k < 64 ? (mant & ((1ull << k) - 1)) : mant;
    while (ip != 0) {
      buf[bn++] = (char)('0' + ip % 10);
      ip /= 10;
    }
    while (bn > 0) {
      tmp[nd++] = buf[--bn];
    }
    intdigits = nd;
    L = (k + 31) / 32;
    for (i = 0; i < 40; ++i) {
      F[i] = 0;
    }
    s = 32 * L - k;
    {
      unsigned __int128 t = (unsigned __int128)num << s;

      F[0] = (uint32_t)t;
      F[1] = (uint32_t)(t >> 32);
      F[2] = (uint32_t)(t >> 64);
    }
    for (;;) {
      uint64_t carry = 0;
      int nz = 0;

      for (i = 0; i < L; ++i) {
        if (F[i] != 0) {
          nz = 1;
          break;
        }
      }
      if (!nz || nd >= 1450) {
        break;
      }
      for (i = 0; i < L; ++i) {
        uint64_t t = (uint64_t)F[i] * 10u + carry;

        F[i] = (uint32_t)t;
        carry = t >> 32;
      }
      tmp[nd++] = (char)('0' + carry);
    }
  }
  // tmp holds the integer digits then the fraction digits.
  {
    int start = 0;

    g_decpt = intdigits;
    while (start < nd && tmp[start] == '0') {
      ++start;
      --g_decpt;
    }
    while (nd > start && tmp[nd - 1] == '0') {
      --nd;
    }
    g_ndig = nd - start;
    for (i = 0; i < g_ndig; ++i) {
      g_dig[i] = tmp[start + i];
    }
    if (g_ndig == 0) {
      g_decpt = 0;
    }
  }
}

// Round the digit string to `keep` digits (ties to even); keep may be zero or negative.
static void dig_round(int keep) {
  int up = 0, i;

  if (keep >= g_ndig) {
    return;
  }
  if (keep < 0) {
    g_ndig = 0;
    g_decpt = 0;
    return;
  }
  if (g_dig[keep] > '5') {
    up = 1;
  } else if (g_dig[keep] == '5') {
    int rest = 0;

    for (i = keep + 1; i < g_ndig; ++i) {
      if (g_dig[i] != '0') {
        rest = 1;
      }
    }
    up = rest || (keep > 0 && (((g_dig[keep - 1] - '0') & 1) != 0));
  }
  g_ndig = keep;
  if (up) {
    i = keep - 1;
    while (i >= 0 && g_dig[i] == '9') {
      g_dig[i] = '0';
      --i;
    }
    if (i >= 0) {
      ++g_dig[i];
    } else {
      for (i = 1; i < keep; ++i) {
        g_dig[i] = '0';
      }
      g_dig[0] = '1';
      g_ndig = keep == 0 ? 1 : keep;
      ++g_decpt;
    }
  }
  if (g_ndig == 0) {
    g_decpt = 0;
  }
}

static char g_fbuf[1200];

static int build_f(int prec, int alt) {
  int n = 0, i;

  dig_round(g_decpt + prec);
  if (g_decpt <= 0 || g_ndig == 0) {
    g_fbuf[n++] = '0';
  } else {
    for (i = 0; i < g_decpt; ++i) {
      g_fbuf[n++] = i < g_ndig ? g_dig[i] : '0';
    }
  }
  if (prec > 0 || alt) {
    g_fbuf[n++] = '.';
  }
  for (i = 0; i < prec; ++i) {
    int idx = g_decpt + i;

    g_fbuf[n++] = (g_ndig != 0 && idx >= 0 && idx < g_ndig) ? g_dig[idx] : '0';
  }
  return n;
}

static int build_e(int prec, int alt, int upper) {
  int n = 0, i, e;

  dig_round(prec + 1);
  e = g_ndig == 0 ? 0 : g_decpt - 1;
  g_fbuf[n++] = g_ndig > 0 ? g_dig[0] : '0';
  if (prec > 0 || alt) {
    g_fbuf[n++] = '.';
  }
  for (i = 1; i <= prec; ++i) {
    g_fbuf[n++] = i < g_ndig ? g_dig[i] : '0';
  }
  g_fbuf[n++] = upper ? 'E' : 'e';
  g_fbuf[n++] = e < 0 ? '-' : '+';
  if (e < 0) {
    e = -e;
  }
  if (e >= 100) {
    g_fbuf[n++] = (char)('0' + e / 100);
  }
  g_fbuf[n++] = (char)('0' + (e / 10) % 10);
  g_fbuf[n++] = (char)('0' + e % 10);
  return n;
}

// ---- the printf engine -----------------------------------------------------------------------------

#define F_LEFT  1
#define F_PLUS  2
#define F_SPACE 4
#define F_ALT   8
#define F_ZERO  16

typedef struct {
  fmt_put_fn put;
  void *ctx;
  int total;
} out_t;

static void out_n(out_t *o, const char *s, size_t n) {
  if (n != 0) {
    o->put(o->ctx, s, n);
    o->total += (int)n;
  }
}

static void out_rep(out_t *o, char c, int n) {
  char b[32];
  int k;

  for (k = 0; k < 32; ++k) {
    b[k] = c;
  }
  while (n > 0) {
    int t = n > 32 ? 32 : n;

    out_n(o, b, (size_t)t);
    n -= t;
  }
}

static void fmt_int(out_t *o, unsigned long long v, int neg, int base, int upper, int fl, int width, int prec,
                    int is_signed) {
  char tmp[72], sign = 0, pre[2];
  int n = 0, zeros, prelen = 0, total, pad, nz = v != 0;
  const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";

  if (!(v == 0 && prec == 0)) {
    do {
      tmp[n++] = digs[v % (unsigned)base];
      v /= (unsigned)base;
    } while (v != 0);
  }
  zeros = prec > n ? prec - n : 0;
  if (is_signed) {
    sign = neg ? '-' : ((fl & F_PLUS) ? '+' : ((fl & F_SPACE) ? ' ' : 0));
  }
  if ((fl & F_ALT) && base == 16 && nz) {
    pre[0] = '0';
    pre[1] = upper ? 'X' : 'x';
    prelen = 2;
  }
  if ((fl & F_ALT) && base == 8 && zeros == 0 && (n == 0 || tmp[n - 1] != '0')) {
    zeros = 1;
  }
  total = (sign != 0) + prelen + zeros + n;
  pad = width > total ? width - total : 0;
  if (!(fl & F_LEFT) && !((fl & F_ZERO) && prec < 0)) {
    out_rep(o, ' ', pad);
  }
  if (sign != 0) {
    out_n(o, &sign, 1);
  }
  out_n(o, pre, (size_t)prelen);
  if (!(fl & F_LEFT) && (fl & F_ZERO) && prec < 0) {
    out_rep(o, '0', pad);
  }
  out_rep(o, '0', zeros);
  while (n > 0) {
    out_n(o, &tmp[--n], 1);
  }
  if (fl & F_LEFT) {
    out_rep(o, ' ', pad);
  }
}

static void fmt_float(out_t *o, double v, char conv, int fl, int width, int prec) {
  union { double d; uint64_t u; } c;
  int neg, n, upper = (conv == 'E' || conv == 'G' || conv == 'F' || conv == 'A'), finite = 1;
  char sign = 0;
  const char *special = 0;

  c.d = v;
  neg = (int)(c.u >> 63);
  if (conv == 'a' || conv == 'A') {
    conv = upper ? 'E' : 'e';
  }
  if (prec < 0) {
    prec = 6;
  }
  if (prec > 500) {
    prec = 500;
  }
  if (((c.u >> 52) & 0x7FF) == 0x7FF) {
    finite = 0;
    if ((c.u & 0xFFFFFFFFFFFFFull) == 0) {
      special = upper ? "INF" : "inf";
    } else if (neg && c.u == 0xFFF8000000000000ull) {
      special = upper ? "-NAN(IND)" : "-nan(ind)";
      neg = 0;
    } else {
      special = upper ? "NAN" : "nan";
    }
  }
  sign = neg ? '-' : ((fl & F_PLUS) ? '+' : ((fl & F_SPACE) ? ' ' : 0));
  if (!finite) {
    int len = (int)rt_strlen(special), pad = width > len + (sign != 0) ? width - len - (sign != 0) : 0;

    if (!(fl & F_LEFT)) {
      out_rep(o, ' ', pad);
    }
    if (sign != 0) {
      out_n(o, &sign, 1);
    }
    out_n(o, special, (size_t)len);
    if (fl & F_LEFT) {
      out_rep(o, ' ', pad);
    }
    return;
  }
  c.u &= 0x7FFFFFFFFFFFFFFFull;
  dbl_expand(c.d);
  if (conv == 'f' || conv == 'F') {
    n = build_f(prec, fl & F_ALT);
  } else if (conv == 'e' || conv == 'E') {
    n = build_e(prec, fl & F_ALT, upper);
  } else {
    int p = prec == 0 ? 1 : prec, x;

    dig_round(p);
    x = g_ndig == 0 ? 0 : g_decpt - 1;
    if (x >= -4 && x < p) {
      n = build_f(p - 1 - x, fl & F_ALT);
      if (!(fl & F_ALT)) {
        int dot = 0, i;

        for (i = 0; i < n; ++i) {
          if (g_fbuf[i] == '.') {
            dot = 1;
          }
        }
        if (dot) {
          while (g_fbuf[n - 1] == '0') {
            --n;
          }
          if (g_fbuf[n - 1] == '.') {
            --n;
          }
        }
      }
    } else {
      n = build_e(p - 1, fl & F_ALT, upper);
      if (!(fl & F_ALT)) {
        int epos = n - 1, i;

        while (epos >= 0 && g_fbuf[epos] != 'e' && g_fbuf[epos] != 'E') {
          --epos;
        }
        {
          int mant_end = epos, dot = 0;

          for (i = 0; i < mant_end; ++i) {
            if (g_fbuf[i] == '.') {
              dot = 1;
            }
          }
          if (dot) {
            int m = mant_end;

            while (g_fbuf[m - 1] == '0') {
              --m;
            }
            if (g_fbuf[m - 1] == '.') {
              --m;
            }
            for (i = epos; i < n; ++i) {
              g_fbuf[m++] = g_fbuf[i];
            }
            n = m;
          }
        }
      }
    }
  }
  {
    int total = n + (sign != 0), pad = width > total ? width - total : 0;

    if (!(fl & F_LEFT) && !(fl & F_ZERO)) {
      out_rep(o, ' ', pad);
    }
    if (sign != 0) {
      out_n(o, &sign, 1);
    }
    if (!(fl & F_LEFT) && (fl & F_ZERO)) {
      out_rep(o, '0', pad);
    }
    out_n(o, g_fbuf, (size_t)n);
    if (fl & F_LEFT) {
      out_rep(o, ' ', pad);
    }
  }
}

static void fmt_str(out_t *o, const void *s, int is_wide, int fl, int width, int prec) {
  int len = 0, pad, i;
  const char *cs = (const char *)s;
  const unsigned short *ws = (const unsigned short *)s;

  if (s == 0) {
    cs = "(null)";
    is_wide = 0;
  }
  if (is_wide) {
    while ((prec < 0 || len < prec) && ws[len] != 0) {
      ++len;
    }
  } else {
    while ((prec < 0 || len < prec) && cs[len] != 0) {
      ++len;
    }
  }
  pad = width > len ? width - len : 0;
  if (!(fl & F_LEFT)) {
    out_rep(o, ' ', pad);
  }
  if (is_wide) {
    for (i = 0; i < len; ++i) {
      char ch = ws[i] < 128 ? (char)ws[i] : '?';

      out_n(o, &ch, 1);
    }
  } else {
    out_n(o, cs, (size_t)len);
  }
  if (fl & F_LEFT) {
    out_rep(o, ' ', pad);
  }
}

int rt_format(fmt_put_fn put, void *ctx, const char *fmt, va_list ap, int wide) {
  out_t o;
  const char *p = fmt;

  o.put = put;
  o.ctx = ctx;
  o.total = 0;
  while (*p != 0) {
    const char *lit = p;
    int fl = 0, width = 0, prec = -1, lenmod = 0;   // lenmod: 1 hh, 2 h, 3 l, 4 ll/I64/z/j/t, 5 w (wide char/string)
    char conv;

    while (*p != 0 && *p != '%') {
      ++p;
    }
    out_n(&o, lit, (size_t)(p - lit));
    if (*p == 0) {
      break;
    }
    ++p;
    for (;; ++p) {
      if (*p == '-') { fl |= F_LEFT; }
      else if (*p == '+') { fl |= F_PLUS; }
      else if (*p == ' ') { fl |= F_SPACE; }
      else if (*p == '#') { fl |= F_ALT; }
      else if (*p == '0') { fl |= F_ZERO; }
      else { break; }
    }
    if (*p == '*') {
      width = va_arg(ap, int);
      if (width < 0) {
        fl |= F_LEFT;
        width = -width;
      }
      ++p;
    } else {
      while (rt_isdigit(*p)) {
        width = width * 10 + (*p++ - '0');
      }
    }
    if (*p == '.') {
      ++p;
      prec = 0;
      if (*p == '*') {
        prec = va_arg(ap, int);
        ++p;
      } else {
        while (rt_isdigit(*p)) {
          prec = prec * 10 + (*p++ - '0');
        }
      }
    }
    for (;;) {
      if (p[0] == 'h' && p[1] == 'h') { lenmod = 1; p += 2; }
      else if (p[0] == 'h') { lenmod = 2; ++p; }
      else if (p[0] == 'l' && p[1] == 'l') { lenmod = 4; p += 2; }
      else if (p[0] == 'l') { lenmod = 3; ++p; }
      else if (p[0] == 'L' || p[0] == 'j' || p[0] == 'z' || p[0] == 't') { lenmod = 4; ++p; }
      else if (p[0] == 'w') { lenmod = 5; ++p; }
      else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { lenmod = 4; p += 3; }
      else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { lenmod = 0; p += 3; }
      else if (p[0] == 'I') { lenmod = 4; ++p; }
      else { break; }
    }
    conv = *p;
    if (conv == 0) {
      break;
    }
    ++p;
    switch (conv) {
    case '%':
      out_n(&o, "%", 1);
      break;
    case 'd':
    case 'i': {
      long long v;

      if (lenmod == 4) { v = va_arg(ap, long long); }
      else if (lenmod == 1) { v = (signed char)va_arg(ap, int); }
      else if (lenmod == 2) { v = (short)va_arg(ap, int); }
      else { v = va_arg(ap, int); }
      fmt_int(&o, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v, v < 0, 10, 0, fl, width, prec, 1);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
      unsigned long long v;

      if (lenmod == 4) { v = va_arg(ap, unsigned long long); }
      else if (lenmod == 1) { v = (unsigned char)va_arg(ap, unsigned int); }
      else if (lenmod == 2) { v = (unsigned short)va_arg(ap, unsigned int); }
      else { v = va_arg(ap, unsigned int); }
      fmt_int(&o, v, 0, conv == 'u' ? 10 : (conv == 'o' ? 8 : 16), conv == 'X', fl, width, prec, 0);
      break;
    }
    case 'p': {
      unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);

      fmt_int(&o, v, 0, 16, 1, (fl & ~F_ALT) | F_ZERO, width > 16 ? width : 16, 16, 0);
      break;
    }
    case 'c':
    case 'C': {
      int ch = va_arg(ap, int);
      char cb = ((ch & 0xFFFF) < 128 || conv == 'c' || lenmod != 5) ? (char)ch : '?';
      int pad = width > 1 ? width - 1 : 0;

      if (!(fl & F_LEFT)) {
        out_rep(&o, ' ', pad);
      }
      out_n(&o, &cb, 1);
      if (fl & F_LEFT) {
        out_rep(&o, ' ', pad);
      }
      break;
    }
    case 's':
    case 'S': {
      const void *s = va_arg(ap, const void *);
      int is_wide;

      if (lenmod == 5 || lenmod == 3) {
        is_wide = 1;
      } else if (lenmod == 2) {
        is_wide = 0;
      } else {
        is_wide = conv == 's' ? (wide != 0) : (wide == 0);
      }
      fmt_str(&o, s, is_wide, fl, width, prec);
      break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
      fmt_float(&o, va_arg(ap, double), conv, fl, width, prec);
      break;
    case 'n':
      (void)va_arg(ap, int *);
      break;
    default:
      out_n(&o, "%", 1);
      out_n(&o, &conv, 1);
      break;
    }
  }
  return o.total;
}

// ---- number parsing --------------------------------------------------------------------------------

typedef struct { uint64_t m; int e; } ext_t;     // m * 2^e, with the top bit of m set (or m == 0)

static ext_t ext_norm(uint64_t m, int e) {
  ext_t r;

  if (m == 0) {
    r.m = 0;
    r.e = 0;
    return r;
  }
  {
    int s = __builtin_clzll(m);

    r.m = m << s;
    r.e = e - s;
  }
  return r;
}

static ext_t ext_mul(ext_t a, ext_t b) {
  unsigned __int128 p = (unsigned __int128)a.m * b.m;
  uint64_t hi = (uint64_t)(p >> 64), lo = (uint64_t)p;
  int e = a.e + b.e + 64;
  ext_t r;

  if (!(hi >> 63)) {
    hi = (hi << 1) | (lo >> 63);
    lo <<= 1;
    --e;
  }
  if (lo >> 63) {
    ++hi;
    if (hi == 0) {
      hi = 1ull << 63;
      ++e;
    }
  }
  r.m = hi;
  r.e = e;
  return r;
}

static ext_t ext_div(ext_t a, ext_t b) {
  uint64_t rem = a.m, q = 0;
  int hiq = 0, i, e = a.e - b.e - 64;
  ext_t r;

  if (rem >= b.m) {
    rem -= b.m;
    hiq = 1;
  }
  for (i = 0; i < 64; ++i) {
    uint64_t carry = rem >> 63;

    rem <<= 1;
    q <<= 1;
    if (carry || rem >= b.m) {
      rem -= b.m;
      q |= 1;
    }
  }
  if (hiq) {
    uint64_t low = q & 1;

    q = (q >> 1) | (1ull << 63);
    ++e;
    if (low && ++q == 0) {
      q = 1ull << 63;
      ++e;
    }
  }
  r.m = q;
  r.e = e;
  return r;
}

static ext_t ext_pow10(int n) {
  ext_t result = ext_norm(1, 0), base = ext_norm(10, 0);

  while (n > 0) {
    if (n & 1) {
      result = ext_mul(result, base);
    }
    n >>= 1;
    if (n > 0) {
      base = ext_mul(base, base);
    }
  }
  return result;
}

static double ext_to_double(ext_t x, int neg) {
  union { double d; uint64_t u; } c;
  int E = x.e + 63;
  uint64_t q;

  if (x.m == 0) {
    c.u = (uint64_t)neg << 63;
    return c.d;
  }
  if (E > 1023) {
    c.u = 0x7FF0000000000000ull | ((uint64_t)neg << 63);
    rt_errno = RT_ERANGE;
    return c.d;
  }
  if (E >= -1022) {
    uint64_t low = x.m & 0x7FF;

    q = x.m >> 11;
    if (low > 0x400 || (low == 0x400 && (q & 1))) {
      ++q;
    }
    if (q == (1ull << 53)) {
      q >>= 1;
      ++E;
      if (E > 1023) {
        c.u = 0x7FF0000000000000ull | ((uint64_t)neg << 63);
        rt_errno = RT_ERANGE;
        return c.d;
      }
    }
    c.u = ((uint64_t)(E + 1023) << 52) | (q & 0xFFFFFFFFFFFFFull) | ((uint64_t)neg << 63);
    return c.d;
  }
  {
    int shift = (-1022 - E) + 11;

    if (shift > 64) {
      q = 0;
    } else if (shift == 64) {
      q = x.m > (1ull << 63) ? 1 : 0;
    } else {
      uint64_t rem = x.m & ((1ull << shift) - 1), half = 1ull << (shift - 1);

      q = x.m >> shift;
      if (rem > half || (rem == half && (q & 1))) {
        ++q;
      }
    }
    if (q == 0) {
      rt_errno = RT_ERANGE;
    }
    c.u = q | ((uint64_t)neg << 63);
    return c.d;
  }
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int match_ci(const char *p, const char *word) {
  while (*word != 0) {
    if (rt_tolower(*p) != *word) {
      return 0;
    }
    ++p;
    ++word;
  }
  return 1;
}

double rt_strtod(const char *s, char **end) {
  const char *p = s;
  int neg = 0, any = 0, nd = 0, dexp = 0, exp10 = 0, seen_point = 0;
  uint64_t m = 0;
  union { double d; uint64_t u; } c;
  ext_t x;
  int total;

  while (rt_isspace(*p)) {
    ++p;
  }
  if (*p == '-' || *p == '+') {
    neg = *p == '-';
    ++p;
  }
  if (match_ci(p, "inf")) {
    p += match_ci(p, "infinity") ? 8 : 3;
    if (end != 0) {
      *end = (char *)p;
    }
    c.u = 0x7FF0000000000000ull | ((uint64_t)neg << 63);
    return c.d;
  }
  if (match_ci(p, "nan")) {
    p += 3;
    if (*p == '(') {
      const char *q = p + 1;

      while (*q != 0 && *q != ')') {
        ++q;
      }
      if (*q == ')') {
        p = q + 1;
      }
    }
    if (end != 0) {
      *end = (char *)p;
    }
    c.u = 0x7FF8000000000000ull | ((uint64_t)neg << 63);
    return c.d;
  }
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (hexval(p[2]) >= 0 || (p[2] == '.' && hexval(p[3]) >= 0))) {
    int bexp = 0;

    p += 2;
    for (;; ++p) {
      int h = hexval(*p);

      if (*p == '.' && !seen_point) {
        seen_point = 1;
        continue;
      }
      if (h < 0) {
        break;
      }
      if (nd < 16) {
        m = (m << 4) | (uint64_t)h;
        if (m != 0) {
          ++nd;
        }
        if (seen_point) {
          bexp -= 4;
        }
      } else if (!seen_point) {
        bexp += 4;
      }
    }
    if (*p == 'p' || *p == 'P') {
      const char *q = p + 1;
      int eneg = 0, ev = 0;

      if (*q == '-' || *q == '+') {
        eneg = *q == '-';
        ++q;
      }
      if (rt_isdigit(*q)) {
        while (rt_isdigit(*q)) {
          if (ev < 100000) {
            ev = ev * 10 + (*q - '0');
          }
          ++q;
        }
        bexp += eneg ? -ev : ev;
        p = q;
      }
    }
    if (end != 0) {
      *end = (char *)p;
    }
    x = ext_norm(m, bexp);
    return ext_to_double(x, neg);
  }
  for (;; ++p) {
    int d;

    if (*p == '.' && !seen_point) {
      seen_point = 1;
      continue;
    }
    if (!rt_isdigit(*p)) {
      break;
    }
    d = *p - '0';
    any = 1;
    if (nd == 0 && d == 0) {
      if (seen_point) {
        --dexp;
      }
    } else if (nd < 19) {
      m = m * 10u + (uint64_t)d;
      ++nd;
      if (seen_point) {
        --dexp;
      }
    } else if (!seen_point) {
      ++dexp;
    }
  }
  if (!any) {
    if (end != 0) {
      *end = (char *)s;
    }
    return 0.0;
  }
  if (*p == 'e' || *p == 'E') {
    const char *q = p + 1;
    int eneg = 0, ev = 0;

    if (*q == '-' || *q == '+') {
      eneg = *q == '-';
      ++q;
    }
    if (rt_isdigit(*q)) {
      while (rt_isdigit(*q)) {
        if (ev < 100000) {
          ev = ev * 10 + (*q - '0');
        }
        ++q;
      }
      exp10 = eneg ? -ev : ev;
      p = q;
    }
  }
  if (end != 0) {
    *end = (char *)p;
  }
  if (m == 0) {
    c.u = (uint64_t)neg << 63;
    return c.d;
  }
  total = dexp + exp10;
  if (total > 400) {
    c.u = 0x7FF0000000000000ull | ((uint64_t)neg << 63);
    rt_errno = RT_ERANGE;
    return c.d;
  }
  if (total < -400) {
    rt_errno = RT_ERANGE;
    c.u = (uint64_t)neg << 63;
    return c.d;
  }
  x = ext_norm(m, 0);
  if (total >= 0) {
    x = ext_mul(x, ext_pow10(total));
  } else {
    x = ext_div(x, ext_pow10(-total));
  }
  return ext_to_double(x, neg);
}

unsigned long long rt_strtoull(const char *s, char **end, int base, int *neg, int *overflow) {
  const char *p = s;
  unsigned long long v = 0;
  int any = 0;

  *neg = 0;
  *overflow = 0;
  while (rt_isspace(*p)) {
    ++p;
  }
  if (*p == '-' || *p == '+') {
    *neg = *p == '-';
    ++p;
  }
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hexval(p[2]) >= 0) {
    p += 2;
    base = 16;
  } else if (base == 0) {
    base = p[0] == '0' ? 8 : 10;
  }
  if (base < 2 || base > 36) {
    if (end != 0) {
      *end = (char *)s;
    }
    return 0;
  }
  for (;; ++p) {
    int d;

    if (rt_isdigit(*p)) {
      d = *p - '0';
    } else if (rt_isalpha(*p)) {
      d = rt_tolower(*p) - 'a' + 10;
    } else {
      break;
    }
    if (d >= base) {
      break;
    }
    any = 1;
    if (v > (0xFFFFFFFFFFFFFFFFull - (unsigned)d) / (unsigned)base) {
      *overflow = 1;
      v = 0xFFFFFFFFFFFFFFFFull;
    } else if (!*overflow) {
      v = v * (unsigned)base + (unsigned)d;
    }
  }
  if (end != 0) {
    *end = (char *)(any ? p : s);
  }
  return v;
}

EXPORT_SPECIAL double strtod(const char *s, char **end) { return rt_strtod(s, end); }
EXPORT_SPECIAL double _strtod_l(const char *s, char **end, void *loc) { (void)loc; return rt_strtod(s, end); }
EXPORT_SPECIAL float strtof(const char *s, char **end) { return (float)rt_strtod(s, end); }
EXPORT_SPECIAL double atof(const char *s) { return rt_strtod(s, 0); }

EXPORT_SPECIAL long long strtoll(const char *s, char **end, int base) {
  int neg, ovf;
  unsigned long long v = rt_strtoull(s, end, base, &neg, &ovf);

  if (ovf || (!neg && v > 0x7FFFFFFFFFFFFFFFull) || (neg && v > 0x8000000000000000ull)) {
    rt_errno = RT_ERANGE;
    return neg ? (long long)0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFll;
  }
  return neg ? (long long)(0ull - v) : (long long)v;
}

EXPORT_SPECIAL unsigned long long strtoull(const char *s, char **end, int base) {
  int neg, ovf;
  unsigned long long v = rt_strtoull(s, end, base, &neg, &ovf);

  if (ovf) {
    rt_errno = RT_ERANGE;
    return 0xFFFFFFFFFFFFFFFFull;
  }
  return neg ? 0ull - v : v;
}

EXPORT_SPECIAL long long _strtoi64(const char *s, char **end, int base) { return strtoll(s, end, base); }
EXPORT_SPECIAL unsigned long long _strtoui64(const char *s, char **end, int base) { return strtoull(s, end, base); }

EXPORT_SPECIAL int32_t strtol(const char *s, char **end, int base) {
  int neg, ovf;
  unsigned long long v = rt_strtoull(s, end, base, &neg, &ovf);

  if (ovf || (!neg && v > 0x7FFFFFFFull) || (neg && v > 0x80000000ull)) {
    rt_errno = RT_ERANGE;
    return neg ? (int32_t)0x80000000u : 0x7FFFFFFF;
  }
  return neg ? (int32_t)(0ull - v) : (int32_t)v;
}

EXPORT_SPECIAL uint32_t strtoul(const char *s, char **end, int base) {
  int neg, ovf;
  unsigned long long v = rt_strtoull(s, end, base, &neg, &ovf);

  if (ovf || v > 0xFFFFFFFFull) {
    rt_errno = RT_ERANGE;
    return 0xFFFFFFFFu;
  }
  return neg ? (uint32_t)(0ull - v) : (uint32_t)v;
}

EXPORT_SPECIAL int atoi(const char *s) { return (int)strtol(s, 0, 10); }
EXPORT_SPECIAL int32_t atol(const char *s) { return strtol(s, 0, 10); }
EXPORT_SPECIAL long long atoll(const char *s) { return strtoll(s, 0, 10); }
EXPORT_SPECIAL long long _atoi64(const char *s) { return strtoll(s, 0, 10); }

static char *utoa_core(unsigned long long v, char *buf, int base, int neg) {
  char tmp[72];
  int n = 0, i = 0;

  if (base < 2 || base > 36) {
    base = 10;
  }
  do {
    int d = (int)(v % (unsigned)base);

    tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v /= (unsigned)base;
  } while (v != 0);
  if (neg) {
    buf[i++] = '-';
  }
  while (n > 0) {
    buf[i++] = tmp[--n];
  }
  buf[i] = 0;
  return buf;
}

EXPORT_SPECIAL char *_itoa(int v, char *buf, int base) {
  return utoa_core(v < 0 && base == 10 ? 0u - (unsigned)v : (unsigned)v, buf, base, v < 0 && base == 10);
}
EXPORT_SPECIAL char *_ltoa(int32_t v, char *buf, int base) { return _itoa(v, buf, base); }
EXPORT_SPECIAL char *_ultoa(uint32_t v, char *buf, int base) { return utoa_core(v, buf, base, 0); }
EXPORT_SPECIAL char *_i64toa(long long v, char *buf, int base) {
  return utoa_core(v < 0 && base == 10 ? 0ull - (unsigned long long)v : (unsigned long long)v, buf, base, v < 0 && base == 10);
}
EXPORT_SPECIAL char *_ui64toa(unsigned long long v, char *buf, int base) { return utoa_core(v, buf, base, 0); }
EXPORT_SPECIAL int _itoa_s(int v, char *buf, size_t size, int base) {
  char tmp[72];

  _itoa(v, tmp, base);
  if (rt_strlen(tmp) >= size) {
    return RT_EINVAL;
  }
  rt_copy(buf, tmp, size);
  return 0;
}

// ---- the scanf engine ------------------------------------------------------------------------------

typedef struct {
  scan_src_t *src;
  int consumed;
  int width;                 // characters left for the current conversion (-1: unlimited)
} scan_t;

static int sc_get(scan_t *s) {
  int c;

  if (s->width == 0) {
    return -1;
  }
  c = s->src->get(s->src->ctx);
  if (c >= 0) {
    ++s->consumed;
    if (s->width > 0) {
      --s->width;
    }
  }
  return c;
}

static void sc_unget(scan_t *s, int c) {
  if (c >= 0) {
    s->src->unget(s->src->ctx, c);
    --s->consumed;
    if (s->width >= 0) {
      ++s->width;
    }
  }
}

static int sc_skip_ws(scan_t *s) {
  int c;

  do {
    c = sc_get(s);
  } while (c >= 0 && rt_isspace(c));
  sc_unget(s, c);
  return c >= 0;
}

int rt_scan(scan_src_t *src, const char *fmt, va_list ap) {
  scan_t s;
  const char *p = fmt;
  int assigned = 0, attempted = 0;

  s.src = src;
  s.consumed = 0;
  s.width = -1;
  while (*p != 0) {
    int suppress = 0, width = -1, lenmod = 0, c;
    char conv;

    if (rt_isspace(*p)) {
      s.width = -1;
      sc_skip_ws(&s);
      ++p;
      continue;
    }
    if (*p != '%') {
      s.width = -1;
      c = sc_get(&s);
      if (c != (unsigned char)*p) {
        sc_unget(&s, c);
        goto done;
      }
      ++p;
      continue;
    }
    ++p;
    if (*p == '*') {
      suppress = 1;
      ++p;
    }
    if (rt_isdigit(*p)) {
      width = 0;
      while (rt_isdigit(*p)) {
        width = width * 10 + (*p++ - '0');
      }
    }
    for (;;) {
      if (p[0] == 'h' && p[1] == 'h') { lenmod = 1; p += 2; }
      else if (p[0] == 'h') { lenmod = 2; ++p; }
      else if (p[0] == 'l' && p[1] == 'l') { lenmod = 4; p += 2; }
      else if (p[0] == 'l') { lenmod = 3; ++p; }
      else if (p[0] == 'L' || p[0] == 'j' || p[0] == 'z' || p[0] == 't') { lenmod = 4; ++p; }
      else if (p[0] == 'w') { lenmod = 5; ++p; }
      else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { lenmod = 4; p += 3; }
      else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { lenmod = 0; p += 3; }
      else { break; }
    }
    conv = *p;
    if (conv == 0) {
      break;
    }
    ++p;
    s.width = width;
    if (conv == '%') {
      sc_skip_ws(&s);
      c = sc_get(&s);
      if (c != '%') {
        sc_unget(&s, c);
        goto done;
      }
      continue;
    }
    if (conv == 'n') {
      if (!suppress) {
        int *dst = va_arg(ap, int *);

        *dst = s.consumed;
      }
      continue;
    }
    ++attempted;
    if (conv == 'c') {
      char *dst = suppress ? 0 : va_arg(ap, char *);
      int n = width < 0 ? 1 : width, got = 0;

      s.width = n;
      while (got < n && (c = sc_get(&s)) >= 0) {
        if (dst != 0) {
          dst[got] = (char)c;
        }
        ++got;
      }
      if (got == 0) {
        goto done;
      }
      if (!suppress) {
        ++assigned;
      }
    } else if (conv == 's') {
      char *dst = suppress ? 0 : va_arg(ap, char *);
      int got = 0;

      if (!sc_skip_ws(&s)) {
        goto done;
      }
      while ((c = sc_get(&s)) >= 0) {
        if (rt_isspace(c)) {
          sc_unget(&s, c);
          break;
        }
        if (dst != 0) {
          dst[got] = (char)c;
        }
        ++got;
      }
      if (got == 0) {
        goto done;
      }
      if (dst != 0) {
        dst[got] = 0;
        ++assigned;
      }
    } else if (conv == '[') {
      char set[256];
      int negate = 0, got = 0, i;
      char *dst = suppress ? 0 : va_arg(ap, char *);

      for (i = 0; i < 256; ++i) {
        set[i] = 0;
      }
      if (*p == '^') {
        negate = 1;
        ++p;
      }
      if (*p == ']') {
        set[(unsigned char)']'] = 1;
        ++p;
      }
      while (*p != 0 && *p != ']') {
        if (p[1] == '-' && p[2] != 0 && p[2] != ']') {
          for (i = (unsigned char)p[0]; i <= (unsigned char)p[2]; ++i) {
            set[i] = 1;
          }
          p += 3;
        } else {
          set[(unsigned char)*p++] = 1;
        }
      }
      if (*p == ']') {
        ++p;
      }
      while ((c = sc_get(&s)) >= 0) {
        if ((set[c & 255] != 0) == negate) {
          sc_unget(&s, c);
          break;
        }
        if (dst != 0) {
          dst[got] = (char)c;
        }
        ++got;
      }
      if (got == 0) {
        goto done;
      }
      if (dst != 0) {
        dst[got] = 0;
        ++assigned;
      }
    } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') {
      char buf[80];
      int n = 0, base = conv == 'd' || conv == 'u' ? 10 : (conv == 'o' ? 8 : ((conv == 'i') ? 0 : 16)), neg = 0, ovf;
      unsigned long long v;
      int digits = 0;

      if (!sc_skip_ws(&s)) {
        goto done;
      }
      c = sc_get(&s);
      if (c == '-' || c == '+') {
        neg = c == '-';
        buf[n++] = (char)c;
        c = sc_get(&s);
      }
      if (c == '0' && (base == 0 || base == 16)) {
        int c2;

        buf[n++] = '0';
        digits = 1;
        c2 = sc_get(&s);
        if (c2 == 'x' || c2 == 'X') {
          buf[n++] = 'x';
          base = 16;
          digits = 0;
          c = sc_get(&s);
        } else {
          if (base == 0) {
            base = 8;
          }
          c = c2;
        }
      }
      if (base == 0) {
        base = 10;
      }
      while (c >= 0 && n < 78) {
        int d = hexval(c);

        if (!(d >= 0 && d < base) && !(base == 10 && rt_isdigit(c))) {
          break;
        }
        buf[n++] = (char)c;
        digits = 1;
        c = sc_get(&s);
      }
      sc_unget(&s, c);
      if (!digits) {
        goto done;
      }
      buf[n] = 0;
      v = rt_strtoull(buf, 0, base, &neg, &ovf);
      if (neg) {
        v = 0ull - v;
      }
      if (!suppress) {
        if (lenmod == 4 || conv == 'p') { *va_arg(ap, unsigned long long *) = v; }
        else if (lenmod == 1) { *va_arg(ap, unsigned char *) = (unsigned char)v; }
        else if (lenmod == 2) { *va_arg(ap, unsigned short *) = (unsigned short)v; }
        else { *va_arg(ap, unsigned int *) = (unsigned int)v; }
        ++assigned;
      }
    } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a' || conv == 'A') {
      char buf[96];
      int n = 0, seen_digit = 0, seen_dot = 0;
      double v;

      if (!sc_skip_ws(&s)) {
        goto done;
      }
      c = sc_get(&s);
      if (c == '-' || c == '+') {
        buf[n++] = (char)c;
        c = sc_get(&s);
      }
      if (rt_isalpha(c)) {                                  // inf, nan
        while (c >= 0 && rt_isalpha(c) && n < 90) {
          buf[n++] = (char)c;
          c = sc_get(&s);
        }
        sc_unget(&s, c);
      } else {
        while (c >= 0 && n < 90) {
          if (rt_isdigit(c)) {
            seen_digit = 1;
          } else if (c == '.' && !seen_dot) {
            seen_dot = 1;
          } else if ((c == 'e' || c == 'E') && seen_digit) {
            buf[n++] = (char)c;
            c = sc_get(&s);
            if (c == '-' || c == '+') {
              buf[n++] = (char)c;
              c = sc_get(&s);
            }
            while (c >= 0 && rt_isdigit(c) && n < 90) {
              buf[n++] = (char)c;
              c = sc_get(&s);
            }
            break;
          } else {
            break;
          }
          buf[n++] = (char)c;
          c = sc_get(&s);
        }
        sc_unget(&s, c);
        if (!seen_digit) {
          goto done;
        }
      }
      buf[n] = 0;
      v = rt_strtod(buf, 0);
      if (!suppress) {
        if (lenmod == 3 || lenmod == 4) { *va_arg(ap, double *) = v; }
        else { *va_arg(ap, float *) = (float)v; }
        ++assigned;
      }
    } else {
      goto done;
    }
  }
done:
  s.width = -1;
  if (assigned == 0 && attempted != 0) {
    int c = sc_get(&s);

    if (c < 0) {
      return -1;                                            // input ended before the first conversion
    }
    sc_unget(&s, c);
  }
  return assigned;
}
