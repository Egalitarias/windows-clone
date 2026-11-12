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
#define KS_WIN_COPY    37
#define KS_WIN_READ    38
#define KS_THREAD_CREATE 33
#define KS_THREAD_EXIT 34
#define KS_FUTEX_WAIT  35
#define KS_FUTEX_WAKE  36

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

// ---- threads ----
// The thread block (TEB) the kernel builds for every thread is at gs:[0x30]; the fields this runtime
// uses: 0x48 thread id, 0x68 last error, 0x300 start routine and 0x308 argument (a new thread),
// 0x310 C errno, 0x318 the thread's object (threads.c), 0xC00 the 64 TLS slots (TlsAlloc).
static inline char *rt_teb(void) {
  char *t;

  __asm__("movq %%gs:0x30, %0" : "=r"(t));
  return t;
}
static inline DWORD rt_tid(void) {
  DWORD t;

  __asm__("movl %%gs:0x48, %0" : "=r"(t));
  return t;
}

// A lock for the runtime's own data: recursive, and waiting yields the CPU (no blocking calls
// are made while holding one).
typedef struct {
  volatile LONG state;
  volatile DWORD owner;
  int depth;
} rtlock_t;

static inline void rt_lock(rtlock_t *l) {
  DWORD me = rt_tid();

  if (l->state != 0 && l->owner == me) {
    ++l->depth;
    return;
  }
  while (__sync_lock_test_and_set(&l->state, 1) != 0) {
    sc3(KS_YIELD, 0, 0, 0);
  }
  l->owner = me;
  l->depth = 1;
}

static inline void rt_unlock(rtlock_t *l) {
  if (--l->depth == 0) {
    l->owner = 0;
    __sync_lock_release(&l->state);
  }
}

// Handles for kernel-object-like things (threads, events, mutexes, semaphores): see threads.c.
enum { H_FREE = 0, H_CONIN, H_CONOUT, H_CONERR, H_FILE, H_FIND, H_THREAD, H_EVENT, H_MUTEX, H_SEM };
HANDLE rt_handle_new(int kind, void *obj);
void *rt_handle_obj(HANDLE h, int *kind);
void rt_handle_free(HANDLE h);
void rt_obj_close(int kind, void *obj);

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
uint64_t rt_exe_base(void);

// ---- user32.c / gdi32.c ----
// A bitmap (bitmap.c): 0x00RRGGBB pixels, top row first unless `flip` (a bottom-up DIB section).
typedef struct bmp {
  int used;
  int w, h;
  int flip;                      // row y is stored at h-1-y (the memory of a bottom-up DIB section)
  int mono;                      // a monochrome bitmap: pixels are 0 or 0xFFFFFF and draw in the DC's text / background colours
  int dib;                       // a DIB section: `pix` is shared with the program
  uint32_t *pix;
} bmp_t;

// A device context: a window's surface (sysid), or with sysid < 0 a memory DC drawing into `bmp`, plus the drawing state.
typedef struct {
  int used;
  int sysid;
  COLORREF text, bk;
  int bkmode, align;
  HGDIOBJ pen, brush, font;
  int px, py;                    // current position (MoveToEx / LineTo)
  int ox, oy;                    // where the window's (0,0) is on the surface (a child window's place)
  int cl, ct, cr, cb;            // clip rectangle, in surface coordinates
  bmp_t *bmp;                    // memory DC: the selected bitmap
} dc_t;

dc_t *gdi_dc_alloc(int sysid, int ox, int oy, const RECT *clip);
void gdi_dc_free(dc_t *dc);
dc_t *gdi_dc(HDC h);
COLORREF gdi_sys_color(int index);
HBRUSH gdi_sys_brush(int index);
unsigned long rt_colorref_to_rgb(COLORREF c);
void gdi_fill(dc_t *d, int x, int y, int w, int h, uint32_t rgb);
void gdi_text(dc_t *d, int x, int y, const char *s, int n, uint32_t rgb);
int gdi_brush_rgb(HBRUSH b, unsigned long *rgb);

// ---- bitmap.c: bitmaps, memory DCs, BitBlt/StretchBlt, DIBs ----
bmp_t *rt_bmp(HGDIOBJ h);                                           // 0 if h is not a bitmap
int rt_bmp_delete(HGDIOBJ h);                                       // 1 if h was a bitmap (deleted unless still selected)
HGDIOBJ rt_bmp_select(dc_t *d, HGDIOBJ h);                          // SelectObject of a bitmap; 0 if not a memory DC
void rt_bmp_dc_gone(dc_t *d);                                       // a memory DC is deleted
int rt_bmp_getobject(HGDIOBJ h, int n, void *p);                    // GetObject; -1 if h is not a bitmap
void rt_mem_fill(dc_t *d, int x, int y, int x1, int y1, uint32_t rgb);       // surface coordinates, already clipped
void rt_mem_text(dc_t *d, int x, int y, const char *s, int n, uint32_t rgb);
int rt_mem_getpixel(dc_t *d, int x, int y, COLORREF *c);
void rt_win_read(int sysid, int x, int y, int w, int h, uint32_t *buf);     // read a rectangle of a window's surface
int rt_scroll_plan(const RECT *a, int dx, int dy, RECT *src, RECT *ex1, RECT *ex2);   // scrolling a: what moves, and the two strips left uncovered
HBITMAP rt_dib_to_bitmap(const BITMAPINFO *bi, const void *bits);   // bits == 0: they follow the header and colour table

// ---- menu.c: menus, the menu bar, popup tracking, accelerators ----
#define RT_MENU_H 18                  // height of the menu bar, at the top of a window's surface
int rt_menu_valid(HMENU m);
void rt_menu_destroy(HMENU m);
void rt_menu_draw_bar(HWND h);
void rt_menu_track_bar(HWND h, int x);          // the mouse went down on the bar at surface x
int rt_menu_alt_key(HWND h, unsigned ch);       // Alt+letter: opens the bar item with that mnemonic
// user32.c: for menu.c
HMENU rt_wnd_menu(HWND h);
int rt_wnd_surface(HWND h, int *sysid, int *cw, int *ch, int *mb);

// ---- scroll bars in the frame of a window (controls.c has the bar, user32.c the window) ----
#define RT_SB_THICK 17                                   // width of a vertical bar, height of a horizontal one
void rt_sb_forget(HWND h);                               // a window was created or destroyed: drop its bars' state
void rt_sb_nc_paint(HWND h, HDC dc, int bar);            // draw the bar (SB_HORZ / SB_VERT) at the DC's (0,0)
void rt_sb_nc_event(HWND h, int bar, UINT msg, int x, int y);   // WM_LBUTTONDOWN / MOUSEMOVE / LBUTTONUP, in the bar's coordinates
void rt_wnd_nc_draw(HWND h);                             // user32.c: draw the window's frame bars now
void rt_wnd_set_bars(HWND h, int vert, int horz);        // user32.c: show (1) or hide (0) the frame bars, -1 leaves one alone

// ---- controls.c: the standard controls (BUTTON, STATIC, EDIT, LISTBOX), registered as window classes ----
void rt_controls_register(void);
LRESULT CALLBACK rt_listview_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);   // listview.c
LRESULT CALLBACK rt_tooltip_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);    // tooltip.c
LRESULT CALLBACK rt_toolbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);    // toolbar.c
void rt_comctl_register(void);                       // comctl.c: progress bar, status bar, trackbar, up-down, tab control
LRESULT CALLBACK rt_dlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);        // dialog.c: the "#32770" window class
// user32.c: what a control keeps with its window.
void *rt_wnd_ctl(HWND h);
void rt_wnd_set_ctl(HWND h, void *p);
const char *rt_wnd_text(HWND h);
void rt_wnd_set_text(HWND h, const char *t);

#endif
