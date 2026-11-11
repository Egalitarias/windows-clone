// The C runtime part of WIN32.DLL: what programs built with the dynamic CRT (/MD) import from
// api-ms-win-crt-*.dll ("ucrtbase") and VCRUNTIME140.dll. The startup code the MSVC linker puts
// into the program (mainCRTStartup -> __scrt_common_main_seh) runs unchanged on top of this: it
// finds argv, runs the static initialisers, calls main and exits through exit().
// Single-threaded, ASCII only, "C" locale, no wall-clock time zone. See docs/WIN32.md.
#include "crt.h"

#define X EXPORT_SPECIAL

// <ctype.h>, <wctype.h> and <stdio.h> define many of these as macros; here they are functions.
#undef isalpha
#undef isupper
#undef islower
#undef isdigit
#undef isxdigit
#undef isspace
#undef ispunct
#undef isalnum
#undef isprint
#undef isgraph
#undef iscntrl
#undef isblank
#undef isascii
#undef toupper
#undef tolower
#undef iswalpha
#undef iswdigit
#undef iswspace
#undef iswupper
#undef iswlower
#undef iswalnum
#undef iswpunct
#undef towlower
#undef towupper
#undef getc
#undef putc
#undef getchar
#undef putchar
#undef feof
#undef ferror
#undef __isascii
#undef __iscsymf
#undef __iscsym
#undef _toupper
#undef _tolower
#undef __toascii
typedef struct cfile cfile_t;

static unsigned long g_doserrno;

X int *_errno(void) { return &rt_errno; }
X unsigned long *__doserrno(void) { return &g_doserrno; }
X int _set_errno(int v) { rt_errno = v; return 0; }
X int _get_errno(int *v) { if (v == 0) { return RT_EINVAL; } *v = rt_errno; return 0; }

// ---- the program's start (see the kernel's pe_finish) --------------------------------------------------
// The loader enters here instead of the program's entry point: run the TLS callbacks the
// linker's CRT registered (thread_local initialisation), then the program's real entry.

uint64_t rt_exe_base(void);

X int __winclone_start(void) {
  const uint8_t *base = (const uint8_t *)(uintptr_t)rt_exe_base();
  const uint8_t *nt = base + *(const uint32_t *)(base + 0x3C);
  const uint8_t *opt = nt + 24;
  uint32_t entry = *(const uint32_t *)(opt + 16);
  uint32_t tls_rva = *(const uint32_t *)(opt + 112 + 9 * 8), tls_size = *(const uint32_t *)(opt + 112 + 9 * 8 + 4);

  if (tls_rva != 0 && tls_size >= 40) {
    typedef void (NTAPI *tls_cb_t)(PVOID, DWORD, PVOID);
    tls_cb_t *cb = (tls_cb_t *)(uintptr_t)*(const uint64_t *)(base + tls_rva + 24);

    for (; cb != 0 && *cb != 0; ++cb) {
      (*cb)((PVOID)base, DLL_PROCESS_ATTACH, 0);
    }
  }
  return ((int (*)(void))(uintptr_t)(base + entry))();
}

// ---- kernel32 pieces the CRT startup code asks for --------------------------------------------------------

EXPORT BOOL WINAPI IsProcessorFeaturePresent(DWORD f) {
  return f == PF_MMX_INSTRUCTIONS_AVAILABLE || f == PF_XMMI_INSTRUCTIONS_AVAILABLE || f == PF_XMMI64_INSTRUCTIONS_AVAILABLE ||
         f == PF_COMPARE_EXCHANGE_DOUBLE || f == PF_COMPARE_EXCHANGE128 || f == PF_FLOATING_POINT_PRECISION_ERRATA * 0;
}

EXPORT VOID WINAPI InitializeSListHead(PSLIST_HEADER h) { memset(h, 0, sizeof(*h)); }
EXPORT PVOID WINAPI EncodePointer(PVOID p) { return p; }
EXPORT PVOID WINAPI DecodePointer(PVOID p) { return p; }
EXPORT BOOL WINAPI HeapSetInformation(HANDLE h, HEAP_INFORMATION_CLASS c, PVOID i, SIZE_T n) { (void)h; (void)c; (void)i; (void)n; return TRUE; }
EXPORT UINT WINAPI GetACP(void) { return 1252; }
EXPORT UINT WINAPI GetOEMCP(void) { return 437; }
EXPORT VOID WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME ft) { GetSystemTimeAsFileTime(ft); }

// Fiber local storage: one thread, so the same as thread local slots.
EXPORT DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION cb) { (void)cb; return TlsAlloc(); }
EXPORT BOOL WINAPI FlsFree(DWORD i) { return TlsFree(i); }
EXPORT PVOID WINAPI FlsGetValue(DWORD i) { return TlsGetValue(i); }
EXPORT BOOL WINAPI FlsSetValue(DWORD i, PVOID v) { return TlsSetValue(i, v); }

EXPORT LPWCH WINAPI GetEnvironmentStringsW(void) {
  static WCHAR empty[2];

  return empty;
}
EXPORT BOOL WINAPI FreeEnvironmentStringsW(LPWCH p) { (void)p; return TRUE; }

// ---- vcruntime: the pieces that need no unwinding (seh.c and cxxeh.c have the rest) ------------------------------------------------------------------

static void *g_cur_exc, *g_cur_exc_ctx;
X void **__current_exception(void) { return &g_cur_exc; }
X void **__current_exception_context(void) { return &g_cur_exc_ctx; }

static DECLSPEC_NORETURN void die(const char *msg, int code);
static DECLSPEC_NORETURN void term_report(const char *who, unsigned long long ra) {
  static const char crlf[] = {13, 10, 0};
  char hex[17];
  unsigned long long a = ra - rt_exe_base(), v;
  int i;

  for (i = 0; i < 16; ++i) {
    v = (a >> (60 - 4 * i)) & 15;
    hex[i] = (char)(v < 10 ? '0' + v : 'A' + v - 10);
  }
  hex[16] = 0;
  rt_log(crlf);
  rt_log("[EH] ");
  rt_log(who);
  rt_log(" called from exe+0x");
  rt_log(hex);
  die("terminate() was called.", 3);
}

X DECLSPEC_NORETURN void __std_terminate(void) { term_report("__std_terminate", (unsigned long long)(uintptr_t)__builtin_return_address(0)); }
X DECLSPEC_NORETURN void terminate(void) { term_report("terminate", (unsigned long long)(uintptr_t)__builtin_return_address(0)); }
X int _callnewh(size_t n) { (void)n; return 0; }
X DECLSPEC_NORETURN int _purecall(void) { die("Pure virtual function call.", 3); }

// setjmp / longjmp: MSVC's x64 jump buffer layout (Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip,
// MxCsr, FpCsr, Xmm6-15). There is no unwinding: longjmp only restores the registers.
X __attribute__((naked)) int _setjmp(void *buf, void *frame) {
  __asm__(".intel_syntax noprefix\n\t"
      "mov [rcx], rdx\n\t"
      "mov [rcx+8], rbx\n\t"
      "lea rax, [rsp+8]\n\t"
      "mov [rcx+16], rax\n\t"
      "mov [rcx+24], rbp\n\t"
      "mov [rcx+32], rsi\n\t"
      "mov [rcx+40], rdi\n\t"
      "mov [rcx+48], r12\n\t"
      "mov [rcx+56], r13\n\t"
      "mov [rcx+64], r14\n\t"
      "mov [rcx+72], r15\n\t"
      "mov rax, [rsp]\n\t"
      "mov [rcx+80], rax\n\t"
      "stmxcsr [rcx+88]\n\t"
      "fnstcw [rcx+92]\n\t"
      "movups [rcx+96], xmm6\n\t"
      "movups [rcx+112], xmm7\n\t"
      "movups [rcx+128], xmm8\n\t"
      "movups [rcx+144], xmm9\n\t"
      "movups [rcx+160], xmm10\n\t"
      "movups [rcx+176], xmm11\n\t"
      "movups [rcx+192], xmm12\n\t"
      "movups [rcx+208], xmm13\n\t"
      "movups [rcx+224], xmm14\n\t"
      "movups [rcx+240], xmm15\n\t"
      "xor eax, eax\n\t"
      "ret\n\t"
      ".att_syntax\n");
}
X __attribute__((naked)) int _setjmpex(void *buf, void *frame) { __asm__(".intel_syntax noprefix\n\tjmp _setjmp\n\t.att_syntax\n"); }
X __attribute__((naked)) int __intrinsic_setjmp(void *buf, void *frame) { __asm__(".intel_syntax noprefix\n\tjmp _setjmp\n\t.att_syntax\n"); }
X __attribute__((naked)) int __intrinsic_setjmpex(void *buf, void *frame) { __asm__(".intel_syntax noprefix\n\tjmp _setjmp\n\t.att_syntax\n"); }
X __attribute__((naked)) void longjmp(void *buf, int val) {
  __asm__(".intel_syntax noprefix\n\t"
      "mov eax, edx\n\t"
      "test eax, eax\n\t"
      "jnz 1f\n\t"
      "inc eax\n\t"
      "1:\n\t"
      "mov rbx, [rcx+8]\n\t"
      "mov rbp, [rcx+24]\n\t"
      "mov rsi, [rcx+32]\n\t"
      "mov rdi, [rcx+40]\n\t"
      "mov r12, [rcx+48]\n\t"
      "mov r13, [rcx+56]\n\t"
      "mov r14, [rcx+64]\n\t"
      "mov r15, [rcx+72]\n\t"
      "ldmxcsr [rcx+88]\n\t"
      "fldcw [rcx+92]\n\t"
      "movups xmm6, [rcx+96]\n\t"
      "movups xmm7, [rcx+112]\n\t"
      "movups xmm8, [rcx+128]\n\t"
      "movups xmm9, [rcx+144]\n\t"
      "movups xmm10, [rcx+160]\n\t"
      "movups xmm11, [rcx+176]\n\t"
      "movups xmm12, [rcx+192]\n\t"
      "movups xmm13, [rcx+208]\n\t"
      "movups xmm14, [rcx+224]\n\t"
      "movups xmm15, [rcx+240]\n\t"
      "mov rsp, [rcx+16]\n\t"
      "jmp qword ptr [rcx+80]\n\t"
      ".att_syntax\n");
}
X __attribute__((naked)) void _longjmpex(void *buf, int val) { __asm__(".intel_syntax noprefix\n\tjmp longjmp\n\t.att_syntax\n"); }

// ---- memory ------------------------------------------------------------------------------------------------------------

X void *malloc(size_t n) {
  void *p = HeapAlloc(GetProcessHeap(), 0, n != 0 ? n : 1);

  if (p == 0) {
    rt_errno = RT_ENOMEM;
  }
  return p;
}

X void free(void *p) {
  if (p != 0) {
    HeapFree(GetProcessHeap(), 0, p);
  }
}

X void *calloc(size_t n, size_t size) {
  void *p;

  if (size != 0 && n > (size_t)-1 / size) {
    rt_errno = RT_ENOMEM;
    return 0;
  }
  p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * size != 0 ? n * size : 1);
  if (p == 0) {
    rt_errno = RT_ENOMEM;
  }
  return p;
}

X void *realloc(void *p, size_t n) {
  void *q;

  if (p == 0) {
    return malloc(n);
  }
  if (n == 0) {
    free(p);
    return 0;
  }
  q = HeapReAlloc(GetProcessHeap(), 0, p, n);
  if (q == 0) {
    rt_errno = RT_ENOMEM;
  }
  return q;
}

X void *_recalloc(void *p, size_t n, size_t size) {
  size_t old = p != 0 ? HeapSize(GetProcessHeap(), 0, p) : 0, want = n * size;
  void *q = realloc(p, want);

  if (q != 0 && want > old) {
    memset((char *)q + old, 0, want - old);
  }
  return q;
}

X size_t _msize(void *p) { return HeapSize(GetProcessHeap(), 0, p); }

X void *_aligned_malloc(size_t n, size_t align) {
  uint8_t *raw, *p;

  if (align < 16) {
    align = 16;
  }
  raw = (uint8_t *)malloc(n + align + 16);
  if (raw == 0) {
    return 0;
  }
  p = (uint8_t *)(((uintptr_t)raw + 16 + align - 1) & ~(uintptr_t)(align - 1));
  ((void **)p)[-1] = raw;
  return p;
}

X void _aligned_free(void *p) {
  if (p != 0) {
    free(((void **)p)[-1]);
  }
}

// ---- process end -------------------------------------------------------------------------------------------------------------

typedef void (__cdecl *vfn_t)(void);
static vfn_t g_atexit[128];
static int g_natexit;
static vfn_t g_tl_atexit;
static int g_exiting;

typedef struct { vfn_t *first, *last, *end; } onexit_table_t;

static void flush_all(void);

static void run_atexit(void) {
  while (g_natexit > 0) {
    vfn_t f = g_atexit[--g_natexit];

    if (f != 0) {
      f();
    }
  }
}

static void cleanup(void) {
  if (!g_exiting) {
    g_exiting = 1;
    if (g_tl_atexit != 0) {
      g_tl_atexit();
    }
    run_atexit();
  }
  flush_all();
}

X DECLSPEC_NORETURN void exit(int code) { cleanup(); ExitProcess((UINT)code); for (;;) { } }
X DECLSPEC_NORETURN void _exit(int code) { ExitProcess((UINT)code); for (;;) { } }
X DECLSPEC_NORETURN void _Exit(int code) { ExitProcess((UINT)code); for (;;) { } }
X DECLSPEC_NORETURN void quick_exit(int code) { flush_all(); ExitProcess((UINT)code); for (;;) { } }
X void _cexit(void) { cleanup(); }
X void _c_exit(void) { }

static DECLSPEC_NORETURN void die(const char *msg, int code) {
  flush_all();
  rt_log("\r\n");
  rt_log(msg);
  rt_log("\r\n");
  ExitProcess((UINT)code);
  for (;;) {
  }
}

X DECLSPEC_NORETURN void abort(void) { die("abnormal program termination", 3); }
X DECLSPEC_NORETURN void _invalid_parameter_noinfo_noreturn(void) { die("Invalid parameter passed to a C runtime function.", 3); }
X void _invalid_parameter_noinfo(void) { rt_errno = RT_EINVAL; }
X void _invalid_parameter(const WCHAR *a, const WCHAR *b, const WCHAR *c, unsigned d, uintptr_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; rt_errno = RT_EINVAL; }
X void *_set_invalid_parameter_handler(void *h) { (void)h; return 0; }
X void *_set_purecall_handler(void *h) { (void)h; return 0; }
X unsigned int _set_abort_behavior(unsigned int a, unsigned int b) { (void)a; (void)b; return 0; }
X int _set_new_mode(int m) { (void)m; return 0; }
X int _set_app_type(int t) { (void)t; return 0; }
X int _configthreadlocale(int m) { (void)m; return 0; }
X void __setusermatherr(void *f) { (void)f; }
X int _seh_filter_exe(unsigned long code, void *ptrs) { (void)code; (void)ptrs; return EXCEPTION_CONTINUE_SEARCH; }
X int _crt_atexit(vfn_t f) { if (g_natexit >= 128) { return -1; } g_atexit[g_natexit++] = f; return 0; }
X int _crt_at_quick_exit(vfn_t f) { (void)f; return 0; }
X int atexit(vfn_t f) { return _crt_atexit(f); }
X void _register_thread_local_exe_atexit_callback(vfn_t f) { g_tl_atexit = f; }

X int _initialize_onexit_table(onexit_table_t *t) { t->first = t->last = t->end = 0; return 0; }
X int _register_onexit_function(onexit_table_t *t, vfn_t f) {
  if (t->first == 0 || t->first == (vfn_t *)(intptr_t)-1) {
    t->first = t->last = (vfn_t *)malloc(32 * sizeof(vfn_t));
    if (t->first == 0) {
      return -1;
    }
    t->end = t->first + 32;
  }
  if (t->last == t->end) {
    size_t n = (size_t)(t->end - t->first), used = (size_t)(t->last - t->first);
    vfn_t *p = (vfn_t *)realloc(t->first, n * 2 * sizeof(vfn_t));

    if (p == 0) {
      return -1;
    }
    t->first = p;
    t->last = p + used;
    t->end = p + n * 2;
  }
  *t->last++ = f;
  return 0;
}
X int _execute_onexit_table(onexit_table_t *t) {
  vfn_t *f, *first = t->first, *last = t->last;

  if (first == 0 || first == (vfn_t *)(intptr_t)-1) {
    return 0;
  }
  t->first = t->last = t->end = 0;
  for (f = last; f > first;) {
    --f;
    if (*f != 0) {
      (*f)();
    }
  }
  free(first);
  return 0;
}

X void _initterm(vfn_t *first, vfn_t *last) {
  for (; first < last; ++first) {
    if (*first != 0) {
      (*first)();
    }
  }
}

X int _initterm_e(int (__cdecl **first)(void), int (__cdecl **last)(void)) {
  for (; first < last; ++first) {
    if (*first != 0) {
      int r = (*first)();

      if (r != 0) {
        return r;
      }
    }
  }
  return 0;
}

// ---- argv, environment --------------------------------------------------------------------------------------------------------

static int g_argc;
static char **g_argv;
static WCHAR **g_wargv;
static char *g_acmdln;
static char *g_envp[1];
static WCHAR *g_wenvp[1];
static char **g_environ = g_envp;
static WCHAR **g_wenviron = g_wenvp;
static int g_fmode, g_commode;

static void argv_init(void) {
  const char *cmd;
  char *buf, **argv;
  size_t len, n = 0, cap = 8, i;
  int in_arg = 0, quoted = 0;
  char *w;

  if (g_argv != 0) {
    return;
  }
  cmd = GetCommandLineA();
  len = rt_strlen(cmd);
  buf = (char *)malloc(len + 2);
  argv = (char **)malloc(cap * sizeof(char *));
  w = buf;
  for (i = 0; i <= len; ++i) {
    char c = cmd[i];

    if (c == 0 || (!quoted && (c == ' ' || c == '\t'))) {
      if (in_arg) {
        *w++ = 0;
        in_arg = 0;
      }
      if (c == 0) {
        break;
      }
      continue;
    }
    if (!in_arg) {
      in_arg = 1;
      if (n + 2 > cap) {
        cap *= 2;
        argv = (char **)realloc(argv, cap * sizeof(char *));
      }
      argv[n++] = w;
    }
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    if (c == '\\') {                                       // backslashes before a quote
      size_t k = i, bs = 0;

      while (cmd[k] == '\\') {
        ++k;
        ++bs;
      }
      if (cmd[k] == '"') {
        size_t j;

        for (j = 0; j < bs / 2; ++j) {
          *w++ = '\\';
        }
        i = k - 1;
        if (bs & 1) {
          *w++ = '"';
          i = k;
        }
        continue;
      }
    }
    *w++ = c;
  }
  argv[n] = 0;
  g_argc = (int)n;
  g_argv = argv;
  g_acmdln = (char *)cmd;
}

static void wargv_init(void) {
  int i;

  if (g_wargv != 0) {
    return;
  }
  argv_init();
  g_wargv = (WCHAR **)malloc((size_t)(g_argc + 1) * sizeof(WCHAR *));
  for (i = 0; i < g_argc; ++i) {
    size_t n = rt_strlen(g_argv[i]), k;

    g_wargv[i] = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    for (k = 0; k <= n; ++k) {
      g_wargv[i][k] = (unsigned char)g_argv[i][k];
    }
  }
  g_wargv[g_argc] = 0;
}

X int _configure_narrow_argv(int mode) { (void)mode; argv_init(); return 0; }
X int _configure_wide_argv(int mode) { (void)mode; wargv_init(); return 0; }
X int *__p___argc(void) { argv_init(); return &g_argc; }
X char ***__p___argv(void) { argv_init(); return &g_argv; }
X WCHAR ***__p___wargv(void) { wargv_init(); return &g_wargv; }
X char **__p__acmdln(void) { argv_init(); return &g_acmdln; }
X int _initialize_narrow_environment(void) { return 0; }
X int _initialize_wide_environment(void) { return 0; }
X char **_get_initial_narrow_environment(void) { return g_envp; }
X WCHAR **_get_initial_wide_environment(void) { return g_wenvp; }
X char ***__p__environ(void) { return &g_environ; }
X WCHAR ***__p__wenviron(void) { return &g_wenviron; }
X int *__p__fmode(void) { return &g_fmode; }
X int _set_fmode(int m) { g_fmode = m; return 0; }
X int *__p__commode(void) { return &g_commode; }
X char *getenv(const char *n) { (void)n; return 0; }
X int _putenv(const char *s) { (void)s; return -1; }
X int system(const char *c) { (void)c; return -1; }

typedef void (__cdecl *sighandler_t)(int);
static sighandler_t g_sig[23];
X sighandler_t signal(int sig, sighandler_t h) {
  sighandler_t old;

  if (sig < 1 || sig >= 23) {
    rt_errno = RT_EINVAL;
    return (sighandler_t)(intptr_t)-1;
  }
  old = g_sig[sig];
  g_sig[sig] = h;
  return old;
}
X int raise(int sig) {
  sighandler_t h;

  if (sig < 1 || sig >= 23) {
    return -1;
  }
  h = g_sig[sig];
  if (h == (sighandler_t)(intptr_t)1) {
    return 0;                                              // SIG_IGN
  }
  if (h != 0) {
    g_sig[sig] = 0;
    h(sig);
    return 0;
  }
  if (sig == 22) {
    abort();                                               // SIGABRT
  }
  ExitProcess(3);
  return 0;
}

// ---- strings ------------------------------------------------------------------------------------------------------------------------

X size_t strlen(const char *s) { return rt_strlen(s); }
X size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i] != 0) { ++i; } return i; }
X int strcmp(const char *a, const char *b) { return rt_strcmp(a, b); }
X int strncmp(const char *a, const char *b, size_t n) {
  size_t i;

  for (i = 0; i < n; ++i) {
    if (a[i] != b[i] || a[i] == 0) {
      return (unsigned char)a[i] - (unsigned char)b[i];
    }
  }
  return 0;
}
X int _stricmp(const char *a, const char *b) { return rt_stricmp(a, b); }
X int _strnicmp(const char *a, const char *b, size_t n) {
  size_t i;

  for (i = 0; i < n; ++i) {
    int x = rt_tolower((unsigned char)a[i]), y = rt_tolower((unsigned char)b[i]);

    if (x != y || x == 0) {
      return x - y;
    }
  }
  return 0;
}
X int strcoll(const char *a, const char *b) { return rt_strcmp(a, b); }
X char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++) != 0) { } return r; }
X char *strncpy(char *d, const char *s, size_t n) {
  size_t i = 0;

  for (; i < n && s[i] != 0; ++i) {
    d[i] = s[i];
  }
  for (; i < n; ++i) {
    d[i] = 0;
  }
  return d;
}
X char *strcat(char *d, const char *s) { strcpy(d + rt_strlen(d), s); return d; }
X char *strncat(char *d, const char *s, size_t n) {
  size_t dl = rt_strlen(d), i = 0;

  for (; i < n && s[i] != 0; ++i) {
    d[dl + i] = s[i];
  }
  d[dl + i] = 0;
  return d;
}
X char *strchr(const char *s, int c) {
  for (;; ++s) {
    if (*s == (char)c) {
      return (char *)s;
    }
    if (*s == 0) {
      return 0;
    }
  }
}
X char *strrchr(const char *s, int c) {
  const char *r = 0;

  for (;; ++s) {
    if (*s == (char)c) {
      r = s;
    }
    if (*s == 0) {
      return (char *)r;
    }
  }
}
X char *strstr(const char *h, const char *n) {
  size_t nl = rt_strlen(n);

  if (nl == 0) {
    return (char *)h;
  }
  for (; *h != 0; ++h) {
    if (*h == *n && strncmp(h, n, nl) == 0) {
      return (char *)h;
    }
  }
  return 0;
}
X size_t strspn(const char *s, const char *set) { size_t n = 0; while (s[n] != 0 && strchr(set, s[n]) != 0) { ++n; } return n; }
X size_t strcspn(const char *s, const char *set) { size_t n = 0; while (s[n] != 0 && strchr(set, s[n]) == 0) { ++n; } return n; }
X char *strpbrk(const char *s, const char *set) { for (; *s != 0; ++s) { if (strchr(set, *s) != 0) { return (char *)s; } } return 0; }
X void *memchr(const void *p, int c, size_t n) {
  const unsigned char *s = (const unsigned char *)p;
  size_t i;

  for (i = 0; i < n; ++i) {
    if (s[i] == (unsigned char)c) {
      return (void *)(s + i);
    }
  }
  return 0;
}
X char *strtok_s(char *s, const char *delim, char **ctx) {
  char *start;

  if (s == 0) {
    s = *ctx;
  }
  if (s == 0) {
    return 0;
  }
  s += strspn(s, delim);
  if (*s == 0) {
    *ctx = 0;
    return 0;
  }
  start = s;
  s += strcspn(s, delim);
  if (*s != 0) {
    *s++ = 0;
  }
  *ctx = s;
  return start;
}
static char *g_strtok_ctx;
X char *strtok(char *s, const char *delim) { return strtok_s(s, delim, &g_strtok_ctx); }
X char *_strdup(const char *s) {
  size_t n = rt_strlen(s) + 1;
  char *p = (char *)malloc(n);

  if (p != 0) {
    memcpy(p, s, n);
  }
  return p;
}
X char *strdup(const char *s) { return _strdup(s); }
X char *_strlwr(char *s) { char *r = s; for (; *s != 0; ++s) { *s = (char)rt_tolower((unsigned char)*s); } return r; }
X char *_strupr(char *s) { char *r = s; for (; *s != 0; ++s) { *s = (char)rt_toupper((unsigned char)*s); } return r; }
X char *_strrev(char *s) {
  size_t i = 0, j = rt_strlen(s);

  while (j > i + 1) {
    char t = s[i];

    s[i++] = s[--j];
    s[j] = t;
  }
  return s;
}
X int strcpy_s(char *d, size_t n, const char *s) {
  size_t l = rt_strlen(s);

  if (d == 0 || n == 0) {
    return RT_EINVAL;
  }
  if (l >= n) {
    d[0] = 0;
    return RT_ERANGE;
  }
  memcpy(d, s, l + 1);
  return 0;
}
X int strcat_s(char *d, size_t n, const char *s) {
  size_t dl = strnlen(d, n), l = rt_strlen(s);

  if (dl == n || l >= n - dl) {
    if (n != 0) {
      d[0] = 0;
    }
    return RT_ERANGE;
  }
  memcpy(d + dl, s, l + 1);
  return 0;
}
X int strncpy_s(char *d, size_t n, const char *s, size_t cnt) {
  size_t l = strnlen(s, cnt);

  if (d == 0 || n == 0) {
    return RT_EINVAL;
  }
  if (l >= n) {
    d[0] = 0;
    return RT_ERANGE;
  }
  memcpy(d, s, l);
  d[l] = 0;
  return 0;
}
X char *strerror(int e) {
  static char buf[32];

  switch (e) {
    case 0: return "No error";
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 9: return "Bad file descriptor";
    case 12: return "Not enough space";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 28: return "No space left on device";
    case 33: return "Domain error";
    case 34: return "Result too large";
    default: break;
  }
  {
    char *w = buf;
    int v = e < 0 ? -e : e, n = 0;
    char tmp[12];

    rt_copy(buf, "Unknown error ", sizeof(buf));
    w = buf + rt_strlen(buf);
    if (e < 0) {
      *w++ = '-';
    }
    do {
      tmp[n++] = (char)('0' + v % 10);
      v /= 10;
    } while (v != 0);
    while (n > 0) {
      *w++ = tmp[--n];
    }
    *w = 0;
  }
  return buf;
}
X char *_strerror(const char *m) { (void)m; return strerror(rt_errno); }
X int strerror_s(char *b, size_t n, int e) { rt_copy(b, (char *)strerror(e), n); return 0; }

// wide strings (UTF-16 code units; anything above 127 is treated as is)
X size_t wcslen(const WCHAR *s) { size_t n = 0; while (s[n] != 0) { ++n; } return n; }
X int wcscmp(const WCHAR *a, const WCHAR *b) { while (*a != 0 && *a == *b) { ++a; ++b; } return (int)*a - (int)*b; }
X int wcsncmp(const WCHAR *a, const WCHAR *b, size_t n) {
  size_t i;

  for (i = 0; i < n; ++i) {
    if (a[i] != b[i] || a[i] == 0) {
      return (int)a[i] - (int)b[i];
    }
  }
  return 0;
}
static int wlower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
X int _wcsicmp(const WCHAR *a, const WCHAR *b) { while (*a != 0 && wlower(*a) == wlower(*b)) { ++a; ++b; } return wlower(*a) - wlower(*b); }
X WCHAR *wcscpy(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++) != 0) { } return r; }
X WCHAR *wcsncpy(WCHAR *d, const WCHAR *s, size_t n) {
  size_t i = 0;

  for (; i < n && s[i] != 0; ++i) {
    d[i] = s[i];
  }
  for (; i < n; ++i) {
    d[i] = 0;
  }
  return d;
}
X WCHAR *wcscat(WCHAR *d, const WCHAR *s) { wcscpy(d + wcslen(d), s); return d; }
X WCHAR *wcschr(const WCHAR *s, WCHAR c) { for (;; ++s) { if (*s == c) { return (WCHAR *)s; } if (*s == 0) { return 0; } } }
X WCHAR *wcsrchr(const WCHAR *s, WCHAR c) { const WCHAR *r = 0; for (;; ++s) { if (*s == c) { r = s; } if (*s == 0) { return (WCHAR *)r; } } }
X WCHAR *wcsstr(const WCHAR *h, const WCHAR *n) {
  size_t nl = wcslen(n);

  if (nl == 0) {
    return (WCHAR *)h;
  }
  for (; *h != 0; ++h) {
    if (wcsncmp(h, n, nl) == 0) {
      return (WCHAR *)h;
    }
  }
  return 0;
}
X unsigned short towlower(unsigned short c) { return (unsigned short)wlower(c); }
X unsigned short towupper(unsigned short c) { return (unsigned short)(c >= 'a' && c <= 'z' ? c - 32 : c); }
X int iswalpha(unsigned short c) { return rt_isalpha(c); }
X int iswdigit(unsigned short c) { return rt_isdigit(c); }
X int iswspace(unsigned short c) { return rt_isspace(c); }
X int iswupper(unsigned short c) { return c >= 'A' && c <= 'Z'; }
X int iswlower(unsigned short c) { return c >= 'a' && c <= 'z'; }
X int iswalnum(unsigned short c) { return rt_isalpha(c) || rt_isdigit(c); }
X int iswpunct(unsigned short c) { return c > 32 && c < 127 && !rt_isalpha(c) && !rt_isdigit(c); }
X size_t mbstowcs(WCHAR *d, const char *s, size_t n) {
  size_t i = 0;

  for (; s[i] != 0 && (d == 0 || i < n); ++i) {
    if (d != 0) {
      d[i] = (unsigned char)s[i];
    }
  }
  if (d != 0 && i < n) {
    d[i] = 0;
  }
  return i;
}
X size_t wcstombs(char *d, const WCHAR *s, size_t n) {
  size_t i = 0;

  for (; s[i] != 0 && (d == 0 || i < n); ++i) {
    if (d != 0) {
      d[i] = s[i] < 256 ? (char)s[i] : '?';
    }
  }
  if (d != 0 && i < n) {
    d[i] = 0;
  }
  return i;
}
int atoi(const char *s);
X int _wtoi(const WCHAR *s) {
  char b[40];
  size_t i = 0;

  for (; s[i] != 0 && i < 39; ++i) {
    b[i] = (char)s[i];
  }
  b[i] = 0;
  return atoi(b);
}
int atoi(const char *s);

// ---- ctype and locale ------------------------------------------------------------------------------------------------------------------

static unsigned short g_ctype[257];
static int g_ctype_ready;

static void ctype_init(void) {
  int c;

  if (g_ctype_ready) {
    return;
  }
  for (c = 0; c < 256; ++c) {
    unsigned short f = 0;

    if (c >= 'A' && c <= 'Z') { f |= 0x1 | 0x100; }
    if (c >= 'a' && c <= 'z') { f |= 0x2 | 0x100; }
    if (c >= '0' && c <= '9') { f |= 0x4; }
    if (c == ' ' || (c >= 9 && c <= 13)) { f |= 0x8; }
    if (c == ' ' || c == 9) { f |= 0x40; }
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) { f |= 0x10; }
    if (c < 32 || c == 127) { f |= 0x20; }
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) { f |= 0x80; }
    g_ctype[c + 1] = f;
  }
  g_ctype_ready = 1;
}

X const unsigned short *__pctype_func(void) { ctype_init(); return &g_ctype[1]; }
X const unsigned short *__pwctype_func(void) { ctype_init(); return &g_ctype[1]; }
X int ___mb_cur_max_func(void) { return 1; }
X int ___lc_codepage_func(void) { return 1252; }
X int __mb_cur_max_dummy_unused(void) { return 1; }

static int cflag(int c, unsigned short mask) { ctype_init(); return c >= -1 && c <= 255 ? (g_ctype[c + 1] & mask) != 0 : 0; }
X int isalpha(int c) { return cflag(c, 0x100); }
X int isupper(int c) { return cflag(c, 0x1); }
X int islower(int c) { return cflag(c, 0x2); }
X int isdigit(int c) { return cflag(c, 0x4); }
X int isxdigit(int c) { return cflag(c, 0x80); }
X int isspace(int c) { return cflag(c, 0x8); }
X int ispunct(int c) { return cflag(c, 0x10); }
X int isalnum(int c) { return cflag(c, 0x100 | 0x4); }
X int isprint(int c) { return c >= 32 && c < 127; }
X int isgraph(int c) { return c > 32 && c < 127; }
X int iscntrl(int c) { return cflag(c, 0x20); }
X int isblank(int c) { return cflag(c, 0x40); }
X int isascii(int c) { return (unsigned)c < 128; }
X int __isascii(int c) { return (unsigned)c < 128; }
X int __iscsymf(int c) { return rt_isalpha(c) || c == '_'; }
X int __iscsym(int c) { return rt_isalpha(c) || rt_isdigit(c) || c == '_'; }
X int toupper(int c) { return rt_toupper(c); }
X int tolower(int c) { return rt_tolower(c); }
X int _toupper(int c) { return c - 32; }
X int _tolower(int c) { return c + 32; }
X int __toascii(int c) { return c & 0x7F; }
X int _isctype(int c, int mask) { return cflag(c, (unsigned short)mask); }

struct lconv_ {
  char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep,
      *mon_grouping, *positive_sign, *negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
};
X struct lconv_ *localeconv(void) {
  static struct lconv_ lc;
  static char dot[] = ".", empty[] = "";

  lc.decimal_point = dot;
  lc.thousands_sep = lc.grouping = lc.int_curr_symbol = lc.currency_symbol = lc.mon_decimal_point = empty;
  lc.mon_thousands_sep = lc.mon_grouping = lc.positive_sign = lc.negative_sign = empty;
  lc.int_frac_digits = lc.frac_digits = lc.p_cs_precedes = lc.p_sep_by_space = lc.n_cs_precedes = lc.n_sep_by_space = lc.p_sign_posn = lc.n_sign_posn = 127;
  return &lc;
}
X char *setlocale(int cat, const char *loc) {
  (void)cat;
  if (loc == 0 || loc[0] == 0 || (loc[0] == 'C' && loc[1] == 0)) {
    return "C";
  }
  return 0;
}

// ---- stdlib: random numbers, sorting, searching ------------------------------------------------------------------------------------------

static unsigned int g_seed = 1;
X void srand(unsigned int s) { g_seed = s; }
X int rand(void) { g_seed = g_seed * 214013u + 2531011u; return (int)((g_seed >> 16) & 0x7FFF); }
X int rand_s(unsigned int *v) { *v = ((unsigned)rand() << 17) ^ ((unsigned)rand() << 8) ^ (unsigned)rand(); return 0; }

static void swap_bytes(char *a, char *b, size_t w) {
  while (w-- != 0) {
    char t = *a;

    *a++ = *b;
    *b++ = t;
  }
}

typedef int (__cdecl *cmp_fn)(const void *, const void *);
typedef int (__cdecl *cmps_fn)(void *, const void *, const void *);

static void sort_core(char *base, size_t n, size_t w, cmps_fn cmp, void *ctx, cmp_fn cmp2) {
  size_t i, parent, child, end;

#define LESS(a, b) ((cmp2 != 0 ? cmp2((a), (b)) : cmp(ctx, (a), (b))) < 0)
  if (n < 2 || w == 0) {
    return;
  }
  for (i = n / 2; i-- > 0;) {                       // build a max heap
    parent = i;
    for (;;) {
      child = 2 * parent + 1;
      if (child >= n) {
        break;
      }
      if (child + 1 < n && LESS(base + child * w, base + (child + 1) * w)) {
        ++child;
      }
      if (!LESS(base + parent * w, base + child * w)) {
        break;
      }
      swap_bytes(base + parent * w, base + child * w, w);
      parent = child;
    }
  }
  for (end = n - 1; end > 0; --end) {
    swap_bytes(base, base + end * w, w);
    parent = 0;
    for (;;) {
      child = 2 * parent + 1;
      if (child >= end) {
        break;
      }
      if (child + 1 < end && LESS(base + child * w, base + (child + 1) * w)) {
        ++child;
      }
      if (!LESS(base + parent * w, base + child * w)) {
        break;
      }
      swap_bytes(base + parent * w, base + child * w, w);
      parent = child;
    }
  }
#undef LESS
}

X void qsort(void *base, size_t n, size_t w, cmp_fn cmp) { sort_core((char *)base, n, w, 0, 0, cmp); }
X void qsort_s(void *base, size_t n, size_t w, cmps_fn cmp, void *ctx) { sort_core((char *)base, n, w, cmp, ctx, 0); }

X void *bsearch(const void *key, const void *base, size_t n, size_t w, cmp_fn cmp) {
  size_t lo = 0, hi = n;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const char *p = (const char *)base + mid * w;
    int r = cmp(key, p);

    if (r == 0) {
      return (void *)p;
    }
    if (r < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

// ---- time ----------------------------------------------------------------------------------------------------------------------------------------

struct tm_ { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };

static long long now_unix(void) {
  FILETIME ft;
  unsigned long long t;

  GetSystemTimeAsFileTime(&ft);
  t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (long long)((t - 116444736000000000ull) / 10000000ull);
}

X long long _time64(long long *out) { long long t = now_unix(); if (out != 0) { *out = t; } return t; }
X int32_t _time32(int32_t *out) { int32_t t = (int32_t)now_unix(); if (out != 0) { *out = t; } return t; }
X long long time(long long *out) { return _time64(out); }
X int32_t clock(void) { return (int32_t)GetTickCount(); }
X double _difftime64(long long a, long long b) { return (double)(a - b); }

static long long days_from_civil(long long y, unsigned m, unsigned d) {
  long long era;
  unsigned yoe, doy, doe;

  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d) {
  long long era;
  unsigned doe, yoe, doy, mp;

  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  *y = (long long)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  if (*m <= 2) {
    ++*y;
  }
}

static int fill_tm(struct tm_ *tm, long long t) {
  long long days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400), rem, y;
  unsigned m, d;

  if (t < 0 && t < -43200LL * 365 * 1000) {
    return 0;
  }
  rem = t - days * 86400;
  civil_from_days(days, &y, &m, &d);
  tm->tm_sec = (int)(rem % 60);
  tm->tm_min = (int)(rem / 60 % 60);
  tm->tm_hour = (int)(rem / 3600);
  tm->tm_mday = (int)d;
  tm->tm_mon = (int)m - 1;
  tm->tm_year = (int)(y - 1900);
  tm->tm_wday = (int)(((days % 7) + 11) % 7);               // 1970-01-01 was a Thursday
  tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
  tm->tm_isdst = 0;
  return 1;
}

static struct tm_ g_tm;
X struct tm_ *_gmtime64(const long long *t) { return fill_tm(&g_tm, *t) ? &g_tm : 0; }
X struct tm_ *_localtime64(const long long *t) { return fill_tm(&g_tm, *t) ? &g_tm : 0; }
X int _gmtime64_s(struct tm_ *tm, const long long *t) { return fill_tm(tm, *t) ? 0 : RT_EINVAL; }
X int _localtime64_s(struct tm_ *tm, const long long *t) { return fill_tm(tm, *t) ? 0 : RT_EINVAL; }
X struct tm_ *_gmtime32(const int32_t *t) { long long v = *t; return _gmtime64(&v); }
X struct tm_ *_localtime32(const int32_t *t) { long long v = *t; return _gmtime64(&v); }

X long long _mktime64(struct tm_ *tm) {
  long long y = tm->tm_year + 1900LL, mon = tm->tm_mon, days;

  y += mon / 12;
  mon %= 12;
  if (mon < 0) {
    mon += 12;
    --y;
  }
  days = days_from_civil(y, (unsigned)mon + 1, 1) + (tm->tm_mday - 1);
  {
    long long t = days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;

    fill_tm(tm, t);
    return t;
  }
}
X int32_t _mktime32(struct tm_ *tm) { return (int32_t)_mktime64(tm); }
X long long mktime(struct tm_ *tm) { return _mktime64(tm); }

static const char *const g_days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const g_months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

typedef struct { char *buf; size_t cap, pos; } tbuf_t;
static void tb_put(void *ctx, const char *s, size_t n) {
  tbuf_t *b = (tbuf_t *)ctx;

  while (n-- != 0) {
    if (b->pos < b->cap) {
      b->buf[b->pos] = *s;
    }
    ++b->pos;
    ++s;
  }
}
static void tb_fmt(tbuf_t *b, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  rt_format(tb_put, b, fmt, ap, 0);
  va_end(ap);
}

X size_t strftime(char *out, size_t max, const char *fmt, const struct tm_ *tm) {
  tbuf_t b;

  b.buf = out;
  b.cap = max;
  b.pos = 0;
  for (; *fmt != 0; ++fmt) {
    if (*fmt != '%') {
      tb_put(&b, fmt, 1);
      continue;
    }
    switch (*++fmt) {
      case 'Y': tb_fmt(&b, "%d", tm->tm_year + 1900); break;
      case 'y': tb_fmt(&b, "%02d", (tm->tm_year + 1900) % 100); break;
      case 'm': tb_fmt(&b, "%02d", tm->tm_mon + 1); break;
      case 'd': tb_fmt(&b, "%02d", tm->tm_mday); break;
      case 'e': tb_fmt(&b, "%2d", tm->tm_mday); break;
      case 'H': tb_fmt(&b, "%02d", tm->tm_hour); break;
      case 'I': tb_fmt(&b, "%02d", tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12); break;
      case 'M': tb_fmt(&b, "%02d", tm->tm_min); break;
      case 'S': tb_fmt(&b, "%02d", tm->tm_sec); break;
      case 'p': tb_fmt(&b, "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
      case 'j': tb_fmt(&b, "%03d", tm->tm_yday + 1); break;
      case 'a': tb_fmt(&b, "%.3s", g_days[tm->tm_wday % 7]); break;
      case 'A': tb_fmt(&b, "%s", g_days[tm->tm_wday % 7]); break;
      case 'b': case 'h': tb_fmt(&b, "%.3s", g_months[tm->tm_mon % 12]); break;
      case 'B': tb_fmt(&b, "%s", g_months[tm->tm_mon % 12]); break;
      case 'w': tb_fmt(&b, "%d", tm->tm_wday); break;
      case 'F': tb_fmt(&b, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
      case 'T': case 'X': tb_fmt(&b, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
      case 'D': case 'x': tb_fmt(&b, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
      case 'c': tb_fmt(&b, "%.3s %.3s %2d %02d:%02d:%02d %d", g_days[tm->tm_wday % 7], g_months[tm->tm_mon % 12], tm->tm_mday,
                       tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
      case 'Z': tb_fmt(&b, "UTC"); break;
      case 'z': tb_fmt(&b, "+0000"); break;
      case '%': tb_put(&b, "%", 1); break;
      case 0: --fmt; break;
      default: tb_put(&b, fmt - 1, 2); break;
    }
  }
  if (b.pos >= max) {
    if (max != 0) {
      out[0] = 0;
    }
    return 0;
  }
  out[b.pos] = 0;
  return b.pos;
}

X char *asctime(const struct tm_ *tm) {
  static char buf[32];

  strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y\n", tm);
  return buf;
}
X char *_ctime64(const long long *t) { struct tm_ *tm = _localtime64(t); return tm != 0 ? asctime(tm) : 0; }
X void _tzset(void) { }
static long g_timezone;
static int g_daylight;
X long *__p__timezone(void) { return &g_timezone; }
X int *__p__daylight(void) { return &g_daylight; }

// ---- stdio ----------------------------------------------------------------------------------------------------------------------------------------

struct cfile {
  HANDLE h;
  int readable, writable, append, unbuf, eof, err, ungot, is_std;
  unsigned char *rbuf;
  int rpos, rlen;
  unsigned char *wbuf;
  int wlen;
};

static rtlock_t g_crt_lock;                 // stdio: the stream table, the buffers and the file position
#define FBUF 4096
static cfile_t g_std[3];
static cfile_t *g_open[64];
static int g_std_ready;

static void std_init(void) {
  int i;

  if (g_std_ready) {
    return;
  }
  g_std_ready = 1;
  for (i = 0; i < 3; ++i) {
    g_std[i].h = GetStdHandle(i == 0 ? STD_INPUT_HANDLE : (i == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE));
    g_std[i].readable = i == 0;
    g_std[i].writable = i != 0;
    g_std[i].unbuf = 1;
    g_std[i].ungot = -1;
    g_std[i].is_std = 1;
  }
}

X void *__acrt_iob_func(unsigned i) { std_init(); return i < 3 ? &g_std[i] : 0; }

static int flush_w(cfile_t *f) {
  int ok = 1;

  if (f->wlen > 0) {
    DWORD done = 0;

    if (f->append) {
      SetFilePointer(f->h, 0, 0, FILE_END);
    }
    if (!WriteFile(f->h, f->wbuf, (DWORD)f->wlen, &done, 0) || done != (DWORD)f->wlen) {
      f->err = 1;
      ok = 0;
    }
    f->wlen = 0;
  }
  return ok;
}

// Before reading after writing, and the other way round: put the file position where the
// program thinks it is.
static void drop_rbuf(cfile_t *f) {
  if (f->rlen - f->rpos > 0 && !f->is_std) {
    SetFilePointer(f->h, -(LONG)(f->rlen - f->rpos), 0, FILE_CURRENT);
  }
  f->rpos = f->rlen = 0;
}

static void flush_all(void) {
  int i;

  std_init();
  for (i = 0; i < 64; ++i) {
    if (g_open[i] != 0) {
      flush_w(g_open[i]);
    }
  }
  for (i = 1; i < 3; ++i) {
    flush_w(&g_std[i]);
  }
}

X int fflush(cfile_t *f) {
  int r = 0;

  rt_lock(&g_crt_lock);
  if (f == 0) {
    flush_all();
  } else {
    r = flush_w(f) ? 0 : -1;
  }
  rt_unlock(&g_crt_lock);
  return r;
}
X int _flushall(void) { flush_all(); return 0; }

static size_t f_write_u(cfile_t *f, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  size_t left = n;

  if (!f->writable) {
    f->err = 1;
    rt_errno = RT_EBADF;
    return 0;
  }
  if (f->rlen != 0) {
    drop_rbuf(f);
  }
  if (f->unbuf) {
    DWORD done = 0;

    if (n != 0 && (!WriteFile(f->h, p, (DWORD)n, &done, 0) || done != n)) {
      f->err = 1;
      return done;
    }
    return n;
  }
  while (left > 0) {
    size_t room = FBUF - (size_t)f->wlen, k = left < room ? left : room;

    memcpy(f->wbuf + f->wlen, p, k);
    f->wlen += (int)k;
    p += k;
    left -= k;
    if (f->wlen == FBUF && !flush_w(f)) {
      break;
    }
  }
  return n - left;
}

static size_t f_write(cfile_t *f, const void *buf, size_t n) {
  size_t r;

  rt_lock(&g_crt_lock);
  r = f_write_u(f, buf, n);
  rt_unlock(&g_crt_lock);
  return r;
}

static int f_fill(cfile_t *f) {
  DWORD got = 0;

  if (f->wlen != 0) {
    flush_w(f);
  }
  if (f->rbuf == 0) {
    f->rbuf = (unsigned char *)malloc(FBUF);
    if (f->rbuf == 0) {
      f->err = 1;
      return 0;
    }
  }
  f->rpos = f->rlen = 0;
  if (!ReadFile(f->h, f->rbuf, FBUF, &got, 0) || got == 0) {
    f->eof = 1;
    return 0;
  }
  f->rlen = (int)got;
  return 1;
}

static int f_getc_u(cfile_t *f) {
  if (!f->readable) {
    f->err = 1;
    return -1;
  }
  if (f->ungot >= 0) {
    int c = f->ungot;

    f->ungot = -1;
    return c;
  }
  if (f->rpos >= f->rlen && !f_fill(f)) {
    return -1;
  }
  return f->rbuf[f->rpos++];
}

static int f_getc(cfile_t *f) {
  int r;

  rt_lock(&g_crt_lock);
  r = f_getc_u(f);
  rt_unlock(&g_crt_lock);
  return r;
}

X int fgetc(cfile_t *f) { return f_getc(f); }
X int getc(cfile_t *f) { return f_getc(f); }
X int getchar(void) { std_init(); return f_getc(&g_std[0]); }
X int ungetc(int c, cfile_t *f) {
  if (c < 0 || f->ungot >= 0) {
    return -1;
  }
  f->ungot = c & 255;
  f->eof = 0;
  return c & 255;
}
X int fputc(int c, cfile_t *f) { unsigned char ch = (unsigned char)c; return f_write(f, &ch, 1) == 1 ? ch : -1; }
X int putc(int c, cfile_t *f) { return fputc(c, f); }
X int putchar(int c) { std_init(); return fputc(c, &g_std[1]); }
X int fputs(const char *s, cfile_t *f) { size_t n = rt_strlen(s); return f_write(f, s, n) == n ? 0 : -1; }
X int puts(const char *s) {
  std_init();
  if (fputs(s, &g_std[1]) < 0) {
    return -1;
  }
  return fputc('\n', &g_std[1]) < 0 ? -1 : 0;
}
X char *fgets(char *buf, int n, cfile_t *f) {
  int i = 0;

  if (n <= 0) {
    return 0;
  }
  while (i < n - 1) {
    int c = f_getc(f);

    if (c < 0) {
      break;
    }
    buf[i++] = (char)c;
    if (c == '\n') {
      break;
    }
  }
  if (i == 0) {
    return 0;
  }
  buf[i] = 0;
  return buf;
}
X size_t fread(void *buf, size_t size, size_t n, cfile_t *f) {
  size_t total = size * n, got = 0;
  char *p = (char *)buf;

  if (size == 0 || n == 0) {
    return 0;
  }
  while (got < total) {
    int c = f_getc(f);

    if (c < 0) {
      break;
    }
    p[got++] = (char)c;
  }
  return got / size;
}
X size_t fwrite(const void *buf, size_t size, size_t n, cfile_t *f) {
  if (size == 0 || n == 0) {
    return 0;
  }
  return f_write(f, buf, size * n) / size;
}
X int feof(cfile_t *f) { return f->eof; }
X int ferror(cfile_t *f) { return f->err; }
X void clearerr(cfile_t *f) { f->eof = f->err = 0; }
X int setvbuf(cfile_t *f, char *b, int mode, size_t size) { (void)b; (void)size; if (mode == 4 /* _IONBF */) { f->unbuf = 1; } return 0; }
X void setbuf(cfile_t *f, char *b) { (void)f; (void)b; }
X int _lock_file(cfile_t *f) { (void)f; return 0; }
X int _unlock_file(cfile_t *f) { (void)f; return 0; }
X void _lock_file_v(cfile_t *f) { (void)f; }
X int _fgetc_nolock(cfile_t *f) { return f_getc(f); }
X int _fputc_nolock(int c, cfile_t *f) { return fputc(c, f); }
X size_t _fread_nolock(void *b, size_t s, size_t n, cfile_t *f) { return fread(b, s, n, f); }
X size_t _fwrite_nolock(const void *b, size_t s, size_t n, cfile_t *f) { return fwrite(b, s, n, f); }
X int _fflush_nolock(cfile_t *f) { return fflush(f); }

static long long f_tell_u(cfile_t *f) {
  LARGE_INTEGER zero, pos;

  if (f->is_std) {
    return -1;
  }
  flush_w(f);
  zero.QuadPart = 0;
  if (!SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT)) {
    return -1;
  }
  return pos.QuadPart - (f->rlen - f->rpos) - (f->ungot >= 0 ? 1 : 0);
}

static int f_seek_u(cfile_t *f, long long off, int whence) {
  LARGE_INTEGER d, np;

  if (f->is_std) {
    return -1;
  }
  flush_w(f);
  if (whence == 1) {
    off -= (f->rlen - f->rpos) + (f->ungot >= 0 ? 1 : 0);
  }
  f->rpos = f->rlen = 0;
  f->ungot = -1;
  f->eof = 0;
  d.QuadPart = off;
  return SetFilePointerEx(f->h, d, &np, whence == 0 ? FILE_BEGIN : (whence == 1 ? FILE_CURRENT : FILE_END)) ? 0 : -1;
}

static long long f_tell(cfile_t *f) {
  long long r;

  rt_lock(&g_crt_lock);
  r = f_tell_u(f);
  rt_unlock(&g_crt_lock);
  return r;
}

static int f_seek(cfile_t *f, long long off, int whence) {
  int r;

  rt_lock(&g_crt_lock);
  r = f_seek_u(f, off, whence);
  rt_unlock(&g_crt_lock);
  return r;
}

X int fseek(cfile_t *f, int32_t off, int whence) { return f_seek(f, off, whence); }
X int _fseeki64(cfile_t *f, long long off, int whence) { return f_seek(f, off, whence); }
X int32_t ftell(cfile_t *f) { return (int32_t)f_tell(f); }
X long long _ftelli64(cfile_t *f) { return f_tell(f); }
X void rewind(cfile_t *f) { f_seek(f, 0, 0); f->err = 0; }

static int fclose_u(cfile_t *f) {
  int i, r = 0;

  if (f == 0) {
    return -1;
  }
  r = flush_w(f) ? 0 : -1;
  if (f->is_std) {
    return r;
  }
  CloseHandle(f->h);
  for (i = 0; i < 64; ++i) {
    if (g_open[i] == f) {
      g_open[i] = 0;
    }
  }
  free(f->rbuf);
  free(f->wbuf);
  free(f);
  return r;
}

X int fclose(cfile_t *f) {
  int r;

  rt_lock(&g_crt_lock);
  r = fclose_u(f);
  rt_unlock(&g_crt_lock);
  return r;
}

static cfile_t *fopen_u(const char *path, const char *mode) {
  cfile_t *f;
  int plus = 0, i;
  DWORD access, disp;
  char m = mode[0];

  for (i = 1; mode[i] != 0; ++i) {
    if (mode[i] == '+') {
      plus = 1;
    }
  }
  if (m == 'r') {
    access = GENERIC_READ | (plus ? GENERIC_WRITE : 0);
    disp = OPEN_EXISTING;
  } else if (m == 'w') {
    access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
    disp = CREATE_ALWAYS;
  } else if (m == 'a') {
    access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
    disp = OPEN_ALWAYS;
  } else {
    rt_errno = RT_EINVAL;
    return 0;
  }
  f = (cfile_t *)calloc(1, sizeof(*f));
  if (f == 0) {
    return 0;
  }
  f->h = CreateFileA(path, access, FILE_SHARE_READ, 0, disp, FILE_ATTRIBUTE_NORMAL, 0);
  if (f->h == INVALID_HANDLE_VALUE) {
    rt_errno = RT_ENOENT;
    free(f);
    return 0;
  }
  f->readable = m == 'r' || plus;
  f->writable = m != 'r' || plus;
  f->append = m == 'a';
  f->ungot = -1;
  if (f->writable) {
    f->wbuf = (unsigned char *)malloc(FBUF);
    if (f->wbuf == 0) {
      CloseHandle(f->h);
      free(f);
      return 0;
    }
  }
  if (f->append) {
    SetFilePointer(f->h, 0, 0, FILE_END);
  }
  for (i = 0; i < 64; ++i) {
    if (g_open[i] == 0) {
      g_open[i] = f;
      break;
    }
  }
  return f;
}

X cfile_t *fopen(const char *path, const char *mode) {
  cfile_t *f;

  rt_lock(&g_crt_lock);
  f = fopen_u(path, mode);
  rt_unlock(&g_crt_lock);
  return f;
}

X int fopen_s(cfile_t **out, const char *path, const char *mode) {
  *out = fopen(path, mode);
  return *out != 0 ? 0 : rt_errno;
}

X int remove(const char *path) { return DeleteFileA(path) ? 0 : (rt_errno = RT_ENOENT, -1); }
X int _unlink(const char *path) { return remove(path); }
X void perror(const char *s) {
  std_init();
  if (s != 0 && s[0] != 0) {
    fputs(s, &g_std[2]);
    fputs(": ", &g_std[2]);
  }
  fputs(strerror(rt_errno), &g_std[2]);
  fputc('\n', &g_std[2]);
}

X int _fileno(cfile_t *f) {
  std_init();
  return f->is_std ? (int)(f - g_std) : 3;
}
X int _isatty(int fd) { return fd >= 0 && fd < 3; }
X int _setmode(int fd, int mode) { (void)fd; (void)mode; return 0x4000; }
X int _write(int fd, const void *buf, unsigned n) {
  std_init();
  return fd >= 1 && fd <= 2 ? (int)f_write(&g_std[fd], buf, n) : -1;
}
X int _read(int fd, void *buf, unsigned n) {
  DWORD got = 0;

  std_init();
  return fd == 0 && ReadFile(g_std[0].h, buf, n, &got, 0) ? (int)got : -1;
}

// ---- printf and scanf glue ------------------------------------------------------------------------------------------------------------------

static void file_put(void *ctx, const char *s, size_t n) { f_write((cfile_t *)ctx, s, n); }

X int __stdio_common_vfprintf(unsigned long long opt, cfile_t *f, const char *fmt, void *loc, va_list ap) {
  (void)opt; (void)loc;
  if (f == 0 || fmt == 0) {
    rt_errno = RT_EINVAL;
    return -1;
  }
  return rt_format(file_put, f, fmt, ap, 0);
}
X int __stdio_common_vfprintf_s(unsigned long long opt, cfile_t *f, const char *fmt, void *loc, va_list ap) {
  return __stdio_common_vfprintf(opt, f, fmt, loc, ap);
}
X int __stdio_common_vfprintf_p(unsigned long long opt, cfile_t *f, const char *fmt, void *loc, va_list ap) {
  return __stdio_common_vfprintf(opt, f, fmt, loc, ap);
}

typedef struct { char *buf; size_t limit, pos; } sbuf_t;
static void sb_put(void *ctx, const char *s, size_t n) {
  sbuf_t *b = (sbuf_t *)ctx;

  while (n-- != 0) {
    if (b->pos < b->limit) {
      b->buf[b->pos] = *s;
    }
    ++b->pos;
    ++s;
  }
}

X int __stdio_common_vsprintf(unsigned long long opt, char *buf, size_t count, const char *fmt, void *loc, va_list ap) {
  sbuf_t sb;
  int total;
  int standard = (opt & 2) != 0;

  (void)loc;
  if (fmt == 0 || (buf == 0 && count != 0)) {
    rt_errno = RT_EINVAL;
    return -1;
  }
  sb.buf = buf;
  sb.pos = 0;
  sb.limit = count == 0 ? 0 : (standard ? count - 1 : count);
  total = rt_format(sb_put, &sb, fmt, ap, 0);
  if (standard) {
    if (count != 0) {
      buf[(size_t)total < count - 1 ? (size_t)total : count - 1] = 0;
    }
    return total;
  }
  if ((size_t)total < count) {
    buf[total] = 0;
    return total;
  }
  if ((opt & 1) && count != 0) {
    buf[count - 1] = 0;
  }
  return -1;
}

X int __stdio_common_vsprintf_s(unsigned long long opt, char *buf, size_t count, const char *fmt, void *loc, va_list ap) {
  sbuf_t sb;
  int total;

  (void)opt; (void)loc;
  if (buf == 0 || count == 0 || fmt == 0) {
    rt_errno = RT_EINVAL;
    return -1;
  }
  sb.buf = buf;
  sb.pos = 0;
  sb.limit = count - 1;
  total = rt_format(sb_put, &sb, fmt, ap, 0);
  if ((size_t)total >= count) {
    buf[0] = 0;
    rt_errno = RT_ERANGE;
    return -1;
  }
  buf[total] = 0;
  return total;
}

X int __stdio_common_vsnprintf_s(unsigned long long opt, char *buf, size_t size, size_t maxcount, const char *fmt, void *loc, va_list ap) {
  sbuf_t sb;
  int total;
  size_t limit;

  (void)opt; (void)loc;
  if (buf == 0 || size == 0 || fmt == 0) {
    rt_errno = RT_EINVAL;
    return -1;
  }
  limit = maxcount == (size_t)-1 ? size - 1 : maxcount;
  if (limit > size - 1) {
    limit = size - 1;
  }
  sb.buf = buf;
  sb.pos = 0;
  sb.limit = limit;
  total = rt_format(sb_put, &sb, fmt, ap, 0);
  buf[(size_t)total < limit ? (size_t)total : limit] = 0;
  return (size_t)total > limit && maxcount == (size_t)-1 ? -1 : (int)((size_t)total < limit ? (size_t)total : limit);
}

// wide printf: the format is narrowed, formatted, and the result widened again.
typedef struct { char *buf; size_t len, cap; } dstr_t;
static void ds_put(void *ctx, const char *s, size_t n) {
  dstr_t *d = (dstr_t *)ctx;

  if (d->len + n + 1 > d->cap) {
    size_t nc = d->cap == 0 ? 128 : d->cap;

    while (nc < d->len + n + 1) {
      nc *= 2;
    }
    d->buf = (char *)realloc(d->buf, nc);
    d->cap = nc;
  }
  if (d->buf != 0) {
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    d->buf[d->len] = 0;
  }
}

static char *wfmt_narrow(const WCHAR *w) {
  size_t n = wcslen(w), i;
  char *s = (char *)malloc(n + 1);

  if (s != 0) {
    for (i = 0; i <= n; ++i) {
      s[i] = w[i] < 256 ? (char)w[i] : '?';
    }
  }
  return s;
}

static int wformat(dstr_t *d, const WCHAR *wfmt, va_list ap) {
  char *fmt = wfmt_narrow(wfmt);
  int r;

  if (fmt == 0) {
    return -1;
  }
  d->buf = 0;
  d->len = d->cap = 0;
  ds_put(d, "", 0);
  r = rt_format(ds_put, d, fmt, ap, 1);
  free(fmt);
  return r;
}

X int __stdio_common_vswprintf(unsigned long long opt, WCHAR *buf, size_t count, const WCHAR *fmt, void *loc, va_list ap) {
  dstr_t d;
  int total = wformat(&d, fmt, ap);
  size_t i, n;

  (void)loc;
  if (total < 0 || (buf == 0 && count != 0)) {
    return -1;
  }
  n = d.len < (count == 0 ? 0 : count - 1) ? d.len : (count == 0 ? 0 : count - 1);
  for (i = 0; i < n; ++i) {
    buf[i] = (unsigned char)d.buf[i];
  }
  if (count != 0) {
    buf[n] = 0;
  }
  free(d.buf);
  if ((opt & 2) == 0 && (size_t)total >= count) {
    return -1;
  }
  return total;
}

X int __stdio_common_vfwprintf(unsigned long long opt, cfile_t *f, const WCHAR *fmt, void *loc, va_list ap) {
  dstr_t d;
  int total = wformat(&d, fmt, ap);

  (void)opt; (void)loc;
  if (total >= 0 && d.buf != 0) {
    f_write(f, d.buf, d.len);
  }
  free(d.buf);
  return total;
}

typedef struct { const char *p; size_t pos, n; } sstr_t;
static int ss_get(void *ctx) { sstr_t *s = (sstr_t *)ctx; return s->pos < s->n && s->p[s->pos] != 0 ? (unsigned char)s->p[s->pos++] : -1; }
static void ss_unget(void *ctx, int c) { sstr_t *s = (sstr_t *)ctx; (void)c; if (s->pos > 0) { --s->pos; } }
static int sf_get(void *ctx) { return f_getc((cfile_t *)ctx); }
static void sf_unget(void *ctx, int c) { ungetc(c, (cfile_t *)ctx); }

X int __stdio_common_vsscanf(unsigned long long opt, const char *buf, size_t len, const char *fmt, void *loc, va_list ap) {
  sstr_t ss;
  scan_src_t src;

  (void)opt; (void)loc;
  ss.p = buf;
  ss.pos = 0;
  ss.n = len;
  src.get = ss_get;
  src.unget = ss_unget;
  src.ctx = &ss;
  return rt_scan(&src, fmt, ap);
}

X int __stdio_common_vfscanf(unsigned long long opt, cfile_t *f, const char *fmt, void *loc, va_list ap) {
  scan_src_t src;

  (void)opt; (void)loc;
  src.get = sf_get;
  src.unget = sf_unget;
  src.ctx = f;
  return rt_scan(&src, fmt, ap);
}
