// kernel32 for the Win32 runtime: process, console, files, memory, time, a few string helpers.
// Single-threaded: there is one thread per process, so locks are no-ops and there is no real
// CreateThread. See internal.h.
#include "internal.h"

// ---- Compiler support (the compiler may call these even in freestanding code) -------------
#pragma clang optimize off
void *memcpy(void *d, const void *s, size_t n) {
  void *r = d;

  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return r;
}

void *memmove(void *d, const void *s, size_t n) {
  unsigned char *dd = (unsigned char *)d;
  const unsigned char *ss = (const unsigned char *)s;

  if (dd <= ss || dd >= ss + n) {
    return memcpy(d, s, n);
  }
  while (n != 0) {
    --n;
    dd[n] = ss[n];
  }
  return d;
}

void *memset(void *d, int c, size_t n) {
  void *r = d;

  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return r;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  size_t i;

  for (i = 0; i < n; ++i) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }
  return 0;
}
#pragma clang optimize on

// Stack probes are not needed: the whole stack is mapped up front.
__asm__(".text\n.globl __chkstk\n__chkstk:\n\tret\n");

void *rt_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *rt_memset(void *d, int c, size_t n) { return memset(d, c, n); }

size_t rt_strlen(const char *s) {
  size_t n = 0;

  while (s[n] != 0) {
    ++n;
  }
  return n;
}

int rt_strcmp(const char *a, const char *b) {
  while (*a != 0 && *a == *b) {
    ++a;
    ++b;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int rt_stricmp(const char *a, const char *b) {
  while (*a != 0 && lower(*a) == lower(*b)) {
    ++a;
    ++b;
  }
  return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

void rt_copy(char *dst, const char *src, size_t max) {
  size_t i = 0;

  if (max == 0) {
    return;
  }
  while (src[i] != 0 && i + 1 < max) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = 0;
}

void rt_log(const char *s) { sc3(KS_WRITE, 2, (sl_t)s, (sl_t)rt_strlen(s)); }

// ---- Process info from the kernel ------------------------------------------------------------

static uint64_t g_info[4];
static int g_info_ok;

static void info_init(void) {
  if (!g_info_ok) {
    sc3(KS_PEINFO, (sl_t)g_info, 0, 0);
    g_info_ok = 1;
  }
}

uint64_t rt_dll_base(void) {
  info_init();
  return g_info[3];
}

uint64_t rt_exe_base(void) {
  info_init();
  return g_info[2];
}

// ---- Heap ------------------------------------------------------------------------------------
// One block of memory from the kernel (PE_HEAP_BASE), carved with a first-fit allocator. Every
// block has a 16-byte header: its total size, with bit 0 set while in use.

typedef struct {
  uint64_t size;
  uint64_t pad;
} hdr_t;

#define HDR 16u
static uint8_t *g_heap, *g_heap_top, *g_heap_end;

static void heap_init(void) {
  if (g_heap == 0) {
    info_init();
    g_heap = (uint8_t *)(uintptr_t)g_info[0];
    g_heap_top = g_heap;
    g_heap_end = g_heap + g_info[1];
  }
}

static rtlock_t g_heap_lock;

static void *rt_alloc_locked(size_t n) {
  uint64_t need;
  uint8_t *p;

  heap_init();
  need = ((uint64_t)n + HDR + 15u) & ~15ull;
  if (need < 32) {
    need = 32;
  }
  for (p = g_heap; p < g_heap_top;) {
    hdr_t *h = (hdr_t *)p;

    if ((h->size & 1u) == 0) {
      // Merge with the free blocks that follow.
      while (p + h->size < g_heap_top && (((hdr_t *)(p + h->size))->size & 1u) == 0) {
        h->size += ((hdr_t *)(p + h->size))->size;
      }
      if (h->size >= need) {
        if (h->size >= need + 32) {                  // split
          hdr_t *rest = (hdr_t *)(p + need);

          rest->size = h->size - need;
          h->size = need;
        }
        h->size |= 1u;
        return p + HDR;
      }
    }
    p += h->size & ~1ull;
  }
  if ((uint64_t)(g_heap_end - g_heap_top) < need) {
    return 0;
  }
  ((hdr_t *)g_heap_top)->size = need | 1u;
  p = g_heap_top + HDR;
  g_heap_top += need;
  return p;
}

void *rt_alloc(size_t n) {
  void *p;

  rt_lock(&g_heap_lock);
  p = rt_alloc_locked(n);
  rt_unlock(&g_heap_lock);
  return p;
}

static int heap_owns(const void *p) {
  heap_init();
  return (const uint8_t *)p >= g_heap + HDR && (const uint8_t *)p < g_heap_top && (((uintptr_t)p) & 15u) == 0;
}

void rt_free(void *p) {
  if (p != 0 && heap_owns(p)) {
    rt_lock(&g_heap_lock);
    ((hdr_t *)((uint8_t *)p - HDR))->size &= ~1ull;
    rt_unlock(&g_heap_lock);
  }
}

static size_t block_usable(const void *p) {
  return (size_t)((((const hdr_t *)((const uint8_t *)p - HDR))->size & ~1ull) - HDR);
}

EXPORT HANDLE WINAPI GetProcessHeap(void) { return (HANDLE)(uintptr_t)0x1000; }
EXPORT HANDLE WINAPI HeapCreate(DWORD o, SIZE_T i, SIZE_T m) { (void)o; (void)i; (void)m; return GetProcessHeap(); }
EXPORT BOOL WINAPI HeapDestroy(HANDLE h) { (void)h; return TRUE; }

EXPORT LPVOID WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) {
  void *p = rt_alloc(n);

  (void)h;
  if (p != 0 && (flags & HEAP_ZERO_MEMORY)) {
    memset(p, 0, n);
  }
  if (p == 0) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  }
  return p;
}

EXPORT BOOL WINAPI HeapFree(HANDLE h, DWORD flags, LPVOID p) {
  (void)h;
  (void)flags;
  rt_free(p);
  return TRUE;
}

EXPORT LPVOID WINAPI HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n) {
  void *q;
  size_t old;

  if (p == 0) {
    return HeapAlloc(h, flags, n);
  }
  old = block_usable(p);
  if (n <= old) {
    return p;
  }
  q = HeapAlloc(h, flags, n);
  if (q != 0) {
    memcpy(q, p, old);
    rt_free(p);
  }
  return q;
}

EXPORT SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, LPCVOID p) {
  (void)h;
  (void)flags;
  return p != 0 && heap_owns(p) ? block_usable(p) : (SIZE_T)-1;
}

EXPORT BOOL WINAPI HeapValidate(HANDLE h, DWORD flags, LPCVOID p) { (void)h; (void)flags; return p == 0 || heap_owns(p); }

// VirtualAlloc: page-aligned blocks out of the same heap; the original pointer sits just
// before the returned address.
EXPORT LPVOID WINAPI VirtualAlloc(LPVOID where, SIZE_T size, DWORD type, DWORD prot) {
  uint8_t *raw, *aligned;

  (void)type;
  (void)prot;
  if (where != 0 || size == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  size = (size + 4095u) & ~(SIZE_T)4095u;
  raw = (uint8_t *)rt_alloc(size + 4096u + 16u);
  if (raw == 0) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  aligned = (uint8_t *)(((uintptr_t)raw + 16u + 4095u) & ~(uintptr_t)4095u);
  memset(aligned, 0, size);
  *(uint8_t **)(aligned - 8) = raw;
  return aligned;
}

EXPORT BOOL WINAPI VirtualFree(LPVOID p, SIZE_T size, DWORD type) {
  (void)size;
  if (p == 0 || (type & MEM_RELEASE) == 0) {
    return p != 0;                                    // MEM_DECOMMIT: nothing to do
  }
  rt_free(*(uint8_t **)((uint8_t *)p - 8));
  return TRUE;
}

EXPORT BOOL WINAPI VirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) {
  (void)p;
  (void)n;
  (void)prot;
  if (old != 0) {
    *old = PAGE_READWRITE;
  }
  return TRUE;
}

EXPORT HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T n) { return HeapAlloc(0, (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, n); }
EXPORT HLOCAL WINAPI LocalFree(HLOCAL p) { rt_free(p); return 0; }
EXPORT HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T n) { return HeapAlloc(0, (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, n); }
EXPORT HGLOBAL WINAPI GlobalFree(HGLOBAL p) { rt_free(p); return 0; }
EXPORT LPVOID WINAPI GlobalLock(HGLOBAL p) { return p; }
EXPORT BOOL WINAPI GlobalUnlock(HGLOBAL p) { (void)p; return TRUE; }

// ---- Last error ------------------------------------------------------------------------------

// The last error lives in the thread block (TEB offset 0x68): one per thread.
EXPORT DWORD WINAPI GetLastError(void) { return *(DWORD *)(rt_teb() + 0x68); }
EXPORT void WINAPI SetLastError(DWORD e) { *(DWORD *)(rt_teb() + 0x68) = e; }

static DWORD map_error(sl_t e) {
  switch (e) {
    case -2: return ERROR_FILE_NOT_FOUND;
    case -13: return ERROR_ACCESS_DENIED;
    case -9: return ERROR_INVALID_HANDLE;
    case -12: return ERROR_NOT_ENOUGH_MEMORY;
    case -17: return ERROR_ALREADY_EXISTS;
    case -39: return ERROR_DIR_NOT_EMPTY;
    case -20: return ERROR_DIRECTORY;
    case -28: return ERROR_DISK_FULL;
    case -22: return ERROR_INVALID_PARAMETER;
    case -24: return ERROR_TOO_MANY_OPEN_FILES;
    default: return ERROR_GEN_FAILURE;
  }
}

// ---- Process -----------------------------------------------------------------------------------

EXPORT DECLSPEC_NORETURN VOID WINAPI ExitProcess(UINT code) {
  sc3(KS_EXIT, (sl_t)code, 0, 0);
  for (;;) {
  }
}

EXPORT BOOL WINAPI TerminateProcess(HANDLE h, UINT code) { (void)h; ExitProcess(code); return FALSE; }
EXPORT HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
EXPORT DWORD WINAPI GetCurrentProcessId(void) { return (DWORD)sc3(KS_GETPID, 0, 0, 0); }

// Reached when the program's entry point returns: the loader put its address on the stack.
EXPORT_SPECIAL __attribute__((naked)) void __winclone_return(void) {
  __asm__ volatile("movl %eax, %ecx\n\tsubq $32, %rsp\n\tcall ExitProcess");
}

// An import the runtime does not have is bound here.
EXPORT_SPECIAL DECLSPEC_NORETURN void __winclone_unimplemented(void) {
  rt_log("\r\nThis program called a Win32 function that WinClone does not provide.\r\n");
  ExitProcess(0xC0000139u);
}

static char g_cmdline[160];
static char g_exename[] = "C:\\APPS\\PROGRAM.EXE";

static void cmdline_init(void) {
  char arg[96];
  sl_t n;

  if (g_cmdline[0] != 0) {
    return;
  }
  n = sc3(KS_GETARG, (sl_t)arg, (sl_t)sizeof(arg), 0);
  rt_copy(g_cmdline, g_exename, sizeof(g_cmdline));
  if (n > 0) {
    size_t len = rt_strlen(g_cmdline);

    g_cmdline[len++] = ' ';
    rt_copy(g_cmdline + len, arg, sizeof(g_cmdline) - len);
  }
}

EXPORT LPSTR WINAPI GetCommandLineA(void) { cmdline_init(); return g_cmdline; }

static WCHAR g_cmdline_w[160];

EXPORT LPWSTR WINAPI GetCommandLineW(void) {
  size_t i;

  cmdline_init();
  for (i = 0; g_cmdline[i] != 0 && i + 1 < 160; ++i) {
    g_cmdline_w[i] = (unsigned char)g_cmdline[i];
  }
  g_cmdline_w[i] = 0;
  return g_cmdline_w;
}

EXPORT DWORD WINAPI GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
  (void)m;
  rt_copy(buf, g_exename, n);
  return (DWORD)rt_strlen(buf);
}

EXPORT HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
  info_init();
  if (name == 0) {
    return (HMODULE)(uintptr_t)g_info[2];
  }
  if (rt_stricmp(name, "kernel32") == 0 || rt_stricmp(name, "kernel32.dll") == 0 || rt_stricmp(name, "user32") == 0 ||
      rt_stricmp(name, "user32.dll") == 0 || rt_stricmp(name, "gdi32") == 0 || rt_stricmp(name, "gdi32.dll") == 0 ||
      rt_stricmp(name, "win32.dll") == 0) {
    return (HMODULE)(uintptr_t)g_info[3];
  }
  SetLastError(ERROR_MOD_NOT_FOUND);
  return 0;
}

EXPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name) {
  char a[64];
  size_t i;

  if (name == 0) {
    return GetModuleHandleA(0);
  }
  for (i = 0; name[i] != 0 && i + 1 < sizeof(a); ++i) {
    a[i] = (char)name[i];
  }
  a[i] = 0;
  return GetModuleHandleA(a);
}

EXPORT HMODULE WINAPI LoadLibraryA(LPCSTR name) { return GetModuleHandleA(name); }
EXPORT BOOL WINAPI FreeLibrary(HMODULE m) { (void)m; return TRUE; }

EXPORT FARPROC WINAPI GetProcAddress(HMODULE mod, LPCSTR name) {
  const uint8_t *base;
  uint32_t lfanew, exp_rva, i, nnames;
  const uint8_t *dir;

  info_init();
  if ((uintptr_t)mod != g_info[3] || ((uintptr_t)name >> 16) == 0) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
  }
  base = (const uint8_t *)(uintptr_t)g_info[3];
  lfanew = *(const uint32_t *)(base + 0x3C);
  exp_rva = *(const uint32_t *)(base + lfanew + 24 + 112);
  dir = base + exp_rva;
  nnames = *(const uint32_t *)(dir + 24);
  for (i = 0; i < nnames; ++i) {
    const char *n = (const char *)(base + ((const uint32_t *)(base + *(const uint32_t *)(dir + 32)))[i]);

    if (rt_strcmp(n, name) == 0) {
      uint32_t ord = ((const uint16_t *)(base + *(const uint32_t *)(dir + 36)))[i];

      return (FARPROC)(uintptr_t)(base + ((const uint32_t *)(base + *(const uint32_t *)(dir + 28)))[ord]);
    }
  }
  SetLastError(ERROR_PROC_NOT_FOUND);
  return 0;
}

EXPORT VOID WINAPI GetStartupInfoA(LPSTARTUPINFOA si) {
  memset(si, 0, sizeof(*si));
  si->cb = sizeof(*si);
}

EXPORT VOID WINAPI GetStartupInfoW(LPSTARTUPINFOW si) {
  memset(si, 0, sizeof(*si));
  si->cb = sizeof(*si);
}

EXPORT DWORD WINAPI GetEnvironmentVariableA(LPCSTR n, LPSTR b, DWORD s) { (void)n; (void)b; (void)s; SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
EXPORT BOOL WINAPI SetEnvironmentVariableA(LPCSTR n, LPCSTR v) { (void)n; (void)v; return TRUE; }
static char g_env[4];
EXPORT LPCH WINAPI GetEnvironmentStringsA(void) { return g_env; }
EXPORT BOOL WINAPI FreeEnvironmentStringsA(LPCH p) { (void)p; return TRUE; }
EXPORT BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }
EXPORT LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) { (void)f; return 0; }
EXPORT UINT WINAPI SetErrorMode(UINT m) { (void)m; return 0; }
EXPORT VOID WINAPI OutputDebugStringA(LPCSTR s) { rt_log(s); }
EXPORT DWORD WINAPI GetVersion(void) { return 0x4A650006u; }                 // 10.0, build 19045

EXPORT BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA v) {
  v->dwMajorVersion = 10;
  v->dwMinorVersion = 0;
  v->dwBuildNumber = 19045;
  v->dwPlatformId = VER_PLATFORM_WIN32_NT;
  v->szCSDVersion[0] = 0;
  return TRUE;
}

EXPORT VOID WINAPI GetSystemInfo(LPSYSTEM_INFO si) {
  memset(si, 0, sizeof(*si));
  si->dwPageSize = 4096;
  si->dwNumberOfProcessors = 1;
  si->dwAllocationGranularity = 4096;
  si->lpMinimumApplicationAddress = (LPVOID)0x10000;
  si->lpMaximumApplicationAddress = (LPVOID)0x00007FFFFFFEFFFFull;
  si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
}

// Locks, thread-local storage, threads and waiting: see threads.c.

// ---- Time ----------------------------------------------------------------------------------------
// There is no wall clock for programs yet: time counts from 2026-01-01 00:00 at boot.

EXPORT DWORD WINAPI GetTickCount(void) { return (DWORD)sc3(KS_UPTIME, 0, 0, 0); }
EXPORT ULONGLONG WINAPI GetTickCount64(void) { return (ULONGLONG)sc3(KS_UPTIME, 0, 0, 0); }

EXPORT BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000; return TRUE; }
EXPORT BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = (LONGLONG)sc3(KS_UPTIME, 0, 0, 0) * 1000; return TRUE; }

EXPORT VOID WINAPI GetSystemTimeAsFileTime(LPFILETIME ft) {
  ULONGLONG t = 155228ULL * 86400ULL * 10000000ULL + (ULONGLONG)sc3(KS_UPTIME, 0, 0, 0) * 10000ULL;

  ft->dwLowDateTime = (DWORD)t;
  ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void filetime_to_systemtime(ULONGLONG t, LPSYSTEMTIME st) {
  ULONGLONG secs = t / 10000000ULL, days = secs / 86400ULL;
  long long z = (long long)days - 135140LL + 719468LL;          // days since 1601-01-01 -> since 0000-03-01
  long long era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
  long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
  long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  long long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;

  st->wYear = (WORD)(m <= 2 ? y + 1 : y);
  st->wMonth = (WORD)m;
  st->wDay = (WORD)d;
  st->wDayOfWeek = (WORD)((days + 1) % 7);                       // 1601-01-01 was a Monday
  st->wHour = (WORD)(secs % 86400ULL / 3600ULL);
  st->wMinute = (WORD)(secs % 3600ULL / 60ULL);
  st->wSecond = (WORD)(secs % 60ULL);
  st->wMilliseconds = (WORD)(t / 10000ULL % 1000ULL);
}

EXPORT VOID WINAPI GetSystemTime(LPSYSTEMTIME st) {
  FILETIME ft;

  GetSystemTimeAsFileTime(&ft);
  filetime_to_systemtime(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, st);
}

EXPORT VOID WINAPI GetLocalTime(LPSYSTEMTIME st) { GetSystemTime(st); }

// ---- Strings ------------------------------------------------------------------------------------

EXPORT int WINAPI lstrlenA(LPCSTR s) { return s != 0 ? (int)rt_strlen(s) : 0; }
EXPORT int WINAPI lstrlenW(LPCWSTR s) { int n = 0; while (s != 0 && s[n] != 0) { ++n; } return n; }
EXPORT LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s) { rt_copy(d, s, (size_t)-1); return d; }
EXPORT LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n) { rt_copy(d, s, (size_t)n); return d; }
EXPORT LPSTR WINAPI lstrcatA(LPSTR d, LPCSTR s) { rt_copy(d + rt_strlen(d), s, (size_t)-1); return d; }
EXPORT int WINAPI lstrcmpA(LPCSTR a, LPCSTR b) { return rt_strcmp(a, b); }
EXPORT int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b) { return rt_stricmp(a, b); }

EXPORT int WINAPI MultiByteToWideChar(UINT cp, DWORD fl, LPCCH s, int n, LPWSTR w, int wn) {
  int i, len = n < 0 ? (int)rt_strlen(s) + 1 : n;

  (void)cp;
  (void)fl;
  if (wn == 0) {
    return len;
  }
  for (i = 0; i < len && i < wn; ++i) {
    w[i] = (unsigned char)s[i];
  }
  return i;
}

EXPORT int WINAPI WideCharToMultiByte(UINT cp, DWORD fl, LPCWCH w, int n, LPSTR s, int sn, LPCCH def, LPBOOL used) {
  int i, len = n;

  (void)cp;
  (void)fl;
  (void)def;
  if (used != 0) {
    *used = FALSE;
  }
  if (len < 0) {
    len = 0;
    while (w[len] != 0) {
      ++len;
    }
    ++len;
  }
  if (sn == 0) {
    return len;
  }
  for (i = 0; i < len && i < sn; ++i) {
    s[i] = w[i] < 0x80 ? (char)w[i] : '?';
  }
  return i;
}

// A small vsnprintf: %d %i %u %x %X %c %s %p %% with flags - and 0, width, .precision, and the
// l, ll, h, I64 size prefixes.
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  size_t n = 0;

#define PUT(c) do { if (n + 1 < size) { buf[n] = (char)(c); } ++n; } while (0)
  while (*fmt != 0) {
    int left = 0, zero = 0, width = 0, prec = -1, longs = 0, neg = 0, base = 10, upper = 0, isnum = 0;
    char tmp[24], *s;
    int len = 0, i;
    unsigned long long v = 0;

    if (*fmt != '%') {
      PUT(*fmt++);
      continue;
    }
    ++fmt;
    for (;; ++fmt) {
      if (*fmt == '-') left = 1;
      else if (*fmt == '0') zero = 1;
      else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') continue;
      else break;
    }
    if (*fmt == '*') { width = va_arg(ap, int); ++fmt; if (width < 0) { left = 1; width = -width; } }
    else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      ++fmt;
      prec = 0;
      if (*fmt == '*') { prec = va_arg(ap, int); ++fmt; }
      else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
    }
    for (;;) {
      if (*fmt == 'l') { ++longs; ++fmt; }
      else if (*fmt == 'h') ++fmt;
      else if (*fmt == 'I' && fmt[1] == '6' && fmt[2] == '4') { longs = 2; fmt += 3; }
      else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { longs = 2; ++fmt; }
      else break;
    }
    switch (*fmt) {
      case 'd': case 'i': {
        long long x = longs >= 2 ? va_arg(ap, long long) : (longs == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int));

        neg = x < 0;
        v = neg ? (unsigned long long)(-x) : (unsigned long long)x;
        isnum = 1;
        break;
      }
      case 'u': case 'x': case 'X': case 'p':
        v = longs >= 2 || *fmt == 'p' ? va_arg(ap, unsigned long long) : (longs == 1 ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned int));
        base = *fmt == 'u' ? 10 : 16;
        upper = *fmt == 'X';
        isnum = 1;
        break;
      case 'c':
        tmp[0] = (char)va_arg(ap, int);
        s = tmp;
        len = 1;
        goto emit_str;
      case 's':
        s = va_arg(ap, char *);
        if (s == 0) s = "(null)";
        len = 0;
        while (s[len] != 0 && (prec < 0 || len < prec)) ++len;
        goto emit_str;
      case '%':
        PUT('%');
        ++fmt;
        continue;
      default:
        PUT('%');
        if (*fmt != 0) PUT(*fmt);
        else --fmt;
        ++fmt;
        continue;
    }
    if (isnum) {
      char digits[24];
      int nd = 0;

      do {
        int dgt = (int)(v % (unsigned)base);

        digits[nd++] = (char)(dgt < 10 ? '0' + dgt : (upper ? 'A' : 'a') + dgt - 10);
        v /= (unsigned)base;
      } while (v != 0);
      i = 0;
      if (neg) tmp[i++] = '-';
      while (nd > 0 && i < 23) tmp[i++] = digits[--nd];
      len = i;
      s = tmp;
      if (zero && !left && width > len) {                  // zero padding goes after the sign
        int pad = width - len, sign = tmp[0] == '-';

        if (sign) PUT('-');
        while (pad-- > 0) PUT('0');
        for (i = sign; i < len; ++i) PUT(tmp[i]);
        ++fmt;
        continue;
      }
    }
  emit_str:
    if (!left) for (i = len; i < width; ++i) PUT(' ');
    for (i = 0; i < len; ++i) PUT(s[i]);
    if (left) for (i = len; i < width; ++i) PUT(' ');
    ++fmt;
  }
  if (size > 0) {
    buf[n < size ? n : size - 1] = 0;
  }
  return (int)n;
#undef PUT
}

// ---- Handles, console and files -------------------------------------------------------------------

typedef struct {
  int kind;
  int fd;
  void *obj;                   // H_THREAD, H_EVENT, ...: the object (threads.c)
  char pattern[64];            // H_FIND: the file name pattern
} hent_t;

static rtlock_t g_h_lock;

#define MAX_HANDLES 64
static hent_t g_h[MAX_HANDLES];
static int g_h_init;

static HANDLE h_make(int i) { return (HANDLE)(uintptr_t)(0x100u + 4u * (unsigned)i); }

static void h_setup(void) {
  if (!g_h_init) {
    g_h_init = 1;
    g_h[0].kind = H_CONIN;
    g_h[0].fd = 0;
    g_h[1].kind = H_CONOUT;
    g_h[1].fd = 1;
    g_h[2].kind = H_CONERR;
    g_h[2].fd = 2;
  }
}

static hent_t *h_get(HANDLE h) {
  uintptr_t v = (uintptr_t)h;

  h_setup();
  if (v < 0x100 || ((v - 0x100) & 3u) != 0 || (v - 0x100) / 4u >= MAX_HANDLES) {
    return 0;
  }
  return g_h[(v - 0x100) / 4u].kind != H_FREE ? &g_h[(v - 0x100) / 4u] : 0;
}

static HANDLE h_alloc(int kind, int fd) {
  int i;

  h_setup();
  rt_lock(&g_h_lock);
  for (i = 3; i < MAX_HANDLES; ++i) {
    if (g_h[i].kind == H_FREE) {
      g_h[i].fd = fd;
      g_h[i].obj = 0;
      g_h[i].kind = kind;
      rt_unlock(&g_h_lock);
      return h_make(i);
    }
  }
  rt_unlock(&g_h_lock);
  SetLastError(ERROR_TOO_MANY_OPEN_FILES);
  return INVALID_HANDLE_VALUE;
}

HANDLE rt_handle_new(int kind, void *obj) {
  HANDLE h = h_alloc(kind, -1);

  if (h != INVALID_HANDLE_VALUE) {
    h_get(h)->obj = obj;
  }
  return h;
}

void *rt_handle_obj(HANDLE h, int *kind) {
  hent_t *e = h_get(h);

  if (e == 0) {
    return 0;
  }
  if (kind != 0) {
    *kind = e->kind;
  }
  return e->obj;
}

void rt_handle_free(HANDLE h) {
  hent_t *e = h_get(h);

  if (e != 0) {
    e->kind = H_FREE;
  }
}

EXPORT HANDLE WINAPI GetStdHandle(DWORD which) {
  h_setup();
  switch (which) {
    case STD_INPUT_HANDLE: return h_make(0);
    case STD_OUTPUT_HANDLE: return h_make(1);
    case STD_ERROR_HANDLE: return h_make(2);
    default: return INVALID_HANDLE_VALUE;
  }
}

EXPORT BOOL WINAPI SetStdHandle(DWORD which, HANDLE h) { (void)which; (void)h; return TRUE; }

// "C:\DIR\FILE.TXT" -> "/DIR/FILE.TXT" (the default volume); relative names are taken from "/".
static int to_path(LPCSTR win, char *out, size_t max) {
  size_t i = 0, o = 0;

  if (win == 0 || max < 3) {
    return 0;
  }
  if (win[0] != 0 && win[1] == ':') {
    i = 2;
  }
  if (win[i] != '\\' && win[i] != '/') {
    out[o++] = '/';
  }
  for (; win[i] != 0; ++i) {
    if (o + 1 >= max) {
      return 0;
    }
    out[o++] = win[i] == '\\' ? '/' : win[i];
  }
  out[o] = 0;
  return 1;
}

EXPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov) {
  hent_t *e = h_get(h);
  sl_t r;

  (void)ov;
  if (e == 0 || e->kind == H_CONIN || e->kind == H_FIND) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  r = sc3(KS_WRITE, e->fd, (sl_t)buf, (sl_t)n);
  if (r < 0) {
    SetLastError(map_error(r));
    return FALSE;
  }
  if (written != 0) {
    *written = (DWORD)r;
  }
  return TRUE;
}

EXPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
  hent_t *e = h_get(h);
  sl_t r;

  (void)ov;
  if (e == 0 || e->kind == H_CONOUT || e->kind == H_CONERR || e->kind == H_FIND) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  r = sc3(KS_READ, e->fd, (sl_t)buf, (sl_t)n);
  if (r < 0) {
    SetLastError(map_error(r));
    return FALSE;
  }
  if (got != 0) {
    *got = (DWORD)r;
  }
  return TRUE;
}

EXPORT BOOL WINAPI WriteConsoleA(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID res) { (void)res; return WriteFile(h, buf, n, written, 0); }
EXPORT BOOL WINAPI ReadConsoleA(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, PCONSOLE_READCONSOLE_CONTROL ctl) { (void)ctl; return ReadFile(h, buf, n, got, 0); }

EXPORT BOOL WINAPI WriteConsoleW(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID res) {
  char tmp[256];
  const WCHAR *w = (const WCHAR *)buf;
  DWORD done = 0;

  (void)res;
  while (done < n) {
    DWORD k = 0;

    while (done + k < n && k < sizeof(tmp)) {
      tmp[k] = w[done + k] < 0x80 ? (char)w[done + k] : '?';
      ++k;
    }
    if (!WriteFile(h, tmp, k, 0, 0)) {
      return FALSE;
    }
    done += k;
  }
  if (written != 0) {
    *written = n;
  }
  return TRUE;
}

EXPORT BOOL WINAPI GetConsoleMode(HANDLE h, LPDWORD m) { (void)h; *m = 3; return TRUE; }
EXPORT BOOL WINAPI SetConsoleMode(HANDLE h, DWORD m) { (void)h; (void)m; return TRUE; }
EXPORT BOOL WINAPI SetConsoleTitleA(LPCSTR t) { (void)t; return TRUE; }
EXPORT BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE r, BOOL add) { (void)r; (void)add; return TRUE; }
EXPORT UINT WINAPI GetConsoleOutputCP(void) { return 437; }
EXPORT BOOL WINAPI SetConsoleOutputCP(UINT cp) { (void)cp; return TRUE; }
EXPORT DWORD WINAPI GetFileType(HANDLE h) {
  hent_t *e = h_get(h);

  return e == 0 ? FILE_TYPE_UNKNOWN : (e->kind == H_FILE ? FILE_TYPE_DISK : FILE_TYPE_CHAR);
}

EXPORT HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD attrs, HANDLE tmpl) {
  char path[128];
  sc_dirent_t st;
  int want_read = (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
  int want_write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
  int exists;
  sl_t fd;
  int flags;

  (void)share;
  (void)sa;
  (void)attrs;
  (void)tmpl;
  if (!to_path(name, path, sizeof(path))) {
    SetLastError(ERROR_BAD_PATHNAME);
    return INVALID_HANDLE_VALUE;
  }
  exists = sc3(KS_STAT, (sl_t)path, (sl_t)&st, 0) == 0;
  if (disp == CREATE_NEW && exists) {
    SetLastError(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
  }
  if ((disp == OPEN_EXISTING || disp == TRUNCATE_EXISTING) && !exists) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  if (!want_write) {
    flags = KS_O_RDONLY;
    if (!exists) {
      SetLastError(ERROR_FILE_NOT_FOUND);
      return INVALID_HANDLE_VALUE;
    }
  } else if (disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING) {
    flags = KS_O_WRONLY;                                  // create or truncate
  } else {
    flags = want_read || !exists ? KS_O_RDWR : KS_O_RDWR;  // keep the contents, create if missing
  }
  fd = sc3(KS_OPEN, (sl_t)path, flags, 0);
  if (fd < 0) {
    SetLastError(map_error(fd));
    return INVALID_HANDLE_VALUE;
  }
  if (disp == OPEN_ALWAYS || disp == CREATE_ALWAYS) {
    SetLastError(exists ? ERROR_ALREADY_EXISTS : 0);
  }
  return h_alloc(H_FILE, (int)fd);
}

EXPORT BOOL WINAPI CloseHandle(HANDLE h) {
  hent_t *e = h_get(h);
  sl_t r = 0;

  if (e == 0) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (e->kind >= H_THREAD) {
    int kind = e->kind;
    void *obj = e->obj;

    e->kind = H_FREE;
    rt_obj_close(kind, obj);
    return TRUE;
  }
  if (e->kind == H_FILE || e->kind == H_FIND) {
    r = sc3(KS_CLOSE, e->fd, 0, 0);
  }
  if ((uintptr_t)h >= 0x100 + 4 * 3) {                    // the standard handles stay
    e->kind = H_FREE;
  }
  if (r < 0) {
    SetLastError(map_error(r));
    return FALSE;
  }
  return TRUE;
}

EXPORT DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method) {
  hent_t *e = h_get(h);
  sl_t r;

  (void)hi;
  if (e == 0 || e->kind != H_FILE) {
    SetLastError(ERROR_INVALID_HANDLE);
    return INVALID_SET_FILE_POINTER;
  }
  r = sc3(KS_LSEEK, e->fd, lo, (sl_t)method);
  if (r < 0) {
    SetLastError(map_error(r));
    return INVALID_SET_FILE_POINTER;
  }
  return (DWORD)r;
}

EXPORT BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER d, PLARGE_INTEGER np, DWORD method) {
  DWORD r = SetFilePointer(h, (LONG)d.QuadPart, 0, method);

  if (r == INVALID_SET_FILE_POINTER) {
    return FALSE;
  }
  if (np != 0) {
    np->QuadPart = r;
  }
  return TRUE;
}

EXPORT DWORD WINAPI GetFileSize(HANDLE h, LPDWORD hi) {
  hent_t *e = h_get(h);
  sl_t cur, end;

  if (hi != 0) {
    *hi = 0;
  }
  if (e == 0 || e->kind != H_FILE) {
    SetLastError(ERROR_INVALID_HANDLE);
    return INVALID_FILE_SIZE;
  }
  cur = sc3(KS_LSEEK, e->fd, 0, 1);
  end = sc3(KS_LSEEK, e->fd, 0, 2);
  sc3(KS_LSEEK, e->fd, cur < 0 ? 0 : cur, 0);
  return end < 0 ? INVALID_FILE_SIZE : (DWORD)end;
}

EXPORT BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER sz) {
  DWORD n = GetFileSize(h, 0);

  if (n == INVALID_FILE_SIZE) {
    return FALSE;
  }
  sz->QuadPart = n;
  return TRUE;
}

EXPORT BOOL WINAPI FlushFileBuffers(HANDLE h) { (void)h; return TRUE; }

EXPORT BOOL WINAPI DeleteFileA(LPCSTR name) {
  char path[128];
  sl_t r;

  if (!to_path(name, path, sizeof(path))) {
    SetLastError(ERROR_BAD_PATHNAME);
    return FALSE;
  }
  r = sc3(KS_UNLINK, (sl_t)path, 0, 0);
  if (r < 0) {
    SetLastError(map_error(r));
    return FALSE;
  }
  return TRUE;
}

EXPORT BOOL WINAPI RemoveDirectoryA(LPCSTR name) { return DeleteFileA(name); }

EXPORT BOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa) {
  char path[128];
  sl_t r;

  (void)sa;
  if (!to_path(name, path, sizeof(path))) {
    SetLastError(ERROR_BAD_PATHNAME);
    return FALSE;
  }
  r = sc3(KS_MKDIR, (sl_t)path, 0, 0);
  if (r < 0) {
    SetLastError(map_error(r));
    return FALSE;
  }
  return TRUE;
}

EXPORT DWORD WINAPI GetFileAttributesA(LPCSTR name) {
  char path[128];
  sc_dirent_t st;
  sl_t r;

  if (!to_path(name, path, sizeof(path))) {
    SetLastError(ERROR_BAD_PATHNAME);
    return INVALID_FILE_ATTRIBUTES;
  }
  r = sc3(KS_STAT, (sl_t)path, (sl_t)&st, 0);
  if (r < 0) {
    SetLastError(map_error(r));
    return INVALID_FILE_ATTRIBUTES;
  }
  return (st.attr & 0x10) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

EXPORT DWORD WINAPI GetCurrentDirectoryA(DWORD n, LPSTR buf) {
  if (n < 4) {
    return 4;
  }
  rt_copy(buf, "C:\\", n);
  return 3;
}

EXPORT BOOL WINAPI SetCurrentDirectoryA(LPCSTR d) { (void)d; return TRUE; }

// Wildcard match with * and ?, case-insensitive.
static int wild(const char *p, const char *s) {
  while (*p != 0) {
    if (*p == '*') {
      ++p;
      if (*p == 0) {
        return 1;
      }
      for (; *s != 0; ++s) {
        if (wild(p, s)) {
          return 1;
        }
      }
      return wild(p, s);
    }
    if (*s == 0 || (*p != '?' && lower(*p) != lower(*s))) {
      return 0;
    }
    ++p;
    ++s;
  }
  return *s == 0;
}

static BOOL find_next(hent_t *e, LPWIN32_FIND_DATAA fd) {
  sc_dirent_t d;

  for (;;) {
    sl_t r = sc3(KS_READDIR, e->fd, (sl_t)&d, 0);

    if (r <= 0) {
      SetLastError(ERROR_NO_MORE_FILES);
      return FALSE;
    }
    if (!wild(e->pattern, d.name)) {
      continue;
    }
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = (d.attr & 0x10) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    fd->nFileSizeLow = d.size;
    rt_copy(fd->cFileName, d.name, sizeof(fd->cFileName));
    return TRUE;
  }
}

EXPORT HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd) {
  char path[128];
  size_t i, cut = 0;
  sl_t dirfd;
  HANDLE h;

  if (!to_path(pattern, path, sizeof(path))) {
    SetLastError(ERROR_BAD_PATHNAME);
    return INVALID_HANDLE_VALUE;
  }
  for (i = 0; path[i] != 0; ++i) {
    if (path[i] == '/') {
      cut = i;
    }
  }
  h = h_alloc(H_FIND, -1);
  if (h == INVALID_HANDLE_VALUE) {
    return h;
  }
  rt_copy(h_get(h)->pattern, path + cut + 1, sizeof(h_get(h)->pattern));
  path[cut == 0 ? 1 : cut] = 0;
  dirfd = sc3(KS_OPEN, (sl_t)path, KS_O_RDONLY, 0);
  if (dirfd < 0) {
    h_get(h)->kind = H_FREE;
    SetLastError(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  h_get(h)->fd = (int)dirfd;
  if (!find_next(h_get(h), fd)) {
    CloseHandle(h);
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  return h;
}

EXPORT BOOL WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) {
  hent_t *e = h_get(h);

  if (e == 0 || e->kind != H_FIND) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  return find_next(e, fd);
}

EXPORT BOOL WINAPI FindClose(HANDLE h) { return CloseHandle(h); }
