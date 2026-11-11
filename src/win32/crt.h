#ifndef WIN32RT_CRT_H
#define WIN32RT_CRT_H

// The C runtime part of the Win32 runtime ("ucrtbase" and "vcruntime140" in one: crt.c, crtfmt.c,
// crtmath.c). Programs built with the dynamic CRT (/MD) import api-ms-win-crt-*.dll,
// VCRUNTIME140.dll and KERNEL32.dll; the kernel loader binds all of them to WIN32.DLL.
// The UCRT headers windows.h pulls in declare these functions dllimport, which cannot be
// redeclared as definitions: the macros below turn that into dllexport first. Functions the
// headers do not declare (math, stdio) are defined here with their own prototypes.
// windows.h pulls in some UCRT headers; what they declare dllimport this DLL defines (and exports).
#define _ACRTIMP __declspec(dllexport)
#define _ACRTIMP_ALT __declspec(dllexport)
#define _VCRTIMP __declspec(dllexport)
#define _CRTIMP __declspec(dllexport)
#include "internal.h"

extern int rt_errno;

#define RT_EDOM   33
#define RT_ERANGE 34
#define RT_ENOENT 2
#define RT_EINVAL 22
#define RT_ENOMEM 12
#define RT_EBADF  9

static inline int rt_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static inline int rt_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int rt_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int rt_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int rt_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// printf engine: formats into pieces handed to `put`. `wide` means the format string is a wide
// one narrowed to bytes (so %s is a wide string and %S a narrow one). Returns the length.
typedef void (*fmt_put_fn)(void *ctx, const char *s, size_t n);
int rt_format(fmt_put_fn put, void *ctx, const char *fmt, va_list ap, int wide);

// scanf engine over a character source.
typedef struct {
  int (*get)(void *ctx);                // next character or -1
  void (*unget)(void *ctx, int c);
  void *ctx;
} scan_src_t;
int rt_scan(scan_src_t *src, const char *fmt, va_list ap);

double rt_strtod(const char *s, char **end);
unsigned long long rt_strtoull(const char *s, char **end, int base, int *neg, int *overflow);

// math helpers shared with the formatter
double rt_pow(double x, double y);

#endif
