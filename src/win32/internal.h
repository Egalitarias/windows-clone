#ifndef WIN32RT_INTERNAL_H
#define WIN32RT_INTERNAL_H

// The Win32 runtime ("WIN32.DLL"): kernel32, user32 and gdi32 in one PE DLL, built from the
// real Windows SDK headers so every function has exactly the SDK's prototype. The kernel
// loader maps it into every PE program and binds the program's imports from KERNEL32.dll,
// USER32.dll, GDI32.dll, ... to its exports. It runs in user mode and talks to the kernel
// through WinClone's own syscalls (src/kernel/syscall.h), not through any Windows system
// call interface.

// Every function the SDK declares with one of these macros is exported from this DLL (when
// it is defined here); the macros are normally dllimport.
#define WINBASEAPI __declspec(dllexport)
#define WINUSERAPI __declspec(dllexport)
#define WINGDIAPI __declspec(dllexport)
#define WINADVAPI __declspec(dllexport)
#define SHSTDAPI_(t) __declspec(dllexport) t __stdcall
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#define EXPORT
#define EXPORT_SPECIAL __declspec(dllexport)

typedef long long sl_t;               // "long" is 32 bits in the Windows ABI: syscalls use this

// ---- WinClone syscalls (keep in sync with src/kernel/syscall.h) ----
#define KS_EXIT        0
#define KS_WRITE       1
#define KS_YIELD       2
#define KS_GETPID      3
#define KS_READ        4
#define KS_SLEEP       5
#define KS_OPEN        6
#define KS_CLOSE       7
#define KS_READDIR     8
#define KS_WIN_CREATE  9
#define KS_WIN_CLOSE   10
#define KS_WIN_BLIT    11
#define KS_WIN_FILL    12
#define KS_WIN_TEXT    13
#define KS_WIN_EVENT   14
#define KS_GETARG      15
#define KS_WIN_TITLE   16
#define KS_LSEEK       17
#define KS_STAT        18
#define KS_UNLINK      19
#define KS_MKDIR       20
#define KS_WIN_WAIT    21
#define KS_WIN_MOVE    22
#define KS_WIN_RESIZE  23
#define KS_CLIP_SET    24
#define KS_CLIP_GET    25
#define KS_UPTIME      29
#define KS_SCREEN      30
#define KS_NOTIFY      31
#define KS_PEINFO      32

#define KS_O_RDONLY 0
#define KS_O_WRONLY 1
#define KS_O_RDWR   2

static inline sl_t sc3(sl_t nr, sl_t a1, sl_t a2, sl_t a3) {
  sl_t r;

  __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
  return r;
}

// Layouts shared with the kernel.
typedef struct {
  uint32_t size;
  uint32_t attr;
  char name[128];
} sc_dirent_t;

typedef struct {
  int32_t x, y, w, h;
  uint32_t color;
  uint32_t scale;
  uint64_t ptr;
} sc_winop_t;

typedef struct {
  uint32_t type;
  int32_t x, y;
  uint32_t ch;
} sc_winev_t;

#define SCEV_KEY         1
#define SCEV_MOUSE_DOWN  2
#define SCEV_MOUSE_MOVE  3
#define SCEV_MOUSE_UP    4
#define SCEV_CLOSE       5
#define SCEV_MOUSE_DOUBLE 6
#define SCEV_RESIZE      7
#define SCEV_WHEEL       8

#define SCKEY_LEFT   0x1C
#define SCKEY_RIGHT  0x1D
#define SCKEY_UP     0x1E
#define SCKEY_DOWN   0x1F
#define SCKEY_DELETE 0x7F
#define SCKEY_HOME   0x80
#define SCKEY_END    0x81
#define SCKEY_PGUP   0x82
#define SCKEY_PGDN   0x83

// ---- shared helpers (kernel32.c) ----
void *rt_memcpy(void *d, const void *s, size_t n);
void *rt_memset(void *d, int c, size_t n);
size_t rt_strlen(const char *s);
int rt_strcmp(const char *a, const char *b);
int rt_stricmp(const char *a, const char *b);
void rt_copy(char *dst, const char *src, size_t max);
void *rt_alloc(size_t n);
void rt_free(void *p);
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
void rt_log(const char *s);
uint64_t rt_dll_base(void);

// ---- user32.c / gdi32.c ----
// A device context: a window's surface (sysid) plus the drawing state.
typedef struct {
  int used;
  int sysid;
  COLORREF text, bk;
  int bkmode, align;
  HGDIOBJ pen, brush, font;
  int px, py;                    // current position (MoveToEx / LineTo)
} dc_t;

dc_t *gdi_dc_alloc(int sysid);
void gdi_dc_free(dc_t *dc);
dc_t *gdi_dc(HDC h);
COLORREF gdi_sys_color(int index);
HBRUSH gdi_sys_brush(int index);
unsigned long rt_colorref_to_rgb(COLORREF c);

#endif
