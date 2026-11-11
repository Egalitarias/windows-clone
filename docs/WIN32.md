# Windows (PE) programs and the Win32 runtime

WinClone runs ordinary x64 Windows executables: programs built with the real Windows SDK
headers and import libraries, written against `kernel32`, `user32` and `gdi32`. A PE loader in the
kernel maps them, and a small Win32 runtime (`WIN32.DLL`, our own code) provides the API on
top of WinClone's own syscalls. No Windows code is involved; it is a re-implementation of a
subset of the Win32 API.

```
  program.exe  ──imports──▶  KERNEL32.dll / USER32.dll / GDI32.dll   (names the loader redirects)
        │                                   │
        │ PE loader (process.c)             ▼
        ▼                              WIN32.DLL  (src/win32: kernel32.c, user32.c, gdi32.c)
  one address space:                        │  WinClone syscalls (syscall.h): write, open, win_create, win_fill, ...
  exe, WIN32.DLL, heap, stack               ▼
                                          kernel
```

## The loader

`process_create` recognises a file starting with `MZ` and `PE\0\0` and, in `src/kernel/process.c`:

- accepts PE32+ (x86-64) **executables** for the console (3) or Windows (2) subsystem; DLLs, 32-bit
  files and .NET assemblies are refused (`PROCESS_ERR_BADPE`, the kernel log says which check failed);
- maps the headers and every section at `PROCESS_LOAD_BASE`, page by page (read-only for the
  headers, code and constants, writable where the section says so), and **applies the base
  relocations**, since the file's `ImageBase` is not where it lands. An image whose relocations
  were stripped must load at its `ImageBase`, which is not possible here and is refused;
- loads `/APPS/WIN32.DLL` into the same address space right behind it, relocates it, and **binds
  the program's imports by name** to its exports. `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`,
  `ADVAPI32.dll`, `SHELL32.dll` and `COMCTL32.dll` all resolve to that one DLL, and so do the C
  runtime's names: `api-ms-win-crt-*.dll`, `api-ms-win-core-*.dll`, `ucrtbase.dll`, `VCRUNTIME140.dll`
  (and `_1`) and `msvcrt.dll` (see "The C runtime" below). An import it
  does not export is bound to a stub that ends the program with exit code `0xC0000139`; the
  kernel log lists them (`[PE] unresolved import KERNEL32.dll!Name`) so a program that never
  calls the missing function still runs. Imports by ordinal, delay-load imports, forwarders and
  other DLLs are not supported;
- gives the program a 2 MiB **heap block** at `PE_HEAP_BASE` (the runtime's `HeapAlloc` and
  `VirtualAlloc` carve it up; the `peinfo` syscall tells it where it is), a 16 KiB stack, and
  starts it with a Windows-style frame (shadow space, return address): returning from the entry
  point exits with the return value;
- builds the **thread block**: the first four pages of the heap block hold a TEB (with the
  `StackBase`/`StackLimit`, `Self`, `ThreadLocalStoragePointer` and `ProcessEnvironmentBlock`
  fields, and a small PEB with `ImageBaseAddress`) and the program's **thread-local storage**.
  The kernel loads `IA32_GS_BASE` with the TEB address whenever the task is switched in
  (`TCB.gs_base`, `cpu_set_gs_base`), so `gs:[0x30]` and `gs:[0x58]` work as on Windows. The
  `.tls` section's template is copied, `_tls_index` is set to 0, and the program's TLS callbacks
  run before its entry point. Up to 12 KiB of TLS data fit; a program with more is refused;
- enters the program through the runtime's `__winclone_start`, which runs the TLS callbacks
  and then calls the real entry point (found with `peinfo`).

Not supported: structured exception handling (an exception ends the process like any other
fault; the `.pdata` tables are ignored and `__C_specific_handler` always says "keep looking"), C++
exceptions (`_CxxThrowException` ends the program with a message), threads (`CreateThread`
fails), COM.

Each program runs in its own address space as before; a PE program next to ELF programs is
fine. The kernel keeps each task's x87/SSE state apart (FXSAVE/FXRSTOR at every task switch,
`TCB.fx_state`), so programs may use floating point and SSE freely.

## What the runtime provides

About 230 functions, with exactly the SDK's prototypes (the runtime is compiled against the SDK
headers). Highlights:

- **kernel32**: `ExitProcess`, `GetCommandLineA/W`, `GetModuleHandle*`, `LoadLibrary`/`GetProcAddress`
  (for the runtime itself); the heap (`HeapAlloc/Free/ReAlloc/Size`, `GetProcessHeap`, `LocalAlloc`,
  `GlobalAlloc`, `VirtualAlloc/Free`); files and the console (`GetStdHandle`, `ReadFile`, `WriteFile`,
  `WriteConsole*`, `CreateFileA`, `CloseHandle`, `SetFilePointer`, `GetFileSize`, `DeleteFileA`,
  `CreateDirectoryA`, `GetFileAttributesA`, `FindFirstFileA`/`FindNextFileA`); time (`GetTickCount*`,
  `QueryPerformanceCounter`, `Sleep`, `GetSystemTime`, `GetLocalTime`); locks and TLS slots (single
  thread, so locks do nothing); string helpers (`lstr*`, `MultiByteToWideChar` for ASCII); `GetLastError`.
  Paths are translated: `C:\DIR\FILE.TXT` is `/DIR/FILE.TXT` on the default volume.
- **user32**: `RegisterClass[Ex]A`, `CreateWindowExA`, `ShowWindow`, `UpdateWindow`, `DestroyWindow`,
  `GetMessage`, `PeekMessage`, `TranslateMessage`, `DispatchMessage`, `PostMessage`, `SendMessage`,
  `PostQuitMessage`, `DefWindowProc`, `InvalidateRect`, `BeginPaint`/`EndPaint`, `GetDC`/`ReleaseDC`,
  `SetTimer`/`KillTimer`, `GetClientRect`, `MoveWindow`/`SetWindowPos`, `SetWindowText`,
  `Get/SetWindowLong(Ptr)`, `GetSystemMetrics`, `GetKeyState`, `MessageBoxA` (a window of its own with
  the standard button sets), `wsprintfA`.
- **gdi32**: device contexts, solid brushes, pens, stock objects, `FillRect`, `FrameRect`, `Rectangle`,
  `Ellipse`, `MoveToEx`/`LineTo`, `Polyline`, `SetPixel`, `PatBlt`, `TextOutA`, `ExtTextOutA`, `DrawTextA`,
  `GetTextExtentPoint32A`, `GetTextMetricsA`, `SetTextColor`, `SetBkColor`, `SetBkMode`, `GetDeviceCaps`.

Windows are WinClone windows: a pixel surface per window, at most 800 x 600 client pixels, two
windows per program, drawn with the window syscalls (so there is no clipping, and text is the
built-in 8x8 font). Input events become messages: a key press gives `WM_KEYDOWN`, `WM_CHAR` and
`WM_KEYUP` together (so `TranslateMessage` does nothing), the left button gives `WM_LBUTTONDOWN/UP`
and `WM_MOUSEMOVE` (moves only while the button is held), the wheel `WM_MOUSEWHEEL`, a resize `WM_SIZE`,
the close button `WM_CLOSE`. `WM_PAINT` is generated for invalidated windows; `BeginPaint` sends
`WM_ERASEBKGND` and the default procedure fills with the class brush.

Missing on purpose (for now): child windows and the standard controls (`BUTTON`, `EDIT`, ...), menus,
dialogs, memory DCs and bitmaps (`CreateCompatibleDC`, `BitBlt`), clipboard, resources, fonts other
than the built-in one, right and middle mouse buttons, wide-character (`W`) window functions
(only `DefWindowProcW`, `GetMessageW`, ...), real wall-clock time (time counts from 2026-01-01 at
boot), environment variables, and anything about threads or sockets.

## The C runtime

Programs built the normal way, with the **dynamic C runtime (`/MD`)**, run unchanged: the
startup code the MSVC linker puts into the program (`mainCRTStartup` -> `__scrt_common_main_seh`,
from `msvcrt.lib`) calls the `ucrtbase` and `vcruntime140` functions it imports, and WIN32.DLL
provides about 400 of them (`src/win32/crt.c`, `crtfmt.c`, `crtmath.c`):

- **startup and exit**: `_configure_narrow_argv` (argv parsed from the command line, quotes and
  backslashes as Windows does), `_initterm`/`_initterm_e`, `_crt_atexit`/`atexit`, `exit`/`_exit`/
  `_cexit`, `abort`, the onexit tables, `signal`/`raise`, `_set_app_type` and friends; the
  security-cookie and `__scrt_*` code in the program itself needs only `QueryPerformanceCounter`,
  `InitializeSListHead`, `IsProcessorFeaturePresent` and similar, which `kernel32` now has;
- **memory**: `malloc`/`calloc`/`realloc`/`free`/`_msize`/`_aligned_malloc`, on the 2 MiB heap;
- **strings and characters**: the `str*`, `mem*`, `wcs*` families, `_stricmp`, `strtok_s`,
  `strcpy_s`..., `ctype` (`isalpha`, ... and the `__pctype_func` table the headers' macros use),
  `towlower`, `mbstowcs`;
- **numbers**: `strtol`/`strtoul`/`strtoll`/`strtod`/`atoi`/`atof`/`_itoa`..., with an exact
  `strtod` (hex floats, `inf`, `nan`, denormals) and an exact `printf` engine: `%d %i %u %x %X %o
  %c %s %p %f %e %g` with flags, width, precision, `*`, `hh h l ll z j t I64 I32 w`, wide strings
  (`%ls`, `%S`); doubles are printed from their exact decimal expansion and rounded to
  nearest-even. `sscanf`/`fscanf` handle `%d %i %u %x %o %c %s %[...] %f %n` and assignment
  suppression;
- **stdio**: `FILE` streams over the `CreateFileA` handles: `fopen`/`fopen_s` (`r w a`, `+`, `b`),
  `fread`/`fwrite`/`fgets`/`fputs`/`fgetc`/`ungetc`/`fseek`/`ftell`/`_fseeki64`/`fflush`/`fclose`,
  `remove`, `perror`; `stdin`/`stdout`/`stderr` on the console; the `__stdio_common_v*printf`/
  `scanf` entry points that the headers' inline `printf`, `snprintf`, `sscanf`... call;
- **math**: `sin cos tan asin acos atan atan2 sinh cosh tanh exp exp2 log log2 log10 pow sqrt
  cbrt hypot fmod floor ceil round trunc rint ldexp frexp modf fabs copysign fmin fmax`, the
  `float` versions, `_dclass`/`_fdclass`. The transcendental functions use the x87 unit with
  extended precision inside, so results are within about one unit in the last place (not always
  correctly rounded);
- **time**: `time`, `clock`, `gmtime`/`localtime` (UTC: there is no time zone), `mktime`,
  `strftime`, `asctime`; **misc**: `rand`/`srand` (the MSVC generator: `srand(1); rand()` is 41),
  `qsort`/`qsort_s`/`bsearch`, `div`, `setjmp`/`longjmp` (registers only: no unwinding),
  `_set_invalid_parameter_handler`, `getenv` (always NULL), `localeconv`, `setlocale` ("C").

Not there: `/MT` (static CRT) programs, which want hundreds of `kernel32` functions the runtime
does not have (`FlsAlloc` and `EncodePointer` are there, the locale, code page and string-type
functions are not); C++ (`msvcp140`: `std::string`, iostreams; `operator new` works through
`malloc`, but exceptions do not); wide `printf` is narrowed to ASCII; text mode does not
translate `\r\n`; `fopen` takes the same translated paths as `CreateFileA`.

## Building programs for it

Install Visual Studio with the C++ tools and a Windows 10/11 SDK (the build looks in the usual
places; set `WINSDK_ROOT`, `WINSDK_VER` and `MSVC_ROOT` if yours are elsewhere). Then:

```bash
make CC=clang uefi-loader     # builds WIN32.DLL and the test programs into out/uefi/APPS
```

Without the SDK that step is skipped with a note and the rest builds as before.

The simplest way is the Visual Studio way, with the real CRT headers and the dynamic C runtime:

```bash
clang --target=x86_64-pc-windows-msvc -O1 -fno-stack-protector -c app.c   # plus the SDK -isystem paths
lld-link -subsystem:console -entry:mainCRTStartup -nodefaultlib app.obj msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib
```

(`src/win32/tests/crttest.c` and the `crttest.exe` rule in the Makefile do exactly this; with
`cl /MD` or `clang-cl /MD` it is the same thing.) A program may also be linked with no C runtime
at all, which keeps it tiny; it then supplies what the compiler expects itself:

```c
#include <windows.h>
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) { ... }
```

```bash
clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -fno-stack-protector -c app.c  # plus the SDK -isystem paths
lld-link -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib app.obj crt.obj entry_gui.obj kernel32.lib user32.lib gdi32.lib
```

`src/win32/tests/crt.c` supplies what the compiler expects without a CRT (`memcpy`, `memset`,
`__chkstk`, `_fltused`) and `entry_console.c` / `entry_gui.c` the entry points (`mainCRTStartup` calls
`main`, `WinMainCRTStartup` calls `WinMain`). Copy the `.exe` into `\APPS` (with `WIN32.DLL`) on the
boot volume or any volume. Start it from Files (double-click) or the Console (`run NAME`, which
finds `NAME.BIN` or `NAME.EXE` in `\APPS`, or `run /PATH/NAME.EXE`).

## Testing

`make CC=clang USER_SELFTEST=10 uefi-loader`, boot, and read the `[PE]` lines on the serial log
(`scripts/run-qemu-windows.sh`, with `out/run/data.img` attached: the tests write files, and QEMU's
virtual FAT drive is not a good place for that). It runs, on the real desktop:

- `WINHELLO.EXE`: console output, the command line, the heap, a file round trip (exit code 7);
- `WINCRT.EXE` (`tests/crttest.c`, built with `/MD`): 119 checks of the C runtime: `printf` (integers,
  exact doubles, widths, wide strings), `sscanf`, string functions, `strtod`/`strtol`, math,
  `malloc`/`qsort`/`bsearch`, file I/O round trips, `gmtime`/`strftime`/`mktime`, `setjmp`/`longjmp`,
  `__declspec(thread)` variables, `atexit`; the exit code is the number of failed checks (0);
- `WINBAD.EXE`: calls an import the runtime lacks (ends with `0xC0000139`); a file with only an
  `MZ` header is refused;
- `WINFP.EXE` three at once: long floating-point computations that must repeat exactly while the
  others run (fails if the SSE state is not saved per task: checked by disabling the save);
- `WINMSG.EXE`: `MessageBoxA`; Enter returns `IDYES`;
- `WINGUI.EXE`: window class, `WM_PAINT` with GDI (pixels are checked), mouse clicks, typed
  characters, a timer and `PostQuitMessage` (the exit code encodes what it saw);
- six more runs and a leak check (image, DLL and heap are freed).

`USER_SELFTEST=11` just starts `WINGUI.EXE` and `WINMSG.EXE` and leaves them on the desktop, for a
screenshot (`python scripts/qemu-screenshot.py out/run/shot.png 28`).

## Limits worth knowing

- One thread. Anything that waits for another thread or process never wakes up.
- A 16 KiB stack: the CRT's `printf` is frugal, but deep recursion or big local arrays will fault.
- The 2 MiB heap and 16 KiB stack are fixed; a program that needs more fails its allocations.
- Programs larger than about 900 KB (together with the runtime) do not fit the load window.
- Not a Windows compatibility layer in the sense of running arbitrary software: it runs programs
  that stay inside the subset above. Programs from real toolchains that link the CRT, use
  exceptions or threads, or draw with bitmaps and controls will not get far yet.
