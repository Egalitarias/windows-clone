// <math.h> for the C runtime part of WIN32.DLL. The transcendental functions use the x87 unit
// (extended precision inside, so the results are good to about one unit in the last place).
// Built with SSE enabled (unlike the rest of the runtime): doubles travel in XMM registers.
#include "crt.h"

int _fltused = 1;                          // the compiler asks for this symbol when floating point is used

typedef union { double d; uint64_t u; } dbl_t;
typedef union { float f; uint32_t u; } flt_t;

#define M EXPORT_SPECIAL double
#define MF EXPORT_SPECIAL float

#define X87(body) ".intel_syntax noprefix\n\t" body "\n\t.att_syntax\n"

static double mkd(uint64_t u) { dbl_t b; b.u = u; return b.d; }
static uint64_t bitsd(double d) { dbl_t b; b.d = d; return b.u; }
#define INF_ mkd(0x7FF0000000000000ull)
#define NAN_ mkd(0x7FF8000000000000ull)
#define ISNAN(x) ((x) != (x))
#define ISINF(x) (((bitsd(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull))

M fabs(double x) { return mkd(bitsd(x) & 0x7FFFFFFFFFFFFFFFull); }
M copysign(double x, double y) { return mkd((bitsd(x) & 0x7FFFFFFFFFFFFFFFull) | (bitsd(y) & 0x8000000000000000ull)); }
M _copysign(double x, double y) { return copysign(x, y); }
M _chgsign(double x) { return mkd(bitsd(x) ^ 0x8000000000000000ull); }
M sqrt(double x) { return __builtin_sqrt(x); }

static int signbit_(double x) { return (int)(bitsd(x) >> 63); }

M trunc(double x) {
  if (!(fabs(x) < 4503599627370496.0)) {
    return x;
  }
  return copysign((double)(long long)x, x);
}

M floor(double x) {
  double r;

  if (!(fabs(x) < 4503599627370496.0)) {
    return x;
  }
  r = (double)(long long)x;
  if (r > x) {
    r -= 1.0;
  }
  return r == 0.0 ? copysign(0.0, x) : r;
}

M ceil(double x) {
  double r;

  if (!(fabs(x) < 4503599627370496.0)) {
    return x;
  }
  r = (double)(long long)x;
  if (r < x) {
    r += 1.0;
  }
  return r == 0.0 ? copysign(0.0, x) : r;
}

M round(double x) {
  double t;

  if (!(fabs(x) < 4503599627370496.0)) {
    return x;
  }
  t = trunc(x);
  if (fabs(x - t) >= 0.5) {
    t += copysign(1.0, x);
  }
  return t == 0.0 ? copysign(0.0, x) : t;
}

M rint(double x) {
  double r;

  if (!(fabs(x) < 4503599627370496.0)) {
    return x;
  }
  __asm__(X87("frndint") : "=t"(r) : "0"(x));
  return r == 0.0 ? copysign(0.0, x) : r;
}
M nearbyint(double x) { return rint(x); }

EXPORT_SPECIAL int32_t lround(double x) { return (int32_t)round(x); }
EXPORT_SPECIAL long long llround(double x) { return (long long)round(x); }
EXPORT_SPECIAL int32_t lrint(double x) { return (int32_t)rint(x); }
EXPORT_SPECIAL long long llrint(double x) { return (long long)rint(x); }

M fmod(double x, double y) {
  double r;

  if (ISNAN(x) || ISNAN(y) || ISINF(x) || y == 0.0) {
    return NAN_;
  }
  if (ISINF(y) || x == 0.0) {
    return x;
  }
  __asm__(X87("1: fprem\n\tfnstsw ax\n\ttest ah, 4\n\tjnz 1b\n\tfstp st(1)") : "=t"(r) : "0"(fabs(x)), "u"(fabs(y)) : "st(1)", "ax", "cc");
  return signbit_(x) ? -r : r;
}

M modf(double x, double *ip) {
  double t = trunc(x);

  *ip = t;
  if (ISINF(x)) {
    return copysign(0.0, x);
  }
  return copysign(x - t, x);
}

M frexp(double x, int *e) {
  uint64_t b = bitsd(x);
  int ex = (int)((b >> 52) & 0x7FF);

  *e = 0;
  if (x == 0.0 || ex == 0x7FF) {
    return x;
  }
  if (ex == 0) {                                   // subnormal: scale up first
    x *= 18014398509481984.0;                      // 2^54
    b = bitsd(x);
    ex = (int)((b >> 52) & 0x7FF) - 54;
  }
  *e = ex - 1022;
  return mkd((b & 0x800FFFFFFFFFFFFFull) | (1022ull << 52));
}

M ldexp(double x, int n) {
  double r;

  if (x == 0.0 || ISNAN(x) || ISINF(x)) {
    return x;
  }
  if (n > 5000) {
    n = 5000;
  }
  if (n < -5000) {
    n = -5000;
  }
  __asm__(X87("fscale\n\tfstp st(1)") : "=t"(r) : "0"(x), "u"((double)n) : "st(1)");
  return r;
}
M scalbn(double x, int n) { return ldexp(x, n); }
M _scalb(double x, int32_t n) { return ldexp(x, n); }

// ---- trigonometry ----------------------------------------------------------------------------------

static double reduce_big(double x) {
  double r;

  __asm__(X87("fldpi\n\tfadd st(0), st(0)\n\tfxch\n\t1: fprem1\n\tfnstsw ax\n\ttest ah, 4\n\tjnz 1b\n\tfstp st(1)") : "=t"(r) : "0"(x) : "ax", "cc");
  return r;
}

M sin(double x) {
  double r;

  if (ISNAN(x) || ISINF(x)) {
    return NAN_;
  }
  if (fabs(x) >= 4611686018427387904.0) {
    x = reduce_big(x);
  }
  __asm__(X87("fsin") : "=t"(r) : "0"(x));
  return r;
}

M cos(double x) {
  double r;

  if (ISNAN(x) || ISINF(x)) {
    return NAN_;
  }
  if (fabs(x) >= 4611686018427387904.0) {
    x = reduce_big(x);
  }
  __asm__(X87("fcos") : "=t"(r) : "0"(x));
  return r;
}

M tan(double x) {
  double r;

  if (ISNAN(x) || ISINF(x)) {
    return NAN_;
  }
  if (fabs(x) >= 4611686018427387904.0) {
    x = reduce_big(x);
  }
  __asm__(X87("fptan\n\tfstp st(0)") : "=t"(r) : "0"(x));
  return r;
}

M atan(double x) {
  double r;

  if (ISNAN(x)) {
    return x;
  }
  __asm__(X87("fld1\n\tfpatan") : "=t"(r) : "0"(x));
  return r;
}

M atan2(double y, double x) {
  double r;

  if (ISNAN(x) || ISNAN(y)) {
    return NAN_;
  }
  __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
  return r;
}

M asin(double x) {
  if (!(fabs(x) <= 1.0)) {
    rt_errno = RT_EDOM;
    return NAN_;
  }
  return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

M acos(double x) {
  if (!(fabs(x) <= 1.0)) {
    rt_errno = RT_EDOM;
    return NAN_;
  }
  return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

// ---- exponentials and logarithms ---------------------------------------------------------------------

#define EXP2_SEQ "fld st(0)\n\tfrndint\n\tfxch st(1)\n\tfsub st(0), st(1)\n\tf2xm1\n\tfld1\n\tfaddp st(1), st(0)\n\tfscale\n\tfstp st(1)"

M exp2(double x) {
  double r;

  if (ISNAN(x)) {
    return x;
  }
  if (x > 1100.0) {
    return INF_;
  }
  if (x < -1200.0) {
    return 0.0;
  }
  __asm__(X87(EXP2_SEQ) : "=t"(r) : "0"(x));
  return r;
}

M exp(double x) {
  double r;

  if (ISNAN(x)) {
    return x;
  }
  if (x > 710.0) {
    rt_errno = RT_ERANGE;
    return INF_;
  }
  if (x < -746.0) {
    return 0.0;
  }
  __asm__(X87("fldl2e\n\tfmulp st(1), st(0)\n\t" EXP2_SEQ) : "=t"(r) : "0"(x));
  return r;
}

M log(double x) {
  double r;

  if (ISNAN(x)) {
    return x;
  }
  if (x < 0.0) {
    rt_errno = RT_EDOM;
    return NAN_;
  }
  if (x == 0.0) {
    rt_errno = RT_ERANGE;
    return -INF_;
  }
  if (ISINF(x)) {
    return x;
  }
  __asm__(X87("fldln2\n\tfxch\n\tfyl2x") : "=t"(r) : "0"(x));
  return r;
}

M log10(double x) {
  double r;

  if (ISNAN(x)) {
    return x;
  }
  if (x < 0.0) {
    rt_errno = RT_EDOM;
    return NAN_;
  }
  if (x == 0.0) {
    rt_errno = RT_ERANGE;
    return -INF_;
  }
  if (ISINF(x)) {
    return x;
  }
  __asm__(X87("fldlg2\n\tfxch\n\tfyl2x") : "=t"(r) : "0"(x));
  return r;
}

M log2(double x) {
  double r;

  if (ISNAN(x)) {
    return x;
  }
  if (x < 0.0) {
    rt_errno = RT_EDOM;
    return NAN_;
  }
  if (x == 0.0) {
    rt_errno = RT_ERANGE;
    return -INF_;
  }
  if (ISINF(x)) {
    return x;
  }
  __asm__(X87("fld1\n\tfxch\n\tfyl2x") : "=t"(r) : "0"(x));
  return r;
}

M expm1(double x) { return fabs(x) < 1e-5 ? x + x * x / 2.0 : exp(x) - 1.0; }
M log1p(double x) { return fabs(x) < 1e-5 ? x - x * x / 2.0 : log(1.0 + x); }

static int is_int(double y) { return fabs(y) >= 9007199254740992.0 || (double)(long long)y == y; }
static int is_odd(double y) { return fabs(y) < 9007199254740992.0 && (((long long)y) & 1) != 0; }

static double pow_pos(double x, double y) {
  double r;

  __asm__(X87("fyl2x\n\t" EXP2_SEQ) : "=t"(r) : "0"(x), "u"(y) : "st(1)");
  return r;
}

double rt_pow(double x, double y) {
  if (y == 0.0 || x == 1.0) {
    return 1.0;
  }
  if (ISNAN(x) || ISNAN(y)) {
    return NAN_;
  }
  if (x == 0.0) {
    int odd = is_odd(y) && signbit_(x);

    if (y < 0.0) {
      rt_errno = RT_ERANGE;
      return odd ? -INF_ : INF_;
    }
    return odd ? -0.0 : 0.0;
  }
  if (ISINF(y)) {
    double a = fabs(x);

    if (a == 1.0) {
      return 1.0;
    }
    return (a > 1.0) == (y > 0.0) ? INF_ : 0.0;
  }
  if (ISINF(x)) {
    int odd = is_odd(y);

    if (x > 0.0) {
      return y > 0.0 ? INF_ : 0.0;
    }
    if (y > 0.0) {
      return odd ? -INF_ : INF_;
    }
    return odd ? -0.0 : 0.0;
  }
  if (x < 0.0) {
    double r;

    if (!is_int(y)) {
      rt_errno = RT_EDOM;
      return NAN_;
    }
    r = pow_pos(-x, y);
    return is_odd(y) ? -r : r;
  }
  return pow_pos(x, y);
}
M pow(double x, double y) { return rt_pow(x, y); }

M hypot(double x, double y) {
  if (ISINF(x) || ISINF(y)) {
    return INF_;
  }
  x = fabs(x);
  y = fabs(y);
  if (x < y) {
    double t = x;

    x = y;
    y = t;
  }
  if (x == 0.0) {
    return 0.0;
  }
  y /= x;
  return x * sqrt(1.0 + y * y);
}
M _hypot(double x, double y) { return hypot(x, y); }

M cbrt(double x) {
  double a = fabs(x), r;

  if (a == 0.0 || ISINF(a) || ISNAN(a)) {
    return x;
  }
  r = pow_pos(a, 1.0 / 3.0);
  r = r - (r * r * r - a) / (3.0 * r * r);
  return copysign(r, x);
}

M sinh(double x) {
  double a = fabs(x), r;

  r = a < 0.5 ? (expm1(a) + expm1(a) / (1.0 + expm1(a))) / 2.0 : (exp(a) - 1.0 / exp(a)) / 2.0;
  return copysign(r, x);
}
M cosh(double x) { double e = exp(fabs(x)); return (e + 1.0 / e) / 2.0; }
M tanh(double x) {
  double a = fabs(x), r;

  if (a > 22.0) {
    return copysign(1.0, x);
  }
  r = expm1(2.0 * a);
  return copysign(r / (r + 2.0), x);
}

M fmin(double a, double b) { return ISNAN(a) ? b : (ISNAN(b) ? a : (a < b ? a : b)); }
M fmax(double a, double b) { return ISNAN(a) ? b : (ISNAN(b) ? a : (a > b ? a : b)); }
M fdim(double a, double b) { return a > b ? a - b : 0.0; }

EXPORT_SPECIAL int _finite(double x) { return (bitsd(x) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }
EXPORT_SPECIAL int _isnan(double x) { return ISNAN(x); }

EXPORT_SPECIAL short _dclass(double x) {
  uint64_t b = bitsd(x);
  uint64_t ex = (b >> 52) & 0x7FF, mant = b & 0xFFFFFFFFFFFFFull;

  if (ex == 0x7FF) {
    return mant != 0 ? 2 : 1;                      // FP_NAN, FP_INFINITE
  }
  if (ex == 0) {
    return mant != 0 ? -2 : 0;                     // FP_SUBNORMAL, FP_ZERO
  }
  return -1;                                       // FP_NORMAL
}
EXPORT_SPECIAL short _fdclass(float x) {
  flt_t b;
  uint32_t ex, mant;

  b.f = x;
  ex = (b.u >> 23) & 0xFF;
  mant = b.u & 0x7FFFFF;
  if (ex == 0xFF) {
    return mant != 0 ? 2 : 1;
  }
  if (ex == 0) {
    return mant != 0 ? -2 : 0;
  }
  return -1;
}
EXPORT_SPECIAL short _dtest(double *x) { return _dclass(*x); }
EXPORT_SPECIAL short _fdtest(float *x) { return _fdclass(*x); }
EXPORT_SPECIAL short _dsign(double x) { return (short)(bitsd(x) >> 63 ? 0x8000 : 0); }
EXPORT_SPECIAL short _fdsign(float x) { flt_t b; b.f = x; return (short)(b.u >> 31 ? 0x8000 : 0); }

EXPORT_SPECIAL int _controlfp_s(unsigned int *cur, unsigned int newval, unsigned int mask) {
  (void)newval;
  (void)mask;
  if (cur != 0) {
    *cur = 0x9001Fu;
  }
  return 0;
}
EXPORT_SPECIAL unsigned int _controlfp(unsigned int newval, unsigned int mask) { (void)newval; (void)mask; return 0x9001Fu; }
EXPORT_SPECIAL unsigned int _clearfp(void) { return 0; }
EXPORT_SPECIAL unsigned int _statusfp(void) { return 0; }
EXPORT_SPECIAL void _fpreset(void) { __asm__ volatile("fninit"); }

// ---- float versions --------------------------------------------------------------------------------------

#define F1(name) MF name##f(float x) { return (float)name((double)x); }
#define F2(name) MF name##f(float x, float y) { return (float)name((double)x, (double)y); }
F1(sin) F1(cos) F1(tan) F1(asin) F1(acos) F1(atan) F1(sinh) F1(cosh) F1(tanh) F1(exp) F1(exp2) F1(expm1)
F1(log) F1(log10) F1(log2) F1(log1p) F1(sqrt) F1(fabs) F1(floor) F1(ceil) F1(round) F1(trunc) F1(rint) F1(cbrt)
F2(atan2) F2(pow) F2(fmod) F2(hypot) F2(fmin) F2(fmax) F2(copysign)
MF ldexpf(float x, int n) { return (float)ldexp((double)x, n); }
MF frexpf(float x, int *e) { return (float)frexp((double)x, e); }
MF modff(float x, float *ip) { double i; double f = modf((double)x, &i); *ip = (float)i; return (float)f; }

// ---- stdlib integer helpers -------------------------------------------------------------------------------

EXPORT_SPECIAL int abs(int x) { return x < 0 ? -x : x; }
EXPORT_SPECIAL int32_t labs(int32_t x) { return x < 0 ? -x : x; }
EXPORT_SPECIAL long long llabs(long long x) { return x < 0 ? -x : x; }
EXPORT_SPECIAL long long _abs64(long long x) { return x < 0 ? -x : x; }

typedef struct { int quot, rem; } div_t_;
typedef struct { int32_t quot, rem; } ldiv_t_;
typedef struct { long long quot, rem; } lldiv_t_;
EXPORT_SPECIAL div_t_ div(int a, int b) { div_t_ r; r.quot = a / b; r.rem = a % b; return r; }
EXPORT_SPECIAL ldiv_t_ ldiv(int32_t a, int32_t b) { ldiv_t_ r; r.quot = a / b; r.rem = a % b; return r; }
EXPORT_SPECIAL lldiv_t_ lldiv(long long a, long long b) { lldiv_t_ r; r.quot = a / b; r.rem = a % b; return r; }
